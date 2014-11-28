
#include <iostream>
#include "Logger.hh"
#include <thread>
#include <limits>
#include <unistd.h>
#include <fcntl.h>
#include <sys/uio.h>



std::string root_folder = "/silo_log"; // this folder stores pepoch, cepoch and other information

bool Logger::g_persist = false;
bool Logger::g_call_fsync = true;
bool Logger::g_use_compression = false;
bool Logger::g_fake_writes = false;
size_t Logger::g_nworkers = 0;


Logger::epoch_array Logger::per_thread_sync_epochs_[Logger::g_nmax_loggers];
util::aligned_padded_elem<std::atomic<uint64_t>> Logger::system_sync_epoch_(0);

Logger::persist_ctx Logger::g_persist_ctxs[MAX_THREADS_];

void Logger::Init(size_t nworkers, const std::vector<std::string> &logfiles, const std::vector<std::vector<unsigned>> &assignments_given, std::vector<std::vector<unsigned>> *assignments_used, bool call_fsync, bool use_compression, bool fake_writes) {
  assert(!g_persist);
  assert(g_nworkers == 0);
  assert(nworkers > 0);
  assert(!logfiles.empty());
  assert(logfiles.size() <= g_nmax_loggers);
  assert(!use_compression || g_perthread_buffers > 1); // need one as scratch buffer
  
  g_persist = true;
  g_call_fsync = call_fsync;
  g_use_compression = use_compression;
  g_fake_writes = fake_writes;
  g_nworkers = nworkers;
  
  // Initialize per_thread_sync_epochs
  for (size_t i = 0; i < g_nmax_loggers; i++)
    for (size_t j = 0; j < g_nworkers; j++)
      per_thread_sync_epochs_[i].epochs_[j].store(0, std::memory_order_release);
  
  std::vector<std::thread> writers;
  std::vector<std::vector<unsigned>> assignments(assignments_given);
  
  if (assignments.empty()) {
    if (g_nworkers <= logfiles.size()) {
      for (size_t i = 0; i < g_nworkers; i++) {
        assignments.push_back({(unsigned) i});
      }
    } else {
      const size_t threads_per_logger = g_nworkers / logfiles.size();
      for (size_t i = 0; i < logfiles.size(); i++) {
        assignments.emplace_back(MakeRange<unsigned>(i * threads_per_logger, ((i + 1) == logfiles.size()) ? g_nworkers : (i+1) * threads_per_logger));
      }
    }
  }
  
  // Assign logger threads
  for (size_t i = 0; i < assignments.size(); i++) {
    writers.emplace_back(Logger::writer, i, logfiles[i], assignments[i]);
    writers.back().detach();
  }
  
  // persist_thread is responsible for calling synch epoch.
  std::thread persist_thread(&Logger::persister, assignments);
  persist_thread.detach();
  
  
  if (assignments_used)
    *assignments_used = assignments;
}

void Logger::persister(std::vector<std::vector<unsigned>> assignments) {
  for (;;) {
    usleep(100000); // sleep for 100 s  (actually 40s was used in Silo)
    advance_system_sync_epoch(assignments);
  }
}

void Logger::advance_system_sync_epoch(const std::vector<std::vector<unsigned>> &assignments) {
  uint64_t min_so_far = std::numeric_limits<uint64_t>::max();
  const uint64_t cur_epoch = Transaction::global_epoch; // TODO: check if transaction's global epoch is implemented as expected.
  const uint64_t best_epoch = cur_epoch ? (cur_epoch - 1) : 0;
  
  for (size_t i = 0; i < assignments.size(); i++) {
    for (auto j :assignments[i]) {
      for (size_t k = j; k < MAX_THREADS_; k += g_nworkers) {
        // we need to arbitrarily advance threads which are not "doing
        // anything", so they don't drag down the persistence of the system. if
        // we can see that a thread is NOT in a guarded section AND its
        // core->logger queue is empty, then that means we can advance its sync
        // epoch up to best_tick_inc, b/c it is guaranteed that the next time
        // it does any actions will be in epoch > best_tick_inc

        
        // we also need to make sure that any outstanding buffer (should only have 1)
        // is written to disk
        
        persist_ctx &ctx = persist_ctx_for(k, INITMODE_NONE);
        
        if (!ctx.persist_buffers_.peek()) {
          // Need to lock the thread
          spinlock &l = ctx.lock_;
          if (!l.is_locked()) {
            bool did_lock = false;
            for (size_t c = 0; c < 3; c++) {
              if (l.try_lock()) {
                did_lock = true;
                break;
              }
            }
            
            if (did_lock) {
              pbuffer * last_px = ctx.all_buffers_.peek();
              if (last_px && last_px->header()->nentries_ > 0) {
                // Outstanding buffer; should remove it and add to the push buffers.
                ctx.all_buffers_.deq();
                ctx.persist_buffers_.enq(last_px);
              }
              if (!ctx.persist_buffers_.peek()) {
                // If everything is written to disk and all buffers are clean, then increment epoch for that worker.
                min_so_far = std::min(min_so_far, best_epoch);
                per_thread_sync_epochs_[i].epochs_[k].store(best_epoch, std::memory_order_release);
                
                l.unlock();
                continue;
              }
              l.unlock();
            }
          }
        }
        
        min_so_far = std::min(per_thread_sync_epochs_[i].epochs_[k].load(std::memory_order_acquire), min_so_far);

      }
    }
  }
  
  const uint64_t syssync = system_sync_epoch_->load(std::memory_order_acquire);
  assert(min_so_far < std::numeric_limits<uint64_t>::max());
  assert(syssync <= min_so_far); // TODO: is this actually true?
  
  
  // write the persistent epoch to disk
  // to avoid failure during write, write to another file and then rename
  
  // Is this correct? Does min_so_far represent the actual persisted epoch?
  if (syssync < min_so_far) {
    std::string persist_epoch_symlink = root_folder + "/pepoch";
    std::string persist_epoch_symlinked_name = "persist_epoch_";
    std::string persist_epoch_filename = root_folder + "/persist_epoch_";
    
    persist_epoch_filename.append(std::to_string(min_so_far));
    persist_epoch_symlinked_name.append(std::to_string(min_so_far));
    
    int fd = open((char*) persist_epoch_filename.c_str(), O_WRONLY|O_CREAT, 0777);
    ssize_t epoch_ret = write(fd, (char *) &min_so_far, 8);
    if (epoch_ret == -1) {
      perror("Writing persist epoch to disk failed");
      assert(false);
    }
    fsync(fd);
    close(fd);
    
    if (rename(persist_epoch_filename.c_str(), persist_epoch_symlink.c_str()) < 0) {
      perror("Renaming failure");
      assert(false);
    }
  }
  
  system_sync_epoch_->store(min_so_far, std::memory_order_release);
}

#define LOGBUFSIZE (4 * 1024 * 1024)
void Logger::writer(unsigned id, std::string logfile, std::vector<unsigned> assignment) {
  // TODO: deal with pinning to numa nodes later
  
  int fd = -1;
  uint64_t min_epoch_so_far = 0;
  uint64_t max_epoch_so_far = 0;
  
  std::vector<iovec> iovs(std::min(size_t(IOV_MAX), g_nworkers * g_perthread_buffers));
  std::vector<pbuffer*> pxs;
  
  uint64_t epoch_prefixes[MAX_THREADS_];
  memset(&epoch_prefixes, 0, sizeof(epoch_prefixes));
  
  std::string logfile_name(logfile);
  logfile_name.append("data.log");
  
  size_t nbufswritten = 0, nbyteswritten = 0, totalbyteswritten = 0, totalbufswritten = 0;
  for(;;) {
    usleep(100000); // To support batch IO
    
    if (fd == -1 || max_epoch_so_far - min_epoch_so_far > 200 ) {
      if (max_epoch_so_far - min_epoch_so_far > 200) {
        std::string fname(logfile);
        fname.append("old_data");
        fname.append(std::to_string(max_epoch_so_far));
        
        if (rename(logfile_name.c_str(), fname.c_str()) < 0){
          perror("Renaming failure");
          assert(false);
        }
            
        close(fd);
      }
      
      fd = open (logfile_name.c_str(), O_CREAT|O_WRONLY|O_TRUNC, 0664);
      if (fd == -1) {
        perror("Log file open failure");
        assert(false);
      }
      
      min_epoch_so_far = max_epoch_so_far;
    }
    
    const uint64_t cur_sync_epoch_ex = system_sync_epoch_->load(std::memory_order_acquire) + 1;
    nbufswritten = nbyteswritten = totalbyteswritten = totalbufswritten = 0;
    
    //NOTE: a core id in the persistence system really represets
    //all cores in the regular system modulo g_nworkers
    for (auto idx : assignment) {
      assert(idx >=0 && idx < g_nworkers);
      for (size_t k = idx; k < MAX_THREADS_; k+= g_nworkers) {
        persist_ctx &ctx = persist_ctx_for(k, INITMODE_NONE);
        ctx.persist_buffers_.peekall(pxs);
        for (auto px : pxs) {
          assert(px);
          assert(!px->io_scheduled_);
          assert(nbufswritten < iovs.size());
          assert(px->header()->nentries_);
          assert(px->thread_id_ == k);
          if (nbufswritten == iovs.size()) {
            // Writer limit met
            goto process;
          }
          
          if (epochId(px->header()->last_tid_) >= cur_sync_epoch_ex + g_max_lag_epochs) {
            // logger max log wait
            break;
          }
          iovs[nbufswritten].iov_base = (void*) &px->buffer_start_[0];
          
#ifdef LOGGER_UNSAFE_REDUCE_BUFFER_SIZE
#define PXLEN(px) (((px)->cur_offset_ < 4) ? (px)->cur_offset_ : ((px)->cur_offset_ / 4))
#else
#define PXLEN(px) ((px)->cur_offset_)
#endif
          
          const size_t pxlen = PXLEN(px);
          iovs[nbufswritten].iov_len = pxlen;
          px->io_scheduled_ = true;
          nbufswritten++;
          nbyteswritten += pxlen;
          totalbyteswritten += pxlen;
          totalbufswritten++;
          
          const uint64_t px_epoch = epochId(px->header()->last_tid_);
          epoch_prefixes[k] = px_epoch - 1;
          
          max_epoch_so_far = px_epoch > max_epoch_so_far ? px_epoch : max_epoch_so_far;
        }
      }
      
    process:
      if (!g_fake_writes && nbufswritten > 0) {
        const ssize_t ret = writev(fd, &iovs[0], nbufswritten);
        if (ret == -1) {
          perror("writev");
          assert(false);
        }
        
        nbufswritten = nbyteswritten = 0;
        
        //after writev is called, buffers can be immediately returned to the workers
        for (size_t k = idx; k < MAX_THREADS_; k += g_nworkers) {
          persist_ctx &ctx = persist_ctx_for(k, INITMODE_NONE);
          pbuffer *px, *px0;
          while ((px = ctx.persist_buffers_.peek()) && px->io_scheduled_) {
            px0 = ctx.persist_buffers_.deq();
            assert(px == px0);
            assert(px->header()->nentries_);
            px0->reset();
            assert(ctx.init_);
            assert(px0->thread_id_ == k);
            ctx.all_buffers_.enq(px0);
          }
        }
      }
    }
    
    if (!totalbufswritten) {
      // should probably sleep here
      __asm volatile("pause" : :);
      continue;
    }
    
    if (!g_fake_writes && g_call_fsync) {
      int fret = fsync(fd);
      if (fret < 0) {
        perror("fsync logger failed\n");
        assert(false);
      }
    }
    
    // TODO: why is this necessary?
    epoch_array &ea = per_thread_sync_epochs_[id];
    for (auto idx : assignment) {
      for (size_t k = idx; k < MAX_THREADS_; k += g_nworkers) {
        const uint64_t x0 = ea.epochs_[k].load(std::memory_order_acquire);
        const uint64_t x1 = epoch_prefixes[k];
        if (x1 > x0) {
          ea.epochs_[k].store(x1, std::memory_order_release);
        }
      }
    }
  }
}

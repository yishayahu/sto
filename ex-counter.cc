#include <string>
#include <iostream>
#include <sys/time.h>
#include <assert.h>
#include "Transaction.hh"
#include "TWrapped.hh"
#include "clp.h"
#define GUARDED if (TransactionGuard tguard{})

void print_time(struct timeval tv1, struct timeval tv2) {
  printf("%f\n", (tv2.tv_sec-tv1.tv_sec) + (tv2.tv_usec-tv1.tv_usec)/1000000.0);
}

class TCounter1 : public TObject {
  TWrapped<int> n_;
  TVersion v_;
  static int wval(TransProxy it) {
     return it.write_value<int>(0);
  }
  static int wval(const TransItem& it) {
     return it.write_value<int>(0);
  }
public:
    TCounter1(int n = 0)
        : n_(n) {
    }
    int nontrans_access() {
        return n_.access();
    }
  void increment() {
     auto it = Sto::item(this, 0);
     it.add_write(wval(it) + 1);
  }
  void decrement() {
     auto it = Sto::item(this, 0);
     it.add_write(wval(it) - 1);
  }
  bool test() const {
     auto it = Sto::item(this, 0);
     int n = n_.read(it, v_);
     return n + wval(it) > 0;
  }

    bool lock(TransItem& item, Transaction& txn) override {
        return txn.try_lock(item, v_);
    }
    bool check(const TransItem& it, const Transaction&) override {
        return it.check_version(v_);
    }
    void install(TransItem& it, const Transaction& txn) override {
        n_.access() += wval(it);
        v_.set_version(txn.commit_tid());
    }
    void unlock(TransItem&) override {
        v_.unlock();
    }
};

class TCounter2 : public TObject {
  TWrapped<int> n_;
  TVersion v_;
  TVersion zc_v_;
  static int wval(TransProxy it) {
     return it.write_value<int>(0);
  }
  static int wval(const TransItem& it) {
     return it.write_value<int>(0);
  }
public:
    TCounter2(int n = 0)
        : n_(n) {
    }
    int nontrans_access() {
        return n_.access();
    }
  void increment() {
     auto it = Sto::item(this, 0);
     it.add_write(wval(it) + 1);
  }
  void decrement() {
     auto it = Sto::item(this, 0);
     it.add_write(wval(it) - 1);
  }
  bool test() const {
     auto it = Sto::item(this, 0);
     int n;
     if (!it.has_write()) {
        auto zc_it = Sto::item(this, 1);
        n = n_.read(zc_it, zc_v_);
     } else
        n = n_.read(it, v_);
     return n + wval(it) > 0;
  }

    bool lock(TransItem& it, Transaction& txn) override {
        bool ok = txn.try_lock(it, v_);
        if (ok) {
            int n = n_.access();
            if ((n > 0) != (n + wval(it) > 0))
                zc_v_.lock();
        }
        return ok;
    }
    bool check(const TransItem& it, const Transaction&) override {
        return it.check_version(it.key<int>() ? zc_v_ : v_);
    }
    void install(TransItem& it, const Transaction& txn) override {
        n_.access() += wval(it);
        v_.set_version(txn.commit_tid());
        if (zc_v_.is_locked_here())
            zc_v_.set_version(txn.commit_tid());
    }
    void unlock(TransItem&) override {
        v_.unlock();
        if (zc_v_.is_locked_here())
            zc_v_.unlock();
    }
};

class TCounter3 : public TObject {
  TWrapped<int> n_;
  TVersion v_;
  struct precord {
    int value;
    bool gt;
  };
  static int wval(TransProxy it) {
     return it.write_value<int>(0);
  }
  static int wval(const TransItem& it) {
     return it.write_value<int>(0);
  }
public:
    TCounter3(int n = 0)
        : n_(n) {
    }
    int nontrans_access() {
        return n_.access();
    }
  void increment() {
     auto it = Sto::item(this, 0);
     it.add_write(wval(it) + 1);
  }
  void decrement() {
     auto it = Sto::item(this, 0);
     it.add_write(wval(it) - 1);
  }
  bool test() const {
     auto it = Sto::item(this, 0);
     assert(!it.has_predicate());
     int n = n_.wait_snapshot(it, v_, false);
     bool gt = n + wval(it) > 0;
     it.set_predicate(precord{-wval(it), gt});
     return gt;
  }

    bool lock(TransItem& item, Transaction& txn) override {
        return txn.try_lock(item, v_);
    }
    bool check_predicate(TransItem& item, Transaction& txn, bool committing) override {
        TransProxy p(txn, item);
        precord pred = item.template predicate_value<precord>();
        int n = n_.wait_snapshot(p, v_, committing);
        return (n > pred.value) == pred.gt;
    }
    bool check(const TransItem& it, const Transaction&) override {
        return it.check_version(v_);
    }
    void install(TransItem& it, const Transaction& txn) override {
        n_.access() += wval(it);
        v_.set_version(txn.commit_tid());
    }
    void unlock(TransItem&) override {
        v_.unlock();
    }
};


static int initial_value = 100;
static double test_fraction = 0.5;
static uint64_t nops = 100000000;
static int nthreads = 4;

enum { opt_nthreads = 1, opt_nops, opt_test_fraction, opt_initial_value };

static const Clp_Option options[] = {
  { "nthreads", 'j', opt_nthreads, Clp_ValInt, 0 },
  { "ntrans", 'n', opt_nops, Clp_ValInt, 0 },
  { "test-fraction", 'f', opt_test_fraction, Clp_ValDouble, 0 },
  { "initial-value", 'i', opt_initial_value, Clp_ValInt, 0 },
};


struct result {
    uint64_t ngt;
    int final_value;
};

class Tester {
public:
    virtual result run() = 0;
};

template <typename T>
class TTester : public Tester {
public:
    static T* counter;
    static volatile unsigned go;
    static void* runfunc(void*);
    result run();
};

template <typename T> T* TTester<T>::counter;
template <typename T> volatile unsigned TTester<T>::go;

template <typename T>
void* TTester<T>::runfunc(void* arg) {
    T* counter = TTester<T>::counter;
    TThread::set_id((int) (uintptr_t) arg);
    Transaction* txn = Sto::transaction();
    txn->local_srandom(random());
    unsigned test_threshold = test_fraction * 0xFFFFFFFFU;
    uint64_t ops_per_thread = nops / nthreads;
    uintptr_t count_test = 0;

    while (!go)
        relax_fence();
    uint64_t a = 0, b = 0, c = 0;

    for (uint64_t i = 0; i < ops_per_thread; ++i) {
        unsigned op = txn->local_random();
        if (op < test_threshold) {
            bool isgt;
            TRANSACTION {
                isgt = counter->test();
            } RETRY(true);
            count_test += isgt;
            ++a;
        } else if (op & 1024) {
            TRANSACTION {
                counter->increment();
            } RETRY(true);
            ++b;
        } else {
            TRANSACTION {
                counter->decrement();
            } RETRY(true);
            ++c;
        }
    }

    //printf("%llu %llu %llu\n", (unsigned long long) a, (unsigned long long) b, (unsigned long long) c);
    return reinterpret_cast<void*>(count_test);
}

template <typename T>
result TTester<T>::run() {
    counter = new T(initial_value);
    pthread_t tids[nthreads];
    go = 0;

    for (uintptr_t i = 0; i < uintptr_t(nthreads); ++i)
        pthread_create(&tids[i], NULL, runfunc, reinterpret_cast<void*>(i));

    pthread_t advancer;
    pthread_create(&advancer, NULL, Transaction::epoch_advancer, NULL);
    pthread_detach(advancer);

    go = 1;
    uint64_t total = 0;
    for (int i = 0; i < nthreads; ++i) {
        void* mine;
        pthread_join(tids[i], &mine);
        total += reinterpret_cast<uintptr_t>(mine);
    }
    return result{total, counter->nontrans_access()};
}

static Tester* ttesters[] = { new TTester<TCounter1>,
    new TTester<TCounter2>, new TTester<TCounter3> };

int main(int argc, char *argv[]) {
  Clp_Parser *clp = Clp_NewParser(argc, argv, arraysize(options), options);
  srandomdev();
  int testnum = 1;

  int opt;
  while ((opt = Clp_Next(clp)) != Clp_Done) {
    switch (opt) {
    case opt_nthreads:
      nthreads = clp->val.i;
      break;
    case opt_nops:
      nops = clp->val.i;
      break;
    case opt_test_fraction:
      test_fraction = clp->val.d;
      break;
    case opt_initial_value:
      initial_value = clp->val.i;
      break;
    case Clp_NotOption:
      testnum = atoi(clp->vstr);
      assert(testnum >= 1 && testnum <= 3);
      break;
    default:
      assert(0);
    }
  }
  Clp_DeleteParser(clp);

  struct timeval tv1,tv2;
  struct rusage ru1,ru2;
  gettimeofday(&tv1, NULL);
  getrusage(RUSAGE_SELF, &ru1);
  result r = ttesters[testnum - 1]->run();
  gettimeofday(&tv2, NULL);
  getrusage(RUSAGE_SELF, &ru2);
  printf("real time: ");
  print_time(tv1,tv2);
  printf("utime: ");
  print_time(ru1.ru_utime, ru2.ru_utime);
  printf("stime: ");
  print_time(ru1.ru_stime, ru2.ru_stime);

  printf("test %d, nthreads %d, ntrans %llu, test_fraction %g, initial_value %d\n",
         testnum, nthreads, (unsigned long long) nops, test_fraction, initial_value);
  printf("test() true %llu, value %d\n", (unsigned long long) r.ngt, r.final_value);
#if STO_PROFILE_COUNTERS
  Transaction::print_stats();
  if (txp_count >= txp_total_aborts) {
      txp_counters tc = Transaction::txp_counters_combined();
      const char* sep = "";
      if (txp_count > txp_total_w) {
          printf("%stotal_n: %llu, total_r: %llu, total_w: %llu", sep, tc.p(txp_total_n), tc.p(txp_total_r), tc.p(txp_total_w));
          sep = ", ";
      }
      if (txp_count > txp_total_searched) {
          printf("%stotal_searched: %llu", sep, tc.p(txp_total_searched));
          sep = ", ";
      }
      if (txp_count > txp_total_aborts) {
          printf("%stotal_aborts: %llu (%llu aborts at commit time)\n", sep, tc.p(txp_total_aborts), tc.p(txp_commit_time_aborts));
          sep = ", ";
      }
      if (*sep)
          printf("\n");
  }
#endif
}

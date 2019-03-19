// Garbage collection and other bookkeeping tasks

#pragma once

#include <deque>

#include "MVCCTypes.hh"
#include "TThread.hh"

#ifndef MAX_THREADS
#define MAX_THREADS 128
#endif

class MvRegistry {
public:
    typedef MvHistoryBase base_type;
    typedef TransactionTid::type type;
    const static size_t CYCLE_LENGTH = 10;
    const static size_t GC_PER_FLATTEN = 1;

    MvRegistry() : enable_gc(false) {
        always_assert(
            this == &MvRegistry::registrar_,
            "Only one MvRegistry can be created, which is the "
            "MvRegistry::registrar()");
        for (size_t i = 0; i < (sizeof registries) / (sizeof *registries); i++) {
            registries[i] = new registry_type();
        }
        memset(collect_call_cnts, 0, sizeof collect_call_cnts);
        memset(collect_down_call_cnts, 0, sizeof collect_down_call_cnts);
        memset(collect_up_call_cnts, 0, sizeof collect_up_call_cnts);
        memset(collect_down_visit_cnts, 0, sizeof collect_down_visit_cnts);
        memset(collect_up_visit_cnts, 0, sizeof collect_up_visit_cnts);
        memset(collect_free_cnts, 0, sizeof collect_free_cnts);
        memset(convert_down_up_cnts, 0, sizeof convert_down_up_cnts);
        memset(convert_up_down_cnts, 0, sizeof convert_up_down_cnts);
    }

    ~MvRegistry() {
        is_stopping = true;
        while (is_running > 0);
    }

    static void cleanup() {
        registrar().cleanup_();
    }

    // XXX: VERY NOT THREAD-SAFE!
    static void collect_garbage() {
        if (registrar().enable_gc && !((++cycles) % CYCLE_LENGTH)) {
            for (size_t i = 0; i < (sizeof registries) / (sizeof *registries); i++) {
                registrar().collect_garbage_(i);
            }
        }
    }

    // XXX: NOT THREAD-SAFE FOR TWO CALLS WITH THE SAME INDEX
    static void collect_garbage(const size_t index) {
        if (registrar().enable_gc && !((++cycles) % CYCLE_LENGTH)) {
            registrar().collect_garbage_(index);
        }
    }

    static bool done() {
        return registrar().done_();
    }

    template <typename T>
    static void reg(MvObject<T> * const obj, const type tid, std::atomic<bool> * const flag) {
        registrar().reg_(obj, tid, flag);
    }

    inline static MvRegistry& registrar() {
        return registrar_;
    }

    inline static type rtid_inf() {
        return registrar().compute_rtid_inf();
    }

    static void stop() {
        registrar().stop_();
    }

    static void toggle_gc(const bool enabled) {
        registrar().enable_gc = enabled;
    }

    static void print_counters();

private:
    // Represents the head element of an MvObject
    struct MvRegistryEntry;
    typedef MvRegistryEntry entry_type;

    struct MvRegistryEntry {
        typedef std::atomic<base_type*> head_type;
        MvRegistryEntry(
            head_type * const head, base_type * const ih, base_type * const bv,
            const type tid, std::atomic<bool> * const flag) :
            head(head), inlined(ih), base_version(bv), tid(tid), flag(flag) {}

        head_type * const head;
        base_type * const inlined;
        base_type *base_version;  // A cached pointer to the base version
        const type tid;
        std::atomic<bool> * const flag;
    };

    static __thread size_t cycles;
    static MvRegistry registrar_;

    std::atomic<bool> enable_gc;
    std::atomic<size_t> is_running;
    std::atomic<bool> is_stopping;
    typedef std::deque<entry_type> registry_type;
    registry_type *registries[MAX_THREADS];

    size_t collect_call_cnts[MAX_THREADS];
    size_t collect_down_call_cnts[MAX_THREADS];
    size_t collect_up_call_cnts[MAX_THREADS];
    size_t collect_down_visit_cnts[MAX_THREADS];
    size_t collect_up_visit_cnts[MAX_THREADS];
    size_t collect_free_cnts[MAX_THREADS];
    size_t convert_down_up_cnts[MAX_THREADS];
    size_t convert_up_down_cnts[MAX_THREADS];

    void cleanup_() {
        for (size_t i = 0; i < (sizeof registries) / (sizeof *registries); i++) {
            delete registries[i];
        }
    }

    void collect_(const size_t, const type);

    void collect_garbage_(const size_t index) {
        if (is_stopping) {
            return;
        }
        const type rtid_inf = registrar().compute_rtid_inf();
        registrar().is_running++;
        if (is_stopping) {
            is_running--;
            return;
        }
        if (!(cycles % (CYCLE_LENGTH * GC_PER_FLATTEN))) {
            flatten_(index, rtid_inf);
        }
        collect_(index, rtid_inf);
        is_running--;
    }

    type compute_rtid_inf();
   
    bool done_() {
        return is_running == 0;
    }

    void flatten_(size_t index, const type);

    template <typename T>
    void reg_(MvObject<T> * const, const type, std::atomic<bool> * const);

    inline registry_type& registry(
            const size_t threadid=TThread::id()) {
        return *registries[threadid];
    }

    void stop_() {
        is_stopping = true;
    }
};

template <typename T>
void MvRegistry::reg_(MvObject<T> * const obj, const type tid, std::atomic<bool> * const flag) {
    registry().push_back(entry_type(&obj->h_,
#if MVCC_INLINING
        &obj->ih_
#else
        nullptr
#endif
        , nullptr, tid, flag));
}

#include "runtime_internal.h"
#include "HalideRuntime.h"
#include "printer.h"
#include "scoped_mutex_lock.h"

// Note: The profiler thread may out-live any valid user_context, or
// be used across many different user_contexts, so nothing it calls
// can depend on the user context.

extern "C" {
// Returns the address of the global halide_profiler state
WEAK halide_profiler_state *halide_profiler_get_state() {
    static halide_profiler_state s = {{{0}}, NULL, 1, 0, 0, false, NULL};
    return &s;
}
}

namespace Halide { namespace Runtime { namespace Internal {

WEAK halide_profiler_pipeline_stats *find_or_create_pipeline(const char *pipeline_name, int num_funcs, const uint64_t *func_names) {
    halide_profiler_state *s = halide_profiler_get_state();

    for (halide_profiler_pipeline_stats *p = s->pipelines; p;
         p = (halide_profiler_pipeline_stats *)(p->next)) {
        // The same pipeline will deliver the same global constant
        // string, so they can be compared by pointer.
        if (p->name == pipeline_name &&
            p->num_funcs == num_funcs) {
            return p;
        }
    }
    // Create a new pipeline stats entry.
    halide_profiler_pipeline_stats *p =
        (halide_profiler_pipeline_stats *)malloc(sizeof(halide_profiler_pipeline_stats));
    if (!p) return NULL;
    p->next = s->pipelines;
    p->name = pipeline_name;
    p->first_func_id = s->first_free_id;
    p->num_funcs = num_funcs;
    p->runs = 0;
    p->time = 0;
    p->samples = 0;
    p->memory_current = 0;
    p->memory_peak = 0;
    p->memory_total = 0;
    p->num_allocs = 0;
    p->funcs = (halide_profiler_func_stats *)malloc(num_funcs * sizeof(halide_profiler_func_stats));
    if (!p->funcs) {
        free(p);
        return NULL;
    }
    for (int i = 0; i < num_funcs; i++) {
        p->funcs[i].time = 0;
        p->funcs[i].name = (const char *)(func_names[i]);
        p->funcs[i].memory_current = 0;
        p->funcs[i].memory_peak = 0;
        p->funcs[i].memory_total = 0;
        p->funcs[i].num_allocs = 0;
    }
    s->first_free_id += num_funcs;
    s->pipelines = p;
    return p;
}

WEAK void bill_func(halide_profiler_state *s, int func_id, uint64_t time) {
    halide_profiler_pipeline_stats *mru_p = s->mru_pipeline;
    if (mru_p != NULL) {
        if (func_id >= mru_p->first_func_id && func_id < mru_p->first_func_id + mru_p->num_funcs) {
            mru_p->funcs[func_id - mru_p->first_func_id].time += time;
            mru_p->time += time;
            mru_p->samples++;
            return;
        }
    }

    for (halide_profiler_pipeline_stats *p = s->pipelines; p;
         p = (halide_profiler_pipeline_stats *)(p->next)) {
        if (func_id >= p->first_func_id && func_id < p->first_func_id + p->num_funcs) {
            s->mru_pipeline = p; // Update pipeline cache
            p->funcs[func_id - p->first_func_id].time += time;
            p->time += time;
            p->samples++;
            return;
        }
    }
    // Someone must have called reset_state while a kernel was running. Do nothing.
}

WEAK void sampling_profiler_thread(void *) {
    halide_profiler_state *s = halide_profiler_get_state();

    // grab the lock
    halide_mutex_lock(&s->lock);

    while (s->current_func != halide_profiler_please_stop) {

        uint64_t t1 = halide_current_time_ns(NULL);
        uint64_t t = t1;
        while (1) {
            uint64_t t_now = halide_current_time_ns(NULL);
            int func = s->current_func;
            if (func == halide_profiler_please_stop) {
                break;
            } else if (func >= 0) {
                // Assume all time since I was last awake is due to
                // the currently running func.
                bill_func(s, func, t_now - t);
            }
            t = t_now;

            // Release the lock, sleep, reacquire.
            int sleep_ms = s->sleep_time;
            halide_mutex_unlock(&s->lock);
            halide_sleep_ms(NULL, sleep_ms);
            halide_mutex_lock(&s->lock);
        }
    }

    s->started = false;

    halide_mutex_unlock(&s->lock);
}

WEAK halide_profiler_pipeline_stats *find_pipeline_stats(void *user_context, const char *pipeline_name) {
    halide_profiler_state *s = halide_profiler_get_state();

    // Check cache first
    halide_profiler_pipeline_stats *mru_p = s->mru_pipeline;
    if ((mru_p != NULL) && (mru_p->name == pipeline_name)) {
        return mru_p;
    }

    for (halide_profiler_pipeline_stats *p = s->pipelines; p;
         p = (halide_profiler_pipeline_stats *)(p->next)) {
        // The same pipeline will deliver the same global constant
        // string, so they can be compared by pointer.
        if (p->name == pipeline_name) {
            return p;
        }
    }
    return NULL;
}

}}}

extern "C" {
// Returns the address of the pipeline state associated with pipeline_name.
WEAK halide_profiler_pipeline_stats *halide_profiler_get_pipeline_state(const char *pipeline_name) {
    halide_profiler_state *s = halide_profiler_get_state();

    ScopedMutexLock lock(&s->lock);

    // Check the cache first
    halide_profiler_pipeline_stats *mru_p = s->mru_pipeline;
    if ((mru_p != NULL) && (mru_p->name == pipeline_name)) {
        return mru_p;
    }

    for (halide_profiler_pipeline_stats *p = s->pipelines; p;
         p = (halide_profiler_pipeline_stats *)(p->next)) {
        // The same pipeline will deliver the same global constant
        // string, so they can be compared by pointer.
        if (p->name == pipeline_name) {
            return p;
        }
    }
    return NULL;
}

// Returns a token identifying this pipeline instance.
WEAK int halide_profiler_pipeline_start(void *user_context,
                                        const char *pipeline_name,
                                        int num_funcs,
                                        const uint64_t *func_names) {
    //ctx = user_context;

    halide_profiler_state *s = halide_profiler_get_state();

    ScopedMutexLock lock(&s->lock);

    if (!s->started) {
        halide_start_clock(user_context);
        halide_spawn_thread(user_context, sampling_profiler_thread, NULL);
        s->started = true;
    }

    halide_profiler_pipeline_stats *p =
        find_or_create_pipeline(pipeline_name, num_funcs, func_names);
    if (!p) {
        // Allocating space to track the statistics failed.
        return halide_error_out_of_memory(user_context);
    }
    p->runs++;

    return p->first_func_id;
}

WEAK void halide_profiler_memory_allocate(void *user_context,
                                          void *pipeline_state,
                                          int token,
                                          int func_id,
                                          int incr) {
    func_id += token;

    halide_profiler_pipeline_stats *p_stats = (halide_profiler_pipeline_stats *) pipeline_state;

    halide_assert(user_context, p_stats != NULL);
    halide_assert(user_context, (func_id - p_stats->first_func_id) >= 0);
    halide_assert(user_context, (func_id - p_stats->first_func_id) < p_stats->num_funcs);

    halide_profiler_func_stats *f_stats = &p_stats->funcs[func_id - p_stats->first_func_id];

    // Note: Update to the memory counter is done without grabbing the state's lock to
    // reduce lock contention. One potential issue is that other call that frees the
    // pipeline and function stats structs may be running in parallel. However, the
    // current desctructor (called on profiler shutdown) does not free the structs
    // unless user specifically calls halide_profiler_reset().

    // Update per-pipeline memory stats
    __sync_add_and_fetch(&p_stats->num_allocs, 1);
    __sync_add_and_fetch(&p_stats->memory_total, incr);
    int p_mem_current = __sync_add_and_fetch(&p_stats->memory_current, incr);
    if (p_mem_current > p_stats->memory_peak) {
        p_stats->memory_peak = p_mem_current;
    }

    // Update per-func memory stats
    __sync_add_and_fetch(&f_stats->num_allocs, 1);
    __sync_add_and_fetch(&f_stats->memory_total, incr);
    int f_mem_current = __sync_add_and_fetch(&f_stats->memory_current, incr);
    if (f_mem_current > f_stats->memory_peak) {
        f_stats->memory_peak = f_mem_current;
    }
}

WEAK void halide_profiler_memory_free(void *user_context,
                                      void *pipeline_state,
                                      int token,
                                      int func_id,
                                      int decr) {
    func_id += token;

    halide_profiler_pipeline_stats *p_stats = (halide_profiler_pipeline_stats *) pipeline_state;

    halide_assert(user_context, p_stats != NULL);
    halide_assert(user_context, (func_id - p_stats->first_func_id) >= 0);
    halide_assert(user_context, (func_id - p_stats->first_func_id) < p_stats->num_funcs);

    halide_profiler_func_stats *f_stats = &p_stats->funcs[func_id - p_stats->first_func_id];

    // Note: Update to the memory counter is done without grabbing the state's lock to
    // reduce lock contention. One potential issue is that other call that frees the
    // pipeline and function stats structs may be running in parallel. However, the
    // current desctructor (called on profiler shutdown) does not free the structs
    // unless user specifically calls halide_profiler_reset().

    // Update per-pipeline memory stats
    __sync_sub_and_fetch(&p_stats->memory_current, decr);

    // Update per-func memory stats
    __sync_sub_and_fetch(&f_stats->memory_current, decr);
}

WEAK void halide_profiler_report_unlocked(void *user_context, halide_profiler_state *s) {

    char line_buf[400];
    Printer<StringStreamPrinter, sizeof(line_buf)> sstr(user_context, line_buf);

    for (halide_profiler_pipeline_stats *p = s->pipelines; p;
         p = (halide_profiler_pipeline_stats *)(p->next)) {
        float t = p->time / 1000000.0f;
        if (!p->runs) continue;
        sstr.clear();
        int alloc_avg = 0;
        if (p->num_allocs != 0) {
            alloc_avg = p->memory_total/p->num_allocs;
        }
        sstr << p->name
             << "  total time: " << t << " ms"
             << "  samples: " << p->samples
             << "  runs: " << p->runs
             << "  time/run: " << t / p->runs << " ms"
             << "  num_allocs: " << p->num_allocs
             << "  mem_peak: " << p->memory_peak << " bytes"
             << "  mem_total: " << p->memory_total << " bytes"
             << "  alloc_avg: " << alloc_avg << " bytes\n";
        halide_print(user_context, sstr.str());
        if (p->time || p->memory_total) {
            for (int i = 0; i < p->num_funcs; i++) {
                sstr.clear();
                halide_profiler_func_stats *fs = p->funcs + i;

                // The first func is always a catch-all overhead
                // slot. Only report overhead time if it's non-zero
                if (i == 0 && fs->time == 0) continue;

                sstr << "  " << fs->name << ": ";
                while (sstr.size() < 25) sstr << " ";

                float ft = fs->time / (p->runs * 1000000.0f);
                sstr << ft << "ms";
                while (sstr.size() < 40) sstr << " ";

                int percent = 0;
                if (p->time != 0) {
                    percent = fs->time / (p->time / 100);
                }
                sstr << "(" << percent << "%)";
                while (sstr.size() < 55) sstr << " ";

                int alloc_avg = 0;
                if (fs->num_allocs != 0) {
                    alloc_avg = fs->memory_total/fs->num_allocs;
                }

                sstr << "(" << fs->memory_current << ", " << fs->memory_peak
                     << ", " << fs->memory_total << ", " << fs->num_allocs
                     << ", " << alloc_avg << ") bytes\n";

                halide_print(user_context, sstr.str());
            }
        }
    }
}

WEAK void halide_profiler_report(void *user_context) {
    halide_profiler_state *s = halide_profiler_get_state();
    ScopedMutexLock lock(&s->lock);
    halide_profiler_report_unlocked(user_context, s);
}


WEAK void halide_profiler_reset() {
    // WARNING: Do not call this method while there is other halide pipeline
    // running; halide_profiler_memory_allocate/free updates the profiler
    // pipeline's state without grabbing the global profiler state's lock.
    halide_profiler_state *s = halide_profiler_get_state();

    ScopedMutexLock lock(&s->lock);

    s->mru_pipeline = NULL;
    while (s->pipelines) {
        halide_profiler_pipeline_stats *p = s->pipelines;
        s->pipelines = (halide_profiler_pipeline_stats *)(p->next);
        free(p->funcs);
        free(p);
    }
    s->first_free_id = 0;
}

namespace {
__attribute__((destructor))
WEAK void halide_profiler_shutdown() {
    halide_profiler_state *s = halide_profiler_get_state();
    if (!s->started) return;
    s->current_func = halide_profiler_please_stop;
    do {
        // Memory barrier.
        __sync_synchronize(&s->started,
                           &s->current_func);
    } while (s->started);
    s->current_func = halide_profiler_outside_of_halide;

    s->mru_pipeline = NULL;

    // Print results. No need to lock anything because we just shut
    // down the thread.
    halide_profiler_report_unlocked(NULL, s);

    // Leak the memory. Not all implementations of ScopedMutexLock may
    // be safe to use at static destruction time (windows).
    // halide_profiler_reset();
}
}

WEAK void halide_profiler_pipeline_end(void *user_context, void *state) {
    ((halide_profiler_state *)state)->current_func = halide_profiler_outside_of_halide;
}

}

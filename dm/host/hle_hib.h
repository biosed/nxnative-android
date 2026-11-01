// Hibernation (save state to disk, DM_HIBERNATE=1): what the parts of the host share. hle_hib.c holds the fixed layout and the arena, hle_hibsnap.c the snapshot file,
// hle_thread.c parks the guest threads and describes them, hle_hibaudit.c is the pointer audit.
#pragma once
#include <stdint.h>
#include <stddef.h>

// Where a parked guest thread stopped (hib_thr.stop)
enum { HIB_NOT_STARTED = 0,   // created, StartThread not called yet
       HIB_PRE_SVC = 1,       // at a gate, before its SVC ran: the frame holds the SVC's arguments, a restored thread runs the gate from the start
       HIB_POST_SVC = 2,      // at the exit of an SVC that finished: the frame holds its results
       HIB_RESTART = 3,       // blocked in a restartable host call (hle_hib.c stubs): rs holds the registers at its entry, a restored thread calls it again
       HIB_WAIT = 4 };        // blocked in a kernel wait (lock, condition variable, address, synchronization object, sleep): the frame and the wait below

// One guest thread, as the snapshot keeps it
typedef struct {
    uint32_t handle, stop;
    uint64_t id;
    int32_t prio, core, started, host_owned, closed, last_svc;
    uint64_t mask, entry, arg, stack_top;
    uint64_t k18, ktls, ktlr;          // the thread's slots: the guest's x18, tpidr_el0 (TLS), tpidrro_el0 (ThreadLocalRegion)
    uint64_t svc_pc;                   // the gate's original instruction address (gate frame stops)
    uint64_t frame[100];               // the gate frame (entry.S dm_gate: x0-x30, q0-q31, nzcv / fpsr / fpcr, sp at +792), or the restartable call's entry registers (hle_hib.c)
    // kernel wait (HIB_WAIT): hle_thread.c thr_t
    int32_t wkind, whn;
    uint64_t waddr, wkey, wseq, whs;
    uint32_t wtag, pad;
    int64_t wleft_ns;                  // time left of a timed wait, -1 none
    char name[32];
} hib_thr;

int dm_hib_on(void);   // DM_HIBERNATE=1 (hle_hib.c)
int dm_hib_save(void);   // write the snapshot (hle_hibsnap.c): 0 done
// Park and unpark every guest thread (hle_thread.c). The caller is not a guest thread. Returns 0 when every thread stopped within timeout_ms; otherwise -1 with why filled
// in (the threads that did not stop) and the threads running again
int dm_guest_park_all(int timeout_ms, char *why, size_t n);
void dm_guest_unpark_all(void);
// With every thread parked: describe them (returns the count) / hold the scheduler lock while memory is copied (no waiting thread can finish its SVC meanwhile)
int dm_hib_threads(hib_thr *out, int max);
void dm_hib_hold(int on);
// Restartable host calls (hle_hib.c stubs): the stub's entry frame, registered on entry and cleared on return; and the point a blocked call stops at once it wakes
void dm_hib_rs_enter(const uint64_t *frame);
void dm_hib_rs_leave(void);
#include <pthread.h>
void dm_hib_native_hold(pthread_mutex_t *held);

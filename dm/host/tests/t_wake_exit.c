// DM_WAKE_AFTER_G (vk19.68b): a thread that exits wakes the threads waiting on its handle (wake_sync_waiters) with G held, then stops being a guest thread
// (pthread_setspecific(NULL)) before it releases G, so g_unlock would not find its queued wakes: the joiner slept for good, and the count of queued wakes never
// went back to 0 (the graveyard of freed threads never drained). Here the main thread waits on a worker's handle (WaitSynchronization) while the worker exits, 50 times,
// with the switch on (and off, as a control): every wait must end signalled within 500 ms (its timeout is 1 s), and no queued wake may be left outstanding
#include "../dm.h"
#include <assert.h>
#include <signal.h>
#include <stdatomic.h>
#include <unistd.h>
#include "../hle_thread.c"
static void timeout(int sig) { (void)sig; const char m[] = "[RED] t_wake_exit timed out (a lost wake-up at thread exit)\n"; write(2, m, sizeof m - 1); _exit(99); }
static void worker(uint64_t a) { (void)a; usleep(2000); }   // long enough for the main thread to be waiting on its handle
void dm_thread_enter(uint64_t entry, uint64_t arg, uint64_t sp) {
    (void)sp;
    ((void (*)(uint64_t))entry)(arg);
    uint64_t r[32] = {0}; svc_thread(0x0A, r);   // ExitThread
    abort();
}
int main(int argc, char **argv) {
    (void)argv;
    signal(SIGALRM, timeout); alarm(60);
    if (argc > 1) unsetenv("DM_WAKE_AFTER_G"); else setenv("DM_WAKE_AFTER_G", "1", 1);
    svc_thread_main_init();
    int fails = 0;
    for (int i = 0; i < 50; i++) {
        uint64_t r[32] = {0, (uint64_t)worker, 0, 0, 44, 0};
        assert(svc_thread(0x08, r) && r[0] == 0);   // CreateThread
        uint32_t h = (uint32_t)r[1];
        r[0] = h; assert(svc_thread(0x09, r) && r[0] == 0);   // StartThread
        r[1] = (uint64_t)&h; r[2] = 1; r[3] = 1000000000ull;   // WaitSynchronization on its handle, 1 s
        uint64_t t0 = now_ns(); assert(svc_thread(0x18, r)); uint64_t ms = (now_ns() - t0) / 1000000;
        if (r[0] != 0 || ms > 500) { fails++; printf("wait %d ended with %#llx after %llu ms (a lost wake-up: not signalled within 500 ms)\n", i, (unsigned long long)r[0], (unsigned long long)ms); }
        r[0] = h; assert(svc_thread(0x16, r) && r[0] == 0);   // CloseHandle: the thread is freed (or kept in the graveyard)
    }
    pthread_mutex_lock(&G); int out = atomic_load(&g_pw_out); thr_t *grave = g_grave; pthread_mutex_unlock(&G);
    printf("t_wake_exit (DM_WAKE_AFTER_G %s): 50 joins, %d not signalled; queued wakes outstanding %d; graveyard %s\n", argc > 1 ? "off" : "on", fails, out, grave ? "not empty" : "empty");
    return fails || out || grave;
}

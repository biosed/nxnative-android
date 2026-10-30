// Device check for the Android thread slots (patch_nso.py DM_PATCH_TP=android): bionic's TLS_SLOT_APP (tpidr_el0 + 16) starts empty on every thread, keeps what the host writes there
// across libc, malloc, stdio, pthread and dlopen'ed library calls, and the guest's way of reading it (mrs tpidr_el0; ldr [+16]; ldr [+8 * key]) finds the value. Also: x18 is the shadow call stack
// (rewritten guest code never touches it), and anonymous memory can be made executable (the loader's mprotect of the code area).
// Build: $NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android33-clang -O1 t_tls_slot.c -o t_tls_slot -ldl -lpthread; adb push, run from /data/local/tmp
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static void **tp(void) { void **t; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(t)); return t; }
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL " __VA_ARGS__); printf("\n"); } } while (0)

static uint64_t guest_read(uint64_t key8) {   // what the rewritten code does
    uint64_t v;
    __asm__ volatile("mrs %0, tpidr_el0\n ldr %0, [%0, #16]\n ldr %0, [%0, %1]" : "=&r"(v) : "r"(key8));
    return v;
}
static void *worker(void *a) {
    CHECK(tp()[2] == NULL, "new thread starts with TLS_SLOT_APP %p", tp()[2]);
    uint64_t *blk = calloc(8, 8); blk[3] = (uint64_t)a; tp()[2] = blk;
    for (int i = 0; i < 1000; i++) { free(malloc(64 + i)); char b[64]; snprintf(b, sizeof b, "%d %f", i, i * 0.5); }
    void *h = dlopen("libvulkan.so", RTLD_NOW); if (h) { dlsym(h, "vkCreateInstance"); dlclose(h); }
    FILE *f = fopen("/proc/self/maps", "r"); if (f) { char ln[256]; while (fgets(ln, sizeof ln, f)) {} fclose(f); }
    usleep(1000); sched_yield();
    CHECK(tp()[2] == blk, "TLS_SLOT_APP changed under libc: %p", tp()[2]);
    CHECK(guest_read(3 * 8) == (uint64_t)a, "guest read %llx", (unsigned long long)guest_read(24));
    return NULL;
}
int main(void) {
    printf("main thread TLS_SLOT_APP %p\n", tp()[2]);
    CHECK(tp()[2] == NULL, "main thread TLS_SLOT_APP in use");
    pthread_t th[16];
    for (int i = 0; i < 16; i++) pthread_create(&th[i], NULL, worker, (void *)(uintptr_t)(0x1000 + i));
    for (int i = 0; i < 16; i++) pthread_join(th[i], NULL);
    uint64_t x18; __asm__ volatile("mov %0, x18" : "=r"(x18));
    printf("x18 (shadow call stack) %#llx\n", (unsigned long long)x18);
    // anonymous code: write "mov w0, #42; ret", seal read+execute, call it
    uint32_t *c = mmap(NULL, 16384, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    c[0] = 0x52800540; c[1] = 0xd65f03c0;
    __builtin___clear_cache((char *)c, (char *)(c + 2));
    int pr = mprotect(c, 16384, PROT_READ | PROT_EXEC);
    CHECK(!pr, "mprotect read+execute failed");
    if (!pr) CHECK(((int (*)(void))c)() == 42, "anonymous code did not run");
    printf("page size %ld\n%s (%d failures)\n", sysconf(_SC_PAGESIZE), fails ? "FAILED" : "OK", fails);
    return fails != 0;
}

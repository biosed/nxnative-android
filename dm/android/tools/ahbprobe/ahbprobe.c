// AHB zero-copy feasibility probe (standalone, run from adb shell): an AHardwareBuffer BLOB mapped MAP_SHARED|MAP_FIXED over an existing anonymous range (the "guest pool"),
// imported into Vulkan as a VkBuffer; checks CPU->GPU and GPU->CPU visibility, mprotect write tracking, and timing.
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>
#include <android/hardware_buffer.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <signal.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#define CK(x) do { VkResult r_ = (x); if (r_) { printf("FAIL %s = %d\n", #x, r_); return 1; } } while (0)
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static sigjmp_buf jb; static volatile int faulted;
static void segv(int s, siginfo_t *i, void *u) { (void)s; (void)i; (void)u; faulted = 1; siglongjmp(jb, 1); }
static int recv_fd(int sock) { char buf[512]; struct iovec io = {buf, sizeof buf}; char c[CMSG_SPACE(sizeof(int) * 8)]; struct msghdr m = {0}; m.msg_iov = &io; m.msg_iovlen = 1; m.msg_control = c; m.msg_controllen = sizeof c;
    if (recvmsg(sock, &m, 0) < 0) return -1; for (struct cmsghdr *h = CMSG_FIRSTHDR(&m); h; h = CMSG_NXTHDR(&m, h)) if (h->cmsg_type == SCM_RIGHTS) { int fd; memcpy(&fd, CMSG_DATA(h), sizeof fd); return fd; } return -1; }
int main(int argc, char **argv) {
    size_t SZ = (argc > 1 ? atol(argv[1]) : 16) << 20;
    // 1. the "guest pool": anonymous memory with content
    uint8_t *pool = mmap(NULL, SZ, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    for (size_t i = 0; i < SZ / 4; i++) ((uint32_t *)pool)[i] = (uint32_t)i * 2654435761u;
    uint8_t *save = malloc(SZ); memcpy(save, pool, SZ);
    // 2. AHB BLOB
    AHardwareBuffer_Desc d = {.width = (uint32_t)SZ, .height = 1, .layers = 1, .format = AHARDWAREBUFFER_FORMAT_BLOB,
        .usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN | AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER};
    AHardwareBuffer *ahb = NULL; double t0 = now();
    if (AHardwareBuffer_allocate(&d, &ahb)) { printf("FAIL AHardwareBuffer_allocate\n"); return 1; }
    printf("ahb allocated %zu MB in %.2f ms\n", SZ >> 20, now() - t0);
    int sv[2]; socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    if (AHardwareBuffer_sendHandleToUnixSocket(ahb, sv[0])) { printf("FAIL send handle\n"); return 1; }
    int fd = recv_fd(sv[1]); printf("dmabuf fd %d\n", fd); if (fd < 0) return 1;
    // 3. map it over the pool range (contents copied first)
    t0 = now();
    void *m = mmap(pool, SZ, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
    if (m != pool) { printf("FAIL mmap MAP_FIXED over the pool: %p (%s)\n", m, strerror(errno)); return 1; }
    memcpy(pool, save, SZ);
    printf("MAP_FIXED over pool OK, copy %.2f ms; content ok %d\n", now() - t0, !memcmp(pool, save, SZ));
    // 4. Vulkan import
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "ahbprobe", 1, NULL, 0, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ii = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app}; VkInstance inst; CK(vkCreateInstance(&ii, NULL, &inst));
    uint32_t n = 1; VkPhysicalDevice pd; vkEnumeratePhysicalDevices(inst, &n, &pd);
    const char *ext[] = {"VK_ANDROID_external_memory_android_hardware_buffer", "VK_KHR_external_memory", "VK_EXT_queue_family_foreign", "VK_KHR_dedicated_allocation", "VK_KHR_sampler_ycbcr_conversion"};
    float pr = 1; VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &pr};
    VkDeviceCreateInfo di = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qi, 0, NULL, 5, ext, NULL}; VkDevice dev; CK(vkCreateDevice(pd, &di, NULL, &dev));
    VkQueue q; vkGetDeviceQueue(dev, 0, 0, &q);
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID gp = (void *)vkGetDeviceProcAddr(dev, "vkGetAndroidHardwareBufferPropertiesANDROID");
    VkAndroidHardwareBufferPropertiesANDROID ap = {VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID}; CK(gp(dev, ahb, &ap));
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    uint32_t mt = 0; while (mt < 32 && !(ap.memoryTypeBits >> mt & 1)) mt++;
    printf("ahb props: size %llu, types %#x -> type %u flags %#x\n", (unsigned long long)ap.allocationSize, ap.memoryTypeBits, mt, mp.memoryTypes[mt].propertyFlags);
    VkExternalMemoryBufferCreateInfo eb = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, NULL, VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID};
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &eb, 0, SZ, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT};
    VkBuffer gb; CK(vkCreateBuffer(dev, &bi, NULL, &gb));
    VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, NULL, VK_NULL_HANDLE, gb};
    VkImportAndroidHardwareBufferInfoANDROID imp = {VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID, &ded, ahb};
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, ap.allocationSize, mt}; VkDeviceMemory gm;
    VkResult r = vkAllocateMemory(dev, &ai, NULL, &gm);
    if (r) { printf("dedicated import failed (%d), trying without\n", r); imp.pNext = NULL; CK(vkAllocateMemory(dev, &ai, NULL, &gm)); }
    CK(vkBindBufferMemory(dev, gb, gm, 0)); printf("imported as VkBuffer OK\n");
    // readback buffer
    VkBufferCreateInfo rb = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, SZ, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT}; VkBuffer hb; CK(vkCreateBuffer(dev, &rb, NULL, &hb));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, hb, &mr); uint32_t ht = 0; while (ht < mp.memoryTypeCount && !((mr.memoryTypeBits >> ht & 1) && (mp.memoryTypes[ht].propertyFlags & 6) == 6)) ht++;
    VkMemoryAllocateInfo hai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, ht}; VkDeviceMemory hm; CK(vkAllocateMemory(dev, &hai, NULL, &hm)); CK(vkBindBufferMemory(dev, hb, hm, 0));
    uint32_t *hp; CK(vkMapMemory(dev, hm, 0, SZ, 0, (void **)&hp));
    VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, 0}; VkCommandPool cp; CK(vkCreateCommandPool(dev, &pi, NULL, &cp));
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, cp, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1}; VkCommandBuffer cb; CK(vkAllocateCommandBuffers(dev, &cai, &cb));
    for (int round = 0; round < 3; round++) {
        // CPU writes through the guest view, GPU copies to readback
        uint32_t tag = 0x5a000000u + round; for (size_t i = 0; i < SZ / 4; i += 1024) ((uint32_t *)pool)[i] = tag + (uint32_t)i;
        VkCommandBufferBeginInfo bb = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; vkResetCommandBuffer(cb, 0); vkBeginCommandBuffer(cb, &bb);
        VkBufferCopy c = {0, 0, SZ}; vkCmdCopyBuffer(cb, gb, hb, 1, &c);
        vkCmdFillBuffer(cb, gb, SZ - 4096, 4096, 0xC0FFEE00u + (uint32_t)round);   // GPU writes the last page
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL); vkEndCommandBuffer(cb);
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb}; t0 = now(); CK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE)); vkQueueWaitIdle(q); double gt = now() - t0;
        size_t bad = 0; for (size_t i = 0; i < SZ / 4 - 1024; i += 1024) bad += hp[i] != tag + (uint32_t)i;
        size_t badg = 0; for (size_t i = (SZ - 4096) / 4; i < SZ / 4; i++) badg += ((volatile uint32_t *)pool)[i] != 0xC0FFEE00u + (uint32_t)round;
        printf("round %d: GPU saw CPU writes: %zu wrong of %zu; CPU saw GPU writes: %zu wrong of 1024 (no invalidate); submit+wait %.2f ms\n", round, bad, SZ / 4 / 1024 - 1, badg, gt);
    }
    // 5. write tracking on the shared mapping
    struct sigaction sa = {0}; sa.sa_sigaction = segv; sa.sa_flags = SA_SIGINFO; sigaction(SIGSEGV, &sa, NULL);
    int mp_ok = mprotect(pool, 4096 * 4, PROT_READ) == 0; faulted = 0;
    if (!sigsetjmp(jb, 1)) { pool[100] = 1; }
    printf("mprotect on the AHB mapping: %s, a write faulted: %d\n", mp_ok ? "ok" : strerror(errno), faulted);
    mprotect(pool, 4096 * 4, PROT_READ | PROT_WRITE);
    // 6. madvise / resident cost
    FILE *f = fopen("/proc/self/status", "r"); char ln[256]; while (f && fgets(ln, sizeof ln, f)) if (!strncmp(ln, "VmRSS", 5) || !strncmp(ln, "RssShmem", 8) || !strncmp(ln, "RssFile", 7)) fputs(ln, stdout); if (f) fclose(f);
    printf("done\n");
    return 0;
}

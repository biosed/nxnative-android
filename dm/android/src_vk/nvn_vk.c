// Vulkan graphics backend for Android: the nvn_mtl.h interface the NVN layer calls (on Apple it is nvn_mtl.m), replaying each submission's command list into a Vulkan command buffer.
// Shaders are the SPIR-V the Metal chain translates (dm/android/tools/spv_android.py: <key>.spv + <key>.vk.txt in DM_VK_DIR), every resource in set 0 with a binding of its own; here the vertex
// stage keeps set 0 and the fragment stage is moved to set 1 (its DescriptorSet decorations rewritten at load), so one pipeline layout holds both.
// Memory: Vulkan cannot use the game's memory pools in place (no VK_EXT_external_memory_host on Mali), so what a draw reads from them -- uniform buffers, vertex and index data, the shader's
// own constants -- is copied into the submission's ring buffer when the draw is recorded. The command list is walked in order on the CPU and its in-order writes (inline uniform updates,
// buffer copies and fills) are applied to guest memory where they stand, so each draw copies what the GPU would have read at that point. Textures are Vulkan images: packaged ones
// (block-linear in the pool) are deswizzled and uploaded at first use; render targets, copies and clears stay on the GPU. Every image lives in VK_IMAGE_LAYOUT_GENERAL, and passes and copies
// are separated by full memory barriers: simple and correct first, faster later.
// Event values, counter reports and texture-to-buffer copies reach guest memory when the GPU has finished the submission (the completion thread), as on the Metal backend.
// With a window (the app, app/front_android.c) presented frames are scaled onto a swapchain, keeping the game's aspect unless DM_SCREEN_FIT=stretch; DM_SHOW_FPS=1 draws a frame-rate
// counter, DM_FPS=30 holds every frame two vsyncs. Without one (the adb front end) nothing is shown: DM_VK_SHOTS=<frame>,<frame>,... writes those frames to DM_VK_SHOT_DIR as BMP files.
#include "hle.h"
#include "nvn_obj.h"
#include "nvn_mtl.h"
#include "../tex/nvn_deswizzle.h"
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>
#include <android/native_window.h>
#include <stdatomic.h>
#include <sys/stat.h>

#define VKCHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) die("Vulkan: %s failed (%d)", #x, (int)r_); } while (0)
#define RED_ONCE(id, ...) do { static int once_; if (!once_++) LOG("[RED] vk: " __VA_ARGS__); } while (0)

static VkInstance g_inst; static VkPhysicalDevice g_pd; static VkDevice g_dev; static VkQueue g_q; static uint32_t g_qf;
static VkPhysicalDeviceMemoryProperties g_memp; static VkPhysicalDeviceProperties g_props;
static pthread_mutex_t g_qmu = PTHREAD_MUTEX_INITIALIZER;   // the queue (submissions, the present readback)
static pthread_mutex_t g_rmu = PTHREAD_MUTEX_INITIALIZER;   // texture / sampler registrations and the image table (game threads register while the submit thread reads)

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < g_memp.memoryTypeCount; i++) if ((bits >> i & 1) && (g_memp.memoryTypes[i].propertyFlags & want) == want) return i;
    for (uint32_t i = 0; i < g_memp.memoryTypeCount; i++) if (bits >> i & 1) return i;
    die("Vulkan: no memory type for %#x", bits);
}
static VkDeviceMemory mem_alloc(VkMemoryRequirements r, VkMemoryPropertyFlags want) {
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, r.size, mem_type(r.memoryTypeBits, want)};
    VkDeviceMemory m; VKCHECK(vkAllocateMemory(g_dev, &ai, NULL, &m)); return m;
}
static VkBuffer buf_new(VkDeviceSize n, VkBufferUsageFlags u, VkDeviceMemory *m, void **map) {
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, n, u, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkBuffer b; VKCHECK(vkCreateBuffer(g_dev, &bi, NULL, &b));
    VkMemoryRequirements r; vkGetBufferMemoryRequirements(g_dev, b, &r);
    *m = mem_alloc(r, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VKCHECK(vkBindBufferMemory(g_dev, b, *m, 0));
    if (map) VKCHECK(vkMapMemory(g_dev, *m, 0, VK_WHOLE_SIZE, 0, map));
    return b;
}
#define ALL_BUF_USAGE (VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | \
                       VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT)

// ---------- Image memory: 64 MB blocks handed out in order; a block is freed when its last image is (thousands of textures would exceed the allocation count) ----------
typedef struct { VkDeviceMemory m; VkDeviceSize used, size; int live; uint32_t type; } mblock;
static mblock g_blk[256]; static int g_nblk; static pthread_mutex_t g_blkmu = PTHREAD_MUTEX_INITIALIZER;   // images are made on the submit thread and freed on the completion thread
static int img_mem_(VkMemoryRequirements r, VkDeviceMemory *m, VkDeviceSize *off);
static int img_mem(VkMemoryRequirements r, VkDeviceMemory *m, VkDeviceSize *off) { pthread_mutex_lock(&g_blkmu); int b = img_mem_(r, m, off); pthread_mutex_unlock(&g_blkmu); return b; }
static int img_mem_(VkMemoryRequirements r, VkDeviceMemory *m, VkDeviceSize *off) {
    uint32_t type = mem_type(r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (r.size > (32u << 20)) { *m = mem_alloc(r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT); *off = 0; return -1; }   // big images get memory of their own
    for (int i = g_nblk - 1; i >= 0 && i >= g_nblk - 2; i--) {
        mblock *b = &g_blk[i]; VkDeviceSize o = (b->used + r.alignment - 1) / r.alignment * r.alignment;
        if (b->m && b->type == type && o + r.size <= b->size) { b->used = o + r.size; b->live++; *m = b->m; *off = o; return i; }
    }
    int i = 0; while (i < g_nblk && g_blk[i].m) i++;
    if (i == 256) die("Vulkan: image memory blocks exhausted");
    if (i == g_nblk) g_nblk++;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, 64u << 20, type};
    VKCHECK(vkAllocateMemory(g_dev, &ai, NULL, &g_blk[i].m));
    g_blk[i].size = 64u << 20; g_blk[i].used = r.size; g_blk[i].live = 1; g_blk[i].type = type; *m = g_blk[i].m; *off = 0; return i;
}
static void img_mem_free(int blk, VkDeviceMemory m) {
    if (blk < 0) { vkFreeMemory(g_dev, m, NULL); return; }
    pthread_mutex_lock(&g_blkmu);
    if (!--g_blk[blk].live) { vkFreeMemory(g_dev, g_blk[blk].m, NULL); g_blk[blk].m = VK_NULL_HANDLE; }
    pthread_mutex_unlock(&g_blkmu);
}

// ---------- Submission contexts: command buffers, ring buffer, descriptor pool, the writes that wait for completion ----------
#define NCTX 3
#define RING (64u << 20)
typedef struct { int kind; void *dst; uint32_t mode, v; uint64_t addr; uint32_t type; const uint8_t *src; size_t n; } post_t;   // kind 1 event, 2 counter, 3 copy ring → guest
typedef struct vtex vtex;
typedef struct {
    VkCommandPool pool; VkCommandBuffer cb, up; VkFence fence; VkDescriptorPool dp;
    VkBuffer ring; VkDeviceMemory ringm; uint8_t *ringp; VkDeviceSize used;
    post_t *post; int npost, cpost;
    uint64_t fence_val; int up_used;
    VkQueryPool qp;   // GPU timestamps at the start and the end of the submission
    VkFramebuffer *fb; int nfb, cfb;   // render passes' framebuffers (no dynamic rendering), destroyed when the submission is done
    VkBuffer ring0; VkDeviceMemory ringm0; uint8_t *ringp0;   // the context's own ring; ring / ringp move to extra rings when a submission outgrows it (loading screens upload hundreds of MB)
    struct { VkBuffer b; VkDeviceMemory m; } *extra; int nextra;
} ctx_t;
static _Atomic uint64_t g_gpu_ns;
static ctx_t g_ctx[NCTX]; static ctx_t *C; static int g_ci;
static uint64_t g_fence, g_done;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static int g_pend[NCTX], g_npend;   // contexts submitted and not yet completed, in order (under g_mu)

static uint64_t g_wgen;   // bumped by every in-order write to guest memory (and when the ring changes): the per-submission copies are reused only within one generation
static VkDeviceSize ring_alloc(VkDeviceSize n, VkDeviceSize align) {
    VkDeviceSize o = (C->used + align - 1) / align * align;
    if (o + n > RING) {   // full: an extra ring for the rest of this submission (what was recorded keeps its buffer), freed when the submission is done
        if (n > RING) { RED_ONCE(1, "one copy of %llu MB is larger than a ring buffer: dropped", (unsigned long long)(n >> 20)); return VK_WHOLE_SIZE; }
        C->extra = realloc(C->extra, (C->nextra + 1) * sizeof *C->extra);
        C->extra[C->nextra].b = buf_new(RING, ALL_BUF_USAGE, &C->extra[C->nextra].m, (void **)&C->ringp); C->ring = C->extra[C->nextra++].b;
        C->used = 0; o = 0; g_wgen++;   // copies cached in the old ring are not reused from the new one
    }
    C->used = o + n; return o;
}
static VkDeviceSize ring_put(const void *src, VkDeviceSize n, VkDeviceSize align) {
    VkDeviceSize o = ring_alloc(n, align);
    if (o != VK_WHOLE_SIZE) memcpy(C->ringp + o, src, n);
    return o;
}
static void post_add(post_t p) { if (C->npost == C->cpost) { C->cpost = C->cpost ? 2 * C->cpost : 256; C->post = realloc(C->post, C->cpost * sizeof *C->post); } C->post[C->npost++] = p; }
static uint64_t gpu_ticks(void) { uint64_t g = guest_ns(); if (!g) g = now_ns(); return g * 12 / 625; }   // the GPU's 19.2 MHz timer, as nvn_mtl.m reports it
static void ctx_complete(ctx_t *c) {
    uint64_t ts[2];
    if (vkGetQueryPoolResults(g_dev, c->qp, 0, 2, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && ts[1] > ts[0])
        atomic_fetch_add(&g_gpu_ns, (uint64_t)((double)(ts[1] - ts[0]) * g_props.limits.timestampPeriod));
    for (int i = 0; i < c->npost; i++) {
        post_t *p = &c->post[i];
        if (p->kind == 1) { volatile uint32_t *v = p->dst; if (p->mode == 1) *v += p->v; else *v = p->v; }
        else if (p->kind == 2) { uint64_t t = gpu_ticks(), rec[2] = {p->type == 0 || p->type == 0x10 ? t : 0, t}; memcpy((void *)p->addr, rec, sizeof rec); }
        else if (p->kind == 3) memcpy(p->dst, p->src, p->n);
    }
    c->npost = 0;
    for (int i = 0; i < c->nextra; i++) { vkDestroyBuffer(g_dev, c->extra[i].b, NULL); vkFreeMemory(g_dev, c->extra[i].m, NULL); }
    c->nextra = 0;
    for (int i = 0; i < c->nfb; i++) vkDestroyFramebuffer(g_dev, c->fb[i], NULL);
    c->nfb = 0;
}
static void garbage_free(uint64_t done);
static void *completion_thread(void *a) {
    (void)a;
    for (;;) {
        pthread_mutex_lock(&g_mu);
        while (!g_npend) pthread_cond_wait(&g_cv, &g_mu);
        ctx_t *c = &g_ctx[g_pend[0]];
        pthread_mutex_unlock(&g_mu);
        VkResult r = vkWaitForFences(g_dev, 1, &c->fence, VK_TRUE, UINT64_MAX);
        if (r != VK_SUCCESS) die("Vulkan: waiting for a submission failed (%d)", (int)r);
        ctx_complete(c);
        pthread_mutex_lock(&g_mu);
        g_done = c->fence_val; memmove(g_pend, g_pend + 1, --g_npend * sizeof *g_pend);
        pthread_cond_broadcast(&g_cv); pthread_mutex_unlock(&g_mu);
        garbage_free(c->fence_val);
    }
    return NULL;
}

// ---------- Device ----------
static int g_has_u8;
static VkSampler g_dummy_samp; static VkImage g_dummy_img; static VkImageView g_dummy_view; static VkBuffer g_zero_buf, g_support_buf; static VkDeviceMemory g_zero_mem, g_support_mem;
static void dummies(void);
static int g_has_surface;
// Render passes: dynamic rendering where the GPU has it (Vulkan 1.3, or 1.2 / 1.1 with VK_KHR_dynamic_rendering), else classic render passes and framebuffers made per pass
static int g_dynrend; static PFN_vkCmdBeginRenderingKHR p_begin_rendering; static PFN_vkCmdEndRenderingKHR p_end_rendering;
static void vk_init_(void) {
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "nxnative", 1, "dmhost", 1, VK_API_VERSION_1_3};
    const char *iext[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    uint32_t nie = 0; vkEnumerateInstanceExtensionProperties(NULL, &nie, NULL);
    VkExtensionProperties *iep = calloc(nie ? nie : 1, sizeof *iep); vkEnumerateInstanceExtensionProperties(NULL, &nie, iep);
    int nsurf = 0; for (uint32_t i = 0; i < nie; i++) for (int k = 0; k < 2; k++) nsurf += !strcmp(iep[i].extensionName, iext[k]);
    free(iep); g_has_surface = nsurf == 2;
    VkInstanceCreateInfo ii = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, g_has_surface ? 2 : 0, iext};
    VKCHECK(vkCreateInstance(&ii, NULL, &g_inst));
    uint32_t n = 1; VkResult r = vkEnumeratePhysicalDevices(g_inst, &n, &g_pd);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || !n) die("Vulkan: no GPU");
    vkGetPhysicalDeviceProperties(g_pd, &g_props); vkGetPhysicalDeviceMemoryProperties(g_pd, &g_memp);
    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(g_pd, &nq, NULL);
    VkQueueFamilyProperties qp[16]; if (nq > 16) nq = 16; vkGetPhysicalDeviceQueueFamilyProperties(g_pd, &nq, qp);
    for (g_qf = 0; g_qf < nq && !(qp[g_qf].queueFlags & VK_QUEUE_GRAPHICS_BIT); g_qf++) {}
    if (g_qf == nq) die("Vulkan: no graphics queue");
    // Every feature the GPU has is enabled (the translated shaders may use any of them), except the costly robustness beyond robustBufferAccess. The feature structures
    // chained are only those of the GPU's Vulkan version and extensions (1.1 phones have neither the 1.2 / 1.3 structures nor dynamic rendering)
    uint32_t dv = g_props.apiVersion;
    uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(g_pd, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne ? ne : 1, sizeof *ep); vkEnumerateDeviceExtensionProperties(g_pd, NULL, &ne, ep);
    int has_sc = 0, has_u8 = 0, has_dr = 0;
    for (uint32_t i = 0; i < ne; i++) {
        has_sc |= !strcmp(ep[i].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME); has_u8 |= !strcmp(ep[i].extensionName, VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME);
        has_dr |= !strcmp(ep[i].extensionName, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
    }
    free(ep);
    VkPhysicalDeviceIndexTypeUint8FeaturesEXT u8 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT};
    VkPhysicalDeviceDynamicRenderingFeaturesKHR dr = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR};
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan11Features f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceShaderDrawParametersFeatures sdp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES};
    void *chain = NULL;
    if (has_u8) { u8.pNext = chain; chain = &u8; }
    if (dv >= VK_API_VERSION_1_3) { f13.pNext = chain; chain = &f13; } else if (has_dr) { dr.pNext = chain; chain = &dr; }
    if (dv >= VK_API_VERSION_1_2) { f12.pNext = chain; chain = &f12; f11.pNext = chain; chain = &f11; } else { sdp.pNext = chain; chain = &sdp; }
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, chain};
    vkGetPhysicalDeviceFeatures2(g_pd, &f2);
    f13.robustImageAccess = VK_FALSE;
    const char *ext[4]; uint32_t next = 0;
    if (has_sc && g_has_surface) ext[next++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    if (has_u8 && u8.indexTypeUint8) { ext[next++] = VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME; g_has_u8 = 1; }
    g_dynrend = dv >= VK_API_VERSION_1_3 ? f13.dynamicRendering : has_dr && dr.dynamicRendering;
    if (getenv("DM_VK_NO_DYNREND")) g_dynrend = 0;   // diagnostics: the render pass path of GPUs without dynamic rendering, on one that has it
    if (g_dynrend && dv < VK_API_VERSION_1_3) ext[next++] = VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME;
    float prio = 1;
    VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, g_qf, 1, &prio};
    VkDeviceCreateInfo di = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2, 0, 1, &qi, 0, NULL, next, ext, NULL};
    VKCHECK(vkCreateDevice(g_pd, &di, NULL, &g_dev));
    vkGetDeviceQueue(g_dev, g_qf, 0, &g_q);
    if (g_dynrend) {
        p_begin_rendering = (PFN_vkCmdBeginRenderingKHR)vkGetDeviceProcAddr(g_dev, dv >= VK_API_VERSION_1_3 ? "vkCmdBeginRendering" : "vkCmdBeginRenderingKHR");
        p_end_rendering = (PFN_vkCmdEndRenderingKHR)vkGetDeviceProcAddr(g_dev, dv >= VK_API_VERSION_1_3 ? "vkCmdEndRendering" : "vkCmdEndRenderingKHR");
        if (!p_begin_rendering || !p_end_rendering) g_dynrend = 0;
    }
    for (int i = 0; i < NCTX; i++) {
        ctx_t *c = &g_ctx[i];
        VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, g_qf};
        VKCHECK(vkCreateCommandPool(g_dev, &pi, NULL, &c->pool));
        VkCommandBuffer cbs[2]; VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, c->pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 2};
        VKCHECK(vkAllocateCommandBuffers(g_dev, &ai, cbs)); c->cb = cbs[0]; c->up = cbs[1];
        VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, VK_FENCE_CREATE_SIGNALED_BIT};
        VKCHECK(vkCreateFence(g_dev, &fi, NULL, &c->fence));
        c->ring = c->ring0 = buf_new(RING, ALL_BUF_USAGE, &c->ringm, (void **)&c->ringp); c->ringm0 = c->ringm; c->ringp0 = c->ringp;
        VkDescriptorPoolSize ps[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 65536}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 65536}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8192},
                                     {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4096}, {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 4096}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4096}, {VK_DESCRIPTOR_TYPE_SAMPLER, 4096}};
        VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 32768, sizeof ps / sizeof *ps, ps};
        VKCHECK(vkCreateDescriptorPool(g_dev, &dpi, NULL, &c->dp));
        VkQueryPoolCreateInfo qi2 = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_TIMESTAMP, 2};
        VKCHECK(vkCreateQueryPool(g_dev, &qi2, NULL, &c->qp));
    }
    dummies();
    pthread_t th; if (pthread_create(&th, NULL, completion_thread, NULL)) die("Vulkan: cannot start the completion thread"); pthread_detach(th);
    LOG("vk: %s, Vulkan %u.%u.%u, driver %#x; uint8 indices %s, %s", g_props.deviceName, VK_API_VERSION_MAJOR(g_props.apiVersion), VK_API_VERSION_MINOR(g_props.apiVersion),
        VK_API_VERSION_PATCH(g_props.apiVersion), g_props.driverVersion, g_has_u8 ? "yes" : "no", g_dynrend ? "dynamic rendering" : "render passes (no dynamic rendering)");
}
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static void vk_init(void) { pthread_once(&g_once, vk_init_); }

// One-time commands outside the submissions (dummy contents, the present readback): their own pool, the queue lock held, waited for
static VkCommandPool g_opool;
static VkCommandBuffer once_begin(void) {
    pthread_mutex_lock(&g_qmu);
    if (!g_opool) { VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, g_qf}; VKCHECK(vkCreateCommandPool(g_dev, &pi, NULL, &g_opool)); }
    VkCommandBuffer cb; VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, g_opool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VKCHECK(vkAllocateCommandBuffers(g_dev, &ai, &cb));
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    VKCHECK(vkBeginCommandBuffer(cb, &bi)); return cb;
}
static void once_end(VkCommandBuffer cb) {
    VKCHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
    VKCHECK(vkQueueSubmit(g_q, 1, &si, VK_NULL_HANDLE)); VKCHECK(vkQueueWaitIdle(g_q));
    vkFreeCommandBuffers(g_dev, g_opool, 1, &cb);
    pthread_mutex_unlock(&g_qmu);
}
static void barrier_all(VkCommandBuffer cb) {
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}
static void to_general(VkCommandBuffer cb, VkImage img, VkImageAspectFlags asp) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                              VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img, {asp, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}
static void dummies(void) {
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; si.magFilter = si.minFilter = VK_FILTER_LINEAR; si.maxLod = 0;
    VKCHECK(vkCreateSampler(g_dev, &si, NULL, &g_dummy_samp));
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {1, 1, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VKCHECK(vkCreateImage(g_dev, &ii, NULL, &g_dummy_img));
    VkMemoryRequirements r; vkGetImageMemoryRequirements(g_dev, g_dummy_img, &r); VkDeviceMemory m = mem_alloc(r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VKCHECK(vkBindImageMemory(g_dev, g_dummy_img, m, 0));
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, g_dummy_img, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VKCHECK(vkCreateImageView(g_dev, &vi, NULL, &g_dummy_view));
    void *zp, *sp;
    g_zero_buf = buf_new(65536, ALL_BUF_USAGE, &g_zero_mem, &zp); memset(zp, 0, 65536);
    // Ryujinx's support buffer: alpha test 0, is_bgra[8] at 16, viewport inverse 144, viewport size 160, render scale count 176, render scale float4[73] at 192 (all 1.0)
    g_support_buf = buf_new(4096, ALL_BUF_USAGE, &g_support_mem, &sp); memset(sp, 0, 4096);
    for (int i = 0; i < 73; i++) *(float *)((uint8_t *)sp + 192 + 16 * i) = 1.0f;
    VkCommandBuffer cb = once_begin(); to_general(cb, g_dummy_img, VK_IMAGE_ASPECT_COLOR_BIT);
    VkClearColorValue black = {{0, 0, 0, 1}}; VkImageSubresourceRange rr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, g_dummy_img, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &rr); once_end(cb);
}

// ---------- Formats ----------
enum { DEC_NONE, DEC_BC1, DEC_BC2, DEC_BC3 };   // compressed formats Mali cannot sample, decoded to RGBA8 on upload
typedef struct { VkFormat f; uint8_t noalpha, dec, depth, stencil; } fmt_t;
static fmt_t fmt_of(int nvn) {
    fmt_t r = {VK_FORMAT_UNDEFINED, 0, 0, 0, 0};
    if (nvn >= 0x79 && nvn <= 0x94) {   // ASTC: 4x4 5x4 5x5 6x5 6x6 8x5 8x6 8x8 10x5 10x6 10x8 10x10 12x10 12x12, LDR then sRGB; Vulkan lists UNORM / SRGB in pairs
        int k = (nvn - 0x79) % 14, srgb = nvn >= 0x87; r.f = (VkFormat)(VK_FORMAT_ASTC_4x4_UNORM_BLOCK + 2 * k + srgb); return r;
    }
    switch (nvn) {
        case 0x01: r.f = VK_FORMAT_R8_UNORM; break; case 0x02: r.f = VK_FORMAT_R8_SNORM; break; case 0x03: r.f = VK_FORMAT_R8_UINT; break; case 0x04: r.f = VK_FORMAT_R8_SINT; break;
        case 0x05: r.f = VK_FORMAT_R16_SFLOAT; break; case 0x06: r.f = VK_FORMAT_R16_UNORM; break; case 0x07: r.f = VK_FORMAT_R16_SNORM; break;
        case 0x08: r.f = VK_FORMAT_R16_UINT; break; case 0x09: r.f = VK_FORMAT_R16_SINT; break;
        case 0x0A: r.f = VK_FORMAT_R32_SFLOAT; break; case 0x0B: r.f = VK_FORMAT_R32_UINT; break; case 0x0C: r.f = VK_FORMAT_R32_SINT; break;
        case 0x0D: r.f = VK_FORMAT_R8G8_UNORM; break; case 0x0E: r.f = VK_FORMAT_R8G8_SNORM; break; case 0x0F: r.f = VK_FORMAT_R8G8_UINT; break; case 0x10: r.f = VK_FORMAT_R8G8_SINT; break;
        case 0x11: r.f = VK_FORMAT_R16G16_SFLOAT; break; case 0x12: r.f = VK_FORMAT_R16G16_UNORM; break; case 0x13: r.f = VK_FORMAT_R16G16_SNORM; break;
        case 0x14: r.f = VK_FORMAT_R16G16_UINT; break; case 0x15: r.f = VK_FORMAT_R16G16_SINT; break;
        case 0x16: r.f = VK_FORMAT_R32G32_SFLOAT; break; case 0x17: r.f = VK_FORMAT_R32G32_UINT; break; case 0x18: r.f = VK_FORMAT_R32G32_SINT; break;
        case 0x6D: r.noalpha = 1; /* fall through */ case 0x25: r.f = VK_FORMAT_R8G8B8A8_UNORM; break;
        case 0x6E: r.noalpha = 1; /* fall through */ case 0x26: r.f = VK_FORMAT_R8G8B8A8_SNORM; break;
        case 0x6F: r.noalpha = 1; /* fall through */ case 0x27: r.f = VK_FORMAT_R8G8B8A8_UINT; break;
        case 0x70: r.noalpha = 1; /* fall through */ case 0x28: r.f = VK_FORMAT_R8G8B8A8_SINT; break;
        case 0x71: r.noalpha = 1; /* fall through */ case 0x29: r.f = VK_FORMAT_R16G16B16A16_SFLOAT; break;
        case 0x72: r.noalpha = 1; /* fall through */ case 0x2A: r.f = VK_FORMAT_R16G16B16A16_UNORM; break;
        case 0x73: r.noalpha = 1; /* fall through */ case 0x2B: r.f = VK_FORMAT_R16G16B16A16_SNORM; break;
        case 0x74: r.noalpha = 1; /* fall through */ case 0x2C: r.f = VK_FORMAT_R16G16B16A16_UINT; break;
        case 0x75: r.noalpha = 1; /* fall through */ case 0x2D: r.f = VK_FORMAT_R16G16B16A16_SINT; break;
        case 0x76: r.noalpha = 1; /* fall through */ case 0x2E: r.f = VK_FORMAT_R32G32B32A32_SFLOAT; break;
        case 0x77: r.noalpha = 1; /* fall through */ case 0x2F: r.f = VK_FORMAT_R32G32B32A32_UINT; break;
        case 0x78: r.noalpha = 1; /* fall through */ case 0x30: r.f = VK_FORMAT_R32G32B32A32_SINT; break;
        case 0x31: r.f = VK_FORMAT_D24_UNORM_S8_UINT; r.depth = 1; r.stencil = 1; break;   // STENCIL8 has no sampled / attachment support on Mali: a D24S8 image's stencil
        case 0x32: r.f = VK_FORMAT_D16_UNORM; r.depth = 1; break;
        case 0x33: case 0x35: r.f = VK_FORMAT_D24_UNORM_S8_UINT; r.depth = 1; r.stencil = 1; break;
        case 0x34: r.f = VK_FORMAT_D32_SFLOAT; r.depth = 1; break;
        case 0x36: r.f = VK_FORMAT_D32_SFLOAT_S8_UINT; r.depth = 1; r.stencil = 1; break;
        case 0x37: r.noalpha = 1; /* fall through */ case 0x38: r.f = VK_FORMAT_R8G8B8A8_SRGB; break;
        case 0x39: r.f = VK_FORMAT_A4B4G4R4_UNORM_PACK16; break;
        case 0x3A: r.noalpha = 1; /* fall through */ case 0x3B: r.f = VK_FORMAT_A1R5G5B5_UNORM_PACK16; break;
        case 0x3C: r.f = VK_FORMAT_R5G6B5_UNORM_PACK16; break;
        case 0x3D: r.f = VK_FORMAT_A2B10G10R10_UNORM_PACK32; break; case 0x3E: r.f = VK_FORMAT_A2B10G10R10_UINT_PACK32; break;
        case 0x3F: r.f = VK_FORMAT_B10G11R11_UFLOAT_PACK32; break; case 0x40: r.f = VK_FORMAT_E5B9G9R9_UFLOAT_PACK32; break;
        case 0x41: r.noalpha = 1; /* fall through */ case 0x42: r.f = VK_FORMAT_R8G8B8A8_UNORM; r.dec = DEC_BC1; break;
        case 0x43: r.f = VK_FORMAT_R8G8B8A8_UNORM; r.dec = DEC_BC2; break;
        case 0x44: r.f = VK_FORMAT_R8G8B8A8_UNORM; r.dec = DEC_BC3; break;
        case 0x45: r.noalpha = 1; /* fall through */ case 0x46: r.f = VK_FORMAT_R8G8B8A8_SRGB; r.dec = DEC_BC1; break;
        case 0x47: r.f = VK_FORMAT_R8G8B8A8_SRGB; r.dec = DEC_BC2; break;
        case 0x48: r.f = VK_FORMAT_R8G8B8A8_SRGB; r.dec = DEC_BC3; break;
        case 0x99: r.noalpha = 1; /* fall through */ case 0x9A: r.f = VK_FORMAT_B8G8R8A8_UNORM; break;
        case 0x9B: r.noalpha = 1; /* fall through */ case 0x9C: r.f = VK_FORMAT_B8G8R8A8_SRGB; break;
        case 0x95: r.f = VK_FORMAT_B5G6R5_UNORM_PACK16; break;
        case 0x96: r.noalpha = 1; /* fall through */ case 0x97: r.f = VK_FORMAT_B5G5R5A1_UNORM_PACK16; break;
        case 0x98: r.f = VK_FORMAT_A1R5G5B5_UNORM_PACK16; break;
    }
    return r;
}
// Vertex attribute formats (NVN format → Vulkan, *_UI2F / *_I2F → SCALED), 0 = none
static VkFormat vfmt(int f) {
    static const VkFormat T[0x6D] = {
        [0x01] = VK_FORMAT_R8_UNORM, VK_FORMAT_R8_SNORM, VK_FORMAT_R8_UINT, VK_FORMAT_R8_SINT, VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16_UNORM, VK_FORMAT_R16_SNORM, VK_FORMAT_R16_UINT, VK_FORMAT_R16_SINT,
        VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32_UINT, VK_FORMAT_R32_SINT, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8_SINT,
        VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16_SINT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32_SINT,
        VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8_SNORM, VK_FORMAT_R8G8B8_UINT, VK_FORMAT_R8G8B8_SINT, VK_FORMAT_R16G16B16_SFLOAT, VK_FORMAT_R16G16B16_UNORM, VK_FORMAT_R16G16B16_SNORM,
        VK_FORMAT_R16G16B16_UINT, VK_FORMAT_R16G16B16_SINT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32_SINT,
        VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_SINT, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_UNORM,
        VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_R16G16B16A16_SINT, VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_SINT,
        [0x3D] = VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2B10G10R10_UINT_PACK32, VK_FORMAT_B10G11R11_UFLOAT_PACK32,
        [0x51] = VK_FORMAT_R8_USCALED, VK_FORMAT_R8_SSCALED, VK_FORMAT_R16_USCALED, VK_FORMAT_R16_SSCALED, VK_FORMAT_R32_UINT, VK_FORMAT_R32_SINT,
        VK_FORMAT_R8G8_USCALED, VK_FORMAT_R8G8_SSCALED, VK_FORMAT_R16G16_USCALED, VK_FORMAT_R16G16_SSCALED, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32_SINT,
        VK_FORMAT_R8G8B8_USCALED, VK_FORMAT_R8G8B8_SSCALED, VK_FORMAT_R16G16B16_USCALED, VK_FORMAT_R16G16B16_SSCALED, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32_SINT,
        VK_FORMAT_R8G8B8A8_USCALED, VK_FORMAT_R8G8B8A8_SSCALED, VK_FORMAT_R16G16B16A16_USCALED, VK_FORMAT_R16G16B16A16_SSCALED, VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_SINT,
        [0x69] = VK_FORMAT_A2B10G10R10_SNORM_PACK32, VK_FORMAT_A2B10G10R10_SINT_PACK32, VK_FORMAT_A2B10G10R10_USCALED_PACK32, VK_FORMAT_A2B10G10R10_SSCALED_PACK32,
    };
    return f > 0 && f < 0x6D ? T[f] : VK_FORMAT_UNDEFINED;
}

// BC1 / BC2 / BC3 → RGBA8 (Mali has no BC sampling)
static void bc_color(const uint8_t *b, uint8_t out[16][4], int alpha1) {
    uint16_t c0 = (uint16_t)(b[0] | b[1] << 8), c1 = (uint16_t)(b[2] | b[3] << 8); uint8_t p[4][4];
    for (int i = 0; i < 2; i++) { uint16_t c = i ? c1 : c0; p[i][0] = (uint8_t)((c >> 11) * 255 / 31); p[i][1] = (uint8_t)((c >> 5 & 63) * 255 / 63); p[i][2] = (uint8_t)((c & 31) * 255 / 31); p[i][3] = 255; }
    if (c0 > c1 || !alpha1) for (int k = 0; k < 3; k++) { p[2][k] = (uint8_t)((2 * p[0][k] + p[1][k]) / 3); p[3][k] = (uint8_t)((p[0][k] + 2 * p[1][k]) / 3); }
    else for (int k = 0; k < 3; k++) { p[2][k] = (uint8_t)((p[0][k] + p[1][k]) / 2); p[3][k] = 0; }
    p[2][3] = 255; p[3][3] = c0 > c1 || !alpha1 ? 255 : 0;
    uint32_t idx = (uint32_t)b[4] | (uint32_t)b[5] << 8 | (uint32_t)b[6] << 16 | (uint32_t)b[7] << 24;
    for (int i = 0; i < 16; i++) memcpy(out[i], p[idx >> (2 * i) & 3], 4);
}
static void bc_decode(int dec, const uint8_t *src, size_t src_pitch, uint32_t w, uint32_t h, uint8_t *dst) {   // dst: w * h * 4
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, bs = dec == DEC_BC1 ? 8 : 16;
    for (uint32_t by = 0; by < bh; by++) for (uint32_t bx = 0; bx < bw; bx++) {
        const uint8_t *b = src + by * src_pitch + bx * bs; uint8_t px[16][4];
        if (dec == DEC_BC1) bc_color(b, px, 1);
        else {
            bc_color(b + 8, px, 0);
            if (dec == DEC_BC2) for (int i = 0; i < 16; i++) { int a = b[i / 2] >> (4 * (i & 1)) & 15; px[i][3] = (uint8_t)(a * 17); }
            else {
                uint8_t a[8] = {b[0], b[1]};
                if (a[0] > a[1]) for (int i = 2; i < 8; i++) a[i] = (uint8_t)(((8 - i) * a[0] + (i - 1) * a[1]) / 7);
                else { for (int i = 2; i < 6; i++) a[i] = (uint8_t)(((6 - i) * a[0] + (i - 1) * a[1]) / 5); a[6] = 0; a[7] = 255; }
                uint64_t bits = 0; for (int i = 0; i < 6; i++) bits |= (uint64_t)b[2 + i] << (8 * i);
                for (int i = 0; i < 16; i++) px[i][3] = a[bits >> (3 * i) & 7];
            }
        }
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
            uint32_t X = bx * 4 + x, Y = by * 4 + y;
            if (X < w && Y < h) memcpy(dst + ((size_t)Y * w + X) * 4, px[y * 4 + x], 4);
        }
    }
}

// ---------- Images ----------
typedef struct { uint64_t key; VkImageView v; } vview;
struct vtex {
    uint64_t serial; VkImage img; VkDeviceMemory m; int blk; fmt_t f; int nvn_fmt, target;
    uint32_t w, h, d, levels, layers; VkImageAspectFlags aspect; int uploaded, gpu;
    vview *views; int nviews;
    struct vtex *next;
    uint64_t dead;   // released by the game: freed once the GPU has finished the submission that was being recorded then (fence dead)
};
static vtex **g_gb; static int g_ngb, g_cgb; static pthread_mutex_t g_gbmu = PTHREAD_MUTEX_INITIALIZER;
#define NHASH 4096
static vtex *g_tex[NHASH];
static vtex *tex_find(uint64_t ser) { for (vtex *v = g_tex[ser % NHASH]; v; v = v->next) if (v->serial == ser) return v; return NULL; }
static uint64_t *g_cpu_written; static int g_ncpu, g_ccpu;   // serials WriteTexels wrote (mtl_tex_cpu_written)
static int cpu_written(uint64_t ser) { for (int i = 0; i < g_ncpu; i++) if (g_cpu_written[i] == ser) return 1; return 0; }

static void tex_upload(vtex *v, const tex_t *t);
static vtex *tex_get(const tex_t *t, int for_rt) {
    pthread_mutex_lock(&g_rmu); vtex *v = tex_find(t->serial); pthread_mutex_unlock(&g_rmu);
    if (v) { if (for_rt) v->gpu = 1; return v; }
    fmt_t f = fmt_of(t->d.format);
    if (!f.f) { static int n; if (n++ < 8) LOG("[RED] vk: texture format %#x has no Vulkan format yet (serial %llu)", t->d.format, (unsigned long long)t->serial); return NULL; }
    if (t->d.target == NVN_TEXTURE_TARGET_BUFFER) { RED_ONCE(3, "buffer textures are not implemented yet"); return NULL; }
    v = calloc(1, sizeof *v); v->serial = t->serial; v->f = f; v->nvn_fmt = t->d.format; v->target = t->d.target;
    v->w = t->d.width ? t->d.width : 1; v->h = t->d.height ? t->d.height : 1; v->levels = t->d.levels ? t->d.levels : 1;
    int is3d = t->d.target == NVN_TEXTURE_TARGET_3D, cube = t->d.target == NVN_TEXTURE_TARGET_CUBEMAP || t->d.target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY;
    v->d = is3d ? (t->d.depth ? t->d.depth : 1) : 1;
    v->layers = is3d ? 1 : t->L.layers ? t->L.layers : t->d.target == NVN_TEXTURE_TARGET_2D_ARRAY || t->d.target == NVN_TEXTURE_TARGET_1D_ARRAY ? (t->d.depth ? t->d.depth : 1) : cube ? 6 : 1;
    if (t->d.target == NVN_TEXTURE_TARGET_1D_ARRAY && !t->L.layers) v->layers = t->d.height ? t->d.height : 1;
    v->aspect = f.depth ? VK_IMAGE_ASPECT_DEPTH_BIT | (f.stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0) : VK_IMAGE_ASPECT_COLOR_BIT;
    VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(g_pd, f.f, &fp);
    VkImageUsageFlags u = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) u |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) u |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if ((fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) && (t->d.flags & NVN_TEXTURE_FLAGS_IMAGE)) u |= VK_IMAGE_USAGE_STORAGE_BIT;
    VkSampleCountFlagBits sc = t->d.samples > 1 ? (VkSampleCountFlagBits)t->d.samples : VK_SAMPLE_COUNT_1_BIT;
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | (cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0) | (is3d ? VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT : 0),
                            is3d ? VK_IMAGE_TYPE_3D : t->d.target == NVN_TEXTURE_TARGET_1D || t->d.target == NVN_TEXTURE_TARGET_1D_ARRAY ? VK_IMAGE_TYPE_1D : VK_IMAGE_TYPE_2D,
                            f.f, {v->w, t->d.target == NVN_TEXTURE_TARGET_1D_ARRAY ? 1 : v->h, v->d}, v->levels, v->layers, sc, VK_IMAGE_TILING_OPTIMAL, u, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    if (sc != VK_SAMPLE_COUNT_1_BIT) ii.mipLevels = 1;
    VkResult r = vkCreateImage(g_dev, &ii, NULL, &v->img);
    if (r != VK_SUCCESS) { LOG("[RED] vk: image %ux%ux%u x%u levels %u format %#x target %d not created (%d)", v->w, v->h, v->d, v->layers, v->levels, t->d.format, t->d.target, (int)r); free(v); return NULL; }
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(g_dev, v->img, &mr); VkDeviceSize off;
    v->blk = img_mem(mr, &v->m, &off); VKCHECK(vkBindImageMemory(g_dev, v->img, v->m, off));
    to_general(C->up, v->img, v->aspect); C->up_used = 1;
    v->gpu = for_rt;
    pthread_mutex_lock(&g_rmu); v->next = g_tex[v->serial % NHASH]; g_tex[v->serial % NHASH] = v; pthread_mutex_unlock(&g_rmu);
    int packaged = (t->has_layout || cpu_written(t->serial)) && t->mem && !for_rt;
    if (packaged) tex_upload(v, t);
    else if (!for_rt) {   // contents come from the GPU (copies, render passes): start cleared, as a new texture's storage would read on hardware after a clear
        VkClearColorValue z = {{0}}; VkImageSubresourceRange rr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        if (!f.depth) vkCmdClearColorImage(C->up, v->img, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &rr);
    }
    return v;
}
// Packaged texture: each level and layer deswizzled from the pool storage into the ring, then copied into the image by the upload command buffer (it runs before this submission's draws)
static void tex_upload(vtex *v, const tex_t *t) {
    int bw = 1, bh = 1, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (!bpb) { RED_ONCE(4, "texture format %#x: block size unknown, not uploaded", t->d.format); return; }
    for (unsigned lv = 0; lv < v->levels; lv++) {
        uint32_t w = v->w >> lv ? v->w >> lv : 1, h = v->h >> lv ? v->h >> lv : 1, d = v->d >> lv ? v->d >> lv : 1;
        if (t->d.target == NVN_TEXTURE_TARGET_1D_ARRAY) h = 1;
        uint32_t bx = (w + bw - 1) / bw, by = (h + bh - 1) / bh; size_t pitch = (size_t)bx * bpb, lsz = pitch * by * d;
        for (unsigned ly = 0; ly < v->layers; ly++) {
            size_t out = v->f.dec ? (size_t)w * h * 4 * d : lsz;
            VkDeviceSize o = ring_alloc(out, 16);
            if (o == VK_WHOLE_SIZE) return;
            uint8_t *dst = C->ringp + o, *tmp = v->f.dec ? malloc(lsz) : dst;
            int rc = (t->d.flags & NVN_TEXTURE_FLAGS_LINEAR) ? (memcpy(tmp, t->mem + nvn_tex_level_offset(&t->d, lv), lsz), 0) : nvn_deswizzle(&t->d, lv, ly, t->mem, tmp, pitch);
            if (rc) { RED_ONCE(5, "texture format %#x target %d: deswizzle failed", t->d.format, t->d.target); if (v->f.dec) free(tmp); continue; }
            if (v->f.dec) { for (uint32_t z = 0; z < d; z++) bc_decode(v->f.dec, tmp + pitch * by * z, pitch, w, h, dst + (size_t)w * h * 4 * z); free(tmp); }
            VkBufferImageCopy c = {o, 0, 0, {v->aspect & VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT, lv, ly, 1}, {0, 0, 0}, {w, h, d}};
            vkCmdCopyBufferToImage(C->up, C->ring, v->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        }
    }
    C->up_used = 1; v->uploaded = 1;
}
static void tex_free(vtex *v) {   // released by the game: draws already recorded (and the one being recorded) may still use it
    pthread_mutex_lock(&g_gbmu);
    v->dead = g_fence + 1;
    if (g_ngb == g_cgb) { g_cgb = g_cgb ? 2 * g_cgb : 64; g_gb = realloc(g_gb, g_cgb * sizeof *g_gb); }
    g_gb[g_ngb++] = v;
    pthread_mutex_unlock(&g_gbmu);
}
static void garbage_free(uint64_t done) {
    pthread_mutex_lock(&g_gbmu);
    for (int i = 0; i < g_ngb;) {
        vtex *v = g_gb[i];
        if (v->dead > done) { i++; continue; }
        for (int k = 0; k < v->nviews; k++) vkDestroyImageView(g_dev, v->views[k].v, NULL);
        vkDestroyImage(g_dev, v->img, NULL); img_mem_free(v->blk, v->m); free(v->views); free(v);
        g_gb[i] = g_gb[--g_ngb];
    }
    pthread_mutex_unlock(&g_gbmu);
}
// A view of the image: type and format from the NVN view (or the texture), levels and layers, swizzle (the format's missing alpha, the texture's and the view's swizzles)
static VkImageView tex_view(vtex *v, const tex_t *t, const view_t *vw, int has_vw, int want_type, int rt) {
    int target = has_vw && vw->set_target ? vw->target : t->d.target;
    int fmtn = has_vw && vw->set_format ? vw->format : t->d.format;
    uint32_t bl = has_vw && vw->set_levels ? vw->base_level : 0, nl = has_vw && vw->set_levels ? vw->nlevels : v->levels - bl;
    uint32_t b0 = has_vw && vw->set_layers ? vw->min_layer : 0, nly = has_vw && vw->set_layers ? vw->nlayers : v->layers - b0;
    if (rt) nl = 1;
    if (!nl || bl + nl > v->levels) nl = v->levels - (bl < v->levels ? bl : 0);
    if (!nly || b0 + nly > v->layers) nly = v->layers - (b0 < v->layers ? b0 : 0);
    fmt_t f = fmt_of(fmtn); if (!f.f || f.dec != v->f.dec || (f.dec && f.f != v->f.f)) f = v->f;
    if (f.dec) f.f = v->f.f;
    uint8_t swz[4] = {2, 3, 4, 5};
    if (!rt) {
        if (t->set_swz) for (int i = 0; i < 4; i++) swz[i] = (uint8_t)(t->swz >> (3 * i) & 7);
        if (has_vw && vw->set_swizzle) memcpy(swz, vw->swz, 4);
        if (f.noalpha) for (int i = 0; i < 4; i++) if (swz[i] == 5) swz[i] = 1;
    }
    VkImageViewType vt;
    if (want_type >= 0) vt = (VkImageViewType)want_type;
    else switch (target) {
        case NVN_TEXTURE_TARGET_1D: vt = VK_IMAGE_VIEW_TYPE_1D; break; case NVN_TEXTURE_TARGET_1D_ARRAY: vt = VK_IMAGE_VIEW_TYPE_1D_ARRAY; break;
        case NVN_TEXTURE_TARGET_3D: vt = VK_IMAGE_VIEW_TYPE_3D; break;
        case NVN_TEXTURE_TARGET_2D_ARRAY: case NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY: vt = VK_IMAGE_VIEW_TYPE_2D_ARRAY; break;
        case NVN_TEXTURE_TARGET_CUBEMAP: vt = VK_IMAGE_VIEW_TYPE_CUBE; break; case NVN_TEXTURE_TARGET_CUBEMAP_ARRAY: vt = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY; break;
        default: vt = VK_IMAGE_VIEW_TYPE_2D; break;
    }
    if (vt == VK_IMAGE_VIEW_TYPE_2D && nly > 1) nly = 1;
    if (vt == VK_IMAGE_VIEW_TYPE_CUBE) nly = 6;
    VkImageAspectFlags asp = v->aspect; if (asp & VK_IMAGE_ASPECT_DEPTH_BIT && !rt) asp = VK_IMAGE_ASPECT_DEPTH_BIT;   // sampling reads depth
    uint64_t key = (uint64_t)f.f | (uint64_t)vt << 16 | (uint64_t)bl << 20 | (uint64_t)nl << 25 | (uint64_t)b0 << 30 | (uint64_t)nly << 42 | (uint64_t)(swz[0] | swz[1] << 3 | swz[2] << 6 | swz[3] << 9) << 50 | (uint64_t)rt << 63;
    for (int i = 0; i < v->nviews; i++) if (v->views[i].key == key) return v->views[i].v;
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, v->img, vt, f.f,
                                {(VkComponentSwizzle)(swz[0] + 1), (VkComponentSwizzle)(swz[1] + 1), (VkComponentSwizzle)(swz[2] + 1), (VkComponentSwizzle)(swz[3] + 1)}, {asp, bl, nl, b0, nly}};
    VkImageView view; VkResult r = vkCreateImageView(g_dev, &vi, NULL, &view);
    if (r != VK_SUCCESS) { static int n; if (n++ < 8) LOG("[RED] vk: image view (format %d type %d levels %u+%u layers %u+%u) failed (%d)", f.f, vt, bl, nl, b0, nly, (int)r); return VK_NULL_HANDLE; }
    v->views = realloc(v->views, (v->nviews + 1) * sizeof *v->views); v->views[v->nviews++] = (vview){key, view};
    return view;
}

// ---------- Registrations (game threads) ----------
typedef struct { tex_t t; view_t v; uint8_t ok, has_v; } treg_t;
static treg_t *g_treg; static int g_ntreg;
static samp_par *g_sreg; static uint8_t *g_sreg_ok; static int g_nsreg;
void mtl_reg_tex(int id, const tex_t *t, const view_t *v) {
    if (id < 0) return;
    pthread_mutex_lock(&g_rmu);
    if (id >= g_ntreg) { int n = id + 1024; g_treg = realloc(g_treg, n * sizeof *g_treg); memset(g_treg + g_ntreg, 0, (n - g_ntreg) * sizeof *g_treg); g_ntreg = n; }
    g_treg[id].t = *t; g_treg[id].ok = 1; g_treg[id].has_v = v != NULL; if (v) g_treg[id].v = *v;
    pthread_mutex_unlock(&g_rmu);
}
void mtl_reg_samp(int id, const samp_par *p) {
    if (id < 0) return;
    pthread_mutex_lock(&g_rmu);
    if (id >= g_nsreg) { int n = id + 256; g_sreg = realloc(g_sreg, n * sizeof *g_sreg); g_sreg_ok = realloc(g_sreg_ok, n); memset(g_sreg_ok + g_nsreg, 0, n - g_nsreg); g_nsreg = n; }
    g_sreg[id] = *p; g_sreg_ok[id] = 1;
    pthread_mutex_unlock(&g_rmu);
}
void mtl_set_tex_pool_n(int n) { (void)n; }
void mtl_tex_init(const tex_t *t) { (void)t; }
void mtl_tex_release(tex_t *t) {
    pthread_mutex_lock(&g_rmu);
    for (vtex **pp = &g_tex[t->serial % NHASH]; *pp; pp = &(*pp)->next) if ((*pp)->serial == t->serial) { vtex *v = *pp; *pp = v->next; pthread_mutex_unlock(&g_rmu); tex_free(v); return; }
    pthread_mutex_unlock(&g_rmu);
}
void mtl_tex_cpu_written(const tex_t *t) {
    pthread_mutex_lock(&g_rmu);
    if (!cpu_written(t->serial)) { if (g_ncpu == g_ccpu) { g_ccpu = g_ccpu ? 2 * g_ccpu : 64; g_cpu_written = realloc(g_cpu_written, g_ccpu * sizeof *g_cpu_written); } g_cpu_written[g_ncpu++] = t->serial; }
    pthread_mutex_unlock(&g_rmu);
}
void mtl_prog_release(prog_t *p) { (void)p; }

// Samplers, cached by their parameters
typedef struct { samp_par p; VkSampler s; } scache;
static scache *g_sc; static int g_nsc;
static VkSamplerAddressMode wrap(int w) {
    switch (w) { case 1: return VK_SAMPLER_ADDRESS_MODE_REPEAT; case 6: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT; case 4: case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
                 case 2: case 3: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE; default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; }
}
static VkSampler samp_get(int sid) {
    if (sid <= 0 || sid >= g_nsreg || !g_sreg_ok[sid]) return g_dummy_samp;
    samp_par p = g_sreg[sid];
    for (int i = 0; i < g_nsc; i++) if (!memcmp(&g_sc[i].p, &p, sizeof p)) return g_sc[i].s;
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = p.magf ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.minFilter = p.minf == 1 || p.minf == 3 || p.minf == 5 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.mipmapMode = p.minf == 4 || p.minf == 5 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = wrap(p.ws); si.addressModeV = wrap(p.wt); si.addressModeW = wrap(p.wr);
    si.mipLodBias = p.lod_bias; si.minLod = p.minf >= 2 ? p.lod_min : 0; si.maxLod = p.minf >= 2 ? p.lod_max : 0.25f;
    if (p.aniso > 1) { si.anisotropyEnable = VK_TRUE; si.maxAnisotropy = p.aniso > 16 ? 16 : p.aniso; }
    if (p.cmp_mode) { si.compareEnable = VK_TRUE; si.compareOp = (VkCompareOp)(p.cmp_func ? p.cmp_func - 1 : 7); }
    int white = p.border[0] + p.border[1] + p.border[2] > 1.5f, opaque = p.border[3] > 0.5f;
    si.borderColor = white ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : opaque ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    VkSampler s; if (vkCreateSampler(g_dev, &si, NULL, &s) != VK_SUCCESS) return g_dummy_samp;
    g_sc = realloc(g_sc, (g_nsc + 1) * sizeof *g_sc); g_sc[g_nsc++] = (scache){p, s};
    return s;
}

// ---------- Shaders and pipelines ----------
enum { BT_UBO, BT_SSBO, BT_CIS, BT_TBUF, BT_UTB, BT_STB, BT_SIMG, BT_TEX, BT_SAMP };
enum { K_UNK, K_CBUF, K_C1, K_SUPPORT, K_DRVCB, K_TEX, K_IMG, K_SBUF, K_TBUF };
enum { S_NONE, S_SAMP, S_SAMP_TEX, S_SAMP_ID };
typedef struct { uint32_t binding, count; uint8_t type, kind, skind; int16_t reg, sreg, off; } sbind;
typedef struct {
    char key[17]; int stage;   // 0 vertex, 1 fragment, 5 compute (NVN stage numbers)
    VkShaderModule mod; VkDescriptorSetLayout dsl;
    sbind b[64]; int nb; uint8_t vin[32]; uint32_t vin_mask;   // vin: component type per vertex input location (0 float, 1 uint, 2 int)
    uint8_t *c1; uint32_t c1_len; uint32_t tg[3];
} shader_t;
static shader_t **g_sh; static int g_nsh;
static const char *g_vkdir;
static uint32_t *read_file(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint32_t *p = malloc((size_t)sz + 1); if (fread(p, 1, (size_t)sz, f) != (size_t)sz) { free(p); fclose(f); return NULL; }
    fclose(f); ((char *)p)[sz] = 0; *n = (size_t)sz; return p;
}
static shader_t *shader_get(const uint8_t md[8]) {
    char key[17]; for (int i = 0; i < 8; i++) sprintf(key + 2 * i, "%02x", md[i]);
    for (int i = 0; i < g_nsh; i++) if (!strcmp(g_sh[i]->key, key)) return g_sh[i];
    if (!g_vkdir && !(g_vkdir = getenv("DM_VK_DIR"))) die("vk: DM_VK_DIR is not set (the folder of <key>.spv / <key>.vk.txt from dm/android/tools/spv_android.py)");
    char path[1024]; size_t ns, nt;
    snprintf(path, sizeof path, "%s/%s.spv", g_vkdir, key); uint32_t *spv = read_file(path, &ns);
    snprintf(path, sizeof path, "%s/%s.vk.txt", g_vkdir, key); char *txt = (char *)read_file(path, &nt);
    shader_t *s = calloc(1, sizeof *s); strcpy(s->key, key); s->stage = -1;
    g_sh = realloc(g_sh, (g_nsh + 1) * sizeof *g_sh); g_sh[g_nsh++] = s;
    if (!spv || !txt) { LOG("[RED] vk: shader %s not in %s", key, g_vkdir); free(spv); free(txt); return s; }
    for (char *ln = strtok(txt, "\n"); ln; ln = strtok(NULL, "\n")) {
        char a[16], ty[8], kind[16], sk[16]; int b, cnt, reg, off, sreg, l, t;
        if (sscanf(ln, "stage %15s", a) == 1) s->stage = !strcmp(a, "vs") ? 0 : !strcmp(a, "ps") ? 1 : !strcmp(a, "cs") ? 5 : -1;
        else if (sscanf(ln, "tg %u %u %u", &s->tg[0], &s->tg[1], &s->tg[2]) == 3) {}
        else if (!strncmp(ln, "c1 ", 3)) { size_t n = strlen(ln + 3) / 2; s->c1 = malloc(n + 16); s->c1_len = (uint32_t)n; for (size_t i = 0; i < n; i++) sscanf(ln + 3 + 2 * i, "%2hhx", &s->c1[i]); }
        else if (sscanf(ln, "vin %d %d", &l, &t) == 2 && l >= 0 && l < 32) { s->vin[l] = (uint8_t)t; s->vin_mask |= 1u << l; }
        else if (sscanf(ln, "b %d %7s %d %15s %d %d %15s %d", &b, ty, &cnt, kind, &reg, &off, sk, &sreg) == 8 && s->nb < 64) {
            sbind *e = &s->b[s->nb++]; e->binding = (uint32_t)b; e->count = cnt > 0 ? (uint32_t)cnt : 1; e->reg = (int16_t)reg; e->off = (int16_t)off; e->sreg = (int16_t)sreg;
            e->type = !strcmp(ty, "ubo") ? BT_UBO : !strcmp(ty, "ssbo") ? BT_SSBO : !strcmp(ty, "cis") ? BT_CIS : !strcmp(ty, "tbuf") ? BT_TBUF : !strcmp(ty, "utb") ? BT_UTB :
                      !strcmp(ty, "stb") ? BT_STB : !strcmp(ty, "simg") ? BT_SIMG : !strcmp(ty, "tex") ? BT_TEX : BT_SAMP;
            e->kind = !strcmp(kind, "cbuf") ? K_CBUF : !strcmp(kind, "c1") ? K_C1 : !strcmp(kind, "support") ? K_SUPPORT : !strcmp(kind, "drvcb") ? K_DRVCB : !strcmp(kind, "tex") ? K_TEX :
                      !strcmp(kind, "img") ? K_IMG : !strcmp(kind, "sbuf") ? K_SBUF : !strcmp(kind, "tbuf") ? K_TBUF : K_UNK;
            e->skind = !strcmp(sk, "samp") ? S_SAMP : !strcmp(sk, "samp_tex") ? S_SAMP_TEX : !strcmp(sk, "samp_id") ? S_SAMP_ID : S_NONE;
            if (e->kind == K_UNK) LOG("[RED] vk: shader %s binding %d has kind %s, not implemented (reads zero)", key, b, kind);
        }
    }
    free(txt);
    // the fragment stage's resources move to set 1 (DescriptorSet decorations, operand 34)
    uint32_t set = s->stage == 1 ? 1 : 0;
    for (size_t i = 5; i < ns / 4;) { uint32_t w = spv[i], op = w & 0xffff, len = w >> 16; if (!len) break; if (op == 71 && len >= 4 && spv[i + 2] == 34) spv[i + 3] = set; i += len; }
    VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, ns, spv};
    VkResult r = vkCreateShaderModule(g_dev, &mi, NULL, &s->mod); free(spv);
    if (r != VK_SUCCESS) { LOG("[RED] vk: shader %s rejected (%d)", key, (int)r); s->mod = VK_NULL_HANDLE; }
    VkDescriptorSetLayoutBinding lb[64]; VkShaderStageFlags sf = s->stage == 0 ? VK_SHADER_STAGE_VERTEX_BIT : s->stage == 1 ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_COMPUTE_BIT;
    static const VkDescriptorType DT[] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
                                          VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLER};
    for (int i = 0; i < s->nb; i++) lb[i] = (VkDescriptorSetLayoutBinding){s->b[i].binding, DT[s->b[i].type], s->b[i].count, sf, NULL};
    VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, (uint32_t)s->nb, lb};
    VKCHECK(vkCreateDescriptorSetLayout(g_dev, &li, NULL, &s->dsl));
    return s;
}
static VkDescriptorSetLayout g_empty_dsl;
typedef struct { shader_t *vs, *fs; VkPipelineLayout pl; } prog_vk;
static prog_vk *g_pl; static int g_npl;
static VkPipelineLayout layout_get(shader_t *vs, shader_t *fs) {
    for (int i = 0; i < g_npl; i++) if (g_pl[i].vs == vs && g_pl[i].fs == fs) return g_pl[i].pl;
    if (!g_empty_dsl) { VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; VKCHECK(vkCreateDescriptorSetLayout(g_dev, &li, NULL, &g_empty_dsl)); }
    VkDescriptorSetLayout sl[2] = {vs ? vs->dsl : g_empty_dsl, fs ? fs->dsl : g_empty_dsl};
    VkPipelineLayoutCreateInfo pi = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, fs ? 2 : 1, sl, 0, NULL};
    VkPipelineLayout pl; VKCHECK(vkCreatePipelineLayout(g_dev, &pi, NULL, &pl));
    g_pl = realloc(g_pl, (g_npl + 1) * sizeof *g_pl); g_pl[g_npl++] = (prog_vk){vs, fs, pl};
    return pl;
}

// ---------- Render passes: dynamic rendering, or a render pass + framebuffer made for each pass ----------
typedef struct { int n; VkImageView cv[8]; VkFormat cf[8]; VkAttachmentLoadOp cl[8]; VkClearColorValue cc[8];
                 VkImageView dv; VkFormat df; int stencil; VkAttachmentLoadOp dl, sl; VkClearDepthStencilValue dc; uint32_t w, h; VkImageLayout layout; } rp_desc;
static struct { uint64_t key; VkRenderPass rp; } g_rpc[512]; static int g_nrpc; static pthread_mutex_t g_rpmu = PTHREAD_MUTEX_INITIALIZER;
// The render pass of these attachments (cached; also the "compatible" one pipelines are made against: load actions do not affect compatibility)
static VkRenderPass rp_get(const VkFormat *cf, const VkAttachmentLoadOp *cl, int n, VkFormat df, int stencil, VkAttachmentLoadOp dl, VkAttachmentLoadOp sl, VkImageLayout layout) {
    uint64_t key = 0xcbf29ce484222325ull;
    #define MIX(v) key = (key ^ (uint64_t)(v)) * 0x100000001b3ull
    for (int i = 0; i < n; i++) { MIX(cf[i]); MIX(cl[i]); } MIX(n); MIX(df); MIX(stencil); MIX(dl); MIX(sl); MIX(layout);
    #undef MIX
    pthread_mutex_lock(&g_rpmu);
    for (int i = 0; i < g_nrpc; i++) if (g_rpc[i].key == key) { VkRenderPass r = g_rpc[i].rp; pthread_mutex_unlock(&g_rpmu); return r; }
    VkAttachmentDescription at[9]; VkAttachmentReference cr[8], dr = {VK_ATTACHMENT_UNUSED, layout}; uint32_t na = 0;
    for (int i = 0; i < n; i++) {
        if (!cf[i]) { cr[i] = (VkAttachmentReference){VK_ATTACHMENT_UNUSED, layout}; continue; }
        at[na] = (VkAttachmentDescription){0, cf[i], VK_SAMPLE_COUNT_1_BIT, cl[i], VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, layout, layout};
        cr[i] = (VkAttachmentReference){na++, layout};
    }
    if (df) { at[na] = (VkAttachmentDescription){0, df, VK_SAMPLE_COUNT_1_BIT, dl, VK_ATTACHMENT_STORE_OP_STORE, stencil ? sl : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                                stencil ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE, layout, layout}; dr.attachment = na++; }
    VkSubpassDescription sp = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, (uint32_t)n, cr, NULL, df ? &dr : NULL, 0, NULL};
    VkRenderPassCreateInfo ci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, na, at, 1, &sp, 0, NULL};
    VkRenderPass r; VKCHECK(vkCreateRenderPass(g_dev, &ci, NULL, &r));
    if (g_nrpc < 512) g_rpc[g_nrpc++] = (typeof(g_rpc[0])){key, r};
    pthread_mutex_unlock(&g_rpmu);
    return r;
}
// Begins rendering into d's attachments; without dynamic rendering the framebuffer it makes is returned, for the caller to destroy once the GPU is done with it
static VkFramebuffer rp_begin(VkCommandBuffer cb, const rp_desc *d) {
    if (g_dynrend) {
        VkRenderingAttachmentInfo ca[8], da = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}, sa = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        for (int i = 0; i < d->n; i++) ca[i] = (VkRenderingAttachmentInfo){VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, d->cv[i], d->layout, 0, NULL, 0,
                                                                          d->cv[i] ? d->cl[i] : VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_STORE, {.color = d->cc[i]}};
        if (d->dv) { da.imageView = d->dv; da.imageLayout = d->layout; da.loadOp = d->dl; da.storeOp = VK_ATTACHMENT_STORE_OP_STORE; da.clearValue.depthStencil = d->dc;
                     if (d->stencil) { sa = da; sa.loadOp = d->sl; } }
        VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, {{0, 0}, {d->w, d->h}}, 1, 0, (uint32_t)d->n, ca, d->dv ? &da : NULL, d->dv && d->stencil ? &sa : NULL};
        p_begin_rendering(cb, &ri);
        return VK_NULL_HANDLE;
    }
    VkFormat cf[8]; VkImageView views[9]; VkClearValue cv[9]; uint32_t nv = 0;
    for (int i = 0; i < d->n; i++) { cf[i] = d->cv[i] ? d->cf[i] : VK_FORMAT_UNDEFINED; if (d->cv[i]) { cv[nv].color = d->cc[i]; views[nv++] = d->cv[i]; } }
    if (d->dv) { cv[nv].depthStencil = d->dc; views[nv++] = d->dv; }
    VkRenderPass rp = rp_get(cf, d->cl, d->n, d->dv ? d->df : VK_FORMAT_UNDEFINED, d->stencil, d->dl, d->sl, d->layout);
    VkFramebufferCreateInfo fi = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, rp, nv, views, d->w, d->h, 1};
    VkFramebuffer fb; VKCHECK(vkCreateFramebuffer(g_dev, &fi, NULL, &fb));
    VkRenderPassBeginInfo bi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, rp, fb, {{0, 0}, {d->w, d->h}}, nv, cv};
    vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    return fb;
}
static void rp_end(VkCommandBuffer cb) { if (g_dynrend) p_end_rendering(cb); else vkCmdEndRenderPass(cb); }

// ---------- Replay state ----------
#define NTEX 256
static struct {
    const prog_t *bst[5], *cprog;   // the program bound to each graphics stage (NVN stage bits 1 vertex, 2 fragment, 4 geometry, 8 / 16 tessellation): games bind a program to some stages and NULL to others
    blend_st blend[8]; color_st color; chmask_st chmask; ms_st ms; poly_st poly; ds_st ds;
    va_st attr[16]; int nattr; vs_st stream[16]; int nstream; NVNbufferRange vbuf[16];
    p_bind_buf ubo[6][16], ssbo[6][16];
    uint64_t tex[6][NTEX], samp[6][128], img[6][32];
    uint32_t st_vmask[2], st_wmask[2], st_ref[2];
    p_poly_offset po; float vp[16][4]; float dr[2]; int has_vp; p_scissor sc; int has_sc;
    float blend_color[4]; int64_t copy_stride, copy_img_stride;
} S;
static int g_depth_zero = 1, g_origin_ll;
static struct {   // the current render targets and the open dynamic rendering; clears that come before the pass's first draw become its load actions (a tiler then never reads the old contents)
    int n; vtex *ct[8]; VkImageView cv[8]; vtex *dt; VkImageView dv; uint32_t w, h; int open;
    uint8_t clr_c[8], clr_d, clr_s; VkClearColorValue cc[8]; float cd; uint32_t cs;
} P;
static int pass_begin(void);
static void pass_end(void) {
    if (!P.open) { int pend = P.clr_d || P.clr_s; for (int i = 0; i < 8; i++) pend |= P.clr_c[i]; if (pend) pass_begin(); }   // clears with no draw after them still happen
    if (P.open) {
        rp_end(C->cb); P.open = 0;
        // what the pass wrote is read by later passes, samples and copies; their vertex work may start before it ends
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
        vkCmdPipelineBarrier(C->cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                             VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    }
}
static int pass_begin(void) {
    if (P.open) return 1;
    if (!P.n && !P.dt) return 0;
    rp_desc d = {.n = P.n, .w = P.w, .h = P.h, .layout = VK_IMAGE_LAYOUT_GENERAL};
    for (int i = 0; i < P.n; i++) { d.cv[i] = P.cv[i]; d.cf[i] = P.ct[i] ? P.ct[i]->f.f : VK_FORMAT_UNDEFINED; d.cl[i] = P.clr_c[i] ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD; d.cc[i] = P.cc[i]; }
    if (P.dt) { d.dv = P.dv; d.df = P.dt->f.f; d.stencil = P.dt->f.stencil; d.dl = P.clr_d ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
                d.sl = P.clr_s ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD; d.dc = (VkClearDepthStencilValue){P.cd, P.cs}; }
    memset(P.clr_c, 0, sizeof P.clr_c); P.clr_d = P.clr_s = 0;
    VkFramebuffer fb = rp_begin(C->cb, &d); P.open = 1;
    if (fb) { if (C->nfb == C->cfb) { C->cfb = C->cfb ? 2 * C->cfb : 256; C->fb = realloc(C->fb, C->cfb * sizeof *C->fb); } C->fb[C->nfb++] = fb; }
    return 1;
}
static void set_rt(const p_set_rt *r) {
    pass_end();
    memset(&P, 0, sizeof P);
    P.n = r->n > 8 ? 8 : r->n; P.w = P.h = 0x7fffffff;
    for (int i = 0; i < P.n; i++) {
        if (!r->color[i]) continue;
        vtex *v = tex_get(r->color[i], 1); if (!v) continue;
        P.ct[i] = v; P.cv[i] = tex_view(v, r->color[i], &r->cview[i], r->has_cview[i], -1, 1);
        uint32_t lv = r->has_cview[i] && r->cview[i].set_levels ? r->cview[i].base_level : 0, w = v->w >> lv ? v->w >> lv : 1, h = v->h >> lv ? v->h >> lv : 1;
        if (w < P.w) P.w = w; if (h < P.h) P.h = h;
    }
    if (r->depth) {
        vtex *v = tex_get(r->depth, 1);
        if (v && v->f.depth) { P.dt = v; P.dv = tex_view(v, r->depth, &r->dview, r->has_dview, -1, 1); uint32_t lv = r->has_dview && r->dview.set_levels ? r->dview.base_level : 0;
                               if ((v->w >> lv) < P.w) P.w = v->w >> lv ? v->w >> lv : 1; if ((v->h >> lv) < P.h) P.h = v->h >> lv ? v->h >> lv : 1; }
    }
    if (P.w == 0x7fffffff) P.w = P.h = 1;
}

// Pipelines, cached by everything they are built from
typedef struct {
    shader_t *vs, *fs; int prim;
    va_st attr[16]; int nattr; vs_st stream[16]; int nstream;
    blend_st blend[8]; color_st color; chmask_st chmask; ds_st ds; poly_st poly; int samples;
    VkFormat cf[8]; int nc; VkFormat df; int stencil;
} pso_key;
typedef struct { pso_key k; uint64_t h; VkPipeline p; } pso_ent;
static pso_ent *g_pso; static int g_npso, g_cpso;
static uint64_t fnv(const void *p, size_t n) { const uint8_t *b = p; uint64_t h = 0xcbf29ce484222325ull; while (n--) h = (h ^ *b++) * 0x100000001b3ull; return h; }
static VkBlendFactor bfac(int f) {
    switch (f) {
        case 1: return VK_BLEND_FACTOR_ZERO; case 2: return VK_BLEND_FACTOR_ONE; case 3: return VK_BLEND_FACTOR_SRC_COLOR; case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 5: return VK_BLEND_FACTOR_SRC_ALPHA; case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; case 7: return VK_BLEND_FACTOR_DST_ALPHA; case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case 9: return VK_BLEND_FACTOR_DST_COLOR; case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR; case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        case 0x61: return VK_BLEND_FACTOR_CONSTANT_COLOR; case 0x62: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR; case 0x63: return VK_BLEND_FACTOR_CONSTANT_ALPHA; case 0x64: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        case 0x10: case 0x11: case 0x12: case 0x13: RED_ONCE(10, "dual-source blending: Mali has none, the second source reads as one");   /* fall through */
        default: return f == 0x11 || f == 0x13 ? VK_BLEND_FACTOR_ZERO : VK_BLEND_FACTOR_ONE;
    }
}
static VkBlendOp bop(int e) { switch (e) { case 2: return VK_BLEND_OP_SUBTRACT; case 3: return VK_BLEND_OP_REVERSE_SUBTRACT; case 4: return VK_BLEND_OP_MIN; case 5: return VK_BLEND_OP_MAX; } return VK_BLEND_OP_ADD; }
static VkStencilOp sop(int o) { return o >= 1 && o <= 8 ? (VkStencilOp)(o - 1) : VK_STENCIL_OP_KEEP; }
static VkCompareOp cop(int f) { return f >= 1 && f <= 8 ? (VkCompareOp)(f - 1) : VK_COMPARE_OP_ALWAYS; }
static VkPipeline pso_get(const pso_key *k, VkPipelineLayout pl) {
    uint64_t h = fnv(k, sizeof *k);
    for (int i = 0; i < g_npso; i++) if (g_pso[i].h == h && !memcmp(&g_pso[i].k, k, sizeof *k)) return g_pso[i].p;
    VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, k->vs->mod, "main", NULL},
                                             {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, k->fs ? k->fs->mod : VK_NULL_HANDLE, "main", NULL}};
    VkVertexInputBindingDescription vb[16]; VkVertexInputAttributeDescription va[16]; uint32_t nvb = 0, nva = 0; uint32_t used = 0;
    for (int i = 0; i < k->nattr && i < 16; i++) {
        if (!(k->vs->vin_mask >> i & 1)) continue;
        VkFormat f = vfmt(k->attr[i].fmt);
        if (!f) { static int n; if (n++ < 8) LOG("[RED] vk: vertex attribute %d format %#x has no Vulkan format", i, k->attr[i].fmt); continue; }
        va[nva++] = (VkVertexInputAttributeDescription){(uint32_t)i, k->attr[i].stream, f, k->attr[i].offset};
        used |= 1u << k->attr[i].stream;
    }
    for (int s = 0; s < 16; s++) if (used >> s & 1) {
        uint32_t stride = s < k->nstream ? k->stream[s].stride : 0, div = s < k->nstream ? k->stream[s].divisor : 0;
        if (div > 1) RED_ONCE(11, "instance divisor %u: drawn as 1", div);
        vb[nvb++] = (VkVertexInputBindingDescription){(uint32_t)s, stride, div ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
    }
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0, nvb, vb, nva, va};
    static const VkPrimitiveTopology TOPO[] = {VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_STRIP, VK_PRIMITIVE_TOPOLOGY_LINE_STRIP,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN, VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY, VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY, VK_PRIMITIVE_TOPOLOGY_PATCH_LIST};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, TOPO[k->prim < 15 ? k->prim : 4], VK_FALSE};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = (VkCullModeFlags)k->poly.cull; rs.frontFace = k->poly.front ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    if (g_origin_ll) rs.frontFace = rs.frontFace == VK_FRONT_FACE_CLOCKWISE ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    rs.depthBiasEnable = k->poly.offset_en != 0; rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, (VkSampleCountFlagBits)(k->samples > 1 ? k->samples : 1)};
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    if (k->df) {
        ds.depthTestEnable = k->ds.dt; ds.depthWriteEnable = k->ds.dw; ds.depthCompareOp = k->ds.dt ? cop(k->ds.df) : VK_COMPARE_OP_ALWAYS;
        if (k->stencil && k->ds.st) {
            ds.stencilTestEnable = VK_TRUE;
            ds.front = (VkStencilOpState){sop(k->ds.fail_f), sop(k->ds.zpass_f), sop(k->ds.zfail_f), cop(k->ds.sf_f), 0xff, 0xff, 0};
            ds.back = (VkStencilOpState){sop(k->ds.fail_b), sop(k->ds.zpass_b), sop(k->ds.zfail_b), cop(k->ds.sf_b), 0xff, 0xff, 0};
        }
    }
    VkPipelineColorBlendAttachmentState ba[8];
    for (int i = 0; i < k->nc; i++) {
        const blend_st *b = &k->blend[i]; int en = k->color.blend_en >> i & 1;
        ba[i] = (VkPipelineColorBlendAttachmentState){en, bfac(b->src_rgb), bfac(b->dst_rgb), bop(b->eq_rgb), bfac(b->src_a), bfac(b->dst_a), bop(b->eq_a), (k->chmask.mask >> (4 * i)) & 15};
    }
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_LOGIC_OP_COPY, (uint32_t)k->nc, ba, {0}};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                            VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, sizeof dyn / sizeof *dyn, dyn};
    VkPipelineRenderingCreateInfo rinfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, NULL, 0, (uint32_t)k->nc, k->cf, k->df, k->stencil ? k->df : VK_FORMAT_UNDEFINED};
    VkRenderPass crp = VK_NULL_HANDLE;
    if (!g_dynrend) { VkAttachmentLoadOp ld[8]; for (int i = 0; i < 8; i++) ld[i] = VK_ATTACHMENT_LOAD_OP_LOAD;
                      crp = rp_get(k->cf, ld, k->nc, k->df, k->stencil, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_GENERAL); }
    VkGraphicsPipelineCreateInfo gi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, g_dynrend ? &rinfo : NULL, 0, k->fs ? 2 : 1, st, &vi, &ia, NULL, &vp, &rs, &ms, &ds, &cb, &dy, pl, crp, 0, VK_NULL_HANDLE, -1};
    VkPipeline p; VkResult r = vkCreateGraphicsPipelines(g_dev, VK_NULL_HANDLE, 1, &gi, NULL, &p);
    if (r != VK_SUCCESS) { LOG("[RED] vk: pipeline for %s / %s not created (%d)", k->vs->key, k->fs ? k->fs->key : "-", (int)r); p = VK_NULL_HANDLE; }
    if (g_npso == g_cpso) { g_cpso = g_cpso ? 2 * g_cpso : 256; g_pso = realloc(g_pso, g_cpso * sizeof *g_pso); }
    g_pso[g_npso++] = (pso_ent){*k, h, p};
    return p;
}

// ---------- Draw resources ----------
typedef struct { uint64_t addr, size, gen; VkDeviceSize off; } dcopy;
#define NDC 4096
static dcopy g_dc[NDC]; static int g_ndc;
static VkDeviceSize copy_in(uint64_t addr, uint64_t size, VkDeviceSize align) {   // guest memory → ring, reused when the same range is asked for again unchanged (hashed by address)
    dcopy *e = &g_dc[(addr >> 4 ^ addr >> 13) % NDC];
    if (e->addr == addr && e->size >= size && e->gen == g_wgen && e->gen) return e->off;
    VkDeviceSize o = ring_put((const void *)(uintptr_t)addr, size, align);
    if (o != VK_WHOLE_SIZE) *e = (dcopy){addr, size, g_wgen, o};
    return o;
}
static VkImageView tex_slot(int stage, uint64_t h, VkSampler *sm, int *ok) {
    int tid = (int)(h & 0xfffff); *ok = 0;
    if (!tid) return g_dummy_view;
    pthread_mutex_lock(&g_rmu);
    treg_t tr = tid < g_ntreg ? g_treg[tid] : (treg_t){0};
    pthread_mutex_unlock(&g_rmu);
    if (!tr.ok) return g_dummy_view;
    vtex *v = tex_get(&tr.t, 0); if (!v) return g_dummy_view;
    VkImageView view = tex_view(v, &tr.t, &tr.v, tr.has_v, -1, 0);
    (void)sm; (void)stage; *ok = view != VK_NULL_HANDLE;
    return view ? view : g_dummy_view;
}
static VkDescriptorSet write_set(shader_t *s, int stage) {
    VkDescriptorSet ds; VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, C->dp, 1, &s->dsl};
    if (vkAllocateDescriptorSets(g_dev, &ai, &ds) != VK_SUCCESS) { RED_ONCE(20, "descriptor pool exhausted in one submission: draws are dropped"); return VK_NULL_HANDLE; }
    VkWriteDescriptorSet w[64]; VkDescriptorBufferInfo bi[64]; VkDescriptorImageInfo ii[64]; int nw = 0;
    for (int i = 0; i < s->nb; i++) {
        const sbind *b = &s->b[i];
        VkWriteDescriptorSet *x = &w[nw]; *x = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ds, b->binding, 0, 1};
        if (b->type == BT_UBO || b->type == BT_SSBO) {
            VkDescriptorBufferInfo *d = &bi[nw]; *d = (VkDescriptorBufferInfo){g_zero_buf, 0, 65536};
            if (b->kind == K_CBUF && b->reg >= 0 && b->reg < 16) {
                const p_bind_buf *u = &S.ubo[stage][b->reg];
                uint64_t n = u->size > 65536 ? 65536 : u->size;
                if (u->addr && n) { VkDeviceSize o = copy_in(u->addr, n, 256); if (o != VK_WHOLE_SIZE) *d = (VkDescriptorBufferInfo){C->ring, o, n}; }
            } else if (b->kind == K_C1 && s->c1_len) { VkDeviceSize o = ring_put(s->c1, s->c1_len, 256); if (o != VK_WHOLE_SIZE) *d = (VkDescriptorBufferInfo){C->ring, o, s->c1_len}; }
            else if (b->kind == K_SUPPORT) *d = (VkDescriptorBufferInfo){g_support_buf, 0, 4096};
            else if (b->kind == K_SBUF && b->reg >= 0 && b->reg < 16) {
                const p_bind_buf *u = &S.ssbo[stage][b->reg];
                if (u->addr && u->size) { VkDeviceSize o = copy_in(u->addr, u->size, 256); if (o != VK_WHOLE_SIZE) *d = (VkDescriptorBufferInfo){C->ring, o, u->size}; }
                RED_ONCE(21, "storage buffers are read from a copy: GPU writes to them do not reach guest memory yet");
            } else if (b->kind == K_DRVCB) {   // driver segments: c2 holds the bound texture / sampler handles (as nvn_mtl.m fills them), c0 the storage buffer table
                uint8_t seg[4096]; memset(seg, 0, sizeof seg);
                if (b->off == 2) {
                    for (int r = 0; r < 32 && 0x20 + 8 * r < 0x120; r++) { uint32_t v = (uint32_t)S.tex[stage][r]; memcpy(seg + 0x20 + 8 * r, &v, 4); }
                    for (int r = 0; r < 8; r++) { uint32_t v = (uint32_t)S.img[stage][r]; memcpy(seg + 0x120 + 8 * r, &v, 4); }
                    for (int r = 0; r < 128 && 0x168 + 8 * r + 4 <= 0x568; r++) { uint32_t v = (uint32_t)S.tex[stage][128 + r]; memcpy(seg + 0x168 + 8 * r, &v, 4); }
                    for (int r = 0; r < 128 && 0x568 + 8 * r + 4 <= (int)sizeof seg; r++) { uint32_t v = (uint32_t)S.samp[stage][r]; memcpy(seg + 0x568 + 8 * r, &v, 4); }
                } else {
                    int base = stage == 0 ? 0x110 : stage == 1 ? 0x510 : stage == 5 ? 0x310 : -1;
                    if (base >= 0) for (int r = 0; r < 16; r++) { uint64_t a = S.ssbo[stage][r].addr; uint32_t n = (uint32_t)S.ssbo[stage][r].size; memcpy(seg + base + 16 * r, &a, 8); memcpy(seg + base + 16 * r + 8, &n, 4); }
                    if (stage != 5) { const float mp = 2047.0f; memcpy(seg + 0x610, &mp, 4); }
                }
                VkDeviceSize o = ring_put(seg, sizeof seg, 256); if (o != VK_WHOLE_SIZE) *d = (VkDescriptorBufferInfo){C->ring, o, sizeof seg};
            }
            x->descriptorType = b->type == BT_UBO ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; x->pBufferInfo = d;
        } else if (b->type == BT_CIS) {
            VkSampler sm = g_dummy_samp; int ok = 0; VkImageView view = g_dummy_view;
            if (b->kind == K_TEX && b->reg >= 0 && b->reg < NTEX) {
                view = tex_slot(stage, S.tex[stage][b->reg], &sm, &ok);
                int sid = b->skind == S_SAMP && b->sreg >= 0 && b->sreg < 128 ? (int)(S.samp[stage][b->sreg] >> 20) : b->skind == S_SAMP_TEX ? (int)(S.tex[stage][b->reg] >> 20) : b->skind == S_SAMP_ID ? b->sreg : 0;
                pthread_mutex_lock(&g_rmu); sm = samp_get(sid); pthread_mutex_unlock(&g_rmu);
            }
            ii[nw] = (VkDescriptorImageInfo){sm, view, VK_IMAGE_LAYOUT_GENERAL};
            x->descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; x->pImageInfo = &ii[nw];
        } else { RED_ONCE(22, "descriptor type %d not implemented yet (shader %s)", b->type, s->key); continue; }
        nw++;
    }
    vkUpdateDescriptorSets(g_dev, (uint32_t)nw, w, 0, NULL);
    return ds;
}
static _Atomic uint64_t g_wait_ns, g_rec_ns, g_pwait_ns;   // time the submit thread waited for the GPU, spent recording, and the present waited for its slot
static uint64_t g_stat_draws, g_stat_skip, g_skip_why[8];   // why: 0 no program, 1 shader missing / rejected, 2 no render target, 3 indirect, 4 primitive, 5 pipeline, 6 descriptors
#define SKIP(w) do { g_stat_skip++; g_skip_why[w]++; return; } while (0)
static void draw(const p_draw *dw, const p_draw_idx *di) {
    if (!S.bst[0]) SKIP(0);
    shader_t *vs = NULL, *fs = NULL;
    for (int st = 0; st < 2; st++) {   // the vertex shader of the program bound to the vertex stage, the fragment shader of the one bound to the fragment stage (cached by program and serial)
        const prog_t *p = S.bst[st]; if (!p) continue;
        static struct { const prog_t *p; uint64_t ser; shader_t *sh[2]; } pc[1024];
        unsigned h = (unsigned)(((uintptr_t)p >> 4) ^ p->serial) % 1024;
        if (pc[h].p != p || pc[h].ser != p->serial) {
            pc[h].p = p; pc[h].ser = p->serial; pc[h].sh[0] = pc[h].sh[1] = NULL;
            for (unsigned i = 0; i < p->nstage; i++) { uint8_t md[8]; if (!prog_key(p, i, md)) continue; shader_t *s = shader_get(md); if (s->stage == 0 || s->stage == 1) pc[h].sh[s->stage] = s; }
        }
        if (st) fs = pc[h].sh[1]; else vs = pc[h].sh[0];
    }
    if (S.bst[2] || S.bst[3] || S.bst[4]) RED_ONCE(32, "geometry / tessellation stages are bound: not drawn with them yet");
    if (!vs || !vs->mod || (fs && !fs->mod)) SKIP(1);
    if (!pass_begin()) SKIP(2);
    if ((dw && dw->ind) || (di && di->ind)) { RED_ONCE(30, "indirect draws are not implemented yet"); SKIP(3); }
    int prim = dw ? dw->prim : di->prim;
    if (prim == NVN_DRAW_PRIMITIVE_LINE_LOOP || prim == NVN_DRAW_PRIMITIVE_QUAD_STRIP || prim == NVN_DRAW_PRIMITIVE_PATCHES) { RED_ONCE(31, "primitive %d not implemented yet", prim); SKIP(4); }
    pso_key k; memset(&k, 0, sizeof k);
    k.vs = vs; k.fs = fs; k.prim = prim; k.nattr = S.nattr; memcpy(k.attr, S.attr, sizeof k.attr); k.nstream = S.nstream; memcpy(k.stream, S.stream, sizeof k.stream);
    k.nc = P.n; for (int i = 0; i < P.n; i++) { k.cf[i] = P.ct[i] && P.cv[i] ? P.ct[i]->f.f : VK_FORMAT_UNDEFINED; k.blend[i] = S.blend[i]; }
    k.color = S.color; k.chmask = S.chmask; k.poly = S.poly; k.samples = P.ct[0] ? 1 : 1;
    if (P.dt) { k.df = P.dt->f.f; k.stencil = P.dt->f.stencil; k.ds = S.ds; }
    VkPipelineLayout pl = layout_get(vs, fs);
    VkPipeline pp = pso_get(&k, pl);
    if (!pp) SKIP(5);
    VkDescriptorSet sets[2] = {write_set(vs, 0), fs ? write_set(fs, 1) : VK_NULL_HANDLE};
    if (!sets[0] || (fs && !sets[1])) SKIP(6);
    VkCommandBuffer cb = C->cb;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pp);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, fs ? 2 : 1, sets, 0, NULL);
    // NVN's upper-left origin keeps D3D's y-up clip space; Vulkan's is y-down: the viewport is flipped (negative height)
    float x = 0, y = 0, w = (float)P.w, h = (float)P.h;
    if (S.has_vp) { x = S.vp[0][0]; y = S.vp[0][1]; w = S.vp[0][2]; h = S.vp[0][3]; }
    float yt = g_origin_ll ? (float)P.h - y - h : y;   // lower-left origin: NVN's y counts from the bottom
    VkViewport vv = {x, yt + h, w, -h, S.dr[0], S.dr[1] ? S.dr[1] : 1};
    vkCmdSetViewport(cb, 0, 1, &vv);
    VkRect2D sc = {{0, 0}, {P.w, P.h}};
    if (S.has_sc) {
        int32_t sx = S.sc.x < 0 ? 0 : S.sc.x, sy = S.sc.y < 0 ? 0 : S.sc.y, ex = S.sc.x + S.sc.w, ey = S.sc.y + S.sc.h;
        if (g_origin_ll) { int32_t t = (int32_t)P.h - ey; ey = (int32_t)P.h - S.sc.y; sy = t < 0 ? 0 : t; }
        if (ex > (int32_t)P.w) ex = (int32_t)P.w; if (ey > (int32_t)P.h) ey = (int32_t)P.h;
        sc = (VkRect2D){{sx, sy}, {ex > sx ? (uint32_t)(ex - sx) : 0, ey > sy ? (uint32_t)(ey - sy) : 0}};
    }
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdSetDepthBias(cb, S.po.units, S.po.clamp, S.po.factor);
    vkCmdSetBlendConstants(cb, S.blend_color);
    for (int f = 0; f < 2; f++) {
        VkStencilFaceFlags ff = f ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT;
        vkCmdSetStencilCompareMask(cb, ff, S.st_vmask[f]); vkCmdSetStencilWriteMask(cb, ff, S.st_wmask[f]); vkCmdSetStencilReference(cb, ff, S.st_ref[f]);
    }
    // vertex streams: only the vertices this draw reads are copied (games bind whole vertex pools, tens of MB). Per-vertex streams are copied from vertex vlo on and the draw's
    // vertex numbers shifted down by vlo (a shader reading its vertex index sees the shifted value); per-instance streams from instance 0 up to the last one drawn
    uint32_t inst = dw ? (dw->instances ? (uint32_t)dw->instances : 1) : (di->instances ? (uint32_t)di->instances : 1);
    uint32_t isz = di ? (di->itype == 0 ? 1 : di->itype == 1 ? 2 : 4) : 0;
    const uint8_t *isrc = di ? (const uint8_t *)(uintptr_t)di->ibuf : NULL;
    #define IDX(i) (isz == 1 ? isrc[i] : isz == 2 ? ((const uint16_t *)isrc)[i] : ((const uint32_t *)isrc)[i])
    int64_t vlo = 0, vhi = -1;
    if (dw) { vlo = dw->first; vhi = (int64_t)dw->first + dw->count - 1; }
    else if (di->count > 0) {
        uint32_t lo = UINT32_MAX, hi = 0, restart = isz == 1 ? 0xff : isz == 2 ? 0xffff : 0xffffffffu;
        for (int32_t i = 0; i < di->count; i++) { uint32_t v = IDX(i); if (v == restart) continue; if (v < lo) lo = v; if (v > hi) hi = v; }
        if (lo <= hi) { vlo = (int64_t)lo + di->base_vertex; vhi = (int64_t)hi + di->base_vertex; }
    }
    if (vhi < vlo) { g_stat_draws++; return; }
    if (vlo < 0) vlo = 0;
    uint32_t used = 0, ext[16] = {0};
    for (int i = 0; i < S.nattr; i++) if (vs->vin_mask >> i & 1) { uint32_t st = S.attr[i].stream, e = S.attr[i].offset + 16; used |= 1u << st; if (e > ext[st]) ext[st] = e; }
    for (int s = 0; s < 16; s++) if (used >> s & 1) {
        VkDeviceSize o = 0; VkBuffer b = g_zero_buf;
        uint64_t stride = s < S.nstream ? S.stream[s].stride : 0, size = S.vbuf[s].size, from, to;
        int per_inst = s < S.nstream && S.stream[s].divisor;
        if (per_inst) { from = 0; to = ((uint64_t)(dw ? dw->base_instance : di->base_instance) + inst - 1) * stride + ext[s]; }
        else { from = (uint64_t)vlo * stride; to = (uint64_t)vhi * stride + ext[s]; }
        if (to > size) to = size;
        if (S.vbuf[s].address && to > from) { VkDeviceSize ro = copy_in(S.vbuf[s].address + from, to - from, 16); if (ro != VK_WHOLE_SIZE) { b = C->ring; o = ro; } }
        vkCmdBindVertexBuffers(cb, (uint32_t)s, 1, &b, &o);
    }
    if (dw) {
        if (prim == NVN_DRAW_PRIMITIVE_QUADS) {   // quads as two triangles each, through indices made here
            uint32_t nq = (uint32_t)dw->count / 4; if (!nq) return;
            VkDeviceSize o = ring_alloc((VkDeviceSize)nq * 24, 16); if (o == VK_WHOLE_SIZE) return;
            uint32_t *ix = (uint32_t *)(C->ringp + o);
            for (uint32_t q = 0; q < nq; q++) { uint32_t b = q * 4; uint32_t v[6] = {b, b + 1, b + 2, b, b + 2, b + 3}; memcpy(ix + 6 * q, v, 24); }
            vkCmdBindIndexBuffer(cb, C->ring, o, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cb, nq * 6, inst, 0, (int32_t)(dw->first - vlo), (uint32_t)dw->base_instance);
        } else vkCmdDraw(cb, (uint32_t)dw->count, inst, (uint32_t)(dw->first - vlo), (uint32_t)dw->base_instance);
    } else {
        int32_t voff = (int32_t)(di->base_vertex - vlo);
        if (prim == NVN_DRAW_PRIMITIVE_QUADS || (isz == 1 && !g_has_u8)) {   // quads, or 8-bit indices without the extension: widened to 32 bits on the CPU
            uint32_t n = (uint32_t)di->count, nq = prim == NVN_DRAW_PRIMITIVE_QUADS ? n / 4 : 0, out = nq ? nq * 6 : n;
            VkDeviceSize o = ring_alloc((VkDeviceSize)out * 4, 16); if (o == VK_WHOLE_SIZE) return;
            uint32_t *ix = (uint32_t *)(C->ringp + o);
            if (nq) for (uint32_t q = 0; q < nq; q++) { uint32_t b = 4 * q; uint32_t v[6] = {IDX(b), IDX(b + 1), IDX(b + 2), IDX(b), IDX(b + 2), IDX(b + 3)}; memcpy(ix + 6 * q, v, 24); }
            else for (uint32_t i = 0; i < n; i++) ix[i] = IDX(i);
            vkCmdBindIndexBuffer(cb, C->ring, o, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cb, out, inst, 0, voff, (uint32_t)di->base_instance);
        } else {
            VkDeviceSize o = copy_in(di->ibuf, (uint64_t)di->count * isz, 16); if (o == VK_WHOLE_SIZE) return;
            vkCmdBindIndexBuffer(cb, C->ring, o, isz == 1 ? VK_INDEX_TYPE_UINT8_EXT : isz == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cb, (uint32_t)di->count, inst, 0, voff, (uint32_t)di->base_instance);
        }
    }
    #undef IDX
    g_stat_draws++;
}
static void clear_color(const p_clear_color *c) {
    if (c->index < 0 || c->index >= P.n || !P.cv[c->index]) return;
    if ((c->mask & 15) != 15) RED_ONCE(40, "a color clear with a partial channel mask clears every channel");
    if (!P.open) { P.clr_c[c->index] = 1; memcpy(&P.cc[c->index], c->rgba, 16); return; }   // before the pass's first draw: its load action
    VkClearAttachment a = {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)c->index, {.color = {{c->rgba[0], c->rgba[1], c->rgba[2], c->rgba[3]}}}};
    VkClearRect r = {{{0, 0}, {P.w, P.h}}, 0, 1};
    vkCmdClearAttachments(C->cb, 1, &a, 1, &r);
}
static void clear_ds(const p_clear_ds *c) {
    if (!P.dt) return;
    if (!P.open) { if (c->depth_mask) { P.clr_d = 1; P.cd = c->depth; } if (P.dt->f.stencil && c->stencil_mask) { P.clr_s = 1; P.cs = (uint32_t)c->stencil; } return; }
    if (!pass_begin()) return;
    VkImageAspectFlags asp = (c->depth_mask ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (P.dt->f.stencil && c->stencil_mask ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    if (!asp) return;
    VkClearAttachment a = {asp, 0, {.depthStencil = {c->depth, (uint32_t)c->stencil}}};
    VkClearRect r = {{{0, 0}, {P.w, P.h}}, 0, 1};
    vkCmdClearAttachments(C->cb, 1, &a, 1, &r);
}
static void copy_b2t(const p_copy_b2t *c) {
    vtex *v = tex_get(c->tex, 0); if (!v) return;
    if (v->f.dec) { RED_ONCE(50, "buffer-to-texture copy into a BC texture: not decoded yet"); return; }
    int bw = 1, bh = 1, bpb = nvn_fmt_bpb(c->tex->d.format, &bw, &bh); if (!bpb) return;
    const NVNcopyRegion *r = &c->r; uint32_t w = (uint32_t)r->width, h = (uint32_t)r->height, d = (uint32_t)(r->depth > 0 ? r->depth : 1);
    size_t row = S.copy_stride ? (size_t)S.copy_stride : (size_t)((w + bw - 1) / bw) * bpb, img = S.copy_img_stride ? (size_t)S.copy_img_stride : row * ((h + bh - 1) / bh);
    pass_end();
    VkDeviceSize o = copy_in(c->src, img * d, 16); if (o == VK_WHOLE_SIZE) return;
    uint32_t lv = c->has_view && c->view.set_levels ? c->view.base_level : 0, ly = c->has_view && c->view.set_layers ? c->view.min_layer : 0;
    int is3d = v->target == NVN_TEXTURE_TARGET_3D;
    VkBufferImageCopy bc = {o, (uint32_t)(row / bpb * bw), (uint32_t)(img / row * bh), {v->aspect & VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT, lv, is3d ? 0 : ly + (uint32_t)r->zoffset, is3d ? 1 : d},
                            {r->xoffset, r->yoffset, is3d ? r->zoffset : 0}, {w, h, is3d ? d : 1}};
    vkCmdCopyBufferToImage(C->cb, C->ring, v->img, VK_IMAGE_LAYOUT_GENERAL, 1, &bc);
    barrier_all(C->cb);
}
static void copy_t2t(const p_copy_t2t *c) {
    vtex *s = tex_get(c->src, 0), *d = tex_get(c->dst, 1); if (!s || !d) return;
    pass_end();
    uint32_t sl = c->has_sv && c->sv.set_levels ? c->sv.base_level : 0, dl = c->has_dv && c->dv.set_levels ? c->dv.base_level : 0;
    VkImageAspectFlags sa = s->aspect & VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT, da = d->aspect & VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
    VkImageBlit b = {{sa, sl, (uint32_t)c->sr.zoffset, 1}, {{c->sr.xoffset, c->sr.yoffset, 0}, {c->sr.xoffset + c->sr.width, c->sr.yoffset + c->sr.height, 1}},
                     {da, dl, (uint32_t)c->dr.zoffset, 1}, {{c->dr.xoffset, c->dr.yoffset, 0}, {c->dr.xoffset + c->dr.width, c->dr.yoffset + c->dr.height, 1}}};
    vkCmdBlitImage(C->cb, s->img, VK_IMAGE_LAYOUT_GENERAL, d->img, VK_IMAGE_LAYOUT_GENERAL, 1, &b, (c->flags & 1) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    barrier_all(C->cb);
}
static void copy_t2b(const p_copy_t2t *c) {   // src = the texture, dst field of the record = the destination buffer address (see nvn_obj.h)
    vtex *s = tex_get(c->src, 0); if (!s) return;
    int bw = 1, bh = 1, bpb = nvn_fmt_bpb(c->src->d.format, &bw, &bh); if (!bpb || s->f.dec) { RED_ONCE(51, "texture-to-buffer copy of format %#x not implemented", c->src->d.format); return; }
    pass_end();
    uint32_t w = (uint32_t)c->sr.width, h = (uint32_t)c->sr.height; size_t row = S.copy_stride ? (size_t)S.copy_stride : (size_t)((w + bw - 1) / bw) * bpb, n = row * ((h + bh - 1) / bh);
    VkDeviceSize o = ring_alloc(n, 16); if (o == VK_WHOLE_SIZE) return;
    uint32_t sl = c->has_sv && c->sv.set_levels ? c->sv.base_level : 0;
    VkBufferImageCopy bc = {o, (uint32_t)(row / bpb * bw), 0, {s->aspect & VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT, sl, (uint32_t)c->sr.zoffset, 1},
                            {c->sr.xoffset, c->sr.yoffset, 0}, {w, h, 1}};
    vkCmdCopyImageToBuffer(C->cb, s->img, VK_IMAGE_LAYOUT_GENERAL, C->ring, 1, &bc);
    barrier_all(C->cb);
    post_add((post_t){3, (void *)c->dst, 0, 0, 0, 0, C->ringp + o, n});
}
static void clear_tex(const p_clear_tex *c) {
    vtex *v = tex_get(c->tex, 1); if (!v) return;
    pass_end();
    uint32_t lv = c->has_view && c->view.set_levels ? c->view.base_level : 0;
    VkClearColorValue cc; memcpy(&cc, &c->c, 16);
    VkImageSubresourceRange rr = {VK_IMAGE_ASPECT_COLOR_BIT, lv, 1, 0, VK_REMAINING_ARRAY_LAYERS};
    if (v->f.depth) { VkClearDepthStencilValue dv = {c->c.f[0], 0}; rr.aspectMask = v->aspect; vkCmdClearDepthStencilImage(C->cb, v->img, VK_IMAGE_LAYOUT_GENERAL, &dv, 1, &rr); }
    else vkCmdClearColorImage(C->cb, v->img, VK_IMAGE_LAYOUT_GENERAL, &cc, 1, &rr);
    barrier_all(C->cb);
}

// ---------- Submission ----------
void mtl_sub_begin(void) {
    vk_init();
    g_ci = (g_ci + 1) % NCTX; ctx_t *c = &g_ctx[g_ci];
    uint64_t w0 = now_ns();
    pthread_mutex_lock(&g_mu); while (c->fence_val > g_done) pthread_cond_wait(&g_cv, &g_mu); pthread_mutex_unlock(&g_mu);
    atomic_fetch_add(&g_wait_ns, now_ns() - w0);
    C = c; C->used = 0; C->up_used = 0; g_ndc = 0; g_wgen++; C->ring = C->ring0; C->ringp = C->ringp0;
    VKCHECK(vkResetDescriptorPool(g_dev, C->dp, 0));
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    VKCHECK(vkBeginCommandBuffer(C->cb, &bi)); VKCHECK(vkBeginCommandBuffer(C->up, &bi));
    vkCmdResetQueryPool(C->up, C->qp, 0, 2); vkCmdWriteTimestamp(C->up, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, C->qp, 0);
}
void mtl_sub_index(const uint64_t *h, int n) { (void)h; (void)n; }
void mtl_exec_rest(const uint64_t *handles, int n) { (void)handles; (void)n; }
void mtl_sub_copy(uint64_t src, uint64_t dst, uint64_t n) { memmove((void *)dst, (const void *)src, n); g_wgen++; }
void mtl_sub_event(volatile uint32_t *val, uint32_t mode, uint32_t v) { post_add((post_t){1, (void *)val, mode, v}); }
void mtl_sub_counter(uint64_t addr, uint32_t type) { post_add((post_t){2, NULL, 0, 0, addr, type}); }
static void mtl_exec_(const uint8_t *p);
void mtl_exec(const uint8_t *p) { uint64_t t0 = now_ns(); mtl_exec_(p); atomic_fetch_add(&g_rec_ns, now_ns() - t0); }
static void mtl_exec_(const uint8_t *p) {
    for (;;) {
        op_hdr h; memcpy(&h, p, sizeof h);
        const uint8_t *a = p + 8;
        switch (h.op) {
            case OP_END: return;
            case OP_JUMP: { const uint8_t *t; memcpy(&t, a, 8); p = t; continue; }
            case OP_CALL: { const uint8_t *t; memcpy(&t, a, 8); mtl_exec_(t); break; }
            case OP_SIGNAL_EVENT: { volatile uint32_t *val; uint32_t m, v; memcpy(&val, a, 8); memcpy(&m, a + 8, 4); memcpy(&v, a + 12, 4); mtl_sub_event(val, m, v); break; }
            case OP_REPORT_COUNTER: { uint64_t t, ad; memcpy(&t, a, 8); memcpy(&ad, a + 8, 8); mtl_sub_counter(ad, (uint32_t)t); break; }
            case OP_FENCE_SYNC: { sync_t *s; memcpy(&s, a, 8); s->fence = g_fence + 1; s->fenced = 1; break; }
            case OP_CLEAR_BUF: { const p_clear_buf *cb = (const p_clear_buf *)a; for (uint64_t o = 0; o + 4 <= cb->size; o += 4) memcpy((uint8_t *)cb->addr + o, &cb->value, 4); g_wgen++; break; }
            case OP_COPY_B2B: { uint64_t s, d, n; memcpy(&s, a, 8); memcpy(&d, a + 8, 8); memcpy(&n, a + 16, 8); mtl_sub_copy(s, d, n); break; }
            case OP_UPDATE_UB: { uint64_t d, n; memcpy(&d, a, 8); memcpy(&n, a + 8, 8); memcpy((void *)d, a + 16, n); g_wgen++; break; }
            case OP_SET_RT: set_rt((const p_set_rt *)a); break;
            case OP_CLEAR_COLOR: clear_color((const p_clear_color *)a); break;
            case OP_CLEAR_DS: clear_ds((const p_clear_ds *)a); break;
            case OP_DRAW: { p_draw t; draw(draw_rec(a, h.size, &t), NULL); break; }
            case OP_DRAW_IDX: { p_draw_idx t; draw(NULL, draw_idx_rec(a, h.size, &t)); break; }
            case OP_DISPATCH: RED_ONCE(60, "compute dispatches are not implemented yet"); break;
            case OP_COPY_B2T: copy_b2t((const p_copy_b2t *)a); break;
            case OP_COPY_T2T: copy_t2t((const p_copy_t2t *)a); break;
            case OP_COPY_T2B: copy_t2b((const p_copy_t2t *)a); break;
            case OP_CLEAR_TEX: clear_tex((const p_clear_tex *)a); break;
            case OP_BARRIER: pass_end(); break;
            case OP_SCISSOR: S.sc = *(const p_scissor *)a; S.has_sc = 1; break;
            case OP_BIND_PROGRAM: { const p_bind_program *b = (const p_bind_program *)a; if (b->stages & 0x20) S.cprog = b->prog; for (int st = 0; st < 5; st++) if (b->stages >> st & 1) S.bst[st] = b->prog; break; }
            case OP_BIND_STATE: {
                const p_bind_state *b = (const p_bind_state *)a; blend_st bs;
                switch (b->kind) {
                    case 0: memcpy(&bs, b->bytes, sizeof bs); S.blend[bs.target] = bs; break;
                    case 1: memcpy(&S.color, b->bytes, sizeof S.color); break; case 2: memcpy(&S.chmask, b->bytes, sizeof S.chmask); break;
                    case 3: memcpy(&S.ms, b->bytes, sizeof S.ms); break; case 4: memcpy(&S.poly, b->bytes, sizeof S.poly); break; case 5: memcpy(&S.ds, b->bytes, sizeof S.ds); break;
                }
                break;
            }
            case OP_BIND_VATTR: { const p_bind_vattr *b = (const p_bind_vattr *)a; memcpy(S.attr, b->st, 4 * b->n); S.nattr = b->n; break; }
            case OP_BIND_VSTREAM: { const p_bind_vstream *b = (const p_bind_vstream *)a; memcpy(S.stream, b->st, 8 * b->n); S.nstream = b->n; break; }
            case OP_BIND_VBUF: { const p_bind_vbuf *b = (const p_bind_vbuf *)a; memcpy(&S.vbuf[b->first], b->r, sizeof *b->r * b->n); break; }
            case OP_BIND_UBO: { const p_bind_buf *b = (const p_bind_buf *)a; if (b->stage < 6 && b->index < 16) S.ubo[b->stage][b->index] = *b; break; }
            case OP_BIND_SSBO: { const p_bind_buf *b = (const p_bind_buf *)a; if (b->stage < 6 && b->index < 16) S.ssbo[b->stage][b->index] = *b; break; }
            case OP_BIND_TEX: case OP_BIND_SAMP: case OP_BIND_IMG: {
                const p_bind_handles *b = (const p_bind_handles *)a; uint64_t hh[32]; memcpy(hh, b->h, 8 * (b->n < 32 ? b->n : 32));
                if (b->defer && b->src) memcpy(hh, (const void *)(uintptr_t)b->src, 8 * (b->n < 32 ? b->n : 32));   // BindTexturesDeferred: the handles as they are now
                uint64_t *dst = h.op == OP_BIND_TEX ? S.tex[b->stage] : h.op == OP_BIND_SAMP ? S.samp[b->stage] : S.img[b->stage];
                int lim = h.op == OP_BIND_TEX ? NTEX : h.op == OP_BIND_SAMP ? 128 : 32;
                if (b->stage < 6 && b->first >= 0 && b->first + b->n <= lim) memcpy(dst + b->first, hh, 8 * b->n);
                break;
            }
            case OP_STENCIL: { const p_stencil *b = (const p_stencil *)a; for (int f = 0; f < 2; f++) if (b->face & (1 << f)) { if (b->which == 0) S.st_vmask[f] = b->value_mask; else if (b->which == 1) S.st_wmask[f] = b->mask; else S.st_ref[f] = b->ref; } break; }
            case OP_POLY_OFFSET: S.po = *(const p_poly_offset *)a; break;
            case OP_VIEWPORTS: { const p_viewports *b = (const p_viewports *)a; for (int i = 0; i < b->n; i++) memcpy(S.vp[b->first + i], b->v[i], 16); S.has_vp = 1; break; }
            case OP_DEPTH_RANGE: { const p_depth_range *b = (const p_depth_range *)a; S.dr[0] = b->n; S.dr[1] = b->f; break; }
            case OP_BLEND_COLOR: memcpy(S.blend_color, ((const p_blend_color *)a)->rgba, 16); break;
            case OP_COPY_STRIDE: { const p_copy_stride *cs = (const p_copy_stride *)a; if (cs->image) S.copy_img_stride = cs->stride; else S.copy_stride = cs->stride; break; }
            default: break;
        }
        p += h.size;
    }
}
uint64_t mtl_sub_commit(void) {
    pass_end();
    vkCmdWriteTimestamp(C->cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, C->qp, 1);
    VKCHECK(vkEndCommandBuffer(C->cb)); VKCHECK(vkEndCommandBuffer(C->up));
    uint64_t fence = ++g_fence; C->fence_val = fence;
    VkCommandBuffer cbs[2] = {C->up, C->cb};
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 2, cbs, 0, NULL};
    VKCHECK(vkResetFences(g_dev, 1, &C->fence));
    pthread_mutex_lock(&g_qmu); VkResult r = vkQueueSubmit(g_q, 1, &si, C->fence); pthread_mutex_unlock(&g_qmu);
    if (r != VK_SUCCESS) die("Vulkan: submission failed (%d)", (int)r);
    pthread_mutex_lock(&g_mu); g_pend[g_npend++] = g_ci; pthread_cond_broadcast(&g_cv); pthread_mutex_unlock(&g_mu);
    return fence;
}
uint64_t mtl_last_fence(void) { return g_fence; }
int mtl_wait(uint64_t fence, uint64_t timeout_ns) {
    pthread_mutex_lock(&g_mu);
    if (timeout_ns == UINT64_MAX || timeout_ns > 60ull * 1000000000ull) { while (g_done < fence) pthread_cond_wait(&g_cv, &g_mu); }
    else {
        struct timespec t; clock_gettime(CLOCK_REALTIME, &t); uint64_t ns = (uint64_t)t.tv_nsec + timeout_ns; t.tv_sec += (time_t)(ns / 1000000000ull); t.tv_nsec = (long)(ns % 1000000000ull);
        while (g_done < fence) if (pthread_cond_timedwait(&g_cv, &g_mu, &t)) break;
    }
    int ok = g_done >= fence; pthread_mutex_unlock(&g_mu); return ok;
}
void mtl_finish(void) { mtl_wait(g_fence, UINT64_MAX); }
void mtl_drain(void) { mtl_finish(); }

// ---------- Presentation: counted; DM_VK_SHOTS writes chosen frames as BMP files ----------
static _Atomic uint64_t g_presents;
static void shot(const tex_t *t, uint64_t n) {
    static char *list; static const char *dir; static int init;
    if (!init) { init = 1; const char *e = getenv("DM_VK_SHOTS"); list = e ? strdup(e) : NULL; dir = getenv("DM_VK_SHOT_DIR"); if (!dir) dir = "."; }
    if (!list) return;
    char key[24]; snprintf(key, sizeof key, ",%llu,", (unsigned long long)n); char hay[1024]; snprintf(hay, sizeof hay, ",%s,", list);
    if (!strstr(hay, key)) return;
    mtl_finish();
    pthread_mutex_lock(&g_rmu); vtex *v = tex_find(t->serial); pthread_mutex_unlock(&g_rmu);
    if (!v || (v->f.f != VK_FORMAT_R8G8B8A8_UNORM && v->f.f != VK_FORMAT_R8G8B8A8_SRGB && v->f.f != VK_FORMAT_B8G8R8A8_UNORM)) { LOG("[RED] vk: frame %llu: the presented texture is not an 8-bit RGBA image (%s)", (unsigned long long)n, v ? "format" : "never drawn"); return; }
    uint32_t w = v->w, h = v->h; VkDeviceMemory m; void *px;
    VkBuffer b = buf_new((VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &m, &px);
    VkCommandBuffer cb = once_begin(); barrier_all(cb);
    VkBufferImageCopy c = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
    vkCmdCopyImageToBuffer(cb, v->img, VK_IMAGE_LAYOUT_GENERAL, b, 1, &c); once_end(cb);
    char path[1024]; snprintf(path, sizeof path, "%s/frame%05llu.bmp", dir, (unsigned long long)n);
    FILE *f = fopen(path, "wb");
    if (f) {   // 32-bit BMP, bottom-up rows, BGRA
        uint32_t sz = 54 + w * h * 4; uint8_t hd[54] = {'B', 'M'}; memcpy(hd + 2, &sz, 4); uint32_t o = 54, hs = 40; memcpy(hd + 10, &o, 4); memcpy(hd + 14, &hs, 4);
        memcpy(hd + 18, &w, 4); memcpy(hd + 22, &h, 4); hd[26] = 1; hd[28] = 32;
        fwrite(hd, 1, 54, f);
        uint8_t *row = malloc((size_t)w * 4);
        for (uint32_t y = h; y-- > 0;) {
            const uint8_t *s = (const uint8_t *)px + (size_t)y * w * 4;
            for (uint32_t x = 0; x < w; x++) { int bgr = v->f.f == VK_FORMAT_B8G8R8A8_UNORM; row[4 * x] = s[4 * x + (bgr ? 0 : 2)]; row[4 * x + 1] = s[4 * x + 1]; row[4 * x + 2] = s[4 * x + (bgr ? 2 : 0)]; row[4 * x + 3] = 255; }
            fwrite(row, 1, (size_t)w * 4, f);
        }
        free(row); fclose(f); LOG("vk: frame %llu written to %s (%ux%u; %llu draws so far, %llu dropped)", (unsigned long long)n, path, w, h, (unsigned long long)g_stat_draws, (unsigned long long)g_stat_skip);
    }
    vkDestroyBuffer(g_dev, b, NULL); vkFreeMemory(g_dev, m, NULL);
}
// ---------- Window ----------
__attribute__((weak)) void *front_native_window(int *gen) { (void)gen; return NULL; }   // app/front_android.c; none in the adb front end
__attribute__((weak)) void front_window_lock(int on) { (void)on; }
static struct { VkSurfaceKHR surf; VkSwapchainKHR sc; VkImage img[8]; VkImageView view[8]; uint32_t n; VkFormat fmt; VkExtent2D ext; int gen; } W;
#define NPRES 3
static struct { VkCommandPool pool; VkCommandBuffer cb; VkFence fence; VkSemaphore acq, done; VkFramebuffer fb; } g_pres[NPRES]; static int g_pi;
static void win_drop(void) {
    if (W.sc) { vkDeviceWaitIdle(g_dev); for (uint32_t i = 0; i < W.n; i++) vkDestroyImageView(g_dev, W.view[i], NULL); vkDestroySwapchainKHR(g_dev, W.sc, NULL); W.sc = VK_NULL_HANDLE; }
    if (W.surf) { vkDestroySurfaceKHR(g_inst, W.surf, NULL); W.surf = VK_NULL_HANDLE; }
}
static int win_make(ANativeWindow *aw) {
    VkAndroidSurfaceCreateInfoKHR si = {VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR, NULL, 0, aw};
    if (vkCreateAndroidSurfaceKHR(g_inst, &si, NULL, &W.surf) != VK_SUCCESS) { RED_ONCE(90, "no Vulkan surface for the window"); return 0; }
    VkBool32 ok = 0; vkGetPhysicalDeviceSurfaceSupportKHR(g_pd, g_qf, W.surf, &ok); if (!ok) { RED_ONCE(91, "the queue cannot present to the window"); return 0; }
    VkSurfaceCapabilitiesKHR cap; vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_pd, W.surf, &cap);
    uint32_t nf = 0; vkGetPhysicalDeviceSurfaceFormatsKHR(g_pd, W.surf, &nf, NULL); VkSurfaceFormatKHR sf[32]; if (nf > 32) nf = 32; vkGetPhysicalDeviceSurfaceFormatsKHR(g_pd, W.surf, &nf, sf);
    VkSurfaceFormatKHR pick = sf[0]; for (uint32_t i = 0; i < nf; i++) if (sf[i].format == VK_FORMAT_R8G8B8A8_UNORM || sf[i].format == VK_FORMAT_B8G8R8A8_UNORM) { pick = sf[i]; break; }
    uint32_t npm = 0; vkGetPhysicalDeviceSurfacePresentModesKHR(g_pd, W.surf, &npm, NULL); VkPresentModeKHR pm[8]; if (npm > 8) npm = 8; vkGetPhysicalDeviceSurfacePresentModesKHR(g_pd, W.surf, &npm, pm);
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR; for (uint32_t i = 0; i < npm; i++) if (pm[i] == VK_PRESENT_MODE_MAILBOX_KHR) mode = pm[i];   // the host paces presents itself; mailbox never blocks it
    // The swapchain is made the window's way up and Android's compositor turns it (a phone whose screen is portrait by nature, playing in landscape, reports its
    // current extent and transform rotated); the window's own size is that way up
    VkSurfaceTransformFlagBitsKHR pre = cap.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : cap.currentTransform;
    W.ext = (VkExtent2D){(uint32_t)ANativeWindow_getWidth(aw), (uint32_t)ANativeWindow_getHeight(aw)};
    if (pre == cap.currentTransform && cap.currentExtent.width != UINT32_MAX) W.ext = cap.currentExtent;
    if (W.ext.width < cap.minImageExtent.width) W.ext.width = cap.minImageExtent.width; if (W.ext.width > cap.maxImageExtent.width) W.ext.width = cap.maxImageExtent.width;
    if (W.ext.height < cap.minImageExtent.height) W.ext.height = cap.minImageExtent.height; if (W.ext.height > cap.maxImageExtent.height) W.ext.height = cap.maxImageExtent.height;
    uint32_t nimg = cap.minImageCount + 1; if (cap.maxImageCount && nimg > cap.maxImageCount) nimg = cap.maxImageCount;
    VkCompositeAlphaFlagBitsKHR ca = cap.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, NULL, 0, W.surf, nimg, pick.format, pick.colorSpace, W.ext, 1,
                                   VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, pre, ca, mode, VK_TRUE, VK_NULL_HANDLE};
    if (vkCreateSwapchainKHR(g_dev, &ci, NULL, &W.sc) != VK_SUCCESS) { RED_ONCE(92, "no swapchain for the window"); return 0; }
    W.fmt = pick.format; W.n = 8; vkGetSwapchainImagesKHR(g_dev, W.sc, &W.n, W.img);
    for (uint32_t i = 0; i < W.n; i++) { VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, W.img[i], VK_IMAGE_VIEW_TYPE_2D, W.fmt, {0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
                                         VKCHECK(vkCreateImageView(g_dev, &vi, NULL, &W.view[i])); }
    if (!g_pres[0].pool) for (int i = 0; i < NPRES; i++) {
        VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, g_qf}; VKCHECK(vkCreateCommandPool(g_dev, &pi, NULL, &g_pres[i].pool));
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, g_pres[i].pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1}; VKCHECK(vkAllocateCommandBuffers(g_dev, &ai, &g_pres[i].cb));
        VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, VK_FENCE_CREATE_SIGNALED_BIT}; VKCHECK(vkCreateFence(g_dev, &fi, NULL, &g_pres[i].fence));
        VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; VKCHECK(vkCreateSemaphore(g_dev, &sci, NULL, &g_pres[i].acq)); VKCHECK(vkCreateSemaphore(g_dev, &sci, NULL, &g_pres[i].done));
    }
    LOG("vk: swapchain %ux%u, %u images, format %d, %s", W.ext.width, W.ext.height, W.n, W.fmt, mode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "fifo");
    return 1;
}
// Over the picture: the frame-rate counter and the on-screen controls (front_overlay), drawn as cleared rectangles -- a 3x5 font, button outlines, a filled button while pressed
typedef struct { float x, y, w, h; int on; char label[3]; } dm_ovl;
__attribute__((weak)) int front_overlay(dm_ovl *out, int max) { (void)out; (void)max; return 0; }
static double g_fps_shown;
static uint16_t glyph(char c) {   // rows top to bottom, 3 bits each
    static const char *const G[] = {"0111101101101111", "1010010010010010", "2111001111100111", "3111001111001111", "4101101111001001", "5111100111001111", "6111100111101111",
                                    "7111001001001001", "8111101111101111", "9111101111001111", "A010101111101101", "B110101110101110", "X101101010101101", "Y101101010010010",
                                    "L100100100100111", "R110101110101101", "Z111001010100111", "+000010111010000", "-000000111000000"};
    for (size_t i = 0; i < sizeof G / sizeof *G; i++) if (G[i][0] == c) { uint16_t g = 0; for (int k = 1; k <= 15; k++) g = (uint16_t)(g << 1 | (G[i][k] == '1')); return g; }
    return 0;
}
static uint32_t text_rects(VkClearRect *r, uint32_t nr, uint32_t max, const char *t, int32_t x0, int32_t y0, uint32_t cell) {
    for (size_t d = 0; t[d] && nr + 15 <= max; d++) { uint16_t g = glyph(t[d]);
        for (int row = 0; row < 5; row++) for (int c = 0; c < 3; c++) if (g >> (14 - 3 * row - c) & 1)
            r[nr++] = (VkClearRect){{{x0 + (int32_t)((d * 4 + c) * cell), y0 + (int32_t)(row * cell)}, {cell, cell}}, 0, 1}; }
    return nr;
}
static VkFramebuffer overlay_draw(VkCommandBuffer cb, uint32_t img, int show_fps, const dm_ovl *ov, int nov) {
    rp_desc d = {.n = 1, .cv = {W.view[img]}, .cf = {W.fmt}, .cl = {VK_ATTACHMENT_LOAD_OP_LOAD}, .w = W.ext.width, .h = W.ext.height, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkFramebuffer fb = rp_begin(cb, &d);
    enum { MAXR = 1024 }; static VkClearRect lines[MAXR], fills[MAXR], fg[MAXR], bg[64]; uint32_t nl = 0, nf = 0, ng = 0, nb = 0;
    #define CLIP(x0, y0, w_, h_) do { int32_t X = (int32_t)(x0), Y = (int32_t)(y0), XW = X + (int32_t)(w_), YH = Y + (int32_t)(h_); if (X < 0) X = 0; if (Y < 0) Y = 0; \
        if (XW > (int32_t)W.ext.width) XW = (int32_t)W.ext.width; if (YH > (int32_t)W.ext.height) YH = (int32_t)W.ext.height; \
        if (XW > X && YH > Y) rect = (VkClearRect){{{X, Y}, {(uint32_t)(XW - X), (uint32_t)(YH - Y)}}, 0, 1}; else rect.rect.extent.width = 0; } while (0)
    VkClearRect rect;
    if (show_fps) {
        char t[8]; snprintf(t, sizeof t, "%.0f", g_fps_shown); uint32_t cell = W.ext.height / 120 ? W.ext.height / 120 : 2;
        CLIP(cell * 2, cell * 2, (strlen(t) * 4 + 1) * cell, 7 * cell); if (rect.rect.extent.width) bg[nb++] = rect;
        ng = text_rects(fg, ng, MAXR, t, (int32_t)(cell * 3), (int32_t)(cell * 3), cell);
    }
    for (int i = 0; i < nov && nl + 4 <= MAXR; i++) {
        const dm_ovl *o = &ov[i]; float t = o->w / 18 > 2 ? o->w / 18 : 2;
        if (o->on) { CLIP(o->x, o->y, o->w, o->h); if (rect.rect.extent.width && nf < MAXR) fills[nf++] = rect; }
        CLIP(o->x, o->y, o->w, t); if (rect.rect.extent.width) lines[nl++] = rect;
        CLIP(o->x, o->y + o->h - t, o->w, t); if (rect.rect.extent.width) lines[nl++] = rect;
        CLIP(o->x, o->y, t, o->h); if (rect.rect.extent.width) lines[nl++] = rect;
        CLIP(o->x + o->w - t, o->y, t, o->h); if (rect.rect.extent.width) lines[nl++] = rect;
        if (o->label[0]) { uint32_t cell = (uint32_t)(o->h / 9) ? (uint32_t)(o->h / 9) : 1, tw = (uint32_t)strlen(o->label) * 4 * cell - cell;
                           ng = text_rects(fg, ng, MAXR, o->label, (int32_t)(o->x + (o->w - tw) / 2), (int32_t)(o->y + (o->h - 5 * cell) / 2), cell); }
    }
    #undef CLIP
    VkClearAttachment black = {VK_IMAGE_ASPECT_COLOR_BIT, 0, {.color = {{0, 0, 0, 1}}}}, white = {VK_IMAGE_ASPECT_COLOR_BIT, 0, {.color = {{0.85f, 0.85f, 0.9f, 1}}}},
                      grey = {VK_IMAGE_ASPECT_COLOR_BIT, 0, {.color = {{0.35f, 0.35f, 0.45f, 1}}}}, green = {VK_IMAGE_ASPECT_COLOR_BIT, 0, {.color = {{0.3f, 1, 0.3f, 1}}}};
    if (nb) vkCmdClearAttachments(cb, 1, &black, nb, bg);
    if (nf) vkCmdClearAttachments(cb, 1, &grey, nf, fills);
    if (nl) vkCmdClearAttachments(cb, 1, &white, nl, lines);
    if (ng) vkCmdClearAttachments(cb, 1, nov ? &white : &green, ng, fg);
    rp_end(cb);
    return fb;
}
static void img_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, from, to,
                              VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}
static void win_present(const tex_t *t, const int crop[4]) {
    int gen; ANativeWindow *aw = front_native_window(&gen);
    front_window_lock(1);
    aw = front_native_window(&gen);
    if (!aw || gen != W.gen) { win_drop(); W.gen = gen; if (aw) win_make(aw); }
    if (!aw || !W.sc) { front_window_lock(0); return; }
    pthread_mutex_lock(&g_rmu); vtex *v = tex_find(t->serial); pthread_mutex_unlock(&g_rmu);
    if (!v) { front_window_lock(0); return; }
    typeof(g_pres[0]) *P_ = &g_pres[g_pi]; g_pi = (g_pi + 1) % NPRES;
    { uint64_t w0 = now_ns(); vkWaitForFences(g_dev, 1, &P_->fence, VK_TRUE, UINT64_MAX); atomic_fetch_add(&g_pwait_ns, now_ns() - w0); }
    if (P_->fb) { vkDestroyFramebuffer(g_dev, P_->fb, NULL); P_->fb = VK_NULL_HANDLE; }
    uint32_t idx; VkResult r = vkAcquireNextImageKHR(g_dev, W.sc, 1000000000ull, P_->acq, VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { W.gen = -1; front_window_lock(0); return; }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { front_window_lock(0); return; }
    vkResetFences(g_dev, 1, &P_->fence);
    VkCommandBuffer cb = P_->cb; vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL}; vkBeginCommandBuffer(cb, &bi);
    barrier_all(cb);   // the game's submissions before this one wrote the image
    img_barrier(cb, W.img[idx], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue black = {{0, 0, 0, 1}}; VkImageSubresourceRange rr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, W.img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rr);
    int32_t sx = crop ? crop[0] : 0, sy = crop ? crop[1] : 0, sw = crop && crop[2] ? crop[2] : (int32_t)v->w, sh = crop && crop[3] ? crop[3] : (int32_t)v->h;
    static int stretch = -1; if (stretch < 0) { const char *e = getenv("DM_SCREEN_FIT"); stretch = e && !strcmp(e, "stretch"); }
    int32_t dw = (int32_t)W.ext.width, dh = (int32_t)W.ext.height, dx = 0, dy = 0;
    if (!stretch) { if ((int64_t)dw * sh > (int64_t)dh * sw) { dw = (int32_t)((int64_t)dh * sw / sh); dx = ((int32_t)W.ext.width - dw) / 2; } else { dh = (int32_t)((int64_t)dw * sh / sw); dy = ((int32_t)W.ext.height - dh) / 2; } }
    VkImageBlit b = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{sx, sy, 0}, {sx + sw, sy + sh, 1}}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{dx, dy, 0}, {dx + dw, dy + dh, 1}}};
    vkCmdBlitImage(cb, v->img, VK_IMAGE_LAYOUT_GENERAL, W.img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
    static int show = -1; if (show < 0) { const char *e = getenv("DM_SHOW_FPS"); show = e && atoi(e); }
    static dm_ovl ov[32]; int nov = front_overlay(ov, 32);
    if (show || nov) { img_barrier(cb, W.img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL); P_->fb = overlay_draw(cb, idx, show, ov, nov);
                img_barrier(cb, W.img[idx], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR); }
    else img_barrier(cb, W.img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    vkEndCommandBuffer(cb);
    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 1, &P_->acq, &ws, 1, &cb, 1, &P_->done};
    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, NULL, 1, &P_->done, 1, &W.sc, &idx, NULL};
    pthread_mutex_lock(&g_qmu); vkQueueSubmit(g_q, 1, &si, P_->fence); r = vkQueuePresentKHR(g_q, &pi); pthread_mutex_unlock(&g_qmu);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) W.gen = -1;
    front_window_lock(0);
}
uint64_t mtl_present(const tex_t *t, double period_s, const int crop[4]) {
    (void)period_s;
    uint64_t n = atomic_fetch_add(&g_presents, 1) + 1;
    {   // pacing for DM_FPS=30 (every frame two vsyncs) and the frame-rate counter's reading
        static uint64_t last, t0, n0; uint64_t now = now_ns();
        if (mtl_fps_target() == 30 && last && now - last < 33333333ull) { usleep((useconds_t)((33333333ull - (now - last)) / 1000)); now = now_ns(); }
        last = now; if (!t0) { t0 = now; n0 = n; } else if (now - t0 >= 1000000000ull) { g_fps_shown = (double)(n - n0) * 1e9 / (double)(now - t0); t0 = now; n0 = n; }
    }
    if (t && g_has_surface) win_present(t, crop);
    if (n == 1 || n % 600 == 0) LOG("vk: %llu frames presented, %llu draws, %llu dropped (no program %llu, shader %llu, no target %llu, indirect %llu, primitive %llu, pipeline %llu, descriptors %llu), %d pipelines, %d shaders",
                                    (unsigned long long)n, (unsigned long long)g_stat_draws, (unsigned long long)g_stat_skip, (unsigned long long)g_skip_why[0], (unsigned long long)g_skip_why[1], (unsigned long long)g_skip_why[2],
                                    (unsigned long long)g_skip_why[3], (unsigned long long)g_skip_why[4], (unsigned long long)g_skip_why[5], (unsigned long long)g_skip_why[6], g_npso, g_nsh);
    if (n % 600 == 0) { LOG("vk: per frame over the last 600: GPU %.2f ms, recording %.2f ms, waiting for the GPU %.2f ms, present waiting %.2f ms", atomic_exchange(&g_gpu_ns, 0) / 600e6,
                            atomic_exchange(&g_rec_ns, 0) / 600e6, atomic_exchange(&g_wait_ns, 0) / 600e6, atomic_exchange(&g_pwait_ns, 0) / 600e6); }
    if (t) shot(t, n);
    return n;
}
void mtl_present_wait(uint64_t ticket) { (void)ticket; }
double mtl_presented_time(uint64_t n) { (void)n; return 0; }
int mtl_has_window(void) { return 0; }
int mtl_display_hz(void) { return 0; }
int mtl_fps_target(void) { static int f = -1; if (f < 0) { const char *e = getenv("DM_FPS"); f = e && atoi(e) == 30 ? 30 : 60; } return f; }
int mtl_link_active(void) { return 0; }
int mtl_link_wait_tick(int vsyncs, uint64_t timeout_ns) { (void)vsyncs; (void)timeout_ns; return 0; }
uint32_t mtl_present_qos(void) { return 0; }
void mtl_ub_present(double iv_ms) { (void)iv_ms; }
void mtl_set_depth_zero(int on) { g_depth_zero = on; if (!on) RED_ONCE(70, "NVN's GL depth convention (near = -w) is not converted yet: depth is squeezed"); }
void mtl_set_origin_lower_left(int on) { g_origin_ll = on; }
void mtl_set_frame(uint64_t f) { (void)f; }
void mtl_xv_window_tex(uint64_t serial) { (void)serial; }
void mtl_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img) {
    (void)t; (void)level; (void)layer0; (void)row; RED_ONCE(80, "CPU texture reads return zeros");
    memset(dst, 0, img * (size_t)(r->depth > 0 ? r->depth : 1));
}

// Memory pools: nothing to do (draws copy what they read); mtl_addr answers for the NVN layer whether an address is in one
#define NPOOL 4096
static struct { const uint8_t *mem; uint64_t size; } g_pool[NPOOL];
static int g_npool;
static pthread_mutex_t g_pool_mu = PTHREAD_MUTEX_INITIALIZER;
void mtl_pool_add(const void *mem, uint64_t size) { pthread_mutex_lock(&g_pool_mu); if (g_npool < NPOOL) { g_pool[g_npool].mem = mem; g_pool[g_npool].size = size; g_npool++; } pthread_mutex_unlock(&g_pool_mu); }
void mtl_pool_del(const void *mem) { pthread_mutex_lock(&g_pool_mu); for (int i = 0; i < g_npool; i++) if (g_pool[i].mem == mem) { g_pool[i] = g_pool[--g_npool]; break; } pthread_mutex_unlock(&g_pool_mu); }
int mtl_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off) {
    int hit = 0; pthread_mutex_lock(&g_pool_mu);
    for (int i = 0; i < g_npool && !hit; i++) { uint64_t b = (uint64_t)(uintptr_t)g_pool[i].mem; if (addr >= b && addr + len <= b + g_pool[i].size) { if (buf) *buf = (void *)g_pool[i].mem; if (off) *off = addr - b; hit = 1; } }
    pthread_mutex_unlock(&g_pool_mu); return hit;
}
void mtl_alias_drop_pool(const void *pool) { (void)pool; }

// statistics the NVN layer prints
void mtl_stats(uint64_t *passes, uint64_t *draws) { *passes = 0; *draws = g_stat_draws; }
void mtl_vbuf_stats(uint64_t *resolve, uint64_t *reuse, uint64_t *set, uint64_t *skip) { *resolve = *reuse = *set = *skip = 0; }
void mtl_use_stats(uint64_t *call, uint64_t *skip) { *call = *skip = 0; }
uint64_t mtl_upload_ns_take(void) { return 0; }
uint64_t mtl_upload_gpu_ns_take(void) { return 0; }
uint64_t mtl_upload_bytes(void) { return 0; }
uint64_t mtl_udc_saved_bytes(void) { return 0; }
void mtl_ubp_why(uint64_t w[5]) { memset(w, 0, 5 * sizeof *w); }
void mtl_ubp_stats(uint64_t *a, uint64_t *b, uint64_t *c) { *a = *b = *c = 0; }
void mtl_ubd_stats(uint64_t *a, uint64_t *b) { *a = *b = 0; }
void mtl_res_stats(char *out, size_t n) { if (n) snprintf(out, n, "vulkan"); }
void mtl_pkg_stats(uint64_t *uploads, uint64_t *reuploads) { *uploads = *reuploads = 0; }
uint64_t mtl_pkg_miss(void) { return 0; }
void mtl_mem_stat(unsigned long long a, unsigned long long b) { (void)a; (void)b; }
void mtl_lc_stats(uint64_t *a, uint64_t *b) { *a = *b = 0; }
int mtl_gpu_span(uint64_t f, uint64_t *t0, uint64_t *t1) { (void)f; (void)t0; (void)t1; return 0; }
double mtl_gpu_ms(uint64_t f) { (void)f; return 0; }
uint64_t mtl_ds_omit(void) { return 0; }
uint64_t mtl_drawable_ns_take(void) { return 0; }
uint64_t mtl_cut_count(void) { return 0; }
uint64_t mtl_blit_splits(void) { return 0; }
void mtl_create_stats(uint64_t *lib, uint64_t *pso) { *lib = 0; *pso = (uint64_t)g_npso; }
void mtl_create_ns(uint64_t *lib_ns, uint64_t *pso_ns) { *lib_ns = *pso_ns = 0; }
void mtl_bw(uint64_t *load, uint64_t *store, uint64_t *full) { *load = *store = *full = 0; }
void mtl_barrier_stats(uint64_t *skip, uint64_t *full) { *skip = *full = 0; }
void mtl_args_stats(uint64_t *enc, uint64_t *reuse) { *enc = *reuse = 0; }
void mtl_alloc_stats(uint64_t *w, uint64_t *g, int *n) { *w = *g = 0; *n = 0; }

// secprobe: what secondary command buffers cost on this GPU, and what recording them on several threads gains (standalone, run from adb shell; no game content).
// The scene: D draws in P dynamic-rendering passes into one colour (RGBA8) and one depth (D32) attachment, pass 0 clearing, the others loading; a pass-end barrier between passes
// (as nvn_vk.c pass_end_). Every draw allocates and writes its own descriptor set (a uniform buffer + --tex combined image samplers), binds it, sets viewport and scissor, binds
// the vertex buffer, pushes a constant and draws a blended quad; the pipeline changes every 4 draws (two pipelines). Blending makes the image depend on draw order: each
// variant's image checksum must match prim1's.
// Variants: prim1  one thread, everything recorded inline into one primary
//           sec1   one thread (main), each pass (or chunk) into its own secondary, the primary executes them (pure secondary overhead)
//           w1     the same on one worker thread (hand-off cost)
//           secN   N worker threads (N = 2..8) take chunks in order from a shared counter; the main thread then assembles the primary
// Per frame: reset pools, record, assemble, submit, wait for the fence (one frame in flight, so GPU time is not hidden).
#define _GNU_SOURCE
#include <vulkan/vulkan.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>

#define CK(x) do { VkResult r_ = (x); if (r_) { fprintf(stderr, "FAIL %s = %d (line %d)\n", #x, (int)r_, __LINE__); exit(1); } } while (0)
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static double tcpu(void) { struct timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

static const uint32_t vert_spv[] =
#include "vert.inc"
;
static const uint32_t frag_spv[] =
#include "frag.inc"
;

// ---------- configuration ----------
enum { MAXT = 9, MAXTEX = 128, MAXP = 1024 };
static int D = 1000, NP = 40, NTEX = 8, CHUNK = 0, FR = 300, WARM = 30, W = 1384, H = 780, SPLIT = 1;
static double BIG = 0;
static int cpus[MAXT], ncpus;
static char VARS[256] = "prim1,sec1,sec2,sec3,sec4";

// ---------- Vulkan objects ----------
static VkInstance inst; static VkPhysicalDevice pd; static VkDevice dev; static VkQueue q; static uint32_t qf;
static VkPhysicalDeviceProperties props; static VkPhysicalDeviceMemoryProperties mprops;
static PFN_vkCmdBeginRendering pBegin; static PFN_vkCmdEndRendering pEnd;
static VkDescriptorSetLayout dsl; static VkPipelineLayout pl; static VkPipeline g_pipe[2];
static VkBuffer vb, ub; static VkDeviceSize ubstride;
static VkImage cimg, dimg, timg[2]; static VkImageView cview, dview, tview[2]; static VkSampler samp;
static const VkFormat CF = VK_FORMAT_R8G8B8A8_UNORM, DF = VK_FORMAT_D32_SFLOAT;
static VkQueryPool qp; static VkFence fence; static VkBuffer rbuf; static VkDeviceMemory rmem; static uint8_t *rmap;

typedef struct {
    VkCommandPool pool; VkDescriptorPool dp; VkCommandBuffer prim, *sec; int nsec_used;
    double busy, cpu, t_sets, t_cmds; int nchunks;
} tctx;
static tctx T[MAXT];

// passes and chunks (a chunk: consecutive draws of one pass, recorded into one secondary)
static int pfirst[MAXP], pn[MAXP], pch0[MAXP + 1];
typedef struct { int pass, first, n; VkCommandBuffer cb; } chunk_t;
static chunk_t *CH; static int NCH;

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < mprops.memoryTypeCount; i++) if ((bits >> i & 1) && (mprops.memoryTypes[i].propertyFlags & want) == want) return i;
    fprintf(stderr, "FAIL no memory type %#x for bits %#x\n", want, bits); exit(1);
}
static VkDeviceMemory alloc_for(VkMemoryRequirements r, VkMemoryPropertyFlags want) {
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, r.size, mem_type(r.memoryTypeBits, want)}; VkDeviceMemory m; CK(vkAllocateMemory(dev, &ai, NULL, &m)); return m;
}
static VkBuffer buf_new(VkDeviceSize n, VkBufferUsageFlags u, VkDeviceMemory *m, void **map) {
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, n, u, VK_SHARING_MODE_EXCLUSIVE, 0, NULL}; VkBuffer b; CK(vkCreateBuffer(dev, &bi, NULL, &b));
    VkMemoryRequirements r; vkGetBufferMemoryRequirements(dev, b, &r);
    *m = alloc_for(r, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT); CK(vkBindBufferMemory(dev, b, *m, 0));
    if (map) CK(vkMapMemory(dev, *m, 0, VK_WHOLE_SIZE, 0, map));
    return b;
}
static VkImage img_new(VkFormat f, uint32_t w, uint32_t h, VkImageUsageFlags u, VkImageAspectFlags asp, VkImageView *view) {
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, f, {w, h, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, u, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage im; CK(vkCreateImage(dev, &ii, NULL, &im));
    VkMemoryRequirements r; vkGetImageMemoryRequirements(dev, im, &r); VkDeviceMemory m = alloc_for(r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT); CK(vkBindImageMemory(dev, im, m, 0));
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, im, VK_IMAGE_VIEW_TYPE_2D, f, {0}, {asp, 0, 1, 0, 1}}; CK(vkCreateImageView(dev, &vi, NULL, view));
    return im;
}
static void img_barrier(VkCommandBuffer cb, VkImage im, VkImageAspectFlags asp, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags ss, VkAccessFlags sa, VkPipelineStageFlags ds, VkAccessFlags da) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, sa, da, from, to, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, im, {asp, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, ss, ds, 0, 0, NULL, 0, NULL, 1, &b);
}
static VkCommandBuffer once_begin(void) {
    VkCommandBuffer cb = T[0].prim; CK(vkResetCommandPool(dev, T[0].pool, 0));
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL}; CK(vkBeginCommandBuffer(cb, &bi)); return cb;
}
static void once_end(VkCommandBuffer cb) {
    CK(vkEndCommandBuffer(cb)); VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
    CK(vkResetFences(dev, 1, &fence)); CK(vkQueueSubmit(q, 1, &si, fence)); CK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
}
static VkShaderModule shmod(const uint32_t *code, size_t n) {
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, n, code}; VkShaderModule m; CK(vkCreateShaderModule(dev, &ci, NULL, &m)); return m;
}

// ---------- setup ----------
static void setup(void) {
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "secprobe", 1, NULL, 0, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ii = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL}; CK(vkCreateInstance(&ii, NULL, &inst));
    uint32_t n = 1; CK(vkEnumeratePhysicalDevices(inst, &n, &pd)); if (!n) { fprintf(stderr, "FAIL no Vulkan device\n"); exit(1); }
    vkGetPhysicalDeviceProperties(pd, &props); vkGetPhysicalDeviceMemoryProperties(pd, &mprops);
    if (props.apiVersion < VK_API_VERSION_1_3) { fprintf(stderr, "FAIL the device is Vulkan %u.%u: 1.3 needed (dynamic rendering)\n", VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion)); exit(1); }
    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL); VkQueueFamilyProperties qfp[16]; if (nq > 16) nq = 16; vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qfp);
    for (qf = 0; qf < nq && !(qfp[qf].queueFlags & VK_QUEUE_GRAPHICS_BIT); qf++) {}
    if (qf == nq) { fprintf(stderr, "FAIL no graphics queue\n"); exit(1); }
    if (!qfp[qf].timestampValidBits) fprintf(stderr, "note: the queue has no timestamps: gpu_ts reads 0\n");
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES}; f13.dynamicRendering = VK_TRUE;
    float pr = 1; VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qf, 1, &pr};
    VkDeviceCreateInfo di = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f13, 0, 1, &qi, 0, NULL, 0, NULL, NULL}; CK(vkCreateDevice(pd, &di, NULL, &dev));
    vkGetDeviceQueue(dev, qf, 0, &q);
    pBegin = (PFN_vkCmdBeginRendering)vkGetDeviceProcAddr(dev, "vkCmdBeginRendering"); pEnd = (PFN_vkCmdEndRendering)vkGetDeviceProcAddr(dev, "vkCmdEndRendering");
    if (!pBegin || !pEnd) { fprintf(stderr, "FAIL vkCmdBeginRendering not found\n"); exit(1); }
    VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0}; CK(vkCreateFence(dev, &fi, NULL, &fence));
    VkQueryPoolCreateInfo qpi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_TIMESTAMP, 2, 0}; CK(vkCreateQueryPool(dev, &qpi, NULL, &qp));

    // per-thread pools: one command pool and one descriptor pool each (both externally synchronized: never shared between threads)
    for (int t = 0; t < MAXT; t++) {
        VkCommandPoolCreateInfo ci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, qf}; CK(vkCreateCommandPool(dev, &ci, NULL, &T[t].pool));
        VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, (uint32_t)D}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (uint32_t)(D * NTEX)}};
        VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, (uint32_t)D, 2, ps}; CK(vkCreateDescriptorPool(dev, &dpi, NULL, &T[t].dp));
        T[t].sec = calloc((size_t)NCH, sizeof *T[t].sec);
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, T[t].pool, VK_COMMAND_BUFFER_LEVEL_SECONDARY, (uint32_t)NCH}; CK(vkAllocateCommandBuffers(dev, &ai, T[t].sec));
        if (t == 0) { VkCommandBufferAllocateInfo pa = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, T[0].pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1}; CK(vkAllocateCommandBuffers(dev, &pa, &T[0].prim)); }
    }

    // buffers: a quad, and one 32-byte uniform block per draw (placement on a grid, a colour with alpha 0.5)
    VkDeviceMemory m; float *vp;
    vb = buf_new(6 * 8, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &m, (void **)&vp);
    const float quad[12] = {-1, -1, 1, -1, 1, 1, -1, -1, 1, 1, -1, 1}; memcpy(vp, quad, sizeof quad);
    ubstride = props.limits.minUniformBufferOffsetAlignment > 256 ? props.limits.minUniformBufferOffsetAlignment : 256;
    uint8_t *up; ub = buf_new(ubstride * (VkDeviceSize)D, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &m, (void **)&up);
    for (int i = 0; i < D; i++) {
        float u[8] = {-1 + ((i % 32) + 0.5f) * 2 / 32.f, -1 + (((i / 32) % 32) + 0.5f) * 2 / 32.f, 0.125f, 0.125f,
                      (float)(i * 37 % 255) / 255.f, (float)(i * 91 % 255) / 255.f, (float)(i * 53 % 255) / 255.f, 0.5f};
        memcpy(up + ubstride * (VkDeviceSize)i, u, sizeof u);
    }
    rbuf = buf_new((VkDeviceSize)W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &rmem, (void **)&rmap);

    // images: the attachments and two 4x4 textures
    cimg = img_new(CF, W, H, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT, &cview);
    dimg = img_new(DF, W, H, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, &dview);
    for (int k = 0; k < 2; k++) timg[k] = img_new(VK_FORMAT_R8G8B8A8_UNORM, 4, 4, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT, &tview[k]);
    uint32_t *tp; VkDeviceMemory sm; VkBuffer stg = buf_new(2 * 64, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &sm, (void **)&tp);
    for (int i = 0; i < 16; i++) { tp[i] = 0xffffffffu; tp[16 + i] = (i & 1) ? 0xffc0c0c0u : 0xff808080u; }
    VkCommandBuffer cb = once_begin();
    for (int k = 0; k < 2; k++) {
        img_barrier(cb, timg[k], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy bc = {(VkDeviceSize)k * 64, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {4, 4, 1}};
        vkCmdCopyBufferToImage(cb, stg, timg[k], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
        img_barrier(cb, timg[k], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    }
    img_barrier(cb, cimg, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    img_barrier(cb, dimg, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    once_end(cb);
    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, NULL, 0, VK_FILTER_NEAREST, VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST,
                               VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 0, VK_FALSE, 1, VK_FALSE, VK_COMPARE_OP_ALWAYS, 0, 0, VK_BORDER_COLOR_INT_OPAQUE_BLACK, VK_FALSE};
    CK(vkCreateSampler(dev, &sci, NULL, &samp));

    // layout and the two pipelines (A: alpha blend, depth write; B: additive, no depth write)
    VkDescriptorSetLayoutBinding lb[2] = {{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL}, {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (uint32_t)NTEX, VK_SHADER_STAGE_FRAGMENT_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo dli = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 2, lb}; CK(vkCreateDescriptorSetLayout(dev, &dli, NULL, &dsl));
    VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT, 0, 4};
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsl, 1, &pcr}; CK(vkCreatePipelineLayout(dev, &pli, NULL, &pl));
    VkShaderModule vs = shmod(vert_spv, sizeof vert_spv), fs = shmod(frag_spv, sizeof frag_spv);
    VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL},
                                             {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", NULL}};
    VkVertexInputBindingDescription vbd = {0, 8, VK_VERTEX_INPUT_RATE_VERTEX}; VkVertexInputAttributeDescription vad = {0, 0, VK_FORMAT_R32G32_SFLOAT, 0};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0, 1, &vbd, 1, &vad};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE};
    VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_FALSE, VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, VK_FALSE, 0, 0, 0, 1};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT, VK_FALSE, 1, NULL, VK_FALSE, VK_FALSE};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 2, dyn};
    VkPipelineRenderingCreateInfo pri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, NULL, 0, 1, &CF, DF, VK_FORMAT_UNDEFINED};
    for (int k = 0; k < 2; k++) {
        VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, NULL, 0, VK_TRUE, k ? VK_FALSE : VK_TRUE, VK_COMPARE_OP_LESS_OR_EQUAL, VK_FALSE, VK_FALSE, {0}, {0}, 0, 1};
        VkPipelineColorBlendAttachmentState ba = k ? (VkPipelineColorBlendAttachmentState){VK_TRUE, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, 0xf}
                                                   : (VkPipelineColorBlendAttachmentState){VK_TRUE, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, 0xf};
        VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_LOGIC_OP_COPY, 1, &ba, {0, 0, 0, 0}};
        VkGraphicsPipelineCreateInfo gp = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &pri, 0, 2, st, &vi, &ia, NULL, &vps, &rs, &ms, &ds, &cbs, &dy, pl, VK_NULL_HANDLE, 0, VK_NULL_HANDLE, -1};
        CK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, NULL, &g_pipe[k]));
    }
}

// ---------- recording ----------
static void rec_draws(VkCommandBuffer cb, tctx *t, int first, int n, int timed) {
    int lastp = -1;
    VkViewport vp = {0, 0, (float)W, (float)H, 0, 1}; VkRect2D sc = {{0, 0}, {(uint32_t)W, (uint32_t)H}};
    VkDescriptorImageInfo ii[MAXTEX];
    for (int i = first; i < first + n; i++) {
        double t0 = timed ? now() : 0;
        VkDescriptorSet ds; VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, t->dp, 1, &dsl}; CK(vkAllocateDescriptorSets(dev, &ai, &ds));
        VkDescriptorBufferInfo bi = {ub, ubstride * (VkDeviceSize)i, 32};
        for (int e = 0; e < NTEX; e++) ii[e] = (VkDescriptorImageInfo){samp, tview[(i + e) & 1], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ds, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, NULL, &bi, NULL},
                                     {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ds, 1, 0, (uint32_t)NTEX, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, ii, NULL, NULL}};
        vkUpdateDescriptorSets(dev, 2, w, 0, NULL);
        double t1 = timed ? now() : 0;
        int p = (i / 4) & 1; if (p != lastp) { vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipe[p]); lastp = p; }
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &ds, 0, NULL);
        vkCmdSetViewport(cb, 0, 1, &vp); vkCmdSetScissor(cb, 0, 1, &sc);
        VkDeviceSize off = 0; vkCmdBindVertexBuffers(cb, 0, 1, &vb, &off);
        float z = 1.0f - (float)i / (float)(D + 1); vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_VERTEX_BIT, 0, 4, &z);
        vkCmdDraw(cb, 6, 1, 0, 0);
        if (timed) { double t2 = now(); t->t_sets += t1 - t0; t->t_cmds += t2 - t1; }
    }
}
static void begin_pass(VkCommandBuffer cb, int p, int secondary) {
    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, cview, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED,
                                    p ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, {.color = {.float32 = {0, 0, 0, 1}}}};
    VkRenderingAttachmentInfo da = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, dview, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED,
                                    p ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, {.depthStencil = {1, 0}}};
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, secondary ? VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT : 0, {{0, 0}, {(uint32_t)W, (uint32_t)H}}, 1, 0, 1, &ca, &da, NULL};
    pBegin(cb, &ri);
}
static void pass_barrier(VkCommandBuffer cb) {   // as nvn_vk.c pass_end_: what the pass wrote, for what comes after
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 1, &mb, 0, NULL, 0, NULL);
}
static void prim_begin(VkCommandBuffer cb) {
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL}; CK(vkBeginCommandBuffer(cb, &bi));
    vkCmdResetQueryPool(cb, qp, 0, 2); vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qp, 0);
}
static void prim_end(VkCommandBuffer cb) { vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 1); CK(vkEndCommandBuffer(cb)); }
static double t_sec_be[MAXT];
static void record_chunk(tctx *t, int k, int tid) {
    VkCommandBuffer cb = t->sec[t->nsec_used++];
    VkCommandBufferInheritanceRenderingInfo ir = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO, NULL, 0, 0, 1, &CF, DF, VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT};
    VkCommandBufferInheritanceInfo ih = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO, &ir, VK_NULL_HANDLE, 0, VK_NULL_HANDLE, VK_FALSE, 0, 0};
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT | VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT, &ih};
    double a = now(); CK(vkBeginCommandBuffer(cb, &bi)); double b = now();
    rec_draws(cb, t, CH[k].first, CH[k].n, 0);
    double c = now(); CK(vkEndCommandBuffer(cb)); t_sec_be[tid] += (b - a) + (now() - c);
    CH[k].cb = cb; t->nchunks++;
}
static void assemble(VkCommandBuffer cb) {   // the primary: passes in order, each executing its chunks' secondaries in order
    VkCommandBuffer arr[256];
    for (int p = 0; p < NP; p++) {
        if (p) pass_barrier(cb);
        begin_pass(cb, p, 1);
        int n = 0; for (int k = pch0[p]; k < pch0[p + 1]; k++) { arr[n++] = CH[k].cb; if (n == 256) { vkCmdExecuteCommands(cb, (uint32_t)n, arr); n = 0; } }
        if (n) vkCmdExecuteCommands(cb, (uint32_t)n, arr);
        pEnd(cb);
    }
}

// ---------- worker threads ----------
static _Atomic int g_gen, g_next, g_done, g_quit; static int g_seen0;   // g_seen0: the generation before the variant's first frame (a worker that starts late must not take that frame as seen)
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static void pin(int slot) {
    if (slot >= ncpus) return;
    cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpus[slot], &s);
    if (sched_setaffinity(0, sizeof s, &s)) fprintf(stderr, "note: pinning thread %d to cpu%d failed\n", slot, cpus[slot]);
}
static void *worker(void *arg) {
    int id = (int)(intptr_t)arg; tctx *t = &T[id]; pin(id);
    int seen = g_seen0;
    for (;;) {
        int g = atomic_load(&g_gen);
        for (int s = 0; s < 20000 && g == seen; s++) g = atomic_load(&g_gen);   // stay awake a moment (as nvn_vk's ubc thread), then sleep
        if (g == seen) { pthread_mutex_lock(&g_mu); while ((g = atomic_load(&g_gen)) == seen) pthread_cond_wait(&g_cv, &g_mu); pthread_mutex_unlock(&g_mu); }
        seen = g; if (atomic_load(&g_quit)) break;
        double t0 = now(), c0 = tcpu(); int k;
        while ((k = atomic_fetch_add(&g_next, 1)) < NCH) record_chunk(t, k, id);
        t->busy = now() - t0; t->cpu = tcpu() - c0;
        atomic_fetch_add(&g_done, 1);
    }
    return NULL;
}

// ---------- measurement ----------
typedef struct { double rec, asm_, reset, submit, wait, gpu, frame, thrmax, thrcpu, sec_be; } fstat;
static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static double pct(double *v, int n, double p) { qsort(v, (size_t)n, sizeof *v, cmpd); int i = (int)(p * (n - 1) + 0.5); return v[i]; }
static int read_int(const char *path) { FILE *f = fopen(path, "r"); if (!f) return -1; int v = -1; if (fscanf(f, "%d", &v) != 1) v = -1; fclose(f); return v; }
static void clocks(char *out, size_t n) {
    int o = 0;
    for (int c = 0; c < 8; c++) { char p[96]; snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", c); int k = read_int(p);
                                  o += snprintf(out + o, n - (size_t)o, " c%d=%d", c, k > 0 ? k / 1000 : -1); }
    int g = read_int("/sys/class/kgsl/kgsl-3d0/clock_mhz"); if (g <= 0) { int hz = read_int("/sys/class/kgsl/kgsl-3d0/gpuclk"); g = hz > 0 ? hz / 1000000 : -1; }
    snprintf(out + o, n - (size_t)o, " gpu=%d", g);
}
static uint32_t readback_crc(void) {
    VkCommandBuffer cb = once_begin();
    img_barrier(cb, cimg, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy bc = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {(uint32_t)W, (uint32_t)H, 1}};
    vkCmdCopyImageToBuffer(cb, cimg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbuf, 1, &bc);
    img_barrier(cb, cimg, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    VkMemoryBarrier hb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, NULL, 0, NULL);
    once_end(cb);
    uint32_t h = 2166136261u; for (size_t i = 0; i < (size_t)W * H * 4; i++) h = (h ^ rmap[i]) * 16777619u;
    return h;
}

typedef struct { char name[16]; int sec, nw; double rec50, rec90, asm50, tot50, sub50, wait50, gpu50, gpu90, frame50, thr50, cpu50, be50; uint32_t crc; int secs; } vres;
static vres R[16]; static int NR;
static double g_split_sets, g_split_cmds;

static void run_variant(const char *name) {
    vres *r = &R[NR]; memset(r, 0, sizeof *r); snprintf(r->name, sizeof r->name, "%s", name);
    if (!strcmp(name, "prim1")) { r->sec = 0; r->nw = 0; }
    else if (!strcmp(name, "sec1")) { r->sec = 1; r->nw = 0; }
    else if (!strcmp(name, "w1")) { r->sec = 1; r->nw = 1; }
    else if (!strncmp(name, "sec", 3) && atoi(name + 3) >= 2 && atoi(name + 3) <= MAXT - 1) { r->sec = 1; r->nw = atoi(name + 3); }
    else { fprintf(stderr, "unknown variant %s (prim1, sec1, w1, sec2..sec%d)\n", name, MAXT - 1); return; }
    NR++;
    pthread_t th[MAXT]; atomic_store(&g_quit, 0); g_seen0 = atomic_load(&g_gen);
    for (int i = 1; i <= r->nw; i++) pthread_create(&th[i], NULL, worker, (void *)(intptr_t)i);
    int nf = WARM + FR; fstat *fs = calloc((size_t)nf, sizeof *fs);
    char clk[256] = "";
    for (int f = 0; f < nf; f++) {
        fstat *s = &fs[f];
        double t0 = now();
        int nt = r->nw ? r->nw : 0;
        CK(vkResetCommandPool(dev, T[0].pool, 0)); CK(vkResetDescriptorPool(dev, T[0].dp, 0)); T[0].nsec_used = 0; T[0].nchunks = 0;
        for (int i = 1; i <= nt; i++) { CK(vkResetCommandPool(dev, T[i].pool, 0)); CK(vkResetDescriptorPool(dev, T[i].dp, 0)); T[i].nsec_used = 0; T[i].nchunks = 0; T[i].busy = T[i].cpu = 0; }
        memset(t_sec_be, 0, sizeof t_sec_be);
        double t1 = now(); s->reset = t1 - t0;
        VkCommandBuffer cb = T[0].prim;
        if (!r->sec) {   // prim1: everything inline
            double c0 = tcpu();
            prim_begin(cb);
            for (int p = 0; p < NP; p++) { if (p) pass_barrier(cb); begin_pass(cb, p, 0); rec_draws(cb, &T[0], pfirst[p], pn[p], 0); pEnd(cb); }
            prim_end(cb);
            s->rec = now() - t1; s->thrmax = s->rec; s->thrcpu = tcpu() - c0;
        } else if (!r->nw) {   // sec1: the main thread records every secondary, then the primary
            double c0 = tcpu();
            for (int k = 0; k < NCH; k++) record_chunk(&T[0], k, 0);
            double t2 = now(); s->rec = t2 - t1; s->thrmax = s->rec; s->thrcpu = tcpu() - c0; s->sec_be = t_sec_be[0];
            prim_begin(cb); assemble(cb); prim_end(cb); s->asm_ = now() - t2;
        } else {   // N workers take the chunks in order; the main thread waits, then assembles
            atomic_store(&g_next, 0); atomic_store(&g_done, 0);
            pthread_mutex_lock(&g_mu); atomic_fetch_add(&g_gen, 1); pthread_cond_broadcast(&g_cv); pthread_mutex_unlock(&g_mu);
            while (atomic_load(&g_done) < r->nw) {}
            double t2 = now(); s->rec = t2 - t1;
            for (int i = 1; i <= r->nw; i++) { if (T[i].busy > s->thrmax) s->thrmax = T[i].busy; if (T[i].cpu > s->thrcpu) s->thrcpu = T[i].cpu; s->sec_be += t_sec_be[i]; }
            prim_begin(cb); assemble(cb); prim_end(cb); s->asm_ = now() - t2;
        }
        double t3 = now();
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
        CK(vkResetFences(dev, 1, &fence)); CK(vkQueueSubmit(q, 1, &si, fence));
        double t4 = now(); s->submit = t4 - t3;
        CK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
        double t5 = now(); s->wait = t5 - t4; s->frame = t5 - t0;
        uint64_t ts[2] = {0, 0};
        if (vkGetQueryPoolResults(dev, qp, 0, 2, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS && ts[1] > ts[0])
            s->gpu = (double)(ts[1] - ts[0]) * props.limits.timestampPeriod / 1e6;
        if (f == WARM + FR / 2) clocks(clk, sizeof clk);
    }
    if (r->nw) { atomic_store(&g_quit, 1); pthread_mutex_lock(&g_mu); atomic_fetch_add(&g_gen, 1); pthread_cond_broadcast(&g_cv); pthread_mutex_unlock(&g_mu);
                 for (int i = 1; i <= r->nw; i++) pthread_join(th[i], NULL); }
    double *v = malloc(sizeof(double) * (size_t)FR);
    #define P50(field) (({ for (int i = 0; i < FR; i++) v[i] = fs[WARM + i].field; pct(v, FR, 0.5); }))
    #define P90(field) (({ for (int i = 0; i < FR; i++) v[i] = fs[WARM + i].field; pct(v, FR, 0.9); }))
    r->rec50 = P50(rec); r->rec90 = P90(rec); r->asm50 = P50(asm_); r->sub50 = P50(submit); r->wait50 = P50(wait); r->gpu50 = P50(gpu); r->gpu90 = P90(gpu);
    r->frame50 = P50(frame); r->thr50 = P50(thrmax); r->cpu50 = P50(thrcpu); r->be50 = P50(sec_be);
    for (int i = 0; i < FR; i++) v[i] = fs[WARM + i].rec + fs[WARM + i].asm_; r->tot50 = pct(v, FR, 0.5);
    #undef P50
    #undef P90
    r->secs = r->sec ? NCH : 0;
    r->crc = readback_crc();
    printf("variant %-5s thr %d secs %4d | rec p50 %6.2f p90 %6.2f | asm %5.2f | cpu_total %6.2f | submit %5.2f | gpu_ts p50 %6.2f p90 %6.2f | wait %6.2f | frame %6.2f | thr_busy %6.2f cpu %6.2f | sec_begin_end %5.2f | crc %08x\n",
           r->name, r->nw ? r->nw : 1, r->secs, r->rec50, r->rec90, r->asm50, r->tot50, r->sub50, r->gpu50, r->gpu90, r->wait50, r->frame50, r->thr50, r->cpu50, r->be50, r->crc);
    printf("clk   %-5s (MHz, mid-run)%s\n", r->name, clk);
    if (!r->sec && SPLIT) {   // where prim1's time goes: descriptor set allocate + write against command recording (timers around each draw: a little slower than the run above)
        T[0].t_sets = T[0].t_cmds = 0;
        int ns = 30;
        for (int f = 0; f < ns; f++) {
            CK(vkResetCommandPool(dev, T[0].pool, 0)); CK(vkResetDescriptorPool(dev, T[0].dp, 0));
            VkCommandBuffer cb = T[0].prim; prim_begin(cb);
            for (int p = 0; p < NP; p++) { if (p) pass_barrier(cb); begin_pass(cb, p, 0); rec_draws(cb, &T[0], pfirst[p], pn[p], 1); pEnd(cb); }
            prim_end(cb);
            VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
            CK(vkResetFences(dev, 1, &fence)); CK(vkQueueSubmit(q, 1, &si, fence)); CK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
        }
        g_split_sets = T[0].t_sets / ns; g_split_cmds = T[0].t_cmds / ns;
        printf("split prim1: sets %.2f ms (%.2f us/set, %d descriptors each) | cmds %.2f ms (%.2f us/draw)\n", g_split_sets, g_split_sets * 1e3 / D, 1 + NTEX, g_split_cmds, g_split_cmds * 1e3 / D);
    }
    free(v); free(fs);
    fflush(stdout);
}

static void build_passes(void) {
    if (NP < 1) NP = 1; if (NP > MAXP) NP = MAXP;
    int big = BIG > 0 && NP > 1 ? (int)(BIG * D) : 0, rest = D - big, np = big ? NP - 1 : NP, at = 0;
    for (int p = 0; p < NP; p++) {
        int n = big && p == 0 ? big : rest / np + ((big ? p - 1 : p) < rest % np);
        pfirst[p] = at; pn[p] = n; at += n;
    }
    NCH = 0; for (int p = 0; p < NP; p++) { int k = CHUNK > 0 && pn[p] > CHUNK ? (pn[p] + CHUNK - 1) / CHUNK : (pn[p] ? 1 : 0); NCH += k; }
    CH = calloc((size_t)NCH + 1, sizeof *CH);
    int c = 0;
    for (int p = 0; p < NP; p++) {
        pch0[p] = c;
        int k = CHUNK > 0 && pn[p] > CHUNK ? (pn[p] + CHUNK - 1) / CHUNK : (pn[p] ? 1 : 0);
        for (int j = 0, f = pfirst[p]; j < k; j++) { int n = pn[p] / k + (j < pn[p] % k); CH[c++] = (chunk_t){p, f, n, VK_NULL_HANDLE}; f += n; }
    }
    pch0[NP] = c;
}

static void usage(void) {
    printf("secprobe [--draws N] [--passes P] [--tex T] [--big F] [--chunk K] [--frames N] [--warm N] [--size WxH] [--cpus a,b,c] [--variants list] [--nosplit]\n"
           "  --draws 1000   draws a frame                      --passes 40  render passes a frame (draws spread evenly)\n"
           "  --tex 8        combined image samplers per set (1..%d; Unreal's stages have 60-80 descriptors)\n"
           "  --big 0.5      pass 0 takes this fraction of the draws (a base pass), the rest spread over the other passes\n"
           "  --chunk 64     a pass with more draws is split into secondaries of at most this many draws (0: one secondary a pass)\n"
           "  --cpus 7,4,3,2 pin the main thread to the first cpu, worker i to the (i+1)-th\n"
           "  --variants prim1,sec1,w1,sec2,sec3,sec4  (default prim1,sec1,sec2,sec3,sec4)\n", MAXTEX);
}
int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : "";
        if (!strcmp(a, "--draws")) { D = atoi(v); i++; } else if (!strcmp(a, "--passes")) { NP = atoi(v); i++; } else if (!strcmp(a, "--tex")) { NTEX = atoi(v); i++; }
        else if (!strcmp(a, "--big")) { BIG = atof(v); i++; } else if (!strcmp(a, "--chunk")) { CHUNK = atoi(v); i++; } else if (!strcmp(a, "--frames")) { FR = atoi(v); i++; }
        else if (!strcmp(a, "--warm")) { WARM = atoi(v); i++; } else if (!strcmp(a, "--size")) { sscanf(v, "%dx%d", &W, &H); i++; }
        else if (!strcmp(a, "--cpus")) { ncpus = 0; for (const char *s = v; *s && ncpus < MAXT; ) { cpus[ncpus++] = atoi(s); while (*s && *s != ',') s++; if (*s == ',') s++; } i++; }
        else if (!strcmp(a, "--variants")) { snprintf(VARS, sizeof VARS, "%s", v); i++; } else if (!strcmp(a, "--nosplit")) SPLIT = 0;
        else { usage(); return !strcmp(a, "--help") || !strcmp(a, "-h") ? 0 : 1; }
    }
    if (D < 1) D = 1; if (NTEX < 1) NTEX = 1; if (NTEX > MAXTEX) NTEX = MAXTEX; if (FR < 1) FR = 1; if (WARM < 0) WARM = 0; if (BIG < 0 || BIG >= 1) BIG = 0;
    build_passes();
    setup();
    pin(0);
    int maxp = 0; for (int p = 0; p < NP; p++) if (pn[p] > maxp) maxp = pn[p];
    printf("secprobe: %s | Vulkan %u.%u.%u | driver %#x | timestampPeriod %.2f ns\n", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion),
           props.driverVersion, props.limits.timestampPeriod);
    printf("cfg: draws %d passes %d (largest %d draws) secondaries %d tex %d chunk %d big %.2f frames %d warm %d size %dx%d cpus", D, NP, maxp, NCH, NTEX, CHUNK, BIG, FR, WARM, W, H);
    if (!ncpus) printf(" -"); for (int i = 0; i < ncpus; i++) printf("%s%d", i ? "," : " ", cpus[i]); printf("\n"); fflush(stdout);
    char list[256]; snprintf(list, sizeof list, "%s", VARS);
    for (char *s = strtok(list, ","); s; s = strtok(NULL, ",")) run_variant(s);
    // summary against prim1
    vres *p1 = NULL, *s1 = NULL; for (int i = 0; i < NR; i++) { if (!strcmp(R[i].name, "prim1")) p1 = &R[i]; if (!strcmp(R[i].name, "sec1")) s1 = &R[i]; }
    if (p1 && s1) {
        double dc = s1->tot50 - p1->tot50, dg = s1->gpu50 - p1->gpu50;
        printf("overhead sec1-prim1: cpu %+.2f ms (%+.1f%%), %.2f us per secondary (begin/end %.2f us) | gpu %+.2f ms (%+.1f%%)\n", dc, 100 * dc / p1->tot50, NCH ? dc * 1e3 / NCH : 0,
               NCH ? s1->be50 * 1e3 / NCH : 0, dg, p1->gpu50 > 0 ? 100 * dg / p1->gpu50 : 0);
    }
    if (p1) {
        printf("speedup cpu_total vs prim1:");
        for (int i = 0; i < NR; i++) if (&R[i] != p1) printf(" %s %.2fx", R[i].name, R[i].tot50 > 0 ? p1->tot50 / R[i].tot50 : 0);
        printf("\n");
        int same = 1; for (int i = 0; i < NR; i++) if (R[i].crc != p1->crc) { same = 0; printf("crc MISMATCH: %s %08x vs prim1 %08x (draw order or state differs)\n", R[i].name, R[i].crc, p1->crc); }
        if (same) printf("crc: all %d variants equal (%08x)\n", NR, p1->crc);
    }
    return 0;
}

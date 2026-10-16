#include "vkr.h"

#include "vkr_thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#include "xbox_vert_spv.h"
#include "xbox_frag_spv.h"
#include "xbox_frag_wdepth_spv.h"
#include "xbox_vsh_vert_spv.h"
#include "crt_vert_spv.h"
#include "crt_frag_spv.h"
#include "xbox_frag_a2c_spv.h"
#include "post_frag_spv.h"
#include "rt_soft_frag_spv.h"
#include "scene_ao_frag_spv.h"
#include "scene_ao_ms_frag_spv.h"
#include "scene_frag_spv.h"
#include "scene_ms_frag_spv.h"
#ifdef _WIN32
#include <windows.h>
#define strtok_r strtok_s
#else
#include <time.h>
#endif

static double vkr_now_ms(void)
{
#ifdef _WIN32
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1e3 / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
#endif
}

#define VKR_BUF_BYTES (32u << 20)       /* vertex+index staging per frame */
#define VKR_FRAMES 2                    /* frames in flight */

struct VkrTex {
    uint64_t key;
    uint32_t w, h, levels;          /* levels > 1: a mip chain (sampled with mips) */
    uint32_t last_use;              /* flip serial of the last lookup/update */
    VkImage img;
    VkDeviceMemory mem;
    VkImageView view;
};

/* A render target the title draws and then samples (the rider shadow maps:
 * swizzled 128x128 surfaces), on the GPU at rt_scale times its size
 * (RECOMP_VKR_GPU_RT). Keyed by guest address and size. */
typedef struct {
    uint32_t off, gw, gh;           /* guest surface */
    float zmax;
    uint32_t sw, sh;                /* image size */
    VkImage color, depth;
    VkDeviceMemory color_mem, depth_mem;
    VkImageView color_view, depth_view;
    VkrTex tex;                     /* the colour image as a texture (not in the cache) */
    int state;                      /* 0 never drawn, 1 attachment, 2 shader read */
    float sc[4];                    /* clip in guest pixels */
    /* RECOMP_SHADOW_SOFT: the softened copy (half the image size), redone
     * when the map was drawn since (dirty) and is sampled */
    VkImage soft;
    VkDeviceMemory soft_mem;
    VkImageView soft_view;
    VkrTex soft_tex;
    uint32_t ssw, ssh;
    int soft_state, dirty;
} VkrRT;
#define VKR_MAX_RT 16

/* What one frame in flight owns: recorded while the previous one renders. */
typedef struct {
    VkCommandBuffer cmd;
    VkFence fence;
    VkBuffer buf, rb;
    VkDeviceMemory buf_mem, rb_mem;
    uint8_t *buf_map, *rb_map;
    int rb_noncoherent;             /* cached but not coherent: invalidate before reading */
    VkBuffer rbh;                   /* the full-resolution image (scale != 1) */
    VkDeviceMemory rbh_mem;
    uint8_t *rbh_map;
    int rbh_noncoherent;
    int busy;                       /* submitted, read-back not released yet */
    /* RECOMP_VKR_GPUPROF: a timestamp after every draw of the frame */
    VkQueryPool qp;
    uint32_t nq;
    uint32_t qf[4];                 /* timestamps at the flip: resolve end, scene end, post end, 0 = none */
    int qreset;
    struct VkrProfDraw *meta;
} VkrFrame;

/* What a profiled draw was (RECOMP_VKR_GPUPROF). */
typedef struct VkrProfDraw {
    uint32_t nidx, topo, vsh, blend, src, dst, eq, mask, stages, final0, alpha;
    uint32_t tw[2], th[2], scw, sch;
    float psize;
} VkrProfDraw;
#define VKR_PROF_MAX 8192

struct VkrContext {
    VkInstance inst;
    VkPhysicalDevice phys;
    VkDevice dev;
    VkQueue queue;
    uint32_t qfam;
    VkCommandPool pool;
    VkrFrame fr[VKR_FRAMES];
    int cur;                        /* the frame being recorded */
    pthread_mutex_t lock;           /* fr[].busy */
    pthread_cond_t freed;
    uint32_t tex_gen;               /* bumped when every texture is freed */
    /* aliases of fr[cur] */
    VkCommandBuffer cmd;
    VkFence fence;
    VkPhysicalDeviceMemoryProperties mem;

    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkShaderModule vs, fs, fs_wdepth, vs_vsh;  /* fs_wdepth: W-buffered draws */
    VkShaderModule fs_a2c;                     /* RECOMP_A2C: alpha-tested draws under MSAA */

    struct PipeEntry { uint32_t key[8]; VkPipeline pipe; } pipes[200];
    int npipes;
    struct SamplerEntry { uint32_t key; VkSampler s; } samplers[32];
    int nsamplers;

    VkrTex *white;                  /* 1x1 fallback for unbound stages */
    VkrTex **texs;                  /* every texture, for lookup and teardown */
    uint32_t ntexs, captexs;
    uint64_t tex_bytes;
    /* Lookup by key: open addressing over indices into texs (-1 empty),
     * a power of two at least twice ntexs. The linear search it replaces
     * scanned every texture ever made, for every stage of every draw --
     * after ~6 minutes of play the cache held tens of thousands and the
     * race fell from 60 to ~23 FPS. */
    int32_t *hidx;
    uint32_t hcap;
    uint32_t flip_serial;

    /* Internal resolution (RECOMP_RES): the target is sw x sh = w x h times
     * scale; positions, the guest's copy and the clip rectangle stay in guest
     * pixels (w x h). */
    float scale, scale_x;           /* scale_x = scale x 4/3 in widescreen */
    float squeeze2d;                /* widescreen: 2D (w == 1) narrowed to 4:3 */
    float squeeze_all;              /* 1: pillarbox -- every vertex narrowed to 4:3 */
    uint32_t gsc[4];                /* the clip rectangle in guest pixels */
    uint32_t sw, sh;
    int wide_lines;
    float max_line;
    float aniso;                    /* max anisotropy, 0 = off (RECOMP_VKR_ANISO, default 16) */
    int mips;                       /* RECOMP_VKR_MIPS=0: textures without a mip chain */
    VkImage down;                   /* scale != 1: w x h copy for guest memory */
    VkDeviceMemory down_mem;
    VkImageView down_view;

    /* target */
    uint32_t w, h;
    float zmax;
    VkRect2D sc;                    /* current scissor (the NV2A clip rect) */
    VkImage color, depth;
    VkDeviceMemory color_mem, depth_mem;
    VkImageView color_view, depth_view;
    VkFormat depth_fmt;             /* D32_SFLOAT_S8_UINT (or D24S8; D32 = no stencil) */
    int has_stencil;
    VkImageView depth_sview;        /* depth only, for sampling (the scene pass) */
    int recording, in_pass;
    int color_valid;

    /* per-frame upload buffer (alias of fr[cur]) */
    VkBuffer buf;
    uint8_t *buf_map;
    uint32_t buf_used;
    uint32_t upload_serial;         /* bumped whenever buf_used restarts */

    /* vkr_draw_vsh: the constants and program last uploaded in this
     * recording, reused while unchanged. */
    uint32_t vsh_serial, vsh_const_off, vsh_prog_off, vsh_prog_hash, vsh_prog_n;
    /* RECOMP_VKR_GPUPROF=<ms> */
    double prof_thr, ts_period;
    unsigned prof_said;
    float vsh_consts[192][4];

    /* RECOMP_CRT=1: CRT post-process of the full-resolution image (crt.frag).
     * crt_in: the frame with mips, crt_out: what the window gets. */
    int crt;
    float crt_p[10];                /* curv scan mask vig ab glow bright mpitch flicker noise;
                                     * mpitch 0 = auto */
    VkDescriptorSetLayout crt_dsl;
    VkPipelineLayout crt_layout;
    VkShaderModule crt_vs, crt_fs;
    VkPipeline crt_pipe;
    VkSampler crt_samp;
    VkImage crt_in, crt_out;
    VkDeviceMemory crt_in_mem, crt_out_mem;
    VkImageView crt_in_view, crt_out_view;
    uint32_t crt_levels;

    VkSampler lin_samp;             /* linear, clamped, all mips (post / scene passes) */

    /* RECOMP_VKR_GPU_RT: render targets on the GPU; cur_rt = the one draws
     * go to (-1: the main target). */
    VkrRT rts[VKR_MAX_RT];
    int nrts, cur_rt;
    float rt_scale;
    float rt_soft;                  /* RECOMP_SHADOW_SOFT: blur radius in guest texels, 0 = sharp */
    VkDescriptorSetLayout soft_dsl;
    VkPipelineLayout soft_layout;
    VkShaderModule soft_fs;
    VkPipeline soft_pipe;
    VkSampler soft_samp;

    /* RECOMP_MSAA=<n>: draws go to n-sample colour / depth images, the
     * colour resolved into color at the flip (everything after the draws --
     * post, CRT, read-back -- works on the resolved image). depth is
     * the multisampled one then. RECOMP_A2C=1: alpha tests as coverage. */
    uint32_t msaa;
    int a2c;
    VkImage msc;
    VkDeviceMemory msc_mem;
    VkImageView msc_view;

    /* The scene pass (scene.frag, scene_ao.frag): RECOMP_AO, _HAZE, _SHAFTS,
     * _RIM, _MBLUR, on frames with a camera (vkr_scene_set). */
    int scene, texsmooth;
    int ab_off;                     /* vkr_ab_off: per-frame features off (A/B shots) */
    float sc_p[13];                 /* ao haze shafts rim mblur | radius taps start density rimrange near mindist gain */
    VkrScene cam;
    int cam_valid;
    VkDescriptorSetLayout sc_dsl;
    VkPipelineLayout sc_layout;
    VkShaderModule sc_fs[4];        /* ao, ao ms, composite, composite ms */
    VkPipeline sc_ao_pipe, sc_pipe, sc_pipe_ms;  /* sc_pipe_ms: into the multisampled target */
    int scene_done;                 /* ran this frame (before the HUD, vkr_scene_run) */
    VkSampler sc_near;
    VkImage sc_in, sc_ao;
    VkDeviceMemory sc_in_mem, sc_ao_mem;
    VkImageView sc_in_view, sc_ao_view;
    uint32_t sc_levels, sc_aw, sc_ah;

    /* RECOMP_POST=1: the remaster post pass (post.frag) on the resolved
     * frame: sharpening, bloom, grade, deband. post_in: a copy with mips. */
    int post;
    float post_p[8];                /* sharpen bloom thresh vibrance contrast deband vignette warmth */
    VkDescriptorSetLayout post_dsl;
    VkPipelineLayout post_layout;
    VkShaderModule post_fs;
    VkPipeline post_pipe;
    VkImage post_in;
    VkDeviceMemory post_in_mem;
    VkImageView post_in_view;
    uint32_t post_levels;
};

static VkImageAspectFlags ds_aspect(const VkrContext *c)
{
    return VK_IMAGE_ASPECT_DEPTH_BIT | (c->has_stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
}

static void use_frame(VkrContext *c, int i)
{
    c->cur = i;
    c->cmd = c->fr[i].cmd;
    c->fence = c->fr[i].fence;
    c->buf = c->fr[i].buf;
    c->buf_map = c->fr[i].buf_map;
}

/* Until no frame is in flight or waiting to be read. */
static void wait_all_free(VkrContext *c)
{
    int i;
    pthread_mutex_lock(&c->lock);
    for (i = 0; i < VKR_FRAMES; i++)
        while (c->fr[i].busy)
            pthread_cond_wait(&c->freed, &c->lock);
    pthread_mutex_unlock(&c->lock);
}

#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
    fprintf(stderr, "[VKR] %s failed: %d (%s:%d)\n", #x, (int)r_, __FILE__, __LINE__); \
    return 0; } } while (0)

static int find_mem(VkrContext *c, uint32_t bits, VkMemoryPropertyFlags want)
{
    uint32_t i;
    for (i = 0; i < c->mem.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (c->mem.memoryTypes[i].propertyFlags & want) == want)
            return (int)i;
    return -1;
}

/* readback: the CPU reads it back. Host-visible coherent memory is write-
 * combined (or the GPU's own BAR) on discrete GPUs -- fine for uploads, but
 * reading 1.2 MB of it took ~10 ms per flip (240 ms/s in a race). A readback
 * buffer takes HOST_CACHED memory, coherent if there is such a type, else
 * non-coherent (*noncoherent = 1: invalidate before reading). */
static int make_buffer_ex(VkrContext *c, VkDeviceSize size, VkBufferUsageFlags usage,
                          VkBuffer *b, VkDeviceMemory *m, void **map, int readback,
                          int *noncoherent);

static int make_buffer(VkrContext *c, VkDeviceSize size, VkBufferUsageFlags usage,
                       VkBuffer *b, VkDeviceMemory *m, void **map)
{
    return make_buffer_ex(c, size, usage, b, m, map, 0, NULL);
}

static int make_buffer_ex(VkrContext *c, VkDeviceSize size, VkBufferUsageFlags usage,
                          VkBuffer *b, VkDeviceMemory *m, void **map, int readback,
                          int *noncoherent)
{
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    VkMemoryRequirements mr;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    int t;
    bi.size = size; bi.usage = usage; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    CK(vkCreateBuffer(c->dev, &bi, NULL, b));
    vkGetBufferMemoryRequirements(c->dev, *b, &mr);
    t = -1;
    if (noncoherent) *noncoherent = 0;
    if (readback) {
        t = find_mem(c, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                           | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                           | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        if (t < 0) {
            t = find_mem(c, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                               | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
            if (t >= 0 && noncoherent) *noncoherent = 1;
        }
    }
    if (t < 0)
        t = find_mem(c, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                           | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (t < 0) { fprintf(stderr, "[VKR] no host-visible memory\n"); return 0; }
    ai.allocationSize = mr.size; ai.memoryTypeIndex = (uint32_t)t;
    CK(vkAllocateMemory(c->dev, &ai, NULL, m));
    CK(vkBindBufferMemory(c->dev, *b, *m, 0));
    CK(vkMapMemory(c->dev, *m, 0, VK_WHOLE_SIZE, 0, map));
    return 1;
}

static int make_image_ex(VkrContext *c, VkFormat fmt, VkImageUsageFlags usage,
                         VkImageAspectFlags aspect, VkImage *img, VkDeviceMemory *mem,
                         VkImageView *view, uint32_t width, uint32_t height, uint32_t levels)
{
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    VkMemoryRequirements mr;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    int t;
    ii.imageType = VK_IMAGE_TYPE_2D; ii.format = fmt;
    ii.extent.width = width; ii.extent.height = height; ii.extent.depth = 1;
    ii.mipLevels = levels; ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL; ii.usage = usage;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    CK(vkCreateImage(c->dev, &ii, NULL, img));
    vkGetImageMemoryRequirements(c->dev, *img, &mr);
    t = find_mem(c, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (t < 0) return 0;
    ai.allocationSize = mr.size; ai.memoryTypeIndex = (uint32_t)t;
    CK(vkAllocateMemory(c->dev, &ai, NULL, mem));
    CK(vkBindImageMemory(c->dev, *img, *mem, 0));
    vi.image = *img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt;
    vi.subresourceRange.aspectMask = aspect;
    vi.subresourceRange.levelCount = levels; vi.subresourceRange.layerCount = 1;
    CK(vkCreateImageView(c->dev, &vi, NULL, view));
    return 1;
}

static int make_image(VkrContext *c, VkFormat fmt, VkImageUsageFlags usage,
                      VkImageAspectFlags aspect, VkImage *img, VkDeviceMemory *mem,
                      VkImageView *view, uint32_t width, uint32_t height)
{
    return make_image_ex(c, fmt, usage, aspect, img, mem, view, width, height, 1);
}

/* An attachment with c->msaa samples (RECOMP_MSAA). */
static int make_image_ms(VkrContext *c, VkFormat fmt, VkImageUsageFlags usage,
                         VkImageAspectFlags aspect, VkImage *img, VkDeviceMemory *mem,
                         VkImageView *view, uint32_t width, uint32_t height)
{
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    VkMemoryRequirements mr;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    int t;
    ii.imageType = VK_IMAGE_TYPE_2D; ii.format = fmt;
    ii.extent.width = width; ii.extent.height = height; ii.extent.depth = 1;
    ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = (VkSampleCountFlagBits)c->msaa;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL; ii.usage = usage;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    CK(vkCreateImage(c->dev, &ii, NULL, img));
    vkGetImageMemoryRequirements(c->dev, *img, &mr);
    t = find_mem(c, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (t < 0) return 0;
    ai.allocationSize = mr.size; ai.memoryTypeIndex = (uint32_t)t;
    CK(vkAllocateMemory(c->dev, &ai, NULL, mem));
    CK(vkBindImageMemory(c->dev, *img, *mem, 0));
    vi.image = *img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt;
    vi.subresourceRange.aspectMask = aspect;
    vi.subresourceRange.levelCount = 1; vi.subresourceRange.layerCount = 1;
    CK(vkCreateImageView(c->dev, &vi, NULL, view));
    return 1;
}

/* Colour mip levels [base, base + n) of img. */
static void barrier_mips(VkrContext *c, VkImage img, uint32_t base, uint32_t n,
                         VkImageLayout from, VkImageLayout to,
                         VkPipelineStageFlags2 ss, VkAccessFlags2 sa,
                         VkPipelineStageFlags2 ds, VkAccessFlags2 da)
{
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    VkDependencyInfo d = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    b.srcStageMask = ss; b.srcAccessMask = sa; b.dstStageMask = ds; b.dstAccessMask = da;
    b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.baseMipLevel = base;
    b.subresourceRange.levelCount = n; b.subresourceRange.layerCount = 1;
    d.imageMemoryBarrierCount = 1; d.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(c->cmd, &d);
}

static void barrier(VkrContext *c, VkImage img, VkImageAspectFlags aspect,
                    VkImageLayout from, VkImageLayout to,
                    VkPipelineStageFlags2 ss, VkAccessFlags2 sa,
                    VkPipelineStageFlags2 ds, VkAccessFlags2 da)
{
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    VkDependencyInfo d = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    b.srcStageMask = ss; b.srcAccessMask = sa; b.dstStageMask = ds; b.dstAccessMask = da;
    b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = aspect;
    b.subresourceRange.levelCount = 1; b.subresourceRange.layerCount = 1;
    d.imageMemoryBarrierCount = 1; d.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(c->cmd, &d);
}

static VkShaderModule shader(VkrContext *c, const unsigned char *code, size_t n)
{
    VkShaderModuleCreateInfo si = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    VkShaderModule m = VK_NULL_HANDLE;
    si.codeSize = n; si.pCode = (const uint32_t *)code;
    if (vkCreateShaderModule(c->dev, &si, NULL, &m) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return m;
}

static VkBlendFactor blend_factor(uint32_t f)
{
    switch (f) {
    case 0x0000: return VK_BLEND_FACTOR_ZERO;
    case 0x0001: return VK_BLEND_FACTOR_ONE;
    case 0x0300: return VK_BLEND_FACTOR_SRC_COLOR;
    case 0x0301: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 0x0302: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 0x0303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 0x0304: return VK_BLEND_FACTOR_DST_ALPHA;
    case 0x0305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 0x0306: return VK_BLEND_FACTOR_DST_COLOR;
    case 0x0307: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 0x0308: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x8001: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x8002: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x8003: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x8004: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default:     return VK_BLEND_FACTOR_ONE;
    }
}

static VkBlendOp blend_op(uint32_t e)
{
    switch (e) {
    case 0x800A: return VK_BLEND_OP_SUBTRACT;
    case 0x800B: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 0x8007: return VK_BLEND_OP_MIN;
    case 0x8008: return VK_BLEND_OP_MAX;
    default:     return VK_BLEND_OP_ADD;
    }
}

static int make_layout(VkrContext *c)
{
    VkDescriptorSetLayoutBinding b[3] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        /* the upload buffer, for the GPU vertex program (xbox_vsh.vert) */
        {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL},
    };
    VkDescriptorSetLayoutCreateInfo di = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    VkPushConstantRange pcr[2] = {{VK_SHADER_STAGE_VERTEX_BIT, 0, 64},
                                  {VK_SHADER_STAGE_FRAGMENT_BIT, 64, 20}};
    VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};

    c->vs = shader(c, xbox_vert_spv, xbox_vert_spv_len);
    c->fs = shader(c, xbox_frag_spv, xbox_frag_spv_len);
    c->fs_wdepth = shader(c, xbox_frag_wdepth_spv, xbox_frag_wdepth_spv_len);
    c->vs_vsh = shader(c, xbox_vsh_vert_spv, xbox_vsh_vert_spv_len);
    c->fs_a2c = shader(c, xbox_frag_a2c_spv, xbox_frag_a2c_spv_len);
    if (!c->vs || !c->fs || !c->fs_wdepth || !c->vs_vsh || !c->fs_a2c) { fprintf(stderr, "[VKR] shader module failed\n"); return 0; }
    di.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT;
    di.bindingCount = 3; di.pBindings = b;
    CK(vkCreateDescriptorSetLayout(c->dev, &di, NULL, &c->dsl));
    li.setLayoutCount = 1; li.pSetLayouts = &c->dsl;
    li.pushConstantRangeCount = 2; li.pPushConstantRanges = pcr;
    CK(vkCreatePipelineLayout(c->dev, &li, NULL, &c->layout));
    return 1;
}

/* RECOMP_CRT=1 [RECOMP_CRT_PARAMS=name=value,...]: the CRT post-pass. */
static const char *const crt_names[10] = {
    "curv", "scan", "mask", "vig", "ab", "glow", "bright", "mpitch", "flicker", "noise"};

static void crt_config(VkrContext *c)
{
    static const float def[10] = {0.18f, 0.6f, 0.35f, 0.35f, 0.8f, 0.4f, 1.1f, 0.0f, 0.0f, 0.25f};
    const char *e = getenv("RECOMP_CRT");
    char buf[512], *tok, *save = NULL;
    int i;
    c->crt = e && *e && *e != '0';
    memcpy(c->crt_p, def, sizeof def);
    e = getenv("RECOMP_CRT_PARAMS");
    if (!c->crt || !e)
        return;
    snprintf(buf, sizeof buf, "%s", e);
    for (tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        char *eq = strchr(tok, '=');
        if (!eq) continue;
        *eq = 0;
        for (i = 0; i < 10; i++)
            if (!strcmp(tok, crt_names[i]))
                c->crt_p[i] = (float)atof(eq + 1);
    }
}

static int make_crt(VkrContext *c)
{
    VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                      VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo di = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    VkPushConstantRange pcr = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 72};
    VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    VkPipelineShaderStageCreateInfo stg[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_VERTEX_BIT, 0, "main", 0},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_FRAGMENT_BIT, 0, "main", 0},
    };
    VkPipelineVertexInputStateCreateInfo vin = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cba = {0};
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    VkFormat cf = VK_FORMAT_B8G8R8A8_UNORM;
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};

    c->crt_vs = shader(c, crt_vert_spv, crt_vert_spv_len);
    c->crt_fs = shader(c, crt_frag_spv, crt_frag_spv_len);
    if (!c->crt_vs || !c->crt_fs) { fprintf(stderr, "[VKR] CRT shader module failed\n"); return 0; }
    di.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT;
    di.bindingCount = 1; di.pBindings = &b;
    CK(vkCreateDescriptorSetLayout(c->dev, &di, NULL, &c->crt_dsl));
    li.setLayoutCount = 1; li.pSetLayouts = &c->crt_dsl;
    li.pushConstantRangeCount = 1; li.pPushConstantRanges = &pcr;
    CK(vkCreatePipelineLayout(c->dev, &li, NULL, &c->crt_layout));
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = VK_LOD_CLAMP_NONE;
    CK(vkCreateSampler(c->dev, &si, NULL, &c->crt_samp));

    stg[0].module = c->crt_vs; stg[1].module = c->crt_fs;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    vp.viewportCount = 1; vp.scissorCount = 1;
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    cba.colorWriteMask = 0xF;
    cb.attachmentCount = 1; cb.pAttachments = &cba;
    dy.dynamicStateCount = 2; dy.pDynamicStates = dyn;
    ri.colorAttachmentCount = 1; ri.pColorAttachmentFormats = &cf;
    pi.pNext = &ri; pi.stageCount = 2; pi.pStages = stg;
    pi.pVertexInputState = &vin; pi.pInputAssemblyState = &ia; pi.pViewportState = &vp;
    pi.pRasterizationState = &rs; pi.pMultisampleState = &ms;
    pi.pColorBlendState = &cb; pi.pDynamicState = &dy; pi.layout = c->crt_layout;
    CK(vkCreateGraphicsPipelines(c->dev, VK_NULL_HANDLE, 1, &pi, NULL, &c->crt_pipe));
    fprintf(stderr, "[VKR] CRT on: curv %.2f scan %.2f mask %.2f vig %.2f ab %.2f glow %.2f "
            "bright %.2f mpitch %.1f (0 = auto) flicker %.3f noise %.2f\n",
            c->crt_p[0], c->crt_p[1], c->crt_p[2], c->crt_p[3], c->crt_p[4],
            c->crt_p[5], c->crt_p[6], c->crt_p[7], c->crt_p[8], c->crt_p[9]);
    return 1;
}


/* The remaster post pass (post.frag), one knob per effect, 0 = off:
 * RECOMP_SHARPEN (0..1), RECOMP_BLOOM (strength) with RECOMP_BLOOM_THRESH
 * (0..1, default 0.8), RECOMP_VIBRANCE, RECOMP_CONTRAST (-1..1),
 * RECOMP_DEBAND (LSBs of noise, 1 = default when on), RECOMP_VIGNETTE (0..1).
 * The pass runs when any of them is on. */
static void post_config(VkrContext *c)
{
    static const char *const names[7] = {"RECOMP_SHARPEN", "RECOMP_BLOOM", "RECOMP_BLOOM_THRESH",
                                         "RECOMP_VIBRANCE", "RECOMP_CONTRAST", "RECOMP_DEBAND",
                                         "RECOMP_VIGNETTE"};
    int i;
    memset(c->post_p, 0, sizeof c->post_p);
    c->post_p[2] = 0.8f;
    for (i = 0; i < 7; i++) {
        const char *e = getenv(names[i]);
        if (e && *e)
            c->post_p[i] = (float)atof(e);
    }
    c->post = c->post_p[0] > 0.0f || c->post_p[1] > 0.0f || c->post_p[3] != 0.0f
              || c->post_p[4] != 0.0f || c->post_p[5] > 0.0f || c->post_p[6] > 0.0f;
}

static int make_post(VkrContext *c)
{
    VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                      VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo di = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    VkPushConstantRange pcr = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 60};
    VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkPipelineShaderStageCreateInfo stg[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_VERTEX_BIT, 0, "main", 0},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_FRAGMENT_BIT, 0, "main", 0},
    };
    VkPipelineVertexInputStateCreateInfo vin = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cba = {0};
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    VkFormat cf = VK_FORMAT_B8G8R8A8_UNORM;
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    VkShaderModule vs = shader(c, crt_vert_spv, crt_vert_spv_len);   /* one fullscreen triangle */

    c->post_fs = shader(c, post_frag_spv, post_frag_spv_len);
    if (!vs || !c->post_fs) { fprintf(stderr, "[VKR] post shader module failed\n"); return 0; }
    if (!c->lin_samp) {
        VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod = VK_LOD_CLAMP_NONE;
        CK(vkCreateSampler(c->dev, &si, NULL, &c->lin_samp));
    }
    di.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT;
    di.bindingCount = 1; di.pBindings = &b;
    CK(vkCreateDescriptorSetLayout(c->dev, &di, NULL, &c->post_dsl));
    li.setLayoutCount = 1; li.pSetLayouts = &c->post_dsl;
    li.pushConstantRangeCount = 1; li.pPushConstantRanges = &pcr;
    CK(vkCreatePipelineLayout(c->dev, &li, NULL, &c->post_layout));
    stg[0].module = vs; stg[1].module = c->post_fs;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    vp.viewportCount = 1; vp.scissorCount = 1;
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    cba.colorWriteMask = 0xF;
    cb.attachmentCount = 1; cb.pAttachments = &cba;
    dy.dynamicStateCount = 2; dy.pDynamicStates = dyn;
    ri.colorAttachmentCount = 1; ri.pColorAttachmentFormats = &cf;
    pi.pNext = &ri; pi.stageCount = 2; pi.pStages = stg;
    pi.pVertexInputState = &vin; pi.pInputAssemblyState = &ia; pi.pViewportState = &vp;
    pi.pRasterizationState = &rs; pi.pMultisampleState = &ms;
    pi.pColorBlendState = &cb; pi.pDynamicState = &dy; pi.layout = c->post_layout;
    CK(vkCreateGraphicsPipelines(c->dev, VK_NULL_HANDLE, 1, &pi, NULL, &c->post_pipe));
    vkDestroyShaderModule(c->dev, vs, NULL);
    fprintf(stderr, "[VKR] post pass on: sharpen %.2f bloom %.2f (thresh %.2f) vibrance %.2f contrast %.2f"
            " deband %.2f vignette %.2f\n", c->post_p[0], c->post_p[1], c->post_p[2], c->post_p[3],
            c->post_p[4], c->post_p[5], c->post_p[6]);
    return 1;
}

/* A fullscreen-triangle pipeline: crt.vert + fs, one colour attachment. */
static int make_fs_pipe_ms(VkrContext *c, VkShaderModule fs, VkPipelineLayout layout, VkFormat fmt,
                           uint32_t samples, VkPipeline *out);

static int make_fs_pipe(VkrContext *c, VkShaderModule fs, VkPipelineLayout layout, VkFormat fmt,
                        VkPipeline *out)
{
    return make_fs_pipe_ms(c, fs, layout, fmt, 1, out);
}

/* The same into an attachment with `samples` samples (every sample gets the
 * pixel's colour). */
static int make_fs_pipe_ms(VkrContext *c, VkShaderModule fs, VkPipelineLayout layout, VkFormat fmt,
                           uint32_t samples, VkPipeline *out)
{
    VkPipelineShaderStageCreateInfo stg[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_VERTEX_BIT, 0, "main", 0},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_FRAGMENT_BIT, 0, "main", 0},
    };
    VkPipelineVertexInputStateCreateInfo vin = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cba = {0};
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    VkShaderModule vs = shader(c, crt_vert_spv, crt_vert_spv_len);
    VkResult r;
    if (!vs || !fs) return 0;
    stg[0].module = vs; stg[1].module = fs;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    vp.viewportCount = 1; vp.scissorCount = 1;
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    ms.rasterizationSamples = (VkSampleCountFlagBits)samples;
    cba.colorWriteMask = 0xF;
    cb.attachmentCount = 1; cb.pAttachments = &cba;
    dy.dynamicStateCount = 2; dy.pDynamicStates = dyn;
    ri.colorAttachmentCount = 1; ri.pColorAttachmentFormats = &fmt;
    pi.pNext = &ri; pi.stageCount = 2; pi.pStages = stg;
    pi.pVertexInputState = &vin; pi.pInputAssemblyState = &ia; pi.pViewportState = &vp;
    pi.pRasterizationState = &rs; pi.pMultisampleState = &ms;
    pi.pColorBlendState = &cb; pi.pDynamicState = &dy; pi.layout = layout;
    r = vkCreateGraphicsPipelines(c->dev, VK_NULL_HANDLE, 1, &pi, NULL, out);
    vkDestroyShaderModule(c->dev, vs, NULL);
    return r == VK_SUCCESS;
}

/* The scene pass's knobs (0 = off), RECOMP_SCENE_PARAMS for the finer ones:
 *   RECOMP_AO=<strength 0..1>     RECOMP_HAZE=<strength 0..1>
 *   RECOMP_SHAFTS=<strength>      RECOMP_RIM=<strength>
 *   RECOMP_MBLUR=<strength, 1 = the full frame-to-frame motion>
 *   RECOMP_TEX_BICUBIC=1 (xbox.frag; not part of the pass)
 * RECOMP_SCENE_PARAMS=name=value,...: radius (AO, world units, 200) taps (12)
 * start (haze, 5000) density (haze per unit, 0.00004) rimrange (900)
 * near (motion blur starts, 600) mindist (nearer = HUD models, 120) gain
 * (AO: occlusion per tap, 12). World
 * units are SSX 3's (cm). */
static void scene_config(VkrContext *c)
{
    static const char *const env[5] = {"RECOMP_AO", "RECOMP_HAZE", "RECOMP_SHAFTS", "RECOMP_RIM", "RECOMP_MBLUR"};
    static const char *const names[8] = {"radius", "taps", "start", "density", "rimrange", "near", "mindist", "gain"};
    static const float def[8] = {200.0f, 16.0f, 5000.0f, 0.00004f, 900.0f, 600.0f, 120.0f, 12.0f};
    const char *e;
    char buf[512], *tok, *save = NULL;
    int i;
    for (i = 0; i < 5; i++) {
        e = getenv(env[i]);
        c->sc_p[i] = e && *e ? (float)atof(e) : 0.0f;
    }
    memcpy(&c->sc_p[5], def, sizeof def);
    e = getenv("RECOMP_SCENE_PARAMS");
    if (e) {
        snprintf(buf, sizeof buf, "%s", e);
        for (tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            char *eq = strchr(tok, '=');
            if (!eq) continue;
            *eq = 0;
            for (i = 0; i < 8; i++)
                if (!strcmp(tok, names[i]))
                    c->sc_p[5 + i] = (float)atof(eq + 1);
        }
    }
    if (c->sc_p[6] < 4.0f) c->sc_p[6] = 4.0f;
    if (c->sc_p[6] > 32.0f) c->sc_p[6] = 32.0f;
    c->scene = c->sc_p[0] > 0.0f || c->sc_p[1] > 0.0f || c->sc_p[2] > 0.0f || c->sc_p[3] > 0.0f
               || c->sc_p[4] > 0.0f;
    e = getenv("RECOMP_TEX_BICUBIC");
    c->texsmooth = e && *e == '1';
}

static int make_scene(VkrContext *c)
{
    VkDescriptorSetLayoutBinding b[4] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
    };
    VkDescriptorSetLayoutCreateInfo di = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    int ms = c->msaa > 1;
    c->sc_fs[0] = shader(c, scene_ao_frag_spv, scene_ao_frag_spv_len);
    c->sc_fs[1] = shader(c, scene_ao_ms_frag_spv, scene_ao_ms_frag_spv_len);
    c->sc_fs[2] = shader(c, scene_frag_spv, scene_frag_spv_len);
    c->sc_fs[3] = shader(c, scene_ms_frag_spv, scene_ms_frag_spv_len);
    if (!c->sc_fs[0] || !c->sc_fs[1] || !c->sc_fs[2] || !c->sc_fs[3]) {
        fprintf(stderr, "[VKR] scene shader modules failed\n");
        return 0;
    }
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    CK(vkCreateSampler(c->dev, &si, NULL, &c->sc_near));
    if (!c->lin_samp) {
        VkSamplerCreateInfo li2 = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        li2.magFilter = li2.minFilter = VK_FILTER_LINEAR;
        li2.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        li2.addressModeU = li2.addressModeV = li2.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        li2.maxLod = VK_LOD_CLAMP_NONE;
        CK(vkCreateSampler(c->dev, &li2, NULL, &c->lin_samp));
    }
    di.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT;
    di.bindingCount = 4; di.pBindings = b;
    CK(vkCreateDescriptorSetLayout(c->dev, &di, NULL, &c->sc_dsl));
    li.setLayoutCount = 1; li.pSetLayouts = &c->sc_dsl;
    CK(vkCreatePipelineLayout(c->dev, &li, NULL, &c->sc_layout));
    if (!make_fs_pipe(c, c->sc_fs[ms ? 1 : 0], c->sc_layout, VK_FORMAT_R16G16_SFLOAT, &c->sc_ao_pipe)
        || !make_fs_pipe(c, c->sc_fs[ms ? 3 : 2], c->sc_layout, VK_FORMAT_B8G8R8A8_UNORM, &c->sc_pipe)
        || (ms && !make_fs_pipe_ms(c, c->sc_fs[3], c->sc_layout, VK_FORMAT_B8G8R8A8_UNORM, c->msaa,
                                   &c->sc_pipe_ms))) {
        fprintf(stderr, "[VKR] scene pipelines failed\n");
        return 0;
    }
    fprintf(stderr, "[VKR] scene pass on: ao %.2f (radius %.0f, %.0f taps) haze %.2f (start %.0f, density %g)"
            " shafts %.2f rim %.2f (range %.0f) mblur %.2f (near %.0f)\n", c->sc_p[0], c->sc_p[5], c->sc_p[6],
            c->sc_p[1], c->sc_p[7], c->sc_p[8], c->sc_p[2], c->sc_p[3], c->sc_p[9], c->sc_p[4], c->sc_p[10]);
    return 1;
}

void vkr_ab_off(VkrContext *c, int off)
{
    c->ab_off = off;
}

void vkr_scene_set(VkrContext *c, const VkrScene *s)
{
    c->cam = *s;
    c->cam_valid = 1;
}

/* RECOMP_SHADOW_SOFT: rt_soft.frag, one attachment, nearest sampling. */
static int make_soft(VkrContext *c)
{
    VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                      VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo di = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    VkPushConstantRange pcr = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 24};
    VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    VkPipelineShaderStageCreateInfo stg[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_VERTEX_BIT, 0, "main", 0},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_FRAGMENT_BIT, 0, "main", 0},
    };
    VkPipelineVertexInputStateCreateInfo vin = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cba = {0};
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    VkFormat cf = VK_FORMAT_B8G8R8A8_UNORM;
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    VkShaderModule vs = shader(c, crt_vert_spv, crt_vert_spv_len);

    c->soft_fs = shader(c, rt_soft_frag_spv, rt_soft_frag_spv_len);
    if (!vs || !c->soft_fs) { fprintf(stderr, "[VKR] shadow soft shader module failed\n"); return 0; }
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    CK(vkCreateSampler(c->dev, &si, NULL, &c->soft_samp));
    di.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT;
    di.bindingCount = 1; di.pBindings = &b;
    CK(vkCreateDescriptorSetLayout(c->dev, &di, NULL, &c->soft_dsl));
    li.setLayoutCount = 1; li.pSetLayouts = &c->soft_dsl;
    li.pushConstantRangeCount = 1; li.pPushConstantRanges = &pcr;
    CK(vkCreatePipelineLayout(c->dev, &li, NULL, &c->soft_layout));
    stg[0].module = vs; stg[1].module = c->soft_fs;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    vp.viewportCount = 1; vp.scissorCount = 1;
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    cba.colorWriteMask = 0xF;
    cb.attachmentCount = 1; cb.pAttachments = &cba;
    dy.dynamicStateCount = 2; dy.pDynamicStates = dyn;
    ri.colorAttachmentCount = 1; ri.pColorAttachmentFormats = &cf;
    pi.pNext = &ri; pi.stageCount = 2; pi.pStages = stg;
    pi.pVertexInputState = &vin; pi.pInputAssemblyState = &ia; pi.pViewportState = &vp;
    pi.pRasterizationState = &rs; pi.pMultisampleState = &ms;
    pi.pColorBlendState = &cb; pi.pDynamicState = &dy; pi.layout = c->soft_layout;
    CK(vkCreateGraphicsPipelines(c->dev, VK_NULL_HANDLE, 1, &pi, NULL, &c->soft_pipe));
    vkDestroyShaderModule(c->dev, vs, NULL);
    return 1;
}

/* RECOMP_A2C: an alpha test the A2C variant turns into coverage (GREATER /
 * GEQUAL, i.e. "keep the opaque part"). */
static int uses_a2c(const VkrContext *c, const VkrDrawState *st)
{
    return c->msaa > 1 && c->a2c && c->cur_rt < 0 && (st->alpha_func == 0x204 || st->alpha_func == 0x206);
}

/* Pipeline for one blend/write-mask combination (depth state is dynamic). */
static VkPipeline get_pipeline(VkrContext *c, const VkrDrawState *st, int gpu_vsh)
{
    uint32_t key[8];
    VkPipelineShaderStageCreateInfo stg[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_VERTEX_BIT, 0, "main", 0},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_FRAGMENT_BIT, 0, "main", 0},
    };
    VkVertexInputBindingDescription bind = {0, sizeof(VkrVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription at[8];
    VkPipelineVertexInputStateCreateInfo vin = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cba[1] = {{0}};
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                            VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
                            VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                            VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP,
                            VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
                            VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    VkFormat cf[1] = {VK_FORMAT_B8G8R8A8_UNORM};
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    VkPipeline pipe;
    int i;

    key[0] = (uint32_t)st->blend_enable; key[1] = st->blend_src; key[2] = st->blend_dst;
    key[3] = st->blend_eq; key[4] = st->write_mask;
    if (!st->blend_enable) key[1] = key[2] = key[3] = 0;
    key[5] = (uint32_t)gpu_vsh;
    key[6] = st->topology;
    key[7] = (uint32_t)(st->w_buffer != 0);
    if (!(key[7] & 1u) && uses_a2c(c, st))
        key[7] |= 4u;
    if (c->cur_rt >= 0)                 /* a render target: one attachment, one sample */
        key[7] = (key[7] & 1u) | 8u;
    for (i = 0; i < c->npipes; i++)
        if (!memcmp(c->pipes[i].key, key, sizeof key))
            return c->pipes[i].pipe;
    if (c->npipes == (int)(sizeof c->pipes / sizeof c->pipes[0])) {
        fprintf(stderr, "[VKR] pipeline cache full\n");
        return VK_NULL_HANDLE;
    }

    stg[0].module = gpu_vsh ? c->vs_vsh : c->vs; stg[1].module = st->w_buffer ? c->fs_wdepth : c->fs;
    if (key[7] & 4u)
        stg[1].module = c->fs_a2c;
    for (i = 0; i < 8; i++) {
        at[i].location = (uint32_t)i; at[i].binding = 0;
        at[i].format = VK_FORMAT_R32G32B32A32_SFLOAT;
        at[i].offset = (uint32_t)i * 16;
    }
    if (!gpu_vsh) {             /* the GPU program pulls its inputs from binding 5 */
        vin.vertexBindingDescriptionCount = 1; vin.pVertexBindingDescriptions = &bind;
        vin.vertexAttributeDescriptionCount = 8; vin.pVertexAttributeDescriptions = at;
    }
    ia.topology = st->topology == VKR_TOPO_LINES  ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                : st->topology == VKR_TOPO_POINTS ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
                : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    vp.viewportCount = 1; vp.scissorCount = 1;
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rs.lineWidth = c->wide_lines ? (c->scale < c->max_line ? c->scale : c->max_line) : 1.0f;
    ms.rasterizationSamples = (VkSampleCountFlagBits)(c->msaa > 1 && !(key[7] & 8u) ? c->msaa : 1);
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    cba[0].colorWriteMask = st->write_mask & 0xF;
    if (st->blend_enable) {
        cba[0].blendEnable = VK_TRUE;
        cba[0].srcColorBlendFactor = cba[0].srcAlphaBlendFactor = blend_factor(st->blend_src);
        cba[0].dstColorBlendFactor = cba[0].dstAlphaBlendFactor = blend_factor(st->blend_dst);
        cba[0].colorBlendOp = cba[0].alphaBlendOp = blend_op(st->blend_eq);
    }
    cb.attachmentCount = 1; cb.pAttachments = cba;
    dy.dynamicStateCount = (uint32_t)(sizeof dyn / sizeof dyn[0]); dy.pDynamicStates = dyn;
    ri.colorAttachmentCount = cb.attachmentCount; ri.pColorAttachmentFormats = cf;
    ri.depthAttachmentFormat = c->depth_fmt;
    if (c->has_stencil)
        ri.stencilAttachmentFormat = c->depth_fmt;
    pi.pNext = &ri; pi.stageCount = 2; pi.pStages = stg;
    pi.pVertexInputState = &vin; pi.pInputAssemblyState = &ia; pi.pViewportState = &vp;
    pi.pRasterizationState = &rs; pi.pMultisampleState = &ms; pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cb; pi.pDynamicState = &dy; pi.layout = c->layout;
    {
        double t0 = vkr_now_ms();
        if (vkCreateGraphicsPipelines(c->dev, VK_NULL_HANDLE, 1, &pi, NULL, &pipe) != VK_SUCCESS) {
            fprintf(stderr, "[VKR] pipeline creation failed\n");
            return VK_NULL_HANDLE;
        }
        fprintf(stderr, "[VKR] pipeline %d: %.1f ms (blend %u %u/%u eq %u mask %X vsh %u topo %u wdepth %u)\n",
                c->npipes, vkr_now_ms() - t0,
                key[0], key[1], key[2], key[3], key[4], key[5], key[6], key[7]);
    }
    memcpy(c->pipes[c->npipes].key, key, sizeof key);
    c->pipes[c->npipes++].pipe = pipe;
    return pipe;
}

static VkSampler get_sampler(VkrContext *c, uint32_t au, uint32_t av, int linear, int mip)
{
    uint32_t key = (au & 7) | (av & 7) << 3 | (uint32_t)(linear != 0) << 6 | (uint32_t)(mip && linear) << 7;
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    VkSamplerAddressMode m[2];
    uint32_t a[2] = {au, av}, k;
    int i;

    for (i = 0; i < c->nsamplers; i++)
        if (c->samplers[i].key == key)
            return c->samplers[i].s;
    if (c->nsamplers == 32)
        return c->samplers[0].s;
    for (k = 0; k < 2; k++)
        switch (a[k]) {
        case 2:  m[k] = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT; break;
        case 3: case 5: m[k] = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; break;
        case 4:  m[k] = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER; break;
        default: m[k] = VK_SAMPLER_ADDRESS_MODE_REPEAT; break;
        }
    si.magFilter = si.minFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.addressModeU = m[0]; si.addressModeV = m[1]; si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    if (mip && linear) {
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        si.maxLod = VK_LOD_CLAMP_NONE;
        if (c->aniso > 1.0f) {
            si.anisotropyEnable = VK_TRUE;
            si.maxAnisotropy = c->aniso;
        }
    }
    if (vkCreateSampler(c->dev, &si, NULL, &c->samplers[c->nsamplers].s) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    c->samplers[c->nsamplers].key = key;
    return c->samplers[c->nsamplers++].s;
}

VkrContext *vkr_create(void)
{
    VkrContext *c = (VkrContext *)calloc(1, sizeof *c);
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    VkInstanceCreateInfo ii = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    VkPhysicalDevice devs[8];
    uint32_t n = 8, i, nq = 0;
    VkQueueFamilyProperties qp[16];
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan14Features f14 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};
    VkDeviceCreateInfo di = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    void *map;
    int f;

    if (!c) return NULL;
    app.pApplicationName = "ssx3"; app.apiVersion = VK_API_VERSION_1_4;
    ii.pApplicationInfo = &app;
    if (vkCreateInstance(&ii, NULL, &c->inst) != VK_SUCCESS) goto fail;
    if (vkEnumeratePhysicalDevices(c->inst, &n, devs) < 0 || !n) goto fail;
    /* Prefer a real GPU over llvmpipe. */
    c->phys = devs[0];
    for (i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devs[i], &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU
            || p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            c->phys = devs[i];
            break;
        }
    }
    {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(c->phys, &p);
        fprintf(stderr, "[VKR] device: %s (Vulkan %u.%u)\n", p.deviceName,
                VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion));
    }
    vkGetPhysicalDeviceMemoryProperties(c->phys, &c->mem);
    vkGetPhysicalDeviceQueueFamilyProperties(c->phys, &nq, NULL);
    if (nq > 16) nq = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(c->phys, &nq, qp);
    for (i = 0; i < nq && !(qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT); i++)
        ;
    if (i == nq) goto fail;
    c->qfam = i;
    qi.queueFamilyIndex = i; qi.queueCount = 1; qi.pQueuePriorities = &prio;
    {
        /* RECOMP_RES: the internal resolution, as a frame height (1080 =
         * x2.25 of 480) or a factor (2 = x2). Default x1. */
        const char *r = getenv("RECOMP_RES");
        double v = r ? atof(r) : 0;
        c->scale = v >= 240 ? (float)(v / 480.0) : (v >= 1.0 && v <= 8.0) ? (float)v : 1.0f;
        /* RECOMP_ASPECT (main.c, from RECOMP_WIDESCREEN): a target that
         * wide (16:9, 32:9, ...); the game renders that view squeezed into
         * 640x480 (ssx3_overrides.c sub_0024EA13). */
        r = getenv("RECOMP_ASPECT");
        vkr_set_aspect(c, r ? (float)atof(r) : 0.0f);
    }
    {
        VkPhysicalDeviceFeatures pf;
        VkPhysicalDeviceProperties pp;
        static VkPhysicalDeviceFeatures en;
        vkGetPhysicalDeviceFeatures(c->phys, &pf);
        vkGetPhysicalDeviceProperties(c->phys, &pp);
        if (pf.wideLines) {             /* lines as wide as the scale */
            en.wideLines = VK_TRUE;
            di.pEnabledFeatures = &en;
            c->wide_lines = 1;
            c->max_line = pp.limits.lineWidthRange[1];
        }
        {
            /* Distant and edge-on textures: a mip chain per texture and
             * anisotropic filtering (the NV2A had both; mip 0 alone
             * shimmered and smeared slopes into streaks). */
            const char *a = getenv("RECOMP_VKR_ANISO"), *mp = getenv("RECOMP_VKR_MIPS");
            float want = a ? (float)atof(a) : 16.0f;
            c->mips = !mp || *mp != '0';
            if (pf.samplerAnisotropy && want > 1.0f) {
                en.samplerAnisotropy = VK_TRUE;
                di.pEnabledFeatures = &en;
                c->aniso = want < pp.limits.maxSamplerAnisotropy ? want : pp.limits.maxSamplerAnisotropy;
            }
            fprintf(stderr, "[VKR] texture mips %s, anisotropy %.0fx\n", c->mips ? "on" : "off", c->aniso);
        }
        {
            /* RECOMP_MSAA=2|4|8: multisampled draws (edges of geometry,
             * lines, sprites); the largest count the GPU has for colour,
             * R16F and depth at or below the asked one. RECOMP_A2C=1 also
             * smooths alpha-tested edges (trees, fences). */
            const char *m = getenv("RECOMP_MSAA"), *a = getenv("RECOMP_A2C");
            uint32_t want = m ? (uint32_t)atoi(m) : 0, ok;
            ok = pp.limits.framebufferColorSampleCounts & pp.limits.framebufferDepthSampleCounts;
            c->msaa = 1;
            while (c->msaa * 2 <= want && (ok & (c->msaa * 2)))
                c->msaa *= 2;
            c->a2c = c->msaa > 1 && a && *a == '1';
            if (c->msaa > 1)
                fprintf(stderr, "[VKR] MSAA %ux%s\n", c->msaa, c->a2c ? ", alpha to coverage" : "");
        }
    }
    f13.dynamicRendering = VK_TRUE; f13.synchronization2 = VK_TRUE;
    f14.pushDescriptor = VK_TRUE;
    f13.pNext = &f14;
    di.pNext = &f13; di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi;
    if (vkCreateDevice(c->phys, &di, NULL, &c->dev) != VK_SUCCESS) goto fail;
    vkGetDeviceQueue(c->dev, c->qfam, 0, &c->queue);
    pci.queueFamilyIndex = c->qfam;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (vkCreateCommandPool(c->dev, &pci, NULL, &c->pool) != VK_SUCCESS) goto fail;
    cai.commandPool = c->pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    for (f = 0; f < VKR_FRAMES; f++) {
        VkrFrame *fr = &c->fr[f];
        if (vkAllocateCommandBuffers(c->dev, &cai, &fr->cmd) != VK_SUCCESS) goto fail;
        if (vkCreateFence(c->dev, &fi, NULL, &fr->fence) != VK_SUCCESS) goto fail;
        if (!make_buffer(c, VKR_BUF_BYTES, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
                         | VK_BUFFER_USAGE_INDEX_BUFFER_BIT
                         | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT
                         | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &fr->buf, &fr->buf_mem, &map))
            goto fail;
        fr->buf_map = (uint8_t *)map;
    }
    {
        /* RECOMP_VKR_GPUPROF=<ms>: GPU time per draw from timestamps; a
         * frame whose GPU work took longer than <ms> is logged with its most
         * expensive draws ([GPUPROF], vkr_frame_pixels). */
        const char *e = getenv("RECOMP_VKR_GPUPROF");
        VkPhysicalDeviceProperties pp;
        vkGetPhysicalDeviceProperties(c->phys, &pp);
        if (e && atof(e) > 0 && qp[c->qfam].timestampValidBits && pp.limits.timestampPeriod > 0) {
            VkQueryPoolCreateInfo qpi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qpi.queryType = VK_QUERY_TYPE_TIMESTAMP; qpi.queryCount = VKR_PROF_MAX;
            c->prof_thr = atof(e);
            c->ts_period = pp.limits.timestampPeriod;
            for (f = 0; f < VKR_FRAMES; f++) {
                if (vkCreateQueryPool(c->dev, &qpi, NULL, &c->fr[f].qp) != VK_SUCCESS
                    || !(c->fr[f].meta = (VkrProfDraw *)calloc(VKR_PROF_MAX, sizeof(VkrProfDraw)))) {
                    c->prof_thr = 0;
                    break;
                }
            }
            fprintf(stderr, "[VKR] GPU profiler %s: frames over %.1f ms\n",
                    c->prof_thr > 0 ? "on" : "failed", c->prof_thr);
        }
    }
    {
        /* The NV2A's Z24S8 has a stencil buffer that SSX 3 uses (the snow
         * trail surfaces fix up depth through it); a format with stencil. */
        static const VkFormat cand[] = {VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT};
        int k;
        c->depth_fmt = VK_FORMAT_D32_SFLOAT;
        for (k = 0; k < 2; k++) {
            VkFormatProperties fp;
            vkGetPhysicalDeviceFormatProperties(c->phys, cand[k], &fp);
            if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
                c->depth_fmt = cand[k];
                c->has_stencil = 1;
                break;
            }
        }
        if (!c->has_stencil)
            fprintf(stderr, "[VKR] no depth-stencil format: stencil ignored\n");
    }
    pthread_mutex_init(&c->lock, NULL);
    pthread_cond_init(&c->freed, NULL);
    use_frame(c, 0);
    if (!make_layout(c)) goto fail;
    crt_config(c);
    if (c->crt && !make_crt(c))
        c->crt = 0;
    scene_config(c);
    if (c->scene && !make_scene(c))
        c->scene = 0;
    post_config(c);
    if (c->post && !make_post(c))
        c->post = 0;
    c->cur_rt = -1;
    {
        /* RECOMP_VKR_RT_SCALE: GPU render targets at this many times the
         * guest size (default 4: a 128x128 shadow map becomes 512x512). */
        const char *e = getenv("RECOMP_VKR_RT_SCALE");
        c->rt_scale = e ? (float)atof(e) : 4.0f;
        if (c->rt_scale < 1.0f) c->rt_scale = 1.0f;
        if (c->rt_scale > 16.0f) c->rt_scale = 16.0f;
        /* RECOMP_SHADOW_SOFT=<radius in map texels>: soft edges for the
         * GPU render targets (the rider shadows); 0 = sharp. */
        e = getenv("RECOMP_SHADOW_SOFT");
        c->rt_soft = e ? (float)atof(e) : 0.0f;
        if (c->rt_soft > 0.0f && !make_soft(c))
            c->rt_soft = 0.0f;
        if (c->rt_soft > 0.0f)
            fprintf(stderr, "[VKR] soft render targets: radius %.2f texels\n", c->rt_soft);
    }
    return c;
fail:
    fprintf(stderr, "[VKR] init failed\n");
    free(c);
    return NULL;
}

static void free_target(VkrContext *c)
{
    int f;
    if (!c->color) return;
    wait_all_free(c);
    vkDeviceWaitIdle(c->dev);
    vkDestroyImageView(c->dev, c->color_view, NULL);
    vkDestroyImageView(c->dev, c->depth_view, NULL);
    if (c->depth_sview) vkDestroyImageView(c->dev, c->depth_sview, NULL);
    c->depth_sview = VK_NULL_HANDLE;
    vkDestroyImage(c->dev, c->color, NULL);
    vkDestroyImage(c->dev, c->depth, NULL);
    vkFreeMemory(c->dev, c->color_mem, NULL);
    vkFreeMemory(c->dev, c->depth_mem, NULL);
    for (f = 0; f < VKR_FRAMES; f++) {
        vkDestroyBuffer(c->dev, c->fr[f].rb, NULL);
        vkFreeMemory(c->dev, c->fr[f].rb_mem, NULL);
        if (c->fr[f].rbh) {
            vkDestroyBuffer(c->dev, c->fr[f].rbh, NULL);
            vkFreeMemory(c->dev, c->fr[f].rbh_mem, NULL);
            c->fr[f].rbh = VK_NULL_HANDLE;
        }
    }
    if (c->down) {
        vkDestroyImageView(c->dev, c->down_view, NULL);
        vkDestroyImage(c->dev, c->down, NULL);
        vkFreeMemory(c->dev, c->down_mem, NULL);
        c->down = VK_NULL_HANDLE;
    }
    if (c->crt_in) {
        vkDestroyImageView(c->dev, c->crt_in_view, NULL);
        vkDestroyImage(c->dev, c->crt_in, NULL);
        vkFreeMemory(c->dev, c->crt_in_mem, NULL);
        vkDestroyImageView(c->dev, c->crt_out_view, NULL);
        vkDestroyImage(c->dev, c->crt_out, NULL);
        vkFreeMemory(c->dev, c->crt_out_mem, NULL);
        c->crt_in = c->crt_out = VK_NULL_HANDLE;
    }
    if (c->msc) {
        vkDestroyImageView(c->dev, c->msc_view, NULL);
        vkDestroyImage(c->dev, c->msc, NULL);
        vkFreeMemory(c->dev, c->msc_mem, NULL);
        c->msc = VK_NULL_HANDLE;
    }
    if (c->sc_in) {
        vkDestroyImageView(c->dev, c->sc_in_view, NULL);
        vkDestroyImage(c->dev, c->sc_in, NULL);
        vkFreeMemory(c->dev, c->sc_in_mem, NULL);
        vkDestroyImageView(c->dev, c->sc_ao_view, NULL);
        vkDestroyImage(c->dev, c->sc_ao, NULL);
        vkFreeMemory(c->dev, c->sc_ao_mem, NULL);
        c->sc_in = c->sc_ao = VK_NULL_HANDLE;
    }
    if (c->post_in) {
        vkDestroyImageView(c->dev, c->post_in_view, NULL);
        vkDestroyImage(c->dev, c->post_in, NULL);
        vkFreeMemory(c->dev, c->post_in_mem, NULL);
        c->post_in = VK_NULL_HANDLE;
    }
    c->color = c->depth = VK_NULL_HANDLE;
}

uint32_t vkr_tex_generation(VkrContext *c)
{
    return c->tex_gen;
}

static void tex_free_all(VkrContext *c)
{
    c->tex_gen++;
    uint32_t i;
    for (i = 0; i < c->ntexs; i++) {
        VkrTex *t = c->texs[i];
        vkDestroyImageView(c->dev, t->view, NULL);
        vkDestroyImage(c->dev, t->img, NULL);
        vkFreeMemory(c->dev, t->mem, NULL);
        free(t);
    }
    free(c->texs);
    c->texs = NULL; c->ntexs = c->captexs = 0; c->tex_bytes = 0; c->white = NULL;
    free(c->hidx);
    c->hidx = NULL; c->hcap = 0;
}

static uint32_t tex_slot(uint64_t key, uint32_t cap)
{
    key ^= key >> 33; key *= 0xff51afd7ed558ccdull; key ^= key >> 33;
    return (uint32_t)key & (cap - 1);
}

/* (Re)build the key index for the current texs. */
static int tex_rehash(VkrContext *c)
{
    uint32_t cap = 1024, i;
    while (cap < c->ntexs * 2 + 2)
        cap <<= 1;
    free(c->hidx);
    c->hidx = (int32_t *)malloc(cap * sizeof *c->hidx);
    if (!c->hidx) { c->hcap = 0; return 0; }
    c->hcap = cap;
    for (i = 0; i < cap; i++)
        c->hidx[i] = -1;
    for (i = 0; i < c->ntexs; i++) {
        uint32_t h = tex_slot(c->texs[i]->key, cap);
        while (c->hidx[h] >= 0)
            h = (h + 1) & (cap - 1);
        c->hidx[h] = (int32_t)i;
    }
    return 1;
}

static void tex_free_one(VkrContext *c, VkrTex *t)
{
    vkDestroyImageView(c->dev, t->view, NULL);
    vkDestroyImage(c->dev, t->img, NULL);
    vkFreeMemory(c->dev, t->mem, NULL);
    c->tex_bytes -= (uint64_t)t->w * t->h * 4;
    free(t);
}

/* Textures not used for `age` flips: certainly not in flight (two frames
 * at most), and very likely dead -- a texture whose content changes every
 * frame gets a new key each time. Freed, the index rebuilt, and the
 * generation bumped so the backend drops the pointers it keeps. */
static void tex_evict_old(VkrContext *c, uint32_t age)
{
    uint32_t i, k = 0, freed = 0;
    for (i = 0; i < c->ntexs; i++) {
        VkrTex *t = c->texs[i];
        if (t != c->white && c->flip_serial - t->last_use > age) {
            tex_free_one(c, t);
            freed++;
        } else
            c->texs[k++] = t;
    }
    if (!freed)
        return;
    c->ntexs = k;
    c->tex_gen++;
    tex_rehash(c);
}

void vkr_destroy(VkrContext *c)
{
    if (!c) return;
    free_target(c);
    vkDeviceWaitIdle(c->dev);
    tex_free_all(c);
    for (int i = 0; i < c->npipes; i++)
        vkDestroyPipeline(c->dev, c->pipes[i].pipe, NULL);
    for (int i = 0; i < c->nsamplers; i++)
        vkDestroySampler(c->dev, c->samplers[i].s, NULL);
    vkDestroyShaderModule(c->dev, c->vs, NULL);
    vkDestroyShaderModule(c->dev, c->fs, NULL);
    vkDestroyShaderModule(c->dev, c->fs_wdepth, NULL);
    vkDestroyShaderModule(c->dev, c->vs_vsh, NULL);
    vkDestroyPipelineLayout(c->dev, c->layout, NULL);
    vkDestroyDescriptorSetLayout(c->dev, c->dsl, NULL);
    if (c->crt_pipe) vkDestroyPipeline(c->dev, c->crt_pipe, NULL);
    if (c->crt_samp) vkDestroySampler(c->dev, c->crt_samp, NULL);
    if (c->crt_layout) vkDestroyPipelineLayout(c->dev, c->crt_layout, NULL);
    if (c->crt_dsl) vkDestroyDescriptorSetLayout(c->dev, c->crt_dsl, NULL);
    if (c->crt_vs) vkDestroyShaderModule(c->dev, c->crt_vs, NULL);
    if (c->crt_fs) vkDestroyShaderModule(c->dev, c->crt_fs, NULL);
    if (c->lin_samp) vkDestroySampler(c->dev, c->lin_samp, NULL);
    if (c->post_pipe) vkDestroyPipeline(c->dev, c->post_pipe, NULL);
    if (c->post_layout) vkDestroyPipelineLayout(c->dev, c->post_layout, NULL);
    if (c->post_dsl) vkDestroyDescriptorSetLayout(c->dev, c->post_dsl, NULL);
    if (c->post_fs) vkDestroyShaderModule(c->dev, c->post_fs, NULL);
    vkDestroyShaderModule(c->dev, c->fs_a2c, NULL);
    if (c->sc_pipe) vkDestroyPipeline(c->dev, c->sc_pipe, NULL);
    if (c->sc_pipe_ms) vkDestroyPipeline(c->dev, c->sc_pipe_ms, NULL);
    if (c->sc_ao_pipe) vkDestroyPipeline(c->dev, c->sc_ao_pipe, NULL);
    if (c->sc_layout) vkDestroyPipelineLayout(c->dev, c->sc_layout, NULL);
    if (c->sc_dsl) vkDestroyDescriptorSetLayout(c->dev, c->sc_dsl, NULL);
    if (c->sc_near) vkDestroySampler(c->dev, c->sc_near, NULL);
    for (int i = 0; i < 4; i++)
        if (c->sc_fs[i]) vkDestroyShaderModule(c->dev, c->sc_fs[i], NULL);
    if (c->soft_pipe) vkDestroyPipeline(c->dev, c->soft_pipe, NULL);
    if (c->soft_layout) vkDestroyPipelineLayout(c->dev, c->soft_layout, NULL);
    if (c->soft_dsl) vkDestroyDescriptorSetLayout(c->dev, c->soft_dsl, NULL);
    if (c->soft_fs) vkDestroyShaderModule(c->dev, c->soft_fs, NULL);
    if (c->soft_samp) vkDestroySampler(c->dev, c->soft_samp, NULL);
    for (int i = 0; i < c->nrts; i++) {
        VkrRT *r = &c->rts[i];
        if (r->soft) {
            vkDestroyImageView(c->dev, r->soft_view, NULL);
            vkDestroyImage(c->dev, r->soft, NULL);
            vkFreeMemory(c->dev, r->soft_mem, NULL);
        }
        vkDestroyImageView(c->dev, r->color_view, NULL);
        vkDestroyImageView(c->dev, r->depth_view, NULL);
        vkDestroyImage(c->dev, r->color, NULL);
        vkDestroyImage(c->dev, r->depth, NULL);
        vkFreeMemory(c->dev, r->color_mem, NULL);
        vkFreeMemory(c->dev, r->depth_mem, NULL);
    }
    for (int i = 0; i < VKR_FRAMES; i++) {
        vkDestroyBuffer(c->dev, c->fr[i].buf, NULL);
        vkFreeMemory(c->dev, c->fr[i].buf_mem, NULL);
        vkDestroyFence(c->dev, c->fr[i].fence, NULL);
    }
    vkDestroyCommandPool(c->dev, c->pool, NULL);
    vkDestroyDevice(c->dev, NULL);
    vkDestroyInstance(c->inst, NULL);
    free(c);
}

static void end_pass(VkrContext *c);

/* Submit what has been recorded and wait for it. Texture uploads are in
 * there: dropping the commands left those textures without content, and
 * they stayed cached that way (magenta HUD in races after a resize). */
static int flush_wait(VkrContext *c)
{
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    if (!c->recording) return 1;
    end_pass(c);
    CK(vkEndCommandBuffer(c->cmd));
    c->recording = 0;
    si.commandBufferCount = 1; si.pCommandBuffers = &c->cmd;
    CK(vkQueueSubmit(c->queue, 1, &si, c->fence));
    CK(vkWaitForFences(c->dev, 1, &c->fence, VK_TRUE, UINT64_MAX));
    c->buf_used = 0;
    c->upload_serial++;
    return 1;
}

int vkr_set_target(VkrContext *c, uint32_t w, uint32_t h, float zmax)
{
    void *map;
    int f;
    if (c->color && !flush_wait(c))
        return 0;
    free_target(c);
    c->w = w; c->h = h; c->zmax = zmax;
    c->sw = (uint32_t)((float)w * c->scale_x + 0.5f);
    c->sh = (uint32_t)((float)h * c->scale + 0.5f);
    c->sc.offset.x = c->sc.offset.y = 0; c->sc.extent.width = c->sw; c->sc.extent.height = c->sh;
    c->gsc[0] = c->gsc[1] = 0; c->gsc[2] = w; c->gsc[3] = h;
    if (!make_image(c, VK_FORMAT_B8G8R8A8_UNORM,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                    | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, &c->color, &c->color_mem, &c->color_view,
                    c->sw, c->sh))
        return 0;
    if (c->msaa > 1) {
        if (!make_image_ms(c, VK_FORMAT_B8G8R8A8_UNORM,
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT, &c->msc, &c->msc_mem, &c->msc_view, c->sw, c->sh)
            || !make_image_ms(c, c->depth_fmt, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                              | VK_IMAGE_USAGE_SAMPLED_BIT, ds_aspect(c), &c->depth, &c->depth_mem,
                              &c->depth_view, c->sw, c->sh))
            return 0;
    } else if (!make_image(c, c->depth_fmt, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                           | VK_IMAGE_USAGE_SAMPLED_BIT, ds_aspect(c), &c->depth, &c->depth_mem,
                           &c->depth_view, c->sw, c->sh))
        return 0;
    {
        /* the scene pass samples depth: a depth-only view (a depth+stencil
         * view cannot be sampled) */
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = c->depth;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = c->depth_fmt;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        vi.subresourceRange.levelCount = 1; vi.subresourceRange.layerCount = 1;
        CK(vkCreateImageView(c->dev, &vi, NULL, &c->depth_sview));
    }
    if (c->scene) {
        c->sc_levels = 4;
        c->sc_aw = c->sw > 1 ? c->sw / 2 : 1; c->sc_ah = c->sh > 1 ? c->sh / 2 : 1;
        if (!make_image_ex(c, VK_FORMAT_B8G8R8A8_UNORM,
                           VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                           | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                           &c->sc_in, &c->sc_in_mem, &c->sc_in_view, c->sw, c->sh, c->sc_levels)
            || !make_image(c, VK_FORMAT_R16G16_SFLOAT,
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT, &c->sc_ao, &c->sc_ao_mem, &c->sc_ao_view,
                           c->sc_aw, c->sc_ah))
            return 0;
    }
    if (c->post) {
        uint32_t m = c->sw > c->sh ? c->sw : c->sh;
        for (c->post_levels = 1; m > 1 && c->post_levels < 8; m >>= 1)
            c->post_levels++;
        if (!make_image_ex(c, VK_FORMAT_B8G8R8A8_UNORM,
                           VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                           | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                           &c->post_in, &c->post_in_mem, &c->post_in_view, c->sw, c->sh, c->post_levels))
            return 0;
    }
    if ((c->scale != 1.0f || c->scale_x != 1.0f)
        && !make_image(c, VK_FORMAT_B8G8R8A8_UNORM,
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT, &c->down, &c->down_mem, &c->down_view, w, h))
        return 0;
    if (c->crt && (c->scale != 1.0f || c->scale_x != 1.0f)) {
        uint32_t m = c->sw > c->sh ? c->sw : c->sh;
        for (c->crt_levels = 1; m > 1; m >>= 1)
            c->crt_levels++;
        if (!make_image_ex(c, VK_FORMAT_B8G8R8A8_UNORM,
                           VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                           | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                           &c->crt_in, &c->crt_in_mem, &c->crt_in_view, c->sw, c->sh, c->crt_levels)
            || !make_image(c, VK_FORMAT_B8G8R8A8_UNORM,
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT, &c->crt_out, &c->crt_out_mem,
                           &c->crt_out_view, c->sw, c->sh))
            return 0;
    } else if (c->crt) {
        static int said;
        if (!said++)
            fprintf(stderr, "[VKR] CRT needs an internal resolution above the guest's (RECOMP_RES)\n");
    }
    for (f = 0; f < VKR_FRAMES; f++) {
        if (!make_buffer_ex(c, (VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            &c->fr[f].rb, &c->fr[f].rb_mem, &map, 1, &c->fr[f].rb_noncoherent))
            return 0;
        c->fr[f].rb_map = (uint8_t *)map;
        if (c->scale != 1.0f || c->scale_x != 1.0f) {
            if (!make_buffer_ex(c, (VkDeviceSize)c->sw * c->sh * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                &c->fr[f].rbh, &c->fr[f].rbh_mem, &map, 1, &c->fr[f].rbh_noncoherent))
                return 0;
            c->fr[f].rbh_map = (uint8_t *)map;
        }
    }
    if (c->scale != 1.0f || c->scale_x != 1.0f)
        fprintf(stderr, "[VKR] internal resolution %ux%u (x%.2f/x%.2f of %ux%u)%s\n", c->sw, c->sh,
                c->scale_x, c->scale, w, h, c->squeeze2d != 1.0f ? ", widescreen" : "");
    c->recording = c->in_pass = c->color_valid = 0;
    return 1;
}

/* RECOMP_VKR_GPUPROF: a timestamp once everything recorded so far has run;
 * the difference to the previous one is (about) that draw's GPU time. */
static void prof_mark(VkrContext *c, const VkrDrawState *st, uint32_t nidx, int vsh)
{
    VkrFrame *fr = &c->fr[c->cur];
    VkrProfDraw *m;
    if (c->prof_thr <= 0 || !fr->qreset || fr->nq >= VKR_PROF_MAX)
        return;
    m = &fr->meta[fr->nq];
    memset(m, 0, sizeof *m);
    if (st) {
        int k;
        m->nidx = nidx; m->topo = st->topology; m->vsh = (uint32_t)vsh;
        m->blend = (uint32_t)st->blend_enable; m->src = st->blend_src; m->dst = st->blend_dst;
        m->eq = st->blend_eq; m->mask = st->write_mask; m->stages = st->u.stage_program;
        m->final0 = st->u.final0; m->alpha = st->alpha_func;
        m->psize = st->point_params ? -1.0f : st->point_size;
        for (k = 0; k < 2; k++)
            if (st->tex[k]) { m->tw[k] = st->tex[k]->w; m->th[k] = st->tex[k]->h; }
        m->scw = c->sc.extent.width; m->sch = c->sc.extent.height;
    }
    vkCmdWriteTimestamp2(c->cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, fr->qp, fr->nq);
    fr->nq++;
}

static void prof_report(VkrContext *c, int slot)
{
    VkrFrame *fr = &c->fr[slot];
    static uint64_t ts[VKR_PROF_MAX];
    int top[8], k;
    uint32_t i, n = fr->nq;
    double total;
    if (c->prof_thr <= 0 || n < 2)
        return;
    if (vkGetQueryPoolResults(c->dev, fr->qp, 0, n, n * sizeof ts[0], ts, sizeof ts[0],
                              VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        return;
    total = (double)(ts[n - 1] - ts[0]) * c->ts_period / 1e6;
    {
        /* every 120 frames: the GPU frame time's average and maximum */
        static double sum, mx, ssc, spo, sdr;
        static unsigned nf;
        const uint32_t *qf = fr->qf;
        sum += total; nf++;
        if (total > mx) mx = total;
        if (qf[0] && qf[1] && qf[2] && qf[2] < n) {
            sdr += (double)(ts[qf[0]] - ts[0]) * c->ts_period / 1e6;
            ssc += (double)(ts[qf[1]] - ts[qf[0]]) * c->ts_period / 1e6;
            spo += (double)(ts[qf[2]] - ts[qf[1]]) * c->ts_period / 1e6;
        }
        if (nf == 120) {
            fprintf(stderr, "[GPUPROF] 120 frames: GPU avg %.2f ms, max %.2f ms (draws+resolve %.2f,"
                    " scene %.2f, post %.2f)\n", sum / nf, mx, sdr / nf, ssc / nf, spo / nf);
            sum = mx = ssc = spo = sdr = 0; nf = 0;
        }
    }
    if (total < c->prof_thr || c->prof_said >= 60)
        return;
    c->prof_said++;
    for (k = 0; k < 8; k++) top[k] = -1;
    for (i = 1; i < n; i++) {
        uint64_t d = ts[i] - ts[i - 1];
        for (k = 0; k < 8; k++) {
            if (top[k] < 0 || d > ts[top[k]] - ts[top[k] - 1]) {
                int j;
                for (j = 7; j > k; j--) top[j] = top[j - 1];
                top[k] = (int)i;
                break;
            }
        }
    }
    fprintf(stderr, "[GPUPROF] frame GPU %.1f ms, %u draws, target %ux%u; top draws:\n",
            total, n - 2, c->sw, c->sh);
    for (k = 0; k < 8 && top[k] > 0; k++) {
        const VkrProfDraw *m = &fr->meta[top[k]];
        fprintf(stderr, "[GPUPROF]   #%d %.2f ms %s n %u %s blend %u %X/%X eq %X mask %X"
                " stages %05X final0 %08X alpha %X tex %ux%u %ux%u clip %ux%u psize %.1f\n",
                top[k], (double)(ts[top[k]] - ts[top[k] - 1]) * c->ts_period / 1e6,
                m->topo == 2 ? "points" : m->topo == 1 ? "lines" : "tris", m->nidx,
                !m->nidx ? "-" : m->vsh ? "vsh" : "cpu", m->blend, m->src, m->dst, m->eq, m->mask,
                m->stages, m->final0, m->alpha, m->tw[0], m->th[0], m->tw[1], m->th[1],
                m->scw, m->sch, m->psize);
    }
}

static int begin_cmd(VkrContext *c)
{
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    if (c->recording) return 1;
    /* The frame's previous use must be rendered and read back. */
    pthread_mutex_lock(&c->lock);
    while (c->fr[c->cur].busy)
        pthread_cond_wait(&c->freed, &c->lock);
    pthread_mutex_unlock(&c->lock);
    CK(vkResetFences(c->dev, 1, &c->fence));
    CK(vkResetCommandBuffer(c->cmd, 0));
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    CK(vkBeginCommandBuffer(c->cmd, &bi));
    c->recording = 1;
    if (c->prof_thr > 0 && !c->fr[c->cur].qreset) {
        vkCmdResetQueryPool(c->cmd, c->fr[c->cur].qp, 0, VKR_PROF_MAX);
        c->fr[c->cur].qreset = 1;
        c->fr[c->cur].nq = 0;
        prof_mark(c, NULL, 0, -1);
    }
    barrier(c, c->color, VK_IMAGE_ASPECT_COLOR_BIT,
            c->color_valid ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    barrier(c, c->depth, ds_aspect(c),
            c->color_valid ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    if (c->msc)
        barrier(c, c->msc, VK_IMAGE_ASPECT_COLOR_BIT,
                c->color_valid ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    return 1;
}

/* A pass on render target c->cur_rt: its first use clears it (transparent
 * black, depth 1), later ones load; out of SHADER_READ when it was sampled. */
static void rt_begin_pass(VkrContext *c)
{
    VkrRT *r = &c->rts[c->cur_rt];
    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingAttachmentInfo da = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    VkViewport vp = {0, 0, (float)r->sw, (float)r->sh, 0.0f, 1.0f};
    VkRect2D sc = {{0, 0}, {r->sw, r->sh}};
    if (r->state != 1)
        barrier(c, r->color, VK_IMAGE_ASPECT_COLOR_BIT,
                r->state ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    if (!r->state)
        barrier(c, r->depth, ds_aspect(c),
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_NONE, 0,
                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    ca.imageView = r->color_view; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = r->state ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    da.imageView = r->depth_view; da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    da.loadOp = ca.loadOp; da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    da.clearValue.depthStencil.depth = 1.0f;
    ri.renderArea = sc; ri.layerCount = 1;
    ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca; ri.pDepthAttachment = &da;
    if (c->has_stencil)
        ri.pStencilAttachment = &da;
    vkCmdBeginRendering(c->cmd, &ri);
    vkCmdSetViewport(c->cmd, 0, 1, &vp);
    vkCmdSetScissor(c->cmd, 0, 1, &c->sc);
    r->state = 1;
    r->dirty = 1;
    c->in_pass = 1;
}

static void begin_pass(VkrContext *c)
{
    VkRenderingAttachmentInfo ca[1] = {{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}};
    VkRenderingAttachmentInfo da = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    VkViewport vp = {0, 0, (float)c->sw, (float)c->sh, 0.0f, 1.0f};
    VkRect2D sc = {{0, 0}, {c->sw, c->sh}};   /* render area: the whole target */
    if (c->in_pass) return;
    if (c->cur_rt >= 0) {
        rt_begin_pass(c);
        return;
    }
    ca[0].imageView = c->msc ? c->msc_view : c->color_view;
    ca[0].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca[0].loadOp = c->color_valid ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
    ca[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    da.imageView = c->depth_view; da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    da.loadOp = c->color_valid ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
    da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    da.clearValue.depthStencil.depth = 1.0f;
    ri.renderArea = sc; ri.layerCount = 1;
    ri.colorAttachmentCount = 1; ri.pColorAttachments = ca; ri.pDepthAttachment = &da;
    if (c->has_stencil)
        ri.pStencilAttachment = &da;
    vkCmdBeginRendering(c->cmd, &ri);
    vkCmdSetViewport(c->cmd, 0, 1, &vp);
    vkCmdSetScissor(c->cmd, 0, 1, &c->sc);
    c->in_pass = 1;
    c->color_valid = 1;
}

static void end_pass(VkrContext *c)
{
    if (!c->in_pass)
        return;
    vkCmdEndRendering(c->cmd);
    c->in_pass = 0;
    if (c->cur_rt >= 0) {               /* ready to be sampled */
        VkrRT *r = &c->rts[c->cur_rt];
        barrier(c, r->color, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        r->state = 2;
    }
}

/* RECOMP_VKR_GPU_RT: draws and clears go to the render target for guest
 * surface off (w x h, the surface's full size), made on first use. */
int vkr_rt_bind(VkrContext *c, uint32_t off, uint32_t w, uint32_t h, float zmax)
{
    int i;
    VkrRT *r;
    for (i = 0; i < c->nrts; i++)
        if (c->rts[i].off == off && c->rts[i].gw == w && c->rts[i].gh == h)
            break;
    if (i == c->nrts) {
        if (c->nrts == VKR_MAX_RT || !w || !h || w > 1024 || h > 1024)
            return 0;
        r = &c->rts[i];
        memset(r, 0, sizeof *r);
        r->off = off; r->gw = w; r->gh = h;
        r->sw = (uint32_t)((float)w * c->rt_scale + 0.5f);
        r->sh = (uint32_t)((float)h * c->rt_scale + 0.5f);
        if (!make_image(c, VK_FORMAT_B8G8R8A8_UNORM,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT, &r->color, &r->color_mem, &r->color_view, r->sw, r->sh)
            || !make_image(c, c->depth_fmt, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                           ds_aspect(c), &r->depth, &r->depth_mem, &r->depth_view,
                           r->sw, r->sh))
            return 0;
        r->tex.w = r->sw; r->tex.h = r->sh; r->tex.levels = 1;
        r->tex.img = r->color; r->tex.view = r->color_view;
        if (c->rt_soft > 0.0f) {
            r->ssw = r->sw > 2 ? r->sw / 2 : 1; r->ssh = r->sh > 2 ? r->sh / 2 : 1;
            if (!make_image(c, VK_FORMAT_B8G8R8A8_UNORM,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT, &r->soft, &r->soft_mem, &r->soft_view,
                            r->ssw, r->ssh))
                return 0;
            r->soft_tex.w = r->ssw; r->soft_tex.h = r->ssh; r->soft_tex.levels = 1;
            r->soft_tex.img = r->soft; r->soft_tex.view = r->soft_view;
        }
        c->nrts++;
        fprintf(stderr, "[VKR] render target %08X %ux%u on the GPU (%ux%u)\n", off, w, h, r->sw, r->sh);
    }
    c->rts[i].zmax = zmax;
    if (c->cur_rt != i) {
        end_pass(c);
        c->cur_rt = i;
    }
    return 1;
}

/* Draws go to the main target again. */
void vkr_use_main(VkrContext *c)
{
    if (c->cur_rt < 0)
        return;
    end_pass(c);
    c->cur_rt = -1;
}

static int begin_cmd(VkrContext *c);

/* RECOMP_SHADOW_SOFT: rt_soft.frag from r's sharp image into r->soft, between
 * passes (the current one is ended; the next draw starts a new one). */
static int rt_soften(VkrContext *c, VkrRT *r)
{
    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    VkDescriptorImageInfo dii = {c->soft_samp, r->color_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    VkViewport vp = {0, 0, (float)r->ssw, (float)r->ssh, 0.0f, 1.0f};
    VkRect2D sc = {{0, 0}, {r->ssw, r->ssh}};
    struct { float inv_dst[2], inv_src[2], radius, thr; } pc;
    if (!begin_cmd(c))
        return 0;
    end_pass(c);
    barrier(c, r->soft, VK_IMAGE_ASPECT_COLOR_BIT,
            r->soft_state ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    pc.inv_dst[0] = 1.0f / (float)r->ssw; pc.inv_dst[1] = 1.0f / (float)r->ssh;
    pc.inv_src[0] = 1.0f / (float)r->sw; pc.inv_src[1] = 1.0f / (float)r->sh;
    pc.radius = c->rt_soft * c->rt_scale;
    pc.thr = 20.0f / 255.0f;
    ca.imageView = r->soft_view; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ri.renderArea = sc; ri.layerCount = 1;
    ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
    vkCmdBeginRendering(c->cmd, &ri);
    vkCmdSetViewport(c->cmd, 0, 1, &vp);
    vkCmdSetScissor(c->cmd, 0, 1, &sc);
    vkCmdBindPipeline(c->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c->soft_pipe);
    w.dstBinding = 0; w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &dii;
    vkCmdPushDescriptorSet(c->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c->soft_layout, 0, 1, &w);
    vkCmdPushConstants(c->cmd, c->soft_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    vkCmdDraw(c->cmd, 3, 1, 0, 0);
    vkCmdEndRendering(c->cmd);
    barrier(c, r->soft, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    r->soft_state = 1;
    r->dirty = 0;
    return 1;
}

/* The render target for guest surface off as a texture (its soft copy with
 * RECOMP_SHADOW_SOFT); NULL if it has never been drawn. */
VkrTex *vkr_rt_texture(VkrContext *c, uint32_t off, uint32_t w, uint32_t h)
{
    int i;
    for (i = 0; i < c->nrts; i++) {
        VkrRT *r = &c->rts[i];
        if (r->off != off || r->gw != w || r->gh != h || !r->state)
            continue;
        if (c->rt_soft > 0.0f && r->soft && i != c->cur_rt) {
            if (r->state == 1 || (r->dirty && !rt_soften(c, r)))
                return &r->tex;
            return r->soft_state ? &r->soft_tex : &r->tex;
        }
        return &r->tex;
    }
    return NULL;
}

/* Draws and clears stay inside the NV2A clip rectangle; the target stays the
 * whole surface (a smaller clip must not recreate it mid-frame). */
static void apply_scissor(VkrContext *c)
{
    uint32_t x = c->gsc[0], y = c->gsc[1], w = c->gsc[2], h = c->gsc[3];
    if (c->cur_rt >= 0) {
        const VkrRT *r = &c->rts[c->cur_rt];
        float fx = (float)r->sw / (float)r->gw, fy = (float)r->sh / (float)r->gh;
        uint32_t x1, y1;
        if (x >= r->gw || y >= r->gh) { x = y = 0; w = h = 0; }
        if (x + w > r->gw) w = r->gw - x;
        if (y + h > r->gh) h = r->gh - y;
        x1 = (uint32_t)((float)(x + w) * fx + 0.5f); y1 = (uint32_t)((float)(y + h) * fy + 0.5f);
        c->sc.offset.x = (int32_t)((float)x * fx + 0.5f); c->sc.offset.y = (int32_t)((float)y * fy + 0.5f);
        c->sc.extent.width = x1 - (uint32_t)c->sc.offset.x;
        c->sc.extent.height = y1 - (uint32_t)c->sc.offset.y;
        if (c->in_pass)
            vkCmdSetScissor(c->cmd, 0, 1, &c->sc);
        return;
    }
    if (x >= c->w || y >= c->h) { x = y = 0; w = h = 0; }
    if (x + w > c->w) w = c->w - x;
    if (y + h > c->h) h = c->h - y;
    if (c->scale != 1.0f || c->scale_x != 1.0f) {   /* guest pixels -> target pixels */
        /* Pillarbox: the guest frame is the 4:3 middle of the target. */
        float sx = c->squeeze_all > 0.5f ? c->scale : c->scale_x;
        float ox = c->squeeze_all > 0.5f ? ((float)c->sw - (float)c->w * c->scale) * 0.5f : 0.0f;
        uint32_t x1 = (uint32_t)(ox + (float)(x + w) * sx + 0.5f);
        uint32_t y1 = (uint32_t)((float)(y + h) * c->scale + 0.5f);
        x = (uint32_t)(ox + (float)x * sx + 0.5f);
        y = (uint32_t)((float)y * c->scale + 0.5f);
        if (x1 > c->sw) x1 = c->sw;
        if (y1 > c->sh) y1 = c->sh;
        w = x1 > x ? x1 - x : 0;
        h = y1 > y ? y1 - y : 0;
    }
    if (c->sc.offset.x == (int32_t)x && c->sc.offset.y == (int32_t)y
        && c->sc.extent.width == w && c->sc.extent.height == h)
        return;
    c->sc.offset.x = (int32_t)x; c->sc.offset.y = (int32_t)y;
    c->sc.extent.width = w; c->sc.extent.height = h;
    if (c->in_pass)
        vkCmdSetScissor(c->cmd, 0, 1, &c->sc);
}

void vkr_set_scissor(VkrContext *c, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    c->gsc[0] = x; c->gsc[1] = y; c->gsc[2] = w; c->gsc[3] = h;
    apply_scissor(c);
}

void vkr_clear(VkrContext *c, const float rgba[4], int clear_depth, float depth)
{
    VkClearAttachment ca[2];
    VkClearRect cr = {c->sc, 0, 1};
    uint32_t n = 1;
    if (!begin_cmd(c)) return;
    begin_pass(c);
    ca[0].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; ca[0].colorAttachment = 0;
    memcpy(ca[0].clearValue.color.float32, rgba, 16);
    if (clear_depth) {
        ca[n].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT; ca[n].colorAttachment = 0;
        ca[n].clearValue.depthStencil.depth = depth; ca[n].clearValue.depthStencil.stencil = 0;
        n++;
    }
    vkCmdClearAttachments(c->cmd, n, ca, 1, &cr);
}

void vkr_clear_ds(VkrContext *c, int clear_depth, float depth, int clear_stencil, uint32_t stencil)
{
    VkClearAttachment ca = {0};
    VkClearRect cr = {c->sc, 0, 1};
    if (clear_depth)
        ca.aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if (clear_stencil && c->has_stencil)
        ca.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
    if (!ca.aspectMask) return;
    if (!begin_cmd(c)) return;
    begin_pass(c);
    ca.clearValue.depthStencil.depth = depth;
    ca.clearValue.depthStencil.stencil = stencil;
    vkCmdClearAttachments(c->cmd, 1, &ca, 1, &cr);
}

VkrTex *vkr_tex_find(VkrContext *c, uint64_t key)
{
    uint32_t h;
    if (!c->hcap)
        return NULL;
    for (h = tex_slot(key, c->hcap); c->hidx[h] >= 0; h = (h + 1) & (c->hcap - 1)) {
        VkrTex *t = c->texs[c->hidx[h]];
        if (t->key == key) {
            t->last_use = c->flip_serial;
            return t;
        }
    }
    return NULL;
}

/* New contents for an existing texture of the same size (a render target
 * the title redraws every frame: its shadow maps). Draws recorded before
 * this still read the old contents -- the copy is ordered after them. */
int vkr_tex_update(VkrContext *c, VkrTex *t, uint64_t key, const uint32_t *pixels)
{
    VkBufferImageCopy cp = {0};
    uint32_t off = (c->buf_used + 15) & ~15u, bytes = t->w * t->h * 4;
    if (off + bytes > VKR_BUF_BYTES || !begin_cmd(c))
        return 0;
    end_pass(c);
    memcpy(c->buf_map + off, pixels, bytes);
    c->buf_used = off + bytes;
    barrier(c, t->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    cp.bufferOffset = off;
    cp.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    cp.imageSubresource.layerCount = 1;
    cp.imageExtent.width = t->w; cp.imageExtent.height = t->h; cp.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(c->cmd, c->buf, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    barrier(c, t->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    t->last_use = c->flip_serial;
    if (t->key != key) {
        t->key = key;
        tex_rehash(c);                  /* the index is by key */
    }
    return 1;
}

VkrTex *vkr_tex_create(VkrContext *c, uint64_t key, uint32_t w, uint32_t h,
                       const uint32_t *pixels)
{
    return vkr_tex_create_ex(c, key, w, h, pixels, 0);
}

/* mips: build the mip chain (blits from level 0); not for textures updated
 * in place (vkr_tex_update writes level 0 only) or stacked cube faces. */
VkrTex *vkr_tex_create_ex(VkrContext *c, uint64_t key, uint32_t w, uint32_t h,
                          const uint32_t *pixels, int mips)
{
    VkrTex *t = (VkrTex *)calloc(1, sizeof *t);
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    VkMemoryRequirements mr;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    VkBufferImageCopy cp = {0};
    uint32_t off = (c->buf_used + 15) & ~15u, bytes = w * h * 4, levels = 1, l;
    int m;

    if (!t || !w || !h || off + bytes > VKR_BUF_BYTES) { free(t); return NULL; }
    if (mips && c->mips)
        for (l = w > h ? w : h; l > 1; l >>= 1)
            levels++;
    ii.imageType = VK_IMAGE_TYPE_2D; ii.format = VK_FORMAT_B8G8R8A8_UNORM;
    ii.extent.width = w; ii.extent.height = h; ii.extent.depth = 1;
    ii.mipLevels = levels; ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(c->dev, &ii, NULL, &t->img) != VK_SUCCESS) { free(t); return NULL; }
    vkGetImageMemoryRequirements(c->dev, t->img, &mr);
    m = find_mem(c, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    ai.allocationSize = mr.size; ai.memoryTypeIndex = (uint32_t)(m < 0 ? 0 : m);
    if (vkAllocateMemory(c->dev, &ai, NULL, &t->mem) != VK_SUCCESS) {
        vkDestroyImage(c->dev, t->img, NULL); free(t); return NULL;
    }
    vkBindImageMemory(c->dev, t->img, t->mem, 0);
    vi.image = t->img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = ii.format;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = levels; vi.subresourceRange.layerCount = 1;
    vkCreateImageView(c->dev, &vi, NULL, &t->view);
    t->key = key; t->w = w; t->h = h; t->levels = levels;

    if (!begin_cmd(c)) return NULL;
    end_pass(c);                         /* copies are not allowed inside a pass */
    memcpy(c->buf_map + off, pixels, bytes);
    c->buf_used = off + bytes;
    barrier_mips(c, t->img, 0, levels, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                 VK_ACCESS_2_TRANSFER_WRITE_BIT);
    cp.bufferOffset = off;
    cp.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    cp.imageSubresource.layerCount = 1;
    cp.imageExtent.width = w; cp.imageExtent.height = h; cp.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(c->cmd, c->buf, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    {
        int32_t mw = (int32_t)w, mh = (int32_t)h;
        for (l = 1; l < levels; l++) {
            VkImageBlit bl = {0};
            barrier_mips(c, t->img, l - 1, 1,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            bl.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; bl.srcSubresource.layerCount = 1;
            bl.srcSubresource.mipLevel = l - 1;
            bl.dstSubresource = bl.srcSubresource; bl.dstSubresource.mipLevel = l;
            bl.srcOffsets[1].x = mw; bl.srcOffsets[1].y = mh; bl.srcOffsets[1].z = 1;
            mw = mw > 1 ? mw / 2 : 1; mh = mh > 1 ? mh / 2 : 1;
            bl.dstOffsets[1].x = mw; bl.dstOffsets[1].y = mh; bl.dstOffsets[1].z = 1;
            vkCmdBlitImage(c->cmd, t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
        }
        if (levels > 1)
            barrier_mips(c, t->img, 0, levels - 1,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }
    barrier_mips(c, t->img, levels - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    if (levels > 1)
        bytes += bytes / 3;

    if (c->ntexs == c->captexs) {
        c->captexs = c->captexs ? c->captexs * 2 : 256;
        c->texs = (VkrTex **)realloc(c->texs, c->captexs * sizeof *c->texs);
    }
    t->last_use = c->flip_serial;
    c->texs[c->ntexs++] = t;
    c->tex_bytes += bytes;
    if (c->ntexs * 2 + 2 > c->hcap)
        tex_rehash(c);
    else {
        uint32_t h = tex_slot(key, c->hcap);
        while (c->hidx[h] >= 0)
            h = (h + 1) & (c->hcap - 1);
        c->hidx[h] = (int32_t)(c->ntexs - 1);
    }
    return t;
}

/* NV2A stencil state -> dynamic Vulkan stencil state (both faces). */
static VkStencilOp stencil_op(uint32_t gl)
{
    switch (gl) {
    case 0x0000: return VK_STENCIL_OP_ZERO;
    case 0x1E01: return VK_STENCIL_OP_REPLACE;
    case 0x1E02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 0x1E03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 0x150A: return VK_STENCIL_OP_INVERT;
    case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default:     return VK_STENCIL_OP_KEEP;
    }
}

static void set_stencil(VkrContext *c, const VkrDrawState *st)
{
    VkStencilFaceFlags f = VK_STENCIL_FACE_FRONT_AND_BACK;
    int on = c->has_stencil && st->stencil_test;
    vkCmdSetStencilTestEnable(c->cmd, on ? VK_TRUE : VK_FALSE);
    if (!c->has_stencil)
        return;
    vkCmdSetStencilOp(c->cmd, f, stencil_op(st->stencil_fail), stencil_op(st->stencil_zpass),
                      stencil_op(st->stencil_zfail),
                      st->stencil_func >= 0x200 && st->stencil_func <= 0x207
                          ? (VkCompareOp)(st->stencil_func - 0x200) : VK_COMPARE_OP_ALWAYS);
    vkCmdSetStencilCompareMask(c->cmd, f, st->stencil_read_mask);
    vkCmdSetStencilWriteMask(c->cmd, f, on ? st->stencil_write_mask : 0);
    vkCmdSetStencilReference(c->cmd, f, st->stencil_ref);
}

/* The draw's push constants, for the current target (the main one or a
 * render target): sizes, depth range, fog, point scale, 2D squeeze, alpha
 * test, W-buffer scale, point sprite, sample count and flags. */
static void push_draw(VkrContext *c, const VkrDrawState *st)
{
    float pcv[8], gw = (float)c->w, gh = (float)c->h, zmax = c->zmax, scale = c->scale;
    float sq[2] = {c->squeeze2d, c->squeeze_all};
    uint32_t pcf[5], samples = c->msaa > 1 ? c->msaa : 1;
    if (c->cur_rt >= 0) {
        const VkrRT *r = &c->rts[c->cur_rt];
        gw = (float)r->gw; gh = (float)r->gh; zmax = r->zmax > 0.0f ? r->zmax : 1.0f;
        scale = c->rt_scale;
        sq[0] = 1.0f; sq[1] = 0.0f;
        samples = 1;
    }
    pcv[0] = 1.0f / gw; pcv[1] = 1.0f / gh;
    pcv[2] = 1.0f / zmax;
    pcv[3] = st->u.fog_enable ? st->u.fog_param[2] : 0.0f;
    pcv[4] = st->u.fog_param[0]; pcv[5] = st->u.fog_param[1];
    pcv[6] = st->point_size * scale; pcv[7] = st->point_params ? scale : 0.0f;
    vkCmdPushConstants(c->cmd, c->layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 32, pcv);
    vkCmdPushConstants(c->cmd, c->layout, VK_SHADER_STAGE_VERTEX_BIT, 48, 8, sq);
    pcf[0] = st->alpha_func; pcf[1] = st->alpha_ref;
    {
        /* W-buffering: depth is the interpolated clip w in depth-buffer
         * units, scaled into 0..1 like z. */
        float ws = st->w_buffer ? 1.0f / zmax : 0.0f;
        memcpy(&pcf[2], &ws, 4);
    }
    pcf[3] = st->topology == VKR_TOPO_POINTS && st->point_sprite;
    pcf[4] = samples << 8;              /* xbox.frag flags */
    if (c->texsmooth && !c->ab_off)
        pcf[4] |= 0x10000u;
    vkCmdPushConstants(c->cmd, c->layout, VK_SHADER_STAGE_FRAGMENT_BIT, 64, 20, pcf);
}

int vkr_draw(VkrContext *c, const VkrVertex *v, uint32_t nverts,
             const uint32_t *idx, uint32_t nidx, const VkrDrawState *st)
{
    uint32_t vb, ib, ub, end, k;
    VkDeviceSize off;
    VkPipeline pipe;
    VkDescriptorImageInfo di[4];
    VkDescriptorBufferInfo bi;
    VkWriteDescriptorSet w[2];

    pipe = get_pipeline(c, st, 0);
    if (!pipe) return 0;
    if (!c->white) {
        static const uint32_t white = 0xFFFFFFFFu;
        c->white = vkr_tex_create(c, 0x77686974ull, 1, 1, &white);
        if (!c->white) return 0;
    }
    if (!begin_cmd(c)) return 0;
    begin_pass(c);
    vb = (c->buf_used + 15) & ~15u;
    ib = vb + nverts * (uint32_t)sizeof *v;
    ub = (ib + nidx * 4 + 255) & ~255u;
    end = ub + (uint32_t)sizeof st->u;
    if (end > VKR_BUF_BYTES) {
        fprintf(stderr, "[VKR] upload buffer full\n");
        return 0;
    }
    off = vb;
    memcpy(c->buf_map + vb, v, nverts * sizeof *v);
    memcpy(c->buf_map + ib, idx, nidx * 4);
    memcpy(c->buf_map + ub, &st->u, sizeof st->u);
    c->buf_used = end;

    vkCmdBindPipeline(c->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    push_draw(c, st);

    for (k = 0; k < 4; k++) {
        VkrTex *t = st->tex[k] ? st->tex[k] : c->white;
        di[k].sampler = get_sampler(c, st->addr_u[k], st->addr_v[k], st->linear[k], t->levels > 1);
        di[k].imageView = t->view;
        di[k].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    bi.buffer = c->buf; bi.offset = ub; bi.range = sizeof st->u;
    memset(w, 0, sizeof w);
    w[0].sType = w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[0].dstBinding = 0; w[0].descriptorCount = 4;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo = di;
    w[1].dstBinding = 4; w[1].descriptorCount = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[1].pBufferInfo = &bi;
    vkCmdPushDescriptorSet(c->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c->layout, 0, 2, w);

    vkCmdSetDepthTestEnable(c->cmd, st->depth_test ? VK_TRUE : VK_FALSE);
    vkCmdSetDepthWriteEnable(c->cmd, st->depth_write ? VK_TRUE : VK_FALSE);
    vkCmdSetDepthCompareOp(c->cmd, st->depth_func >= 0x200 && st->depth_func <= 0x207
                                       ? (VkCompareOp)(st->depth_func - 0x200)
                                       : VK_COMPARE_OP_LESS_OR_EQUAL);
    set_stencil(c, st);
    vkCmdSetBlendConstants(c->cmd, st->blend_color);
    vkCmdBindVertexBuffers(c->cmd, 0, 1, &c->buf, &off);
    vkCmdBindIndexBuffer(c->cmd, c->buf, ib, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(c->cmd, nidx, 1, 0, 0, 0);
    prof_mark(c, st, nidx, 0);
    return 1;
}

/* The same draw with the vertex program run on the GPU (xbox_vsh.vert): in is
 * nverts x nin float4 inputs; prog is nins x 8 words (nv2a_vsh_export);
 * consts the 192 constants. The program and constants are uploaded once per
 * recording while they stay the same. */
int vkr_draw_vsh(VkrContext *c, const float *in, uint32_t nverts, uint32_t nin,
                 const uint32_t *prog, uint32_t nins, uint32_t prog_hash,
                 const float (*consts)[4], const uint32_t *idx, uint32_t nidx,
                 const VkrDrawState *st)
{
    uint32_t vb, ib, ub, end, k, po, co;
    uint32_t in_bytes = nverts * (nin ? nin : 1) * 16, prog_bytes = nins * 32;
    VkPipeline pipe;
    uint32_t pcu[4];
    VkDescriptorImageInfo di[4];
    VkDescriptorBufferInfo bi, sb;
    VkWriteDescriptorSet w[3];

    pipe = get_pipeline(c, st, 1);
    if (!pipe) return 0;
    if (!c->white) {
        static const uint32_t white = 0xFFFFFFFFu;
        c->white = vkr_tex_create(c, 0x77686974ull, 1, 1, &white);
        if (!c->white) return 0;
    }
    if (!begin_cmd(c)) return 0;
    begin_pass(c);
    if (c->vsh_serial != c->upload_serial) {
        c->vsh_serial = c->upload_serial;
        c->vsh_const_off = c->vsh_prog_off = 0xFFFFFFFFu;
    }
    end = (c->buf_used + 15) & ~15u;
    co = c->vsh_const_off;
    if (co == 0xFFFFFFFFu || memcmp(c->vsh_consts, consts, sizeof c->vsh_consts)) {
        co = end;
        end += (uint32_t)sizeof c->vsh_consts;
    }
    po = c->vsh_prog_off;
    if (po == 0xFFFFFFFFu || c->vsh_prog_hash != prog_hash || c->vsh_prog_n != nins) {
        po = end;
        end += prog_bytes;
    }
    vb = end;
    ib = vb + in_bytes;
    ub = (ib + nidx * 4 + 255) & ~255u;
    end = ub + (uint32_t)sizeof st->u;
    if (end > VKR_BUF_BYTES) {
        fprintf(stderr, "[VKR] upload buffer full\n");
        return 0;
    }
    if (co != c->vsh_const_off) {
        memcpy(c->buf_map + co, consts, sizeof c->vsh_consts);
        memcpy(c->vsh_consts, consts, sizeof c->vsh_consts);
        c->vsh_const_off = co;
    }
    if (po != c->vsh_prog_off) {
        memcpy(c->buf_map + po, prog, prog_bytes);
        c->vsh_prog_off = po;
        c->vsh_prog_hash = prog_hash;
        c->vsh_prog_n = nins;
    }
    memcpy(c->buf_map + vb, in, in_bytes);
    memcpy(c->buf_map + ib, idx, nidx * 4);
    memcpy(c->buf_map + ub, &st->u, sizeof st->u);
    c->buf_used = end;

    vkCmdBindPipeline(c->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    push_draw(c, st);
    pcu[0] = vb / 16; pcu[1] = nin ? nin : 1; pcu[2] = po / 16; pcu[3] = co / 16;
    vkCmdPushConstants(c->cmd, c->layout, VK_SHADER_STAGE_VERTEX_BIT, 32, 16, pcu);

    for (k = 0; k < 4; k++) {
        VkrTex *t = st->tex[k] ? st->tex[k] : c->white;
        di[k].sampler = get_sampler(c, st->addr_u[k], st->addr_v[k], st->linear[k], t->levels > 1);
        di[k].imageView = t->view;
        di[k].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    bi.buffer = c->buf; bi.offset = ub; bi.range = sizeof st->u;
    sb.buffer = c->buf; sb.offset = 0; sb.range = VK_WHOLE_SIZE;
    memset(w, 0, sizeof w);
    w[0].sType = w[1].sType = w[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[0].dstBinding = 0; w[0].descriptorCount = 4;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo = di;
    w[1].dstBinding = 4; w[1].descriptorCount = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[1].pBufferInfo = &bi;
    w[2].dstBinding = 5; w[2].descriptorCount = 1;
    w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[2].pBufferInfo = &sb;
    vkCmdPushDescriptorSet(c->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c->layout, 0, 3, w);

    vkCmdSetDepthTestEnable(c->cmd, st->depth_test ? VK_TRUE : VK_FALSE);
    vkCmdSetDepthWriteEnable(c->cmd, st->depth_write ? VK_TRUE : VK_FALSE);
    vkCmdSetDepthCompareOp(c->cmd, st->depth_func >= 0x200 && st->depth_func <= 0x207
                                       ? (VkCompareOp)(st->depth_func - 0x200)
                                       : VK_COMPARE_OP_LESS_OR_EQUAL);
    set_stencil(c, st);
    vkCmdSetBlendConstants(c->cmd, st->blend_color);
    vkCmdBindIndexBuffer(c->cmd, c->buf, ib, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(c->cmd, nidx, 1, 0, 0, 0);
    prof_mark(c, st, nidx, 1);
    return 1;
}

/* Widescreen target: 1 = pillarbox (narrow every vertex to the 4:3 middle),
 * 0 = widescreen (only 2D ones). */
void vkr_set_pillarbox(VkrContext *c, int on)
{
    float v = on && c->squeeze2d != 1.0f ? 1.0f : 0.0f;
    if (v != c->squeeze_all) {
        c->squeeze_all = v;
        apply_scissor(c);
    }
}

/* The target's shape (width / height): wider than 4:3 widens the target
 * and narrows 2D back to 4:3; anything else is the plain 4:3 target. Takes
 * effect at the next vkr_set_target. */
void vkr_set_aspect(VkrContext *c, float aspect)
{
    c->scale_x = c->scale;
    c->squeeze2d = 1.0f;
    if (aspect > 4.0f / 3.0f + 0.01f) {
        float f = aspect * 0.75f;
        c->scale_x = c->scale * f;
        c->squeeze2d = 1.0f / f;
    }
}

uint32_t vkr_width(const VkrContext *c) { return c->w; }
uint32_t vkr_height(const VkrContext *c) { return c->h; }

/* At a flip, c->color in TRANSFER_SRC: crt_in = c->color with mips, then
 * crt.frag into crt_out, left in TRANSFER_SRC for the read-back. */
static void crt_pass(VkrContext *c)
{
    VkImageCopy ic = {0};
    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    VkDescriptorImageInfo dii = {c->crt_samp, c->crt_in_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    VkViewport vp;
    VkRect2D sc = {{0, 0}, {c->sw, c->sh}};
    struct { float rect[4], tsize[2], curv, scan, mask, vig, ab, glow, bright, time,
             mpitch, lines, flicker, noise; } pc;
    int32_t mw = (int32_t)c->sw, mh = (int32_t)c->sh;
    uint32_t i;

    barrier_mips(c, c->crt_in, 0, c->crt_levels,
                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    ic.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; ic.srcSubresource.layerCount = 1;
    ic.dstSubresource = ic.srcSubresource;
    ic.extent.width = c->sw; ic.extent.height = c->sh; ic.extent.depth = 1;
    vkCmdCopyImage(c->cmd, c->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   c->crt_in, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
    for (i = 1; i < c->crt_levels; i++) {
        VkImageBlit bl = {0};
        barrier_mips(c, c->crt_in, i - 1, 1,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        bl.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; bl.srcSubresource.layerCount = 1;
        bl.srcSubresource.mipLevel = i - 1;
        bl.dstSubresource = bl.srcSubresource; bl.dstSubresource.mipLevel = i;
        bl.srcOffsets[1].x = mw; bl.srcOffsets[1].y = mh; bl.srcOffsets[1].z = 1;
        mw = mw > 1 ? mw / 2 : 1; mh = mh > 1 ? mh / 2 : 1;
        bl.dstOffsets[1].x = mw; bl.dstOffsets[1].y = mh; bl.dstOffsets[1].z = 1;
        vkCmdBlitImage(c->cmd, c->crt_in, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       c->crt_in, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
    }
    barrier_mips(c, c->crt_in, c->crt_levels - 1, 1,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    barrier_mips(c, c->crt_in, 0, c->crt_levels,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    barrier(c, c->crt_out, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    /* The picture: the 4:3 middle when pillarboxed, else the whole target. */
    pc.rect[2] = c->squeeze_all > 0.5f ? (float)c->w * c->scale : (float)c->sw;
    pc.rect[3] = (float)c->sh;
    pc.rect[0] = ((float)c->sw - pc.rect[2]) * 0.5f;
    pc.rect[1] = 0.0f;
    pc.tsize[0] = (float)c->sw; pc.tsize[1] = (float)c->sh;
    pc.curv = c->crt_p[0];
    pc.scan = c->crt_p[1]; pc.mask = c->crt_p[2]; pc.vig = c->crt_p[3];
    pc.ab = c->crt_p[4]; pc.glow = c->crt_p[5]; pc.bright = c->crt_p[6];
    pc.time = (float)(vkr_now_ms() * 1e-3);
    /* One RGB triad per guest pixel (auto), at least 3 target pixels: a
     * slot under one pixel wide only aliases. */
    pc.mpitch = c->crt_p[7] > 0.0f ? c->crt_p[7] : pc.rect[2] / (float)c->w;
    if (pc.mpitch < 3.0f) pc.mpitch = 3.0f;
    pc.lines = (float)c->h;          /* the guest's scanlines */
    pc.flicker = c->crt_p[8]; pc.noise = c->crt_p[9];

    ca.imageView = c->crt_out_view; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ca.clearValue.color.float32[3] = 1.0f;
    ri.renderArea = sc; ri.layerCount = 1;
    ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
    vkCmdBeginRendering(c->cmd, &ri);
    vp.x = pc.rect[0]; vp.y = 0.0f; vp.width = pc.rect[2]; vp.height = pc.rect[3];
    vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
    vkCmdSetViewport(c->cmd, 0, 1, &vp);
    vkCmdSetScissor(c->cmd, 0, 1, &sc);
    vkCmdBindPipeline(c->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c->crt_pipe);
    w.dstBinding = 0; w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &dii;
    vkCmdPushDescriptorSet(c->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c->crt_layout, 0, 1, &w);
    vkCmdPushConstants(c->cmd, c->crt_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    vkCmdDraw(c->cmd, 3, 1, 0, 0);
    vkCmdEndRendering(c->cmd);
    barrier(c, c->crt_out, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
}

/* RECOMP_MSAA: the multisampled image into level 0 of dst (both attachments
 * before and after). */
static void resolve_into(VkrContext *c, VkImage ms, VkImage dst)
{
    VkImageResolve r = {0};
    barrier(c, ms, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    barrier(c, dst, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    r.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; r.srcSubresource.layerCount = 1;
    r.dstSubresource = r.srcSubresource;
    r.extent.width = c->sw; r.extent.height = c->sh; r.extent.depth = 1;
    vkCmdResolveImage(c->cmd, ms, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    barrier(c, dst, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    barrier(c, ms, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
}

/* RECOMP_SHARPEN / _BLOOM / ...: post.frag over the frame. c->color in
 * COLOR_ATTACHMENT before and after; post_in gets a copy with mips. */
static void post_pass(VkrContext *c)
{
    VkImageCopy ic = {0};
    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    VkDescriptorImageInfo dii = {c->lin_samp, c->post_in_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    VkViewport vp = {0, 0, (float)c->sw, (float)c->sh, 0.0f, 1.0f};
    VkRect2D sc = {{0, 0}, {c->sw, c->sh}};
    struct { float rect[4], tsize[2], sharpen, bloom, thresh, vibrance, contrast, deband, vignette,
             time, levels; } pc;
    int32_t mw = (int32_t)c->sw, mh = (int32_t)c->sh;
    uint32_t i;

    barrier(c, c->color, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    barrier_mips(c, c->post_in, 0, c->post_levels,
                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    ic.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; ic.srcSubresource.layerCount = 1;
    ic.dstSubresource = ic.srcSubresource;
    ic.extent.width = c->sw; ic.extent.height = c->sh; ic.extent.depth = 1;
    vkCmdCopyImage(c->cmd, c->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   c->post_in, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
    for (i = 1; i < c->post_levels; i++) {
        VkImageBlit bl = {0};
        barrier_mips(c, c->post_in, i - 1, 1,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        bl.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; bl.srcSubresource.layerCount = 1;
        bl.srcSubresource.mipLevel = i - 1;
        bl.dstSubresource = bl.srcSubresource; bl.dstSubresource.mipLevel = i;
        bl.srcOffsets[1].x = mw; bl.srcOffsets[1].y = mh; bl.srcOffsets[1].z = 1;
        mw = mw > 1 ? mw / 2 : 1; mh = mh > 1 ? mh / 2 : 1;
        bl.dstOffsets[1].x = mw; bl.dstOffsets[1].y = mh; bl.dstOffsets[1].z = 1;
        vkCmdBlitImage(c->cmd, c->post_in, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       c->post_in, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
    }
    barrier_mips(c, c->post_in, c->post_levels - 1, 1,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    barrier_mips(c, c->post_in, 0, c->post_levels,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    barrier(c, c->color, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    pc.rect[2] = c->squeeze_all > 0.5f ? (float)c->w * c->scale : (float)c->sw;
    pc.rect[3] = (float)c->sh;
    pc.rect[0] = ((float)c->sw - pc.rect[2]) * 0.5f;
    pc.rect[1] = 0.0f;
    pc.tsize[0] = (float)c->sw; pc.tsize[1] = (float)c->sh;
    pc.sharpen = c->post_p[0]; pc.bloom = c->post_p[1]; pc.thresh = c->post_p[2];
    pc.vibrance = c->post_p[3]; pc.contrast = c->post_p[4]; pc.deband = c->post_p[5];
    pc.vignette = c->post_p[6];
    pc.time = (float)((long long)vkr_now_ms() % 1000000) * 1e-3f;
    pc.levels = (float)c->post_levels;
    ca.imageView = c->color_view; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ri.renderArea = sc; ri.layerCount = 1;
    ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
    vkCmdBeginRendering(c->cmd, &ri);
    vkCmdSetViewport(c->cmd, 0, 1, &vp);
    vkCmdSetScissor(c->cmd, 0, 1, &sc);
    vkCmdBindPipeline(c->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c->post_pipe);
    w.dstBinding = 0; w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &dii;
    vkCmdPushDescriptorSet(c->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c->post_layout, 0, 1, &w);
    vkCmdPushConstants(c->cmd, c->post_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    vkCmdDraw(c->cmd, 3, 1, 0, 0);
    vkCmdEndRendering(c->cmd);
}

/* c->color (COLOR_ATTACHMENT, left so) into dst with `levels` mips, dst
 * left SHADER_READ. */
static void copy_mips(VkrContext *c, VkImage dst, uint32_t levels)
{
    VkImageCopy ic = {0};
    int32_t mw = (int32_t)c->sw, mh = (int32_t)c->sh;
    uint32_t i;
    barrier(c, c->color, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    barrier_mips(c, dst, 0, levels, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    ic.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; ic.srcSubresource.layerCount = 1;
    ic.dstSubresource = ic.srcSubresource;
    ic.extent.width = c->sw; ic.extent.height = c->sh; ic.extent.depth = 1;
    vkCmdCopyImage(c->cmd, c->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
    for (i = 1; i < levels; i++) {
        VkImageBlit bl = {0};
        barrier_mips(c, dst, i - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        bl.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; bl.srcSubresource.layerCount = 1;
        bl.srcSubresource.mipLevel = i - 1;
        bl.dstSubresource = bl.srcSubresource; bl.dstSubresource.mipLevel = i;
        bl.srcOffsets[1].x = mw; bl.srcOffsets[1].y = mh; bl.srcOffsets[1].z = 1;
        mw = mw > 1 ? mw / 2 : 1; mh = mh > 1 ? mh / 2 : 1;
        bl.dstOffsets[1].x = mw; bl.dstOffsets[1].y = mh; bl.dstOffsets[1].z = 1;
        vkCmdBlitImage(c->cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
    }
    barrier_mips(c, dst, levels - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    barrier_mips(c, dst, 0, levels, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    barrier(c, c->color, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
}

/* One fullscreen draw of pipe into view (w x h) with the scene descriptors. */
static void scene_draw(VkrContext *c, VkPipeline pipe, VkImageView view, uint32_t w, uint32_t h,
                       const VkWriteDescriptorSet *wr)
{
    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    VkViewport vp = {0, 0, (float)w, (float)h, 0.0f, 1.0f};
    VkRect2D sc = {{0, 0}, {w, h}};
    ca.imageView = view; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ri.renderArea = sc; ri.layerCount = 1;
    ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
    vkCmdBeginRendering(c->cmd, &ri);
    vkCmdSetViewport(c->cmd, 0, 1, &vp);
    vkCmdSetScissor(c->cmd, 0, 1, &sc);
    vkCmdBindPipeline(c->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdPushDescriptorSet(c->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c->sc_layout, 0, 4, wr);
    vkCmdDraw(c->cmd, 3, 1, 0, 0);
    vkCmdEndRendering(c->cmd);
}

/* RECOMP_AO / _HAZE / _SHAFTS / _RIM / _MBLUR: scene_ao.frag at half size,
 * then scene.frag into c->color, on a frame with a camera. c->color and the
 * aux image's level 0 in COLOR_ATTACHMENT before and after, depth in
 * DEPTH_ATTACHMENT. */
static void scene_pass(VkrContext *c, int mid)
{
    static const uint32_t bind[4] = {0, 1, 3, 4};   /* img, dep, aot, S */
    VkDescriptorImageInfo ii[3];
    VkDescriptorBufferInfo bi;
    VkWriteDescriptorSet wr[4];
    float u[68];
    uint32_t off = (c->buf_used + 255) & ~255u, k;
    if (off + sizeof u > VKR_BUF_BYTES)
        return;
    memcpy(u, c->cam.inv_m, 64);
    memcpy(u + 16, c->cam.prev_m, 64);
    memcpy(u + 32, c->cam.sun, 16);
    u[36] = c->cam.sun_px[0] * c->scale_x; u[37] = c->cam.sun_px[1] * c->scale;
    u[38] = c->cam.sun_px[2]; u[39] = c->cam.sun_px[3];
    memcpy(u + 40, c->cam.fog, 16);
    u[44] = c->sc_p[0]; u[45] = c->sc_p[5]; u[46] = c->sc_p[6]; u[47] = c->sc_p[11];   /* ao, mindist */
    u[48] = c->sc_p[1]; u[49] = c->sc_p[7]; u[50] = c->sc_p[8]; u[51] = c->sc_p[12];   /* haze, AO gain */
    u[52] = c->sc_p[2]; u[53] = c->sc_p[4]; u[54] = c->sc_p[3]; u[55] = c->sc_p[9];    /* fx */
    u[56] = (float)c->w; u[57] = (float)c->h; u[58] = 1.0f / c->scale_x; u[59] = 1.0f / c->scale;
    u[60] = c->zmax; u[61] = (float)c->sw; u[62] = (float)c->sh; u[63] = c->sc_p[10];
    u[64] = (float)c->sc_aw; u[65] = (float)c->sc_ah;
    u[66] = (float)((long long)vkr_now_ms() % 100000) * 1e-3f;
    {
        /* RECOMP_SCENE_DEBUG: 1 AO, 2 normals, 3 distance (scene.frag) */
        static int dbg = -1;
        if (dbg < 0) { const char *e = getenv("RECOMP_SCENE_DEBUG"); dbg = e ? atoi(e) : 0; }
        u[67] = (float)dbg;
    }
    memcpy(c->buf_map + off, u, sizeof u);
    c->buf_used = off + (uint32_t)sizeof u;

    /* mid-frame with MSAA: the world so far, resolved */
    if (mid && c->msc)
        resolve_into(c, c->msc, c->color);
    copy_mips(c, c->sc_in, c->sc_levels);
    barrier(c, c->depth, ds_aspect(c),
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    barrier(c, c->sc_ao, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    ii[0].sampler = c->lin_samp; ii[0].imageView = c->sc_in_view;
    ii[1].sampler = c->sc_near; ii[1].imageView = c->depth_sview;
    ii[2].sampler = c->lin_samp; ii[2].imageView = c->sc_ao_view;
    for (k = 0; k < 3; k++)
        ii[k].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ii[1].imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    bi.buffer = c->buf; bi.offset = off; bi.range = sizeof u;
    memset(wr, 0, sizeof wr);
    for (k = 0; k < 4; k++) {
        wr[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[k].dstBinding = bind[k]; wr[k].descriptorCount = 1;
        wr[k].descriptorType = k < 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                     : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        if (k < 3) wr[k].pImageInfo = &ii[k]; else wr[k].pBufferInfo = &bi;
    }

    /* AO first (the composite reads it); with AO off the image is still
     * bound, its content unused. The composite's ao binding must be in
     * SHADER_READ, so the AO draw happens either way when cheap: skipped,
     * the image is only transitioned. */
    {
        VkWriteDescriptorSet w2[4];
        VkDescriptorImageInfo self = ii[0];      /* AO does not read aot: bind the frame there */
        memcpy(w2, wr, sizeof w2);
        w2[2].pImageInfo = &self;
        if (c->sc_p[0] > 0.0f)
            scene_draw(c, c->sc_ao_pipe, c->sc_ao_view, c->sc_aw, c->sc_ah, w2);
    }
    barrier(c, c->sc_ao, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    /* mid-frame with MSAA the result goes back into the multisampled
     * target, which the HUD is then drawn into */
    if (mid && c->msc)
        scene_draw(c, c->sc_pipe_ms, c->msc_view, c->sw, c->sh, wr);
    else
        scene_draw(c, c->sc_pipe, c->color_view, c->sw, c->sh, wr);

    barrier(c, c->depth, ds_aspect(c),
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
}

/* Before the first 2D draw after the world (vkr_backend.c): the scene pass
 * on what is drawn so far, so the HUD is drawn on top of it untouched.
 * Once per frame; a frame that never gets here has it at the flip. */
void vkr_scene_run(VkrContext *c)
{
    if (!c->scene || !c->cam_valid || c->scene_done || c->ab_off || !c->sc_in || c->cur_rt >= 0
        || !c->recording)
        return;
    end_pass(c);
    scene_pass(c, 1);
    c->scene_done = 1;
}

int vkr_flip_submit(VkrContext *c)
{
    VkBufferImageCopy cp = {0};
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    int slot = c->cur;
    vkr_use_main(c);
    if (!begin_cmd(c)) return -1;
    if (c->squeeze_all > 0.5f) {
        /* Pillarboxed frame: the bars are black (nothing draws there). */
        VkClearAttachment ca = {VK_IMAGE_ASPECT_COLOR_BIT, 0, {{{0, 0, 0, 1}}}};
        uint32_t bw = (uint32_t)(((float)c->sw - (float)c->w * c->scale) * 0.5f);
        VkClearRect cr[2] = {{{{0, 0}, {bw, c->sh}}, 0, 1},
                             {{{(int32_t)(c->sw - bw), 0}, {bw, c->sh}}, 0, 1}};
        if (bw) {
            begin_pass(c);
            vkCmdClearAttachments(c->cmd, 1, &ca, 2, cr);
        }
    }
    end_pass(c);
    memset(c->fr[slot].qf, 0, sizeof c->fr[slot].qf);
    if (c->msc)
        resolve_into(c, c->msc, c->color);
    if (c->prof_thr > 0) { c->fr[slot].qf[0] = c->fr[slot].nq; prof_mark(c, NULL, 0, -1); }
    if (c->scene && c->cam_valid && c->sc_in && !c->scene_done && !c->ab_off)
        scene_pass(c, 0);
    c->scene_done = 0;
    if (c->prof_thr > 0) { c->fr[slot].qf[1] = c->fr[slot].nq; prof_mark(c, NULL, 0, -1); }
    c->cam_valid = 0;
    if (c->post_in && !c->ab_off)
        post_pass(c);
    if (c->prof_thr > 0) { c->fr[slot].qf[2] = c->fr[slot].nq; prof_mark(c, NULL, 0, -1); }
    barrier(c, c->color, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    cp.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    cp.imageSubresource.layerCount = 1;
    cp.imageExtent.depth = 1;
    if (c->scale == 1.0f && c->scale_x == 1.0f) {
        cp.imageExtent.width = c->w; cp.imageExtent.height = c->h;
        vkCmdCopyImageToBuffer(c->cmd, c->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               c->fr[slot].rb, 1, &cp);
    } else {
        /* The full image for the presenter, and a filtered guest-size copy
         * for guest memory (what the title and the screenshots read). */
        VkImageBlit bl = {0};
        cp.imageExtent.width = c->sw; cp.imageExtent.height = c->sh;
        if (c->crt_in) {
            crt_pass(c);
            vkCmdCopyImageToBuffer(c->cmd, c->crt_out, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   c->fr[slot].rbh, 1, &cp);
        } else
            vkCmdCopyImageToBuffer(c->cmd, c->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   c->fr[slot].rbh, 1, &cp);
        barrier(c, c->down, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        bl.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; bl.srcSubresource.layerCount = 1;
        bl.dstSubresource = bl.srcSubresource;
        bl.srcOffsets[1].x = (int32_t)c->sw; bl.srcOffsets[1].y = (int32_t)c->sh; bl.srcOffsets[1].z = 1;
        bl.dstOffsets[1].x = (int32_t)c->w;  bl.dstOffsets[1].y = (int32_t)c->h;  bl.dstOffsets[1].z = 1;
        vkCmdBlitImage(c->cmd, c->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       c->down, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
        barrier(c, c->down, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        cp.imageExtent.width = c->w; cp.imageExtent.height = c->h;
        vkCmdCopyImageToBuffer(c->cmd, c->down, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               c->fr[slot].rb, 1, &cp);
    }
    barrier(c, c->color, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    prof_mark(c, NULL, 0, -1);
    if (vkEndCommandBuffer(c->cmd) != VK_SUCCESS) return -1;
    c->recording = 0;
    si.commandBufferCount = 1; si.pCommandBuffers = &c->cmd;
    if (vkQueueSubmit(c->queue, 1, &si, c->fence) != VK_SUCCESS) return -1;
    pthread_mutex_lock(&c->lock);
    c->fr[slot].busy = 1;
    pthread_mutex_unlock(&c->lock);
    use_frame(c, (slot + 1) % VKR_FRAMES);
    c->fr[c->cur].qreset = 0;   /* its queries are reset when it is next begun */
    c->buf_used = 0;
    c->upload_serial++;
    if (++c->flip_serial % 120 == 0)     /* every ~2 s: textures unused for 5 s */
        tex_evict_old(c, 300);
    if (c->tex_bytes > (768ull << 20)) {  /* drop the cache once nothing is in flight */
        vkDeviceWaitIdle(c->dev);
        tex_free_all(c);
    }
    return slot;
}

const uint8_t *vkr_frame_pixels(VkrContext *c, int slot)
{
    if (vkWaitForFences(c->dev, 1, &c->fr[slot].fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
        return NULL;
    prof_report(c, slot);
    if (c->fr[slot].rb_noncoherent) {
        VkMappedMemoryRange mr = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        mr.memory = c->fr[slot].rb_mem; mr.offset = 0; mr.size = VK_WHOLE_SIZE;
        vkInvalidateMappedMemoryRanges(c->dev, 1, &mr);
    }
    return c->fr[slot].rb_map;
}

/* After vkr_frame_pixels: the full-resolution image of the same frame
 * (*w x *h), or NULL when the internal resolution is the guest's. */
const uint8_t *vkr_frame_pixels_full(VkrContext *c, int slot, uint32_t *w, uint32_t *h)
{
    if ((c->scale == 1.0f && c->scale_x == 1.0f) || !c->fr[slot].rbh_map)
        return NULL;
    if (c->fr[slot].rbh_noncoherent) {
        VkMappedMemoryRange mr = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        mr.memory = c->fr[slot].rbh_mem; mr.offset = 0; mr.size = VK_WHOLE_SIZE;
        vkInvalidateMappedMemoryRanges(c->dev, 1, &mr);
    }
    *w = c->sw; *h = c->sh;
    return c->fr[slot].rbh_map;
}

void vkr_frame_release(VkrContext *c, int slot)
{
    pthread_mutex_lock(&c->lock);
    c->fr[slot].busy = 0;
    pthread_cond_broadcast(&c->freed);
    pthread_mutex_unlock(&c->lock);
}

int vkr_read_back(VkrContext *c, uint8_t *out)
{
    int slot = vkr_flip_submit(c);
    const uint8_t *px = slot < 0 ? NULL : vkr_frame_pixels(c, slot);
    if (px)
        memcpy(out, px, (size_t)c->w * c->h * 4);
    if (slot >= 0)
        vkr_frame_release(c, slot);
    return px != NULL;
}

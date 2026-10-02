/*
 * spathad — daemon Spatha (host).
 * Un solo hilo, poll(), N clientes, estado por conexion, framing con buffer.
 * Pasamanos hacia el loader Vulkan real (SPATHA_VK_LIB, default /system/lib64/libvulkan.so).
 * Vulkan 1.0 + 1.1/1.2 (hasta min(host, 1.2)).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#include "proto.h"
#include "wire.h"
#include "chain.h"

#define MAX_CLIENTS 64
#define MAX_INST    16
#define MAX_PD      16
#define FRAME_MAX   (sizeof(struct spatha_msg) + SPATHA_MAX_PAYLOAD)
#define BUF_INIT    (64u * 1024u)

#define MAX_SC      4
#define MAX_SC_IMG  8

struct obj { uint64_t h; uint64_t aux; uint32_t type; uint32_t aux2; };

#define DFN_LIST(X) \
    X(DestroyDevice) X(GetDeviceQueue) X(QueueSubmit) X(QueueWaitIdle) X(DeviceWaitIdle) \
    X(AllocateMemory) X(FreeMemory) X(MapMemory) X(UnmapMemory) \
    X(FlushMappedMemoryRanges) X(InvalidateMappedMemoryRanges) \
    X(BindBufferMemory) X(BindImageMemory) X(GetBufferMemoryRequirements) \
    X(GetImageMemoryRequirements) X(GetImageSubresourceLayout) \
    X(CreateFence) X(DestroyFence) X(ResetFences) X(GetFenceStatus) X(WaitForFences) \
    X(CreateSemaphore) X(DestroySemaphore) X(CreateBuffer) X(DestroyBuffer) \
    X(CreateImage) X(DestroyImage) X(CreateImageView) X(DestroyImageView) \
    X(CreateSampler) X(DestroySampler) X(CreateShaderModule) X(DestroyShaderModule) \
    X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) \
    X(CreateDescriptorPool) X(DestroyDescriptorPool) \
    X(AllocateDescriptorSets) X(FreeDescriptorSets) X(UpdateDescriptorSets) \
    X(CreatePipelineLayout) X(DestroyPipelineLayout) X(CreatePipelineCache) \
    X(DestroyPipelineCache) X(CreateRenderPass) X(DestroyRenderPass) \
    X(CreateFramebuffer) X(DestroyFramebuffer) X(CreateGraphicsPipelines) X(DestroyPipeline) \
    X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers) \
    X(FreeCommandBuffers) X(ResetCommandBuffer) X(BeginCommandBuffer) X(EndCommandBuffer) \
    X(CmdBeginRenderPass) X(CmdEndRenderPass) X(CmdNextSubpass) X(CmdBindPipeline) \
    X(CmdBindDescriptorSets) X(CmdSetViewport) X(CmdSetScissor) X(CmdDraw) \
    X(CmdDrawIndexed) X(CmdPipelineBarrier) X(CmdCopyBufferToImage) \
    X(CmdCopyImageToBuffer) X(CmdCopyBuffer) X(CmdCopyImage) X(CmdBlitImage) \
    X(CmdClearColorImage) X(CmdBindVertexBuffers) X(CmdBindIndexBuffer) \
    X(CmdPushConstants) X(CmdSetLineWidth) X(CmdSetBlendConstants) \
    X(CmdSetDepthBias) X(CmdFillBuffer) \
    X(CreateEvent) X(DestroyEvent) X(GetEventStatus) X(SetEvent) X(ResetEvent) \
    X(CreateQueryPool) X(DestroyQueryPool) X(GetQueryPoolResults) \
    X(CreateBufferView) X(DestroyBufferView) X(GetPipelineCacheData) X(MergePipelineCaches) \
    X(CreateComputePipelines) X(ResetCommandPool) X(ResetDescriptorPool) \
    X(CmdDispatch) X(CmdDispatchIndirect) X(CmdDrawIndirect) X(CmdDrawIndexedIndirect) \
    X(CmdClearDepthStencilImage) X(CmdClearAttachments) X(CmdResolveImage) X(CmdUpdateBuffer) \
    X(CmdSetDepthBounds) X(CmdSetStencilCompareMask) X(CmdSetStencilWriteMask) \
    X(CmdSetStencilReference) X(CmdSetEvent) X(CmdResetEvent) X(CmdWaitEvents) \
    X(CmdExecuteCommands) X(CmdResetQueryPool) X(CmdBeginQuery) X(CmdEndQuery) \
    X(CmdWriteTimestamp) X(CmdCopyQueryPoolResults)

/* 1.1 / 1.2: opcionales (NULL si el host es mas viejo) */
#define DFN_LIST_OPT(X) \
    X(GetDescriptorSetLayoutSupport) X(CmdDispatchBase) \
    X(CreateRenderPass2) X(CmdDrawIndirectCount) X(CmdDrawIndexedIndirectCount) \
    X(GetSemaphoreCounterValue) X(WaitSemaphores) X(SignalSemaphore) \
    X(GetBufferDeviceAddress) X(ResetQueryPool)

struct dfn {
#define X(n) PFN_vk##n n;
    DFN_LIST(X)
    DFN_LIST_OPT(X)
#undef X
};

struct sc {
    int used;
    VkFormat fmt;
    uint32_t w, h, n, next;
    uint32_t img_id[MAX_SC_IMG];
    VkDeviceMemory img_mem[MAX_SC_IMG];
    VkBuffer rb;
    VkDeviceMemory rb_mem;
    uint8_t *rb_map;
    int rb_coherent;
    VkCommandPool pool;
    VkCommandBuffer cb;
    VkFence fence;
};

struct vkstate {
    struct obj *objs;
    uint32_t nobjs, capobjs;
    uint32_t *freeids;
    uint32_t nfree, capfree;
    int bad;
    void *arena;

    VkDevice dev;
    uint32_t dev_id;
    VkPhysicalDevice pd;
    struct dfn d;
    VkQueue gq;
    uint32_t gq_family;
    VkPhysicalDeviceMemoryProperties mp;
    VkDeviceSize atom;
    struct sc scs[MAX_SC];
};

struct client {
    int fd;
    size_t len;
    size_t cap;
    uint32_t n_msgs;
    uint8_t *buf;

    VkInstance       instances[MAX_INST];
    uint32_t         instance_ids[MAX_INST];
    uint32_t         next_instance_id;
    VkPhysicalDevice devices[MAX_PD];
    uint32_t         device_ids[MAX_PD];
    uint32_t         device_owner[MAX_PD];
    uint32_t         next_device_id;

    struct vkstate   vs;
};

static volatile sig_atomic_t g_stop;
static int g_debug;
static struct client g_clients[MAX_CLIENTS];

static PFN_vkCreateInstance            p_vkCreateInstance;
static PFN_vkDestroyInstance           p_vkDestroyInstance;
static PFN_vkEnumeratePhysicalDevices  p_vkEnumeratePhysicalDevices;
static PFN_vkGetPhysicalDeviceProperties p_vkGetPhysicalDeviceProperties;
static PFN_vkEnumerateInstanceExtensionProperties p_vkEnumerateInstanceExtensionProperties;
static PFN_vkGetPhysicalDeviceQueueFamilyProperties p_vkGetPhysicalDeviceQueueFamilyProperties;
static PFN_vkGetPhysicalDeviceFeatures            p_vkGetPhysicalDeviceFeatures;
static PFN_vkGetPhysicalDeviceFormatProperties    p_vkGetPhysicalDeviceFormatProperties;
static PFN_vkGetPhysicalDeviceImageFormatProperties p_vkGetPhysicalDeviceImageFormatProperties;
static PFN_vkGetPhysicalDeviceMemoryProperties    p_vkGetPhysicalDeviceMemoryProperties;
static PFN_vkGetPhysicalDeviceProperties2 p_vkGetPhysicalDeviceProperties2;
static PFN_vkCreateDevice                         p_vkCreateDevice;
static PFN_vkGetDeviceProcAddr                    p_vkGetDeviceProcAddr;
static PFN_vkEnumerateInstanceVersion             p_vkEnumerateInstanceVersion;
static PFN_vkGetPhysicalDeviceFeatures2           p_vkGetPhysicalDeviceFeatures2;
static PFN_vkGetInstanceProcAddr                  g_gipa;

#define DBG(...) do { if (g_debug) { printf("[spathad] " __VA_ARGS__); \
    putchar('\n'); fflush(stdout); } } while (0)

static void on_signal(int s) { (void)s; g_stop = 1; }

/* ---- carga del loader Vulkan real ---- */

static void *resolve(void *lib, PFN_vkGetInstanceProcAddr gipa, const char *name) {
    void *f = dlsym(lib, name);
    if (!f && gipa) f = (void *)gipa(VK_NULL_HANDLE, name);
    if (!f) fprintf(stderr, "[spathad] no se pudo resolver %s\n", name);
    return f;
}

static int load_vulkan(void) {
    const char *path = getenv("SPATHA_VK_LIB");
    if (!path || !*path) path = "/system/lib64/libvulkan.so";

    void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "[spathad] dlopen %s: %s\n", path, dlerror()); return -1; }

    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    if (!gipa) { fprintf(stderr, "[spathad] sin vkGetInstanceProcAddr en %s\n", path); return -1; }

    p_vkCreateInstance  = (PFN_vkCreateInstance)resolve(lib, gipa, "vkCreateInstance");
    p_vkDestroyInstance = (PFN_vkDestroyInstance)resolve(lib, gipa, "vkDestroyInstance");
    p_vkEnumeratePhysicalDevices =
        (PFN_vkEnumeratePhysicalDevices)resolve(lib, gipa, "vkEnumeratePhysicalDevices");
    p_vkGetPhysicalDeviceProperties =
        (PFN_vkGetPhysicalDeviceProperties)resolve(lib, gipa, "vkGetPhysicalDeviceProperties");
    p_vkEnumerateInstanceExtensionProperties =
        (PFN_vkEnumerateInstanceExtensionProperties)resolve(lib, gipa, "vkEnumerateInstanceExtensionProperties");
    p_vkGetPhysicalDeviceQueueFamilyProperties =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties)resolve(lib, gipa, "vkGetPhysicalDeviceQueueFamilyProperties");

    p_vkGetPhysicalDeviceFeatures = (PFN_vkGetPhysicalDeviceFeatures)resolve(lib, gipa, "vkGetPhysicalDeviceFeatures");
    p_vkGetPhysicalDeviceFormatProperties = (PFN_vkGetPhysicalDeviceFormatProperties)resolve(lib, gipa, "vkGetPhysicalDeviceFormatProperties");
    p_vkGetPhysicalDeviceImageFormatProperties = (PFN_vkGetPhysicalDeviceImageFormatProperties)resolve(lib, gipa, "vkGetPhysicalDeviceImageFormatProperties");
    p_vkGetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)resolve(lib, gipa, "vkGetPhysicalDeviceMemoryProperties");
    p_vkCreateDevice = (PFN_vkCreateDevice)resolve(lib, gipa, "vkCreateDevice");
    p_vkGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)resolve(lib, gipa, "vkGetDeviceProcAddr");
    p_vkGetPhysicalDeviceProperties2 = (PFN_vkGetPhysicalDeviceProperties2)dlsym(lib, "vkGetPhysicalDeviceProperties2");  /* opcional */

    g_gipa = gipa;
    p_vkEnumerateInstanceVersion = (PFN_vkEnumerateInstanceVersion)dlsym(lib, "vkEnumerateInstanceVersion");
    if (!p_vkEnumerateInstanceVersion)
        p_vkEnumerateInstanceVersion = (PFN_vkEnumerateInstanceVersion)gipa(VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
    p_vkGetPhysicalDeviceFeatures2 = (PFN_vkGetPhysicalDeviceFeatures2)dlsym(lib, "vkGetPhysicalDeviceFeatures2");

    if (!p_vkGetPhysicalDeviceFeatures || !p_vkGetPhysicalDeviceFormatProperties ||
        !p_vkGetPhysicalDeviceImageFormatProperties || !p_vkGetPhysicalDeviceMemoryProperties ||
        !p_vkCreateDevice || !p_vkGetDeviceProcAddr)
        return -1;
    if (!p_vkCreateInstance || !p_vkDestroyInstance ||
        !p_vkEnumeratePhysicalDevices || !p_vkGetPhysicalDeviceProperties ||
        !p_vkEnumerateInstanceExtensionProperties ||
        !p_vkGetPhysicalDeviceQueueFamilyProperties)
        return -1;
    printf("[spathad] Vulkan real: %s\n", path);
    return 0;
}

/* ---- tablas de handles por conexion ---- */

static int find_inst(const struct client *c, uint32_t id) {
    if (!id) return -1;
    for (int i = 0; i < MAX_INST; i++)
        if (c->instance_ids[i] == id) return i;
    return -1;
}

static int find_pd(const struct client *c, uint32_t id) {
    if (!id) return -1;
    for (int i = 0; i < MAX_PD; i++)
        if (c->device_ids[i] == id) return i;
    return -1;
}

static uint32_t map_pd(struct client *c, uint32_t inst_id, VkPhysicalDevice h) {
    int freeslot = -1;
    for (int i = 0; i < MAX_PD; i++) {
        if (c->device_ids[i] && c->device_owner[i] == inst_id && c->devices[i] == h)
            return c->device_ids[i];
        if (!c->device_ids[i] && freeslot < 0) freeslot = i;
    }
    if (freeslot < 0) return 0;
    uint32_t id = ++c->next_device_id;
    if (!id) id = ++c->next_device_id;
    c->devices[freeslot] = h;
    c->device_ids[freeslot] = id;
    c->device_owner[freeslot] = inst_id;
    return id;
}

static void destroy_instance_slot(struct client *c, int slot) {
    uint32_t id = c->instance_ids[slot];
    for (int i = 0; i < MAX_PD; i++)
        if (c->device_ids[i] && c->device_owner[i] == id) {
            c->device_ids[i] = 0; c->device_owner[i] = 0; c->devices[i] = NULL;
        }
    p_vkDestroyInstance(c->instances[slot], NULL);
    c->instances[slot] = NULL;
    c->instance_ids[slot] = 0;
}

static void vk_cleanup(struct client *c);

static void client_close(struct client *c) {
    DBG("fd=%d cerrado (%u msgs)", c->fd, c->n_msgs);
    vk_cleanup(c);
    for (int i = 0; i < MAX_INST; i++)
        if (c->instance_ids[i]) destroy_instance_slot(c, i);
    close(c->fd);
    free(c->buf);
    memset(c, 0, sizeof(*c));
    c->fd = -1;
}

/* ---- handlers ---- */

static int reply_error(struct client *c, uint32_t req, uint32_t code) {
    return spatha_send(c->fd, SPATHA_OP_ERROR, req, &code, sizeof(code));
}

static int h_create_instance(struct client *c, const struct spatha_msg *m, const uint8_t *p) {
    const char *names[SPATHA_MAX_INST_EXT];
    uint32_t n_ext = 0;
    if (m->len) {
        if (m->len < 4) return reply_error(c, m->req_id, EINVAL);
        memcpy(&n_ext, p, 4);
        if (n_ext > SPATHA_MAX_INST_EXT ||
            m->len != 4 + (size_t)n_ext * SPATHA_EXT_NAME_MAX)
            return reply_error(c, m->req_id, EINVAL);
        for (uint32_t i = 0; i < n_ext; i++) {
            names[i] = (const char *)(p + 4 + (size_t)i * SPATHA_EXT_NAME_MAX);
            if (!memchr(names[i], 0, SPATHA_EXT_NAME_MAX))
                return reply_error(c, m->req_id, EINVAL);
            DBG("create_instance ext[%u]=%s", i, names[i]);
        }
    }

    struct spatha_resp_create_instance r = { VK_ERROR_TOO_MANY_OBJECTS, 0 };
    int slot = -1;
    for (int i = 0; i < MAX_INST; i++)
        if (!c->instance_ids[i]) { slot = i; break; }

    if (slot >= 0) {
        uint32_t iv = VK_API_VERSION_1_0;
        if (p_vkEnumerateInstanceVersion) {
            uint32_t v = 0;
            if (p_vkEnumerateInstanceVersion(&v) == VK_SUCCESS) iv = v;
        }
        if (iv > VK_API_VERSION_1_2) iv = VK_API_VERSION_1_2;
        VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                  .pApplicationName = "spathad", .apiVersion = iv };
        VkInstanceCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                    .pApplicationInfo = &app,
                                    .enabledExtensionCount = n_ext,
                                    .ppEnabledExtensionNames = n_ext ? names : NULL };
        VkInstance inst = VK_NULL_HANDLE;
        VkResult vr = p_vkCreateInstance(&ci, NULL, &inst);
        r.vk_result = vr;
        if (vr == VK_SUCCESS) {
            if (!p_vkGetPhysicalDeviceFeatures2)
                p_vkGetPhysicalDeviceFeatures2 = (PFN_vkGetPhysicalDeviceFeatures2)g_gipa(inst, "vkGetPhysicalDeviceFeatures2");
            if (!p_vkGetPhysicalDeviceProperties2)
                p_vkGetPhysicalDeviceProperties2 = (PFN_vkGetPhysicalDeviceProperties2)g_gipa(inst, "vkGetPhysicalDeviceProperties2");
            uint32_t id = ++c->next_instance_id;
            if (!id) id = ++c->next_instance_id;
            c->instances[slot] = inst;
            c->instance_ids[slot] = id;
            r.instance_id = id;
        }
    }
    DBG("create_instance -> result=%d id=%u", r.vk_result, r.instance_id);
    return spatha_send(c->fd, m->op, m->req_id, &r, sizeof(r));
}

static int h_destroy_instance(struct client *c, const struct spatha_msg *m, const uint8_t *p) {
    if (m->len != 4) return reply_error(c, m->req_id, EINVAL);
    uint32_t id; memcpy(&id, p, 4);
    int32_t result = VK_ERROR_UNKNOWN;
    int slot = find_inst(c, id);
    if (slot >= 0) { destroy_instance_slot(c, slot); result = VK_SUCCESS; }
    DBG("destroy_instance id=%u -> %d", id, result);
    return spatha_send(c->fd, m->op, m->req_id, &result, sizeof(result));
}

static int h_enum_pd(struct client *c, const struct spatha_msg *m, const uint8_t *p) {
    if (m->len != 4) return reply_error(c, m->req_id, EINVAL);
    uint32_t inst_id; memcpy(&inst_id, p, 4);

    struct spatha_resp_enum_pd hdr = { VK_ERROR_UNKNOWN, 0 };
    uint32_t ids[SPATHA_MAX_PD];
    int slot = find_inst(c, inst_id);
    if (slot >= 0) {
        VkPhysicalDevice tmp[SPATHA_MAX_PD];
        uint32_t n = SPATHA_MAX_PD;
        VkResult vr = p_vkEnumeratePhysicalDevices(c->instances[slot], &n, tmp);
        hdr.vk_result = vr;
        if (vr == VK_SUCCESS || vr == VK_INCOMPLETE) {
            for (uint32_t k = 0; k < n; k++) {
                uint32_t id = map_pd(c, inst_id, tmp[k]);
                if (!id) { hdr.vk_result = VK_ERROR_TOO_MANY_OBJECTS; hdr.count = 0; break; }
                ids[hdr.count++] = id;
            }
        }
    }
    DBG("enum_pd inst=%u -> result=%d count=%u", inst_id, hdr.vk_result, hdr.count);

    uint8_t out[sizeof(hdr) + sizeof(ids)];
    memcpy(out, &hdr, sizeof(hdr));
    memcpy(out + sizeof(hdr), ids, hdr.count * sizeof(uint32_t));
    return spatha_send(c->fd, m->op, m->req_id, out, sizeof(hdr) + hdr.count * 4);
}

static int h_get_pd_props(struct client *c, const struct spatha_msg *m, const uint8_t *p) {
    if (m->len != 4) return reply_error(c, m->req_id, EINVAL);
    uint32_t id; memcpy(&id, p, 4);

    uint8_t out[sizeof(int32_t) + sizeof(VkPhysicalDeviceProperties)];
    int32_t result = VK_ERROR_UNKNOWN;
    uint32_t outlen = sizeof(int32_t);
    int slot = find_pd(c, id);
    if (slot >= 0) {
        VkPhysicalDeviceProperties props;
        p_vkGetPhysicalDeviceProperties(c->devices[slot], &props);
        memcpy(out + 4, &props, sizeof(props));
        outlen = sizeof(out);
        result = VK_SUCCESS;
        DBG("pd_props id=%u -> %s api=%u.%u.%u", id, props.deviceName,
            VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
            VK_API_VERSION_PATCH(props.apiVersion));
    }
    memcpy(out, &result, 4);
    return spatha_send(c->fd, m->op, m->req_id, out, outlen);
}

static int h_get_pd_qf(struct client *c, const struct spatha_msg *m, const uint8_t *p) {
    if (m->len != 4) return reply_error(c, m->req_id, EINVAL);
    uint32_t id; memcpy(&id, p, 4);

    struct spatha_resp_enum_qf hdr = { VK_ERROR_UNKNOWN, 0 };
    VkQueueFamilyProperties qf[SPATHA_MAX_QF];
    int slot = find_pd(c, id);
    if (slot >= 0) {
        uint32_t n = 0;
        p_vkGetPhysicalDeviceQueueFamilyProperties(c->devices[slot], &n, NULL);
        if (n > SPATHA_MAX_QF) n = SPATHA_MAX_QF;
        if (n) p_vkGetPhysicalDeviceQueueFamilyProperties(c->devices[slot], &n, qf);
        hdr.vk_result = VK_SUCCESS;
        hdr.count = n;
        for (uint32_t i = 0; i < n; i++)
            DBG("qf pd=%u [%u] flags=0x%x queues=%u", id, i, qf[i].queueFlags, qf[i].queueCount);
    }
    DBG("pd_qf id=%u -> result=%d count=%u", id, hdr.vk_result, hdr.count);

    uint8_t out[sizeof(hdr) + sizeof(qf)];
    memcpy(out, &hdr, sizeof(hdr));
    memcpy(out + sizeof(hdr), qf, hdr.count * sizeof(VkQueueFamilyProperties));
    return spatha_send(c->fd, m->op, m->req_id, out,
                       sizeof(hdr) + hdr.count * sizeof(VkQueueFamilyProperties));
}

static int h_enum_ext(struct client *c, const struct spatha_msg *m) {
    uint32_t count = 0;
    VkResult vr = p_vkEnumerateInstanceExtensionProperties(NULL, &count, NULL);

    struct spatha_resp_enum_ext hdr = { vr, 0 };
    if (vr != VK_SUCCESS || count == 0) {
        DBG("enum_ext -> result=%d count=0", vr);
        return spatha_send(c->fd, m->op, m->req_id, &hdr, sizeof(hdr));
    }

    if (count > 128) count = 128;

    VkExtensionProperties props[128];
    uint32_t fill = count;
    vr = p_vkEnumerateInstanceExtensionProperties(NULL, &fill, props);
    if (vr != VK_SUCCESS && vr != VK_INCOMPLETE) {
        hdr.vk_result = vr;
        return spatha_send(c->fd, m->op, m->req_id, &hdr, sizeof(hdr));
    }
    hdr.count = fill;

    uint8_t out[sizeof(hdr) + sizeof(props)];
    memcpy(out, &hdr, sizeof(hdr));
    memcpy(out + sizeof(hdr), props, fill * sizeof(VkExtensionProperties));

    DBG("enum_ext -> result=%d count=%u", vr, fill);
    return spatha_send(c->fd, m->op, m->req_id, out, sizeof(hdr) + fill * sizeof(VkExtensionProperties));
}

#define V   (c->vs)
#define D   (c->vs.d)

static void oom(void) { fprintf(stderr, "[spathad] sin memoria\n"); exit(1); }

static void *ar_alloc(struct client *c, size_t n) {
    uint8_t *b = calloc(1, n + 16 + 8);
    if (!b) oom();
    *(void **)b = V.arena;
    V.arena = b;
    return b + 16;
}
static void ar_free(struct client *c) {
    while (V.arena) { void *n = *(void **)V.arena; free(V.arena); V.arena = n; }
}
static void *ar_dup(struct client *c, const void *src, size_t n) {
    void *d = ar_alloc(c, n);
    if (src && n) memcpy(d, src, n);
    return d;
}

static uint32_t obj_add(struct client *c, uint32_t type, uint64_t h, uint64_t aux, uint32_t aux2) {
    uint32_t id;
    if (V.nfree) id = V.freeids[--V.nfree];
    else {
        if (V.nobjs == V.capobjs) {
            uint32_t nc = V.capobjs ? V.capobjs * 2 : 256;
            struct obj *no = realloc(V.objs, nc * sizeof(*no));
            if (!no) oom();
            V.objs = no; V.capobjs = nc;
        }
        id = ++V.nobjs;
    }
    V.objs[id - 1] = (struct obj){ h, aux, type, aux2 };
    return id;
}
static struct obj *obj_find(struct client *c, uint32_t type, uint64_t id) {
    if (!id || id > V.nobjs || V.objs[id - 1].type != type) return NULL;
    return &V.objs[id - 1];
}
static uint64_t H(struct client *c, uint32_t type, uint64_t id) {
    if (!id) return 0;
    struct obj *o = obj_find(c, type, id);
    if (!o) { V.bad = 1; return 0; }
    return o->h;
}
static void obj_del(struct client *c, uint64_t id) {
    if (!id || id > V.nobjs || !V.objs[id - 1].type) return;
    V.objs[id - 1].type = 0;
    if (V.nfree == V.capfree) {
        uint32_t nc = V.capfree ? V.capfree * 2 : 64;
        uint32_t *nf = realloc(V.freeids, nc * sizeof(*nf));
        if (!nf) oom();
        V.freeids = nf; V.capfree = nc;
    }
    V.freeids[V.nfree++] = (uint32_t)id;
}
static void obj_purge_children(struct client *c, uint32_t type, uint64_t pool_h) {
    for (uint32_t i = 0; i < V.nobjs; i++)
        if (V.objs[i].type == type && V.objs[i].aux == pool_h) obj_del(c, i + 1);
}

#define HV(type, VT, id) ((VT)(uintptr_t)H(c, (type), (id)))
#define U2H(VT, v)       ((VT)(uintptr_t)(v))
#define H2U(h)           ((uint64_t)(uintptr_t)(h))

static VkImageLayout xl(VkImageLayout l) {
    return l == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : l;
}

#define RD_STRUCT(r, T, var) T var; do { const void *b_ = r_blob(&(r), sizeof(var)); \
    if (b_) memcpy(&var, b_, sizeof(var)); else memset(&var, 0, sizeof(var)); } while (0)

static int reply_data(struct client *c, const struct spatha_msg *m, int32_t res,
                      const void *data, size_t len) {
    uint8_t *out = malloc(8 + len);
    if (!out) oom();
    memset(out, 0, 8);
    memcpy(out, &res, 4);
    if (len) memcpy(out + 8, data, len);
    int rc = spatha_send(c->fd, m->op, m->req_id, out, (uint32_t)(8 + len));
    free(out);
    return rc;
}
static int reply_res(struct client *c, const struct spatha_msg *m, int32_t res) {
    return reply_data(c, m, res, NULL, 0);
}

/* ---- destruccion ---- */
static void destroy_h(struct client *c, uint32_t type, uint64_t h, uint64_t aux) {
    VkDevice d = V.dev;
    switch (type) {
    case SPATHA_T_SEM:     D.DestroySemaphore(d, U2H(VkSemaphore, h), NULL); break;
    case SPATHA_T_FENCE:   D.DestroyFence(d, U2H(VkFence, h), NULL); break;
    case SPATHA_T_BUF:     D.DestroyBuffer(d, U2H(VkBuffer, h), NULL); break;
    case SPATHA_T_IMG:     D.DestroyImage(d, U2H(VkImage, h), NULL); break;
    case SPATHA_T_VIEW:    D.DestroyImageView(d, U2H(VkImageView, h), NULL); break;
    case SPATHA_T_SAMPLER: D.DestroySampler(d, U2H(VkSampler, h), NULL); break;
    case SPATHA_T_SHADER:  D.DestroyShaderModule(d, U2H(VkShaderModule, h), NULL); break;
    case SPATHA_T_DSL:     D.DestroyDescriptorSetLayout(d, U2H(VkDescriptorSetLayout, h), NULL); break;
    case SPATHA_T_DPOOL:   D.DestroyDescriptorPool(d, U2H(VkDescriptorPool, h), NULL); break;
    case SPATHA_T_PLAYOUT: D.DestroyPipelineLayout(d, U2H(VkPipelineLayout, h), NULL); break;
    case SPATHA_T_PCACHE:  D.DestroyPipelineCache(d, U2H(VkPipelineCache, h), NULL); break;
    case SPATHA_T_RPASS:   D.DestroyRenderPass(d, U2H(VkRenderPass, h), NULL); break;
    case SPATHA_T_FB:      D.DestroyFramebuffer(d, U2H(VkFramebuffer, h), NULL); break;
    case SPATHA_T_PIPE:    D.DestroyPipeline(d, U2H(VkPipeline, h), NULL); break;
    case SPATHA_T_CMDPOOL: D.DestroyCommandPool(d, U2H(VkCommandPool, h), NULL); break;
    case SPATHA_T_MEM:     D.FreeMemory(d, U2H(VkDeviceMemory, h), NULL); break;
    case SPATHA_T_EVENT:   D.DestroyEvent(d, U2H(VkEvent, h), NULL); break;
    case SPATHA_T_QPOOL:   D.DestroyQueryPool(d, U2H(VkQueryPool, h), NULL); break;
    case SPATHA_T_BVIEW:   D.DestroyBufferView(d, U2H(VkBufferView, h), NULL); break;
    case SPATHA_T_CMDBUF: {
        VkCommandBuffer cb = U2H(VkCommandBuffer, h);
        D.FreeCommandBuffers(d, U2H(VkCommandPool, aux), 1, &cb);
        break; }
    default: break;
    }
}

static void sc_destroy(struct client *c, struct sc *s) {
    if (!s->used) return;
    VkDevice d = V.dev;
    D.DeviceWaitIdle(d);
    if (s->fence) D.DestroyFence(d, s->fence, NULL);
    if (s->pool)  D.DestroyCommandPool(d, s->pool, NULL);
    if (s->rb_map) D.UnmapMemory(d, s->rb_mem);
    if (s->rb)     D.DestroyBuffer(d, s->rb, NULL);
    if (s->rb_mem) D.FreeMemory(d, s->rb_mem, NULL);
    for (uint32_t i = 0; i < s->n; i++) {
        struct obj *o = obj_find(c, SPATHA_T_IMG, s->img_id[i]);
        if (o) { D.DestroyImage(d, U2H(VkImage, o->h), NULL); obj_del(c, s->img_id[i]); }
        if (s->img_mem[i]) D.FreeMemory(d, s->img_mem[i], NULL);
    }
    memset(s, 0, sizeof(*s));
}

static void vk_cleanup(struct client *c) {
    if (V.dev) {
        D.DeviceWaitIdle(V.dev);
        for (int i = 0; i < MAX_SC; i++) sc_destroy(c, &V.scs[i]);
        for (uint32_t i = V.nobjs; i > 0; i--) {
            struct obj *o = &V.objs[i - 1];
            if (o->type && o->type != SPATHA_T_DEV) destroy_h(c, o->type, o->h, o->aux);
        }
        D.DestroyDevice(V.dev, NULL);
    }
    free(V.objs); free(V.freeids);
    ar_free(c);
    memset(&c->vs, 0, sizeof(c->vs));
}

/* ---- consultas de physical device ---- */
static int h_pd_query(struct client *c, const struct spatha_msg *m, const uint8_t *p) {
    struct rbuf r; r_init(&r, p, m->len);
    uint64_t id = r_u64(&r);
    int slot = find_pd(c, (uint32_t)id);
    if (r.err || slot < 0) return reply_res(c, m, VK_ERROR_UNKNOWN);
    VkPhysicalDevice pd = c->devices[slot];
    switch (m->op) {
    case SPATHA_OP_GET_PD_FEATURES: {
        VkPhysicalDeviceFeatures f; p_vkGetPhysicalDeviceFeatures(pd, &f);
        return reply_data(c, m, VK_SUCCESS, &f, sizeof(f)); }
    case SPATHA_OP_GET_PD_PROPS2: {
        if (!p_vkGetPhysicalDeviceProperties2) return reply_res(c, m, VK_ERROR_FEATURE_NOT_PRESENT);
        VkPhysicalDeviceVulkan11Properties v11; VkPhysicalDeviceVulkan12Properties v12;
        memset(&v11, 0, sizeof(v11)); memset(&v12, 0, sizeof(v12));
        v11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES;
        v12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES;
        v11.pNext = &v12;
        VkPhysicalDeviceProperties2 pp2; memset(&pp2, 0, sizeof(pp2));
        pp2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        pp2.pNext = &v11;
        p_vkGetPhysicalDeviceProperties2(pd, &pp2);
        v11.pNext = NULL; v12.pNext = NULL;
        uint8_t blob[sizeof(v11) + sizeof(v12)];
        memcpy(blob, &v11, sizeof(v11)); memcpy(blob + sizeof(v11), &v12, sizeof(v12));
        return reply_data(c, m, VK_SUCCESS, blob, sizeof(blob)); }
    case SPATHA_OP_GET_PD_MEMPROPS: {
        VkPhysicalDeviceMemoryProperties mp; p_vkGetPhysicalDeviceMemoryProperties(pd, &mp);
        return reply_data(c, m, VK_SUCCESS, &mp, sizeof(mp)); }
    case SPATHA_OP_GET_PD_FORMAT_PROPS: {
        VkFormatProperties fp; p_vkGetPhysicalDeviceFormatProperties(pd, (VkFormat)r_u64(&r), &fp);
        return reply_data(c, m, VK_SUCCESS, &fp, sizeof(fp)); }
    default: {
        VkFormat f = (VkFormat)r_u64(&r); VkImageType t = (VkImageType)r_u64(&r);
        VkImageTiling ti = (VkImageTiling)r_u64(&r); VkImageUsageFlags u = (VkImageUsageFlags)r_u64(&r);
        VkImageCreateFlags fl = (VkImageCreateFlags)r_u64(&r);
        VkImageFormatProperties ip; memset(&ip, 0, sizeof(ip));
        VkResult vr = p_vkGetPhysicalDeviceImageFormatProperties(pd, f, t, ti, u, fl, &ip);
        return reply_data(c, m, vr, &ip, sizeof(ip)); }
    }
}

/* ---- device ---- */
static int load_device_fns(struct client *c) {
    int ok = 1;
#define X(n) D.n = (PFN_vk##n)p_vkGetDeviceProcAddr(V.dev, "vk" #n); \
             if (!D.n) { fprintf(stderr, "[spathad] falta vk" #n "\n"); ok = 0; }
    DFN_LIST(X)
#undef X
#define X(n) D.n = (PFN_vk##n)p_vkGetDeviceProcAddr(V.dev, "vk" #n);
    DFN_LIST_OPT(X)
#undef X
    return ok;
}

static int h_create_device(struct client *c, const struct spatha_msg *m, const uint8_t *p) {
    struct rbuf r; r_init(&r, p, m->len);
    uint64_t pdid = r_u64(&r);
    int slot = find_pd(c, (uint32_t)pdid);
    uint64_t nq = r_u64(&r);
    if (r.err || slot < 0 || nq == 0 || nq > 16 || V.dev) return reply_res(c, m, V.dev ? VK_ERROR_TOO_MANY_OBJECTS : VK_ERROR_UNKNOWN);

    VkDeviceQueueCreateInfo *qci = ar_alloc(c, nq * sizeof(*qci));
    for (uint64_t i = 0; i < nq; i++) {
        uint32_t fam = (uint32_t)r_u64(&r), cnt = (uint32_t)r_u64(&r);
        const float *pr = r_arr(&r, cnt, 4);
        qci[i] = (VkDeviceQueueCreateInfo){ .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = fam, .queueCount = cnt, .pQueuePriorities = pr };
    }
    VkPhysicalDeviceFeatures feat; memset(&feat, 0, sizeof(feat));
    int has_feat = (int)r_u64(&r);
    { const void *b = r_blob(&r, sizeof(feat)); if (b) memcpy(&feat, b, sizeof(feat)); }

    /* structs de features 1.1/1.2 enviadas por el ICD */
    void *chain = NULL;
    uint64_t nchain = r_u64(&r);
    if (nchain > 32) r.err = 1;
    for (uint64_t k = 0; !r.err && k < nchain; k++) {
        uint64_t st = r_u64(&r), sz = r_u64(&r);
        const void *b = (sz >= sizeof(VkBaseOutStructure) && sz <= 512) ? r_blob(&r, sz) : NULL;
        if (!b) { r.err = 1; break; }
        VkBaseOutStructure *s = ar_dup(c, b, sz);
        s->sType = (VkStructureType)st; s->pNext = chain; chain = s;
    }
    if (r.err) { ar_free(c); return reply_res(c, m, VK_ERROR_UNKNOWN); }

    VkDeviceCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = chain,
        .queueCreateInfoCount = (uint32_t)nq, .pQueueCreateInfos = qci,
        .pEnabledFeatures = has_feat ? &feat : NULL };
    VkPhysicalDevice pd = c->devices[slot];
    VkDevice dev = VK_NULL_HANDLE;
    VkResult vr = p_vkCreateDevice(pd, &ci, NULL, &dev);
    if (vr != VK_SUCCESS) { ar_free(c); return reply_res(c, m, vr); }

    V.dev = dev; V.pd = pd;
    if (!load_device_fns(c)) {
        V.dev = VK_NULL_HANDLE; ar_free(c);
        return reply_res(c, m, VK_ERROR_INITIALIZATION_FAILED);
    }
    p_vkGetPhysicalDeviceMemoryProperties(pd, &V.mp);
    VkPhysicalDeviceProperties pp; p_vkGetPhysicalDeviceProperties(pd, &pp);
    V.atom = pp.limits.nonCoherentAtomSize ? pp.limits.nonCoherentAtomSize : 1;
    V.dev_id = obj_add(c, SPATHA_T_DEV, H2U(dev), 0, 0);

    struct wbuf w; w_init(&w);
    w_u64(&w, V.dev_id);
    uint64_t total = 0;
    for (uint64_t i = 0; i < nq; i++) total += qci[i].queueCount;
    w_u64(&w, total);
    for (uint64_t i = 0; i < nq; i++)
        for (uint32_t j = 0; j < qci[i].queueCount; j++) {
            VkQueue q = VK_NULL_HANDLE;
            D.GetDeviceQueue(dev, qci[i].queueFamilyIndex, j, &q);
            if (!V.gq) { V.gq = q; V.gq_family = qci[i].queueFamilyIndex; }
            w_u64(&w, obj_add(c, SPATHA_T_QUEUE, H2U(q), 0, 0));
        }
    DBG("create_device pd=%u -> dev_id=%u (%llu queues)", (unsigned)pdid, V.dev_id, (unsigned long long)total);
    ar_free(c);
    int rc = reply_data(c, m, VK_SUCCESS, w.p, w.len);
    w_free(&w);
    return rc;
}

static int h_destroy_device(struct client *c, const struct spatha_msg *m, const uint8_t *p) {
    struct rbuf r; r_init(&r, p, m->len);
    uint64_t id = r_u64(&r);
    if (r.err || !V.dev || id != V.dev_id) return reply_res(c, m, VK_ERROR_UNKNOWN);
    vk_cleanup(c);
    DBG("destroy_device");
    return reply_res(c, m, VK_SUCCESS);
}

#include "spathad_v12.inc"

/* ---- creacion de objetos simples ---- */
#define CHK() do { if (r.err || V.bad) { vr = VK_ERROR_UNKNOWN; goto out; } } while (0)

static int h_create(struct client *c, const struct spatha_msg *m, const uint8_t *p) {
    struct rbuf r; r_init(&r, p, m->len);
    uint64_t dev = r_u64(&r), type = r_u64(&r);
    VkResult vr = VK_ERROR_UNKNOWN;
    uint64_t h = 0, aux = 0;
    V.bad = 0;
    if (r.err || !V.dev || dev != V.dev_id) goto out;
    VkDevice d = V.dev;
    const void *xn = xr(c, &r, (uint32_t)type);
    if (r.err) goto out;

    switch (type) {
    case SPATHA_T_SEM: {
        VkSemaphoreCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = xn };
        VkSemaphore x = VK_NULL_HANDLE; vr = D.CreateSemaphore(d, &ci, NULL, &x); h = H2U(x); break; }
    case SPATHA_T_FENCE: {
        VkFenceCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                                 .flags = (VkFenceCreateFlags)r_u64(&r) };
        CHK();
        VkFence x = VK_NULL_HANDLE; vr = D.CreateFence(d, &ci, NULL, &x); h = H2U(x); break; }
    case SPATHA_T_BUF: {
        RD_STRUCT(r, VkBufferCreateInfo, ci);
        const uint32_t *qfi = r_arr(&r, ci.queueFamilyIndexCount, 4);
        CHK();
        ci.pNext = NULL; ci.pQueueFamilyIndices = qfi;
        VkBuffer x = VK_NULL_HANDLE; vr = D.CreateBuffer(d, &ci, NULL, &x); h = H2U(x); break; }
    case SPATHA_T_IMG: {
        RD_STRUCT(r, VkImageCreateInfo, ci);
        const uint32_t *qfi = r_arr(&r, ci.queueFamilyIndexCount, 4);
        CHK();
        ci.pNext = xn; ci.pQueueFamilyIndices = qfi; ci.initialLayout = xl(ci.initialLayout);
        VkImage x = VK_NULL_HANDLE; vr = D.CreateImage(d, &ci, NULL, &x); h = H2U(x); break; }
    case SPATHA_T_VIEW: {
        RD_STRUCT(r, VkImageViewCreateInfo, ci);
        CHK();
        ci.pNext = xn; ci.image = HV(SPATHA_T_IMG, VkImage, H2U(ci.image));
        CHK();
        VkImageView x = VK_NULL_HANDLE; vr = D.CreateImageView(d, &ci, NULL, &x); h = H2U(x); break; }
    case SPATHA_T_SAMPLER: {
        RD_STRUCT(r, VkSamplerCreateInfo, ci);
        CHK();
        ci.pNext = xn;
        VkSampler x = VK_NULL_HANDLE; vr = D.CreateSampler(d, &ci, NULL, &x); h = H2U(x); break; }
    case SPATHA_T_SHADER: {
        uint64_t sz = r_u64(&r);
        const void *code = r_blob(&r, sz);
        CHK();
        VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                        .codeSize = sz, .pCode = ar_dup(c, code, sz) };
        VkShaderModule x = VK_NULL_HANDLE; vr = D.CreateShaderModule(d, &ci, NULL, &x); h = H2U(x); break; }
    case SPATHA_T_DSL: {
        VkDescriptorSetLayoutCreateInfo ci;
        if (!dsl_read_body(c, &r, &ci, xn)) { vr = VK_ERROR_UNKNOWN; goto out; }
        VkDescriptorSetLayout x = VK_NULL_HANDLE; vr = D.CreateDescriptorSetLayout(d, &ci, NULL, &x); h = H2U(x); break; }
    case SPATHA_T_DPOOL: {
        RD_STRUCT(r, VkDescriptorPoolCreateInfo, ci);
        const void *ps = r_arr(&r, ci.poolSizeCount, sizeof(VkDescriptorPoolSize));
        CHK();
        ci.pNext = NULL; ci.pPoolSizes = ps;
        VkDescriptorPool x = VK_NULL_HANDLE; vr = D.CreateDescriptorPool(d, &ci, NULL, &x); h = H2U(x); break; }
    case SPATHA_T_PLAYOUT: {
        RD_STRUCT(r, VkPipelineLayoutCreateInfo, ci);
        const uint64_t *ids = r_arr(&r, ci.setLayoutCount, 8);
        const void *pc = r_arr(&r, ci.pushConstantRangeCount, sizeof(VkPushConstantRange));
        CHK();
        VkDescriptorSetLayout *sl = ar_alloc(c, ci.setLayoutCount * sizeof(*sl) + 8);
        for (uint32_t i = 0; i < ci.setLayoutCount; i++) sl[i] = HV(SPATHA_T_DSL, VkDescriptorSetLayout, ids[i]);
        CHK();
        ci.pNext = NULL; ci.pSetLayouts = sl; ci.pPushConstantRanges = pc;
        VkPipelineLayout x = VK_NULL_HANDLE; vr = D.CreatePipelineLayout(d, &ci, NULL, &x); h = H2U(x); break; }
    case SPATHA_T_PCACHE: {
        uint64_t has = r_u64(&r), isz = 0; const void *init = NULL;
        if (has) { isz = r_u64(&r); init = r_blob(&r, isz); }
        CHK();
        VkPipelineCacheCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
            .initialDataSize = (size_t)isz, .pInitialData = init ? ar_dup(c, init, isz) : NULL };
        VkPipelineCache x = VK_NULL_HANDLE; vr = D.CreatePipelineCache(d, &ci, NULL, &x); h = H2U(x); break; }
    case SPATHA_T_EVENT: {
        VkEventCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO,
                                 .flags = (VkEventCreateFlags)r_u64(&r) };
        CHK();
        VkEvent x = VK_NULL_HANDLE; vr = D.CreateEvent(d, &ci, NULL, &x); h = H2U(x); break; }
    case SPATHA_T_QPOOL: {
        RD_STRUCT(r, VkQueryPoolCreateInfo, ci);
        CHK(); ci.pNext = NULL;
        VkQueryPool x = VK_NULL_HANDLE; vr = D.CreateQueryPool(d, &ci, NULL, &x); h = H2U(x); break; }
    case SPATHA_T_BVIEW: {
        RD_STRUCT(r, VkBufferViewCreateInfo, ci);
        CHK(); ci.pNext = NULL; ci.buffer = HV(SPATHA_T_BUF, VkBuffer, H2U(ci.buffer));
        CHK();
        VkBufferView x = VK_NULL_HANDLE; vr = D.CreateBufferView(d, &ci, NULL, &x); h = H2U(x); break; }
    case SPATHA_T_CMDPOOL: {
        RD_STRUCT(r, VkCommandPoolCreateInfo, ci);
        CHK();
        ci.pNext = NULL; ci.flags |= VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        VkCommandPool x = VK_NULL_HANDLE; vr = D.CreateCommandPool(d, &ci, NULL, &x); h = H2U(x); break; }
    case SPATHA_T_RPASS: {
        RD_STRUCT(r, VkRenderPassCreateInfo, ci);
        const void *ain = r_arr(&r, ci.attachmentCount, sizeof(VkAttachmentDescription));
        const void *sin = r_arr(&r, ci.subpassCount, sizeof(VkSubpassDescription));
        CHK();
        VkAttachmentDescription *att = ar_dup(c, ain, ci.attachmentCount * sizeof(*att));
        for (uint32_t i = 0; i < ci.attachmentCount; i++) {
            att[i].initialLayout = xl(att[i].initialLayout);
            att[i].finalLayout = xl(att[i].finalLayout);
        }
        VkSubpassDescription *sp = ar_dup(c, sin, ci.subpassCount * sizeof(*sp));
        for (uint32_t i = 0; i < ci.subpassCount; i++) {
            int has_res = sp[i].pResolveAttachments != NULL, has_ds = sp[i].pDepthStencilAttachment != NULL;
            const VkAttachmentReference *ia = r_arr(&r, sp[i].inputAttachmentCount, sizeof(VkAttachmentReference));
            const VkAttachmentReference *ca = r_arr(&r, sp[i].colorAttachmentCount, sizeof(VkAttachmentReference));
            const VkAttachmentReference *ra = has_res ? r_arr(&r, sp[i].colorAttachmentCount, sizeof(VkAttachmentReference)) : NULL;
            const VkAttachmentReference *da = has_ds ? r_arr(&r, 1, sizeof(VkAttachmentReference)) : NULL;
            const uint32_t *pa = r_arr(&r, sp[i].preserveAttachmentCount, 4);
            CHK();
            VkAttachmentReference *x;
            x = ar_dup(c, ia, sp[i].inputAttachmentCount * sizeof(*x));
            for (uint32_t k = 0; k < sp[i].inputAttachmentCount; k++) x[k].layout = xl(x[k].layout);
            sp[i].pInputAttachments = x;
            x = ar_dup(c, ca, sp[i].colorAttachmentCount * sizeof(*x));
            for (uint32_t k = 0; k < sp[i].colorAttachmentCount; k++) x[k].layout = xl(x[k].layout);
            sp[i].pColorAttachments = x;
            if (has_res) {
                x = ar_dup(c, ra, sp[i].colorAttachmentCount * sizeof(*x));
                for (uint32_t k = 0; k < sp[i].colorAttachmentCount; k++) x[k].layout = xl(x[k].layout);
                sp[i].pResolveAttachments = x;
            }
            if (has_ds) { x = ar_dup(c, da, sizeof(*x)); x->layout = xl(x->layout); sp[i].pDepthStencilAttachment = x; }
            sp[i].pPreserveAttachments = ar_dup(c, pa, sp[i].preserveAttachmentCount * 4);
        }
        const void *dep = r_arr(&r, ci.dependencyCount, sizeof(VkSubpassDependency));
        CHK();
        ci.pNext = xn; ci.pAttachments = att; ci.pSubpasses = sp; ci.pDependencies = dep;
        VkRenderPass x = VK_NULL_HANDLE; vr = D.CreateRenderPass(d, &ci, NULL, &x); h = H2U(x); break; }
    case SPATHA_T_FB: {
        RD_STRUCT(r, VkFramebufferCreateInfo, ci);
        const uint64_t *ids = r_arr(&r, ci.attachmentCount, 8);
        CHK();
        VkImageView *iv = ar_alloc(c, ci.attachmentCount * sizeof(*iv) + 8);
        for (uint32_t i = 0; i < ci.attachmentCount; i++) iv[i] = HV(SPATHA_T_VIEW, VkImageView, ids[i]);
        ci.pNext = NULL; ci.pAttachments = iv;
        ci.renderPass = HV(SPATHA_T_RPASS, VkRenderPass, H2U(ci.renderPass));
        CHK();
        VkFramebuffer x = VK_NULL_HANDLE; vr = D.CreateFramebuffer(d, &ci, NULL, &x); h = H2U(x); break; }
    default: vr = VK_ERROR_FEATURE_NOT_PRESENT; break;
    }
out:;
    uint64_t id = 0;
    if (vr == VK_SUCCESS) id = obj_add(c, (uint32_t)type, h, aux, 0);
    ar_free(c);
    return reply_data(c, m, vr, &id, 8);
}

static int h_destroy(struct client *c, const struct spatha_msg *m, const uint8_t *p) {
    struct rbuf r; r_init(&r, p, m->len);
    uint64_t dev = r_u64(&r), type = r_u64(&r), id = r_u64(&r);
    struct obj *o = (r.err || !V.dev || dev != V.dev_id) ? NULL : obj_find(c, (uint32_t)type, id);
    if (!o) return reply_res(c, m, VK_ERROR_UNKNOWN);
    if (type == SPATHA_T_CMDPOOL) obj_purge_children(c, SPATHA_T_CMDBUF, o->h);
    if (type == SPATHA_T_DPOOL)   obj_purge_children(c, SPATHA_T_DSET, o->h);
    destroy_h(c, (uint32_t)type, o->h, o->aux);
    obj_del(c, id);
    return reply_res(c, m, VK_SUCCESS);
}

/* ---- pipelines graficos ---- */
#define RD_OPT(T, dst) do { if (r_u64(&r)) { const void *b_ = r_blob(&r, sizeof(T)); \
    dst = ar_alloc(c, sizeof(T)); if (b_) memcpy(dst, b_, sizeof(T)); dst->pNext = NULL; } else dst = NULL; } while (0)

static int h_create_gfx(struct client *c, const struct spatha_msg *m, const uint8_t *p) {
    struct rbuf r; r_init(&r, p, m->len);
    uint64_t dev = r_u64(&r), cache = r_u64(&r), count = r_u64(&r);
    VkResult vr = VK_ERROR_UNKNOWN;
    VkPipeline *pipes = NULL;
    V.bad = 0;
    if (r.err || !V.dev || dev != V.dev_id || count == 0 || count > 64) goto out;
    VkGraphicsPipelineCreateInfo *cis = ar_alloc(c, count * sizeof(*cis));
    pipes = ar_alloc(c, count * sizeof(*pipes));

    for (uint64_t k = 0; k < count; k++) {
        RD_STRUCT(r, VkGraphicsPipelineCreateInfo, ci);
        const void *sin = r_arr(&r, ci.stageCount, sizeof(VkPipelineShaderStageCreateInfo));
        CHK();
        VkPipelineShaderStageCreateInfo *st = ar_dup(c, sin, ci.stageCount * sizeof(*st));
        for (uint32_t i = 0; i < ci.stageCount; i++) {
            uint64_t nl = r_u64(&r);
            const char *nm = r_blob(&r, nl);
            CHK();
            char *name = ar_alloc(c, nl + 1);
            if (nl) memcpy(name, nm, nl);
            st[i].pNext = NULL; st[i].pName = name;
            st[i].module = HV(SPATHA_T_SHADER, VkShaderModule, H2U(st[i].module));
            if (r_u64(&r)) {
                RD_STRUCT(r, VkSpecializationInfo, si);
                const void *me = r_arr(&r, si.mapEntryCount, sizeof(VkSpecializationMapEntry));
                const void *dd = r_blob(&r, si.dataSize);
                CHK();
                VkSpecializationInfo *sp = ar_alloc(c, sizeof(*sp));
                *sp = si; sp->pMapEntries = me; sp->pData = dd;
                st[i].pSpecializationInfo = sp;
            } else st[i].pSpecializationInfo = NULL;
        }
        CHK();
        ci.pNext = NULL; ci.pStages = st;

        VkPipelineVertexInputStateCreateInfo *vi; RD_OPT(VkPipelineVertexInputStateCreateInfo, vi);
        if (vi) {
            vi->pVertexBindingDescriptions = r_arr(&r, vi->vertexBindingDescriptionCount, sizeof(VkVertexInputBindingDescription));
            vi->pVertexAttributeDescriptions = r_arr(&r, vi->vertexAttributeDescriptionCount, sizeof(VkVertexInputAttributeDescription));
        }
        VkPipelineInputAssemblyStateCreateInfo *ia; RD_OPT(VkPipelineInputAssemblyStateCreateInfo, ia);
        VkPipelineTessellationStateCreateInfo *te; RD_OPT(VkPipelineTessellationStateCreateInfo, te);
        VkPipelineViewportStateCreateInfo *vp; RD_OPT(VkPipelineViewportStateCreateInfo, vp);
        if (vp) {
            vp->pViewports = r_u64(&r) ? r_arr(&r, vp->viewportCount, sizeof(VkViewport)) : NULL;
            vp->pScissors = r_u64(&r) ? r_arr(&r, vp->scissorCount, sizeof(VkRect2D)) : NULL;
        }
        VkPipelineRasterizationStateCreateInfo *ra; RD_OPT(VkPipelineRasterizationStateCreateInfo, ra);
        VkPipelineMultisampleStateCreateInfo *ms; RD_OPT(VkPipelineMultisampleStateCreateInfo, ms);
        if (ms) ms->pSampleMask = r_u64(&r) ? r_arr(&r, (ms->rasterizationSamples + 31) / 32, 4) : NULL;
        VkPipelineDepthStencilStateCreateInfo *ds; RD_OPT(VkPipelineDepthStencilStateCreateInfo, ds);
        VkPipelineColorBlendStateCreateInfo *cb; RD_OPT(VkPipelineColorBlendStateCreateInfo, cb);
        if (cb) cb->pAttachments = r_arr(&r, cb->attachmentCount, sizeof(VkPipelineColorBlendAttachmentState));
        VkPipelineDynamicStateCreateInfo *dy; RD_OPT(VkPipelineDynamicStateCreateInfo, dy);
        if (dy) dy->pDynamicStates = r_arr(&r, dy->dynamicStateCount, sizeof(VkDynamicState));
        CHK();
        ci.pVertexInputState = vi; ci.pInputAssemblyState = ia; ci.pTessellationState = te;
        ci.pViewportState = vp; ci.pRasterizationState = ra; ci.pMultisampleState = ms;
        ci.pDepthStencilState = ds; ci.pColorBlendState = cb; ci.pDynamicState = dy;
        ci.layout = HV(SPATHA_T_PLAYOUT, VkPipelineLayout, H2U(ci.layout));
        ci.renderPass = HV(SPATHA_T_RPASS, VkRenderPass, H2U(ci.renderPass));
        ci.basePipelineHandle = HV(SPATHA_T_PIPE, VkPipeline, H2U(ci.basePipelineHandle));
        CHK();
        cis[k] = ci;
    }
    VkPipelineCache pc = HV(SPATHA_T_PCACHE, VkPipelineCache, cache);
    CHK();
    vr = D.CreateGraphicsPipelines(V.dev, pc, (uint32_t)count, cis, NULL, pipes);
out:;
    struct wbuf w; w_init(&w);
    if (vr == VK_SUCCESS)
        for (uint64_t k = 0; k < count; k++) w_u64(&w, obj_add(c, SPATHA_T_PIPE, H2U(pipes[k]), 0, 0));
    ar_free(c);
    int rc = reply_data(c, m, vr, w.p, w.len);
    w_free(&w);
    return rc;
}

/* ---- descriptor sets ---- */
static int h_alloc_dsets(struct client *c, const struct spatha_msg *m, const uint8_t *p) {
    struct rbuf r; r_init(&r, p, m->len);
    uint64_t dev = r_u64(&r), pool = r_u64(&r), n = r_u64(&r);
    VkResult vr = VK_ERROR_UNKNOWN; VkDescriptorSet *sets = NULL;
    struct wbuf w; w_init(&w);
    V.bad = 0;
    const uint64_t *ids = (n && n <= 4096) ? r_arr(&r, n, 8) : NULL;
    uint64_t nv = r_u64(&r);
    if (nv > n) r.err = 1;
    uint32_t *vc = ar_alloc(c, (nv ? nv : 1) * 4 + 8);
    for (uint64_t i = 0; !r.err && i < nv; i++) vc[i] = (uint32_t)r_u64(&r);
    if (r.err || !V.dev || dev != V.dev_id || !ids) goto out;
    VkDescriptorSetLayout *sl = ar_alloc(c, n * sizeof(*sl));
    for (uint64_t i = 0; i < n; i++) sl[i] = HV(SPATHA_T_DSL, VkDescriptorSetLayout, ids[i]);
    VkDescriptorPool pl = HV(SPATHA_T_DPOOL, VkDescriptorPool, pool);
    CHK();
    sets = ar_alloc(c, n * sizeof(*sets));
    VkDescriptorSetVariableDescriptorCountAllocateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO,
        .descriptorSetCount = (uint32_t)nv, .pDescriptorCounts = vc };
    VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .pNext = nv ? &vi : NULL,
        .descriptorPool = pl, .descriptorSetCount = (uint32_t)n, .pSetLayouts = sl };
    vr = D.AllocateDescriptorSets(V.dev, &ai, sets);
    if (vr == VK_SUCCESS)
        for (uint64_t i = 0; i < n; i++) w_u64(&w, obj_add(c, SPATHA_T_DSET, H2U(sets[i]), H2U(pl), 0));
out:;
    ar_free(c);
    int rc = reply_data(c, m, vr, w.p, w.len);
    w_free(&w);
    return rc;
}

static int h_free_dsets(struct client *c, const struct spatha_msg *m, const uint8_t *p) {
    struct rbuf r; r_init(&r, p, m->len);
    uint64_t dev = r_u64(&r), pool = r_u64(&r), n = r_u64(&r);
    VkResult vr = VK_ERROR_UNKNOWN;
    V.bad = 0;
    const uint64_t *ids = (n && n <= 4096) ? r_arr(&r, n, 8) : NULL;
    if (r.err || !V.dev || dev != V.dev_id || !ids) goto out;
    VkDescriptorSet *sets = ar_alloc(c, n * sizeof(*sets));
    for (uint64_t i = 0; i < n; i++) sets[i] = HV(SPATHA_T_DSET, VkDescriptorSet, ids[i]);
    VkDescriptorPool pl = HV(SPATHA_T_DPOOL, VkDescriptorPool, pool);
    CHK();
    vr = D.FreeDescriptorSets(V.dev, pl, (uint32_t)n, sets);
    for (uint64_t i = 0; i < n; i++) obj_del(c, ids[i]);
out:;
    ar_free(c);
    return reply_res(c, m, vr);
}

static int is_image_desc(uint32_t t) {
    return t == VK_DESCRIPTOR_TYPE_SAMPLER || t == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
           t == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || t == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
           t == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
}
static int is_buffer_desc(uint32_t t) {
    return t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
           t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
}

static int h_update_dsets(struct client *c, const struct spatha_msg *m, const uint8_t *p) {
    struct rbuf r; r_init(&r, p, m->len);
    uint64_t dev = r_u64(&r), nw = r_u64(&r), nc = r_u64(&r);
    VkResult vr = VK_ERROR_UNKNOWN;
    V.bad = 0;
    if (r.err || !V.dev || dev != V.dev_id || nw > 4096 || nc > 4096) goto out;
    VkWriteDescriptorSet *ws = ar_alloc(c, nw * sizeof(*ws) + 8);
    for (uint64_t i = 0; i < nw; i++) {
        RD_STRUCT(r, VkWriteDescriptorSet, w);
        CHK();
        w.pNext = NULL; w.pImageInfo = NULL; w.pBufferInfo = NULL; w.pTexelBufferView = NULL;
        w.dstSet = HV(SPATHA_T_DSET, VkDescriptorSet, H2U(w.dstSet));
        if (is_image_desc(w.descriptorType)) {
            VkDescriptorImageInfo *ii = ar_dup(c, r_arr(&r, w.descriptorCount, sizeof(*ii)), w.descriptorCount * sizeof(*ii));
            CHK();
            for (uint32_t k = 0; k < w.descriptorCount; k++) {
                ii[k].sampler = HV(SPATHA_T_SAMPLER, VkSampler, H2U(ii[k].sampler));
                ii[k].imageView = HV(SPATHA_T_VIEW, VkImageView, H2U(ii[k].imageView));
                ii[k].imageLayout = xl(ii[k].imageLayout);
            }
            w.pImageInfo = ii;
        } else if (is_buffer_desc(w.descriptorType)) {
            VkDescriptorBufferInfo *bi = ar_dup(c, r_arr(&r, w.descriptorCount, sizeof(*bi)), w.descriptorCount * sizeof(*bi));
            CHK();
            for (uint32_t k = 0; k < w.descriptorCount; k++)
                bi[k].buffer = HV(SPATHA_T_BUF, VkBuffer, H2U(bi[k].buffer));
            w.pBufferInfo = bi;
        } else if (w.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER ||
                   w.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
            const uint64_t *ids = r_arr(&r, w.descriptorCount, 8);
            CHK();
            VkBufferView *bv = ar_alloc(c, w.descriptorCount * sizeof(VkBufferView) + 8);
            for (uint32_t k = 0; k < w.descriptorCount; k++) bv[k] = HV(SPATHA_T_BVIEW, VkBufferView, ids[k]);
            CHK();
            w.pTexelBufferView = bv;
        } else {
            vr = VK_ERROR_FEATURE_NOT_PRESENT; goto out;
        }
        ws[i] = w;
    }
    const VkCopyDescriptorSet *cin = r_arr(&r, nc, sizeof(VkCopyDescriptorSet));
    CHK();
    VkCopyDescriptorSet *cs = ar_dup(c, cin, nc * sizeof(*cs));
    for (uint64_t i = 0; i < nc; i++) {
        cs[i].pNext = NULL;
        cs[i].srcSet = HV(SPATHA_T_DSET, VkDescriptorSet, H2U(cs[i].srcSet));
        cs[i].dstSet = HV(SPATHA_T_DSET, VkDescriptorSet, H2U(cs[i].dstSet));
    }
    CHK();
    D.UpdateDescriptorSets(V.dev, (uint32_t)nw, ws, (uint32_t)nc, cs);
    vr = VK_SUCCESS;
out:;
    ar_free(c);
    return reply_res(c, m, vr);
}

/* ---- memoria ---- */
static int h_alloc_memory(struct client *c, const struct spatha_msg *m, const uint8_t *p) {
    struct rbuf r; r_init(&r, p, m->len);
    uint64_t dev = r_u64(&r), size = r_u64(&r), type = r_u64(&r), mflags = r_u64(&r);
    uint64_t id = 0;
    VkResult vr = VK_ERROR_UNKNOWN;
    if (!r.err && V.dev && dev == V.dev_id && type < V.mp.memoryTypeCount) {
        VkMemoryAllocateFlagsInfo fi = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
            .flags = (VkMemoryAllocateFlags)mflags & VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT };
        VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = fi.flags ? &fi : NULL,
            .allocationSize = size, .memoryTypeIndex = (uint32_t)type };
        VkDeviceMemory x = VK_NULL_HANDLE;
        vr = D.AllocateMemory(V.dev, &ai, NULL, &x);
        if (vr == VK_SUCCESS) id = obj_add(c, SPATHA_T_MEM, H2U(x), size, (uint32_t)type);
    }
    return reply_data(c, m, vr, &id, 8);
}

static int h_bind_memory(struct client *c, const struct spatha_msg *m, const uint8_t *p) {
    struct rbuf r; r_init(&r, p, m->len);
    uint64_t dev = r_u64(&r), kind = r_u64(&r), oid = r_u64(&r), mid = r_u64(&r), off = r_u64(&r);
    VkResult vr = VK_ERROR_UNKNOWN;
    V.bad = 0;
    if (!r.err && V.dev && dev == V.dev_id) {
        VkDeviceMemory mem = HV(SPATHA_T_MEM, VkDeviceMemory, mid);
        if (kind == SPATHA_T_BUF) {
            VkBuffer b = HV(SPATHA_T_BUF, VkBuffer, oid);
            if (!V.bad) vr = D.BindBufferMemory(V.dev, b, mem, off);
        } else if (kind == SPATHA_T_IMG) {
            VkImage im = HV(SPATHA_T_IMG, VkImage, oid);
            if (!V.bad) vr = D.BindImageMemory(V.dev, im, mem, off);
        }
    }
    return reply_res(c, m, vr);
}

static int h_get_mem_reqs(struct client *c, const struct spatha_msg *m, const uint8_t *p) {
    struct rbuf r; r_init(&r, p, m->len);
    uint64_t dev = r_u64(&r), kind = r_u64(&r), oid = r_u64(&r);
    VkMemoryRequirements mr; memset(&mr, 0, sizeof(mr));
    VkResult vr = VK_ERROR_UNKNOWN;
    V.bad = 0;
    if (!r.err && V.dev && dev == V.dev_id) {
        if (kind == SPATHA_T_BUF) {
            VkBuffer b = HV(SPATHA_T_BUF, VkBuffer, oid);
            if (!V.bad) { D.GetBufferMemoryRequirements(V.dev, b, &mr); vr = VK_SUCCESS; }
        } else if (kind == SPATHA_T_IMG) {
            VkImage im = HV(SPATHA_T_IMG, VkImage, oid);
            if (!V.bad) { D.GetImageMemoryRequirements(V.dev, im, &mr); vr = VK_SUCCESS; }
        }
    }
    return reply_data(c, m, vr, &mr, sizeof(mr));
}

static VkResult mem_access(struct client *c, struct obj *mo, uint64_t off, uint64_t size,
                           void *buf, int write) {
    if (off > mo->aux || size > mo->aux - off) return VK_ERROR_MEMORY_MAP_FAILED;
    VkDeviceMemory mem = U2H(VkDeviceMemory, mo->h);
    void *base = NULL;
    VkResult vr = D.MapMemory(V.dev, mem, 0, VK_WHOLE_SIZE, 0, &base);
    if (vr != VK_SUCCESS) return vr;
    int coherent = (V.mp.memoryTypes[mo->aux2].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    VkMappedMemoryRange mr = { .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = mem };
    if (!coherent) {
        uint64_t a = V.atom, s = off / a * a, e = (off + size + a - 1) / a * a;
        if (e > mo->aux) e = mo->aux;
        mr.offset = s; mr.size = (e == mo->aux) ? VK_WHOLE_SIZE : e - s;
    }
    if (write) {
        memcpy((uint8_t *)base + off, buf, size);
        if (!coherent) D.FlushMappedMemoryRanges(V.dev, 1, &mr);
    } else {
        if (!coherent) D.InvalidateMappedMemoryRanges(V.dev, 1, &mr);
        memcpy(buf, (uint8_t *)base + off, size);
    }
    D.UnmapMemory(V.dev, mem);
    return VK_SUCCESS;
}

static int h_mem_write(struct client *c, const struct spatha_msg *m, const uint8_t *p) {
    struct rbuf r; r_init(&r, p, m->len);
    uint64_t dev = r_u64(&r), mid = r_u64(&r), off = r_u64(&r), size = r_u64(&r);
    const void *data = r_blob(&r, size);
    struct obj *mo = obj_find(c, SPATHA_T_MEM, mid);
    VkResult vr = VK_ERROR_UNKNOWN;
    if (!r.err && V.dev && dev == V.dev_id && mo) vr = mem_access(c, mo, off, size, (void *)data, 1);
    return reply_res(c, m, vr);
}

static int h_mem_read(struct client *c, const struct spatha_msg *m, const uint8_t *p) {
    struct rbuf r; r_init(&r, p, m->len);
    uint64_t dev = r_u64(&r), mid = r_u64(&r), off = r_u64(&r), size = r_u64(&r);
    struct obj *mo = obj_find(c, SPATHA_T_MEM, mid);
    VkResult vr = VK_ERROR_UNKNOWN;
    uint8_t *buf = NULL;
    if (!r.err && V.dev && dev == V.dev_id && mo && size <= SPATHA_MAX_PAYLOAD - 64) {
        buf = malloc(size ? size : 1);
        if (!buf) oom();
        vr = mem_access(c, mo, off, size, buf, 0);
    }
    int rc = reply_data(c, m, vr, buf, vr == VK_SUCCESS ? size : 0);
    free(buf);
    return rc;
}

static int h_subres_layout(struct client *c, const struct spatha_msg *m, const uint8_t *p) {
    struct rbuf r; r_init(&r, p, m->len);
    uint64_t dev = r_u64(&r), iid = r_u64(&r);
    RD_STRUCT(r, VkImageSubresource, sr);
    VkSubresourceLayout sl; memset(&sl, 0, sizeof(sl));
    VkResult vr = VK_ERROR_UNKNOWN;
    V.bad = 0;
    if (!r.err && V.dev && dev == V.dev_id) {
        VkImage im = HV(SPATHA_T_IMG, VkImage, iid);
        if (!V.bad) { D.GetImageSubresourceLayout(V.dev, im, &sr, &sl); vr = VK_SUCCESS; }
    }
    return reply_data(c, m, vr, &sl, sizeof(sl));
}

/* ---- command buffers ---- */
static int h_alloc_cmdbufs(struct client *c, const struct spatha_msg *m, const uint8_t *p) {
    struct rbuf r; r_init(&r, p, m->len);
    uint64_t dev = r_u64(&r), pool = r_u64(&r), level = r_u64(&r), n = r_u64(&r);
    VkResult vr = VK_ERROR_UNKNOWN;
    struct wbuf w; w_init(&w);
    V.bad = 0;
    if (!r.err && V.dev && dev == V.dev_id && n && n <= 1024) {
        VkCommandPool pl = HV(SPATHA_T_CMDPOOL, VkCommandPool, pool);
        if (!V.bad) {
            VkCommandBuffer *cbs = ar_alloc(c, n * sizeof(*cbs));
            VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                .commandPool = pl, .level = (VkCommandBufferLevel)level, .commandBufferCount = (uint32_t)n };
            vr = D.AllocateCommandBuffers(V.dev, &ai, cbs);
            if (vr == VK_SUCCESS)
                for (uint64_t i = 0; i < n; i++) w_u64(&w, obj_add(c, SPATHA_T_CMDBUF, H2U(cbs[i]), H2U(pl), 0));
        }
    }
    ar_free(c);
    int rc = reply_data(c, m, vr, w.p, w.len);
    w_free(&w);
    return rc;
}

static int h_free_cmdbufs(struct client *c, const struct spatha_msg *m, const uint8_t *p) {
    struct rbuf r; r_init(&r, p, m->len);
    uint64_t dev = r_u64(&r), n = r_u64(&r);
    if (!r.err && V.dev && dev == V.dev_id && n && n <= 1024) {
        const uint64_t *ids = r_arr(&r, n, 8);
        if (!r.err)
            for (uint64_t i = 0; i < n; i++) {
                struct obj *o = obj_find(c, SPATHA_T_CMDBUF, ids[i]);
                if (!o) continue;
                destroy_h(c, SPATHA_T_CMDBUF, o->h, o->aux);
                obj_del(c, ids[i]);
            }
    }
    return reply_res(c, m, VK_SUCCESS);
}

#define CMD_CHK() do { if (br.err || V.bad) { ok = 0; goto cmd_done; } } while (0)

static int h_cmdbuf_replay(struct client *c, const struct spatha_msg *m, const uint8_t *p) {
    struct rbuf r; r_init(&r, p, m->len);
    uint64_t dev = r_u64(&r), cid = r_u64(&r), flags = r_u64(&r), nbytes = r_u64(&r);
    const uint8_t *stream = r_blob(&r, nbytes);
    uint64_t has_inh = r_u64(&r);
    VkCommandBufferInheritanceInfo inh; memset(&inh, 0, sizeof(inh));
    if (has_inh) { const void *b_ = r_blob(&r, sizeof(inh)); if (b_) memcpy(&inh, b_, sizeof(inh)); }
    VkResult vr = VK_ERROR_UNKNOWN;
    int ok = 1;
    V.bad = 0;
    struct obj *co = obj_find(c, SPATHA_T_CMDBUF, cid);
    if (r.err || !V.dev || dev != V.dev_id || !co) goto out;
    VkCommandBuffer cb = U2H(VkCommandBuffer, co->h);
    D.ResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = (VkCommandBufferUsageFlags)flags };
    if (has_inh) {
        inh.pNext = NULL;
        inh.renderPass = HV(SPATHA_T_RPASS, VkRenderPass, H2U(inh.renderPass));
        inh.framebuffer = HV(SPATHA_T_FB, VkFramebuffer, H2U(inh.framebuffer));
        if (V.bad) goto out;
        bi.pInheritanceInfo = &inh;
    }
    vr = D.BeginCommandBuffer(cb, &bi);
    if (vr != VK_SUCCESS) goto out;

    struct rbuf s; r_init(&s, stream, nbytes);
    while (ok && s.off < s.len) {
        uint64_t cmd = r_u64(&s), len = r_u64(&s);
        const void *body = r_blob(&s, len);
        if (s.err) { ok = 0; break; }
        struct rbuf br; r_init(&br, body, len);
        switch (cmd) {
        case SPATHA_CMD_BEGIN_RENDER_PASS: {
            RD_STRUCT(br, VkRenderPassBeginInfo, bi2);
            const void *cv = r_arr(&br, bi2.clearValueCount, sizeof(VkClearValue));
            VkSubpassContents sc = (VkSubpassContents)r_u64(&br);
            CMD_CHK();
            bi2.pNext = NULL; bi2.pClearValues = cv;
            bi2.renderPass = HV(SPATHA_T_RPASS, VkRenderPass, H2U(bi2.renderPass));
            bi2.framebuffer = HV(SPATHA_T_FB, VkFramebuffer, H2U(bi2.framebuffer));
            CMD_CHK();
            D.CmdBeginRenderPass(cb, &bi2, sc); break; }
        case SPATHA_CMD_END_RENDER_PASS: D.CmdEndRenderPass(cb); break;
        case SPATHA_CMD_NEXT_SUBPASS: D.CmdNextSubpass(cb, (VkSubpassContents)r_u64(&br)); break;
        case SPATHA_CMD_BIND_PIPELINE: {
            VkPipelineBindPoint bp = (VkPipelineBindPoint)r_u64(&br);
            VkPipeline pl = HV(SPATHA_T_PIPE, VkPipeline, r_u64(&br));
            CMD_CHK(); D.CmdBindPipeline(cb, bp, pl); break; }
        case SPATHA_CMD_BIND_DESC_SETS: {
            VkPipelineBindPoint bp = (VkPipelineBindPoint)r_u64(&br);
            VkPipelineLayout lay = HV(SPATHA_T_PLAYOUT, VkPipelineLayout, r_u64(&br));
            uint32_t first = (uint32_t)r_u64(&br);
            uint64_t n = r_u64(&br);
            const uint64_t *ids = r_arr(&br, n, 8);
            uint64_t nd = r_u64(&br);
            const uint32_t *dyn = r_arr(&br, nd, 4);
            CMD_CHK();
            VkDescriptorSet *sets = ar_alloc(c, n * sizeof(*sets) + 8);
            for (uint64_t i = 0; i < n; i++) sets[i] = HV(SPATHA_T_DSET, VkDescriptorSet, ids[i]);
            CMD_CHK();
            D.CmdBindDescriptorSets(cb, bp, lay, first, (uint32_t)n, sets, (uint32_t)nd, dyn); break; }
        case SPATHA_CMD_SET_VIEWPORT: {
            uint32_t first = (uint32_t)r_u64(&br); uint64_t n = r_u64(&br);
            const VkViewport *v = r_arr(&br, n, sizeof(VkViewport));
            CMD_CHK(); D.CmdSetViewport(cb, first, (uint32_t)n, v); break; }
        case SPATHA_CMD_SET_SCISSOR: {
            uint32_t first = (uint32_t)r_u64(&br); uint64_t n = r_u64(&br);
            const VkRect2D *v = r_arr(&br, n, sizeof(VkRect2D));
            CMD_CHK(); D.CmdSetScissor(cb, first, (uint32_t)n, v); break; }
        case SPATHA_CMD_DRAW: {
            uint32_t a = (uint32_t)r_u64(&br), b = (uint32_t)r_u64(&br), cc = (uint32_t)r_u64(&br), d = (uint32_t)r_u64(&br);
            CMD_CHK(); D.CmdDraw(cb, a, b, cc, d); break; }
        case SPATHA_CMD_DRAW_INDEXED: {
            uint32_t a = (uint32_t)r_u64(&br), b = (uint32_t)r_u64(&br), cc = (uint32_t)r_u64(&br);
            int32_t d = (int32_t)r_u64(&br); uint32_t e = (uint32_t)r_u64(&br);
            CMD_CHK(); D.CmdDrawIndexed(cb, a, b, cc, d, e); break; }
        case SPATHA_CMD_PIPELINE_BARRIER: {
            VkPipelineStageFlags ss = (VkPipelineStageFlags)r_u64(&br), ds = (VkPipelineStageFlags)r_u64(&br);
            VkDependencyFlags df = (VkDependencyFlags)r_u64(&br);
            uint64_t nm = r_u64(&br); const void *mb = r_arr(&br, nm, sizeof(VkMemoryBarrier));
            uint64_t nb = r_u64(&br); const void *bb = r_arr(&br, nb, sizeof(VkBufferMemoryBarrier));
            uint64_t ni = r_u64(&br); const void *ib = r_arr(&br, ni, sizeof(VkImageMemoryBarrier));
            CMD_CHK();
            VkBufferMemoryBarrier *b2 = ar_dup(c, bb, nb * sizeof(*b2));
            for (uint64_t i = 0; i < nb; i++) b2[i].buffer = HV(SPATHA_T_BUF, VkBuffer, H2U(b2[i].buffer));
            VkImageMemoryBarrier *i2 = ar_dup(c, ib, ni * sizeof(*i2));
            for (uint64_t i = 0; i < ni; i++) {
                i2[i].image = HV(SPATHA_T_IMG, VkImage, H2U(i2[i].image));
                i2[i].oldLayout = xl(i2[i].oldLayout); i2[i].newLayout = xl(i2[i].newLayout);
            }
            CMD_CHK();
            D.CmdPipelineBarrier(cb, ss, ds, df, (uint32_t)nm, mb, (uint32_t)nb, b2, (uint32_t)ni, i2); break; }
        case SPATHA_CMD_COPY_BUFFER_TO_IMAGE: {
            VkBuffer sb = HV(SPATHA_T_BUF, VkBuffer, r_u64(&br));
            VkImage di = HV(SPATHA_T_IMG, VkImage, r_u64(&br));
            VkImageLayout l = xl((VkImageLayout)r_u64(&br)); uint64_t n = r_u64(&br);
            const void *rg = r_arr(&br, n, sizeof(VkBufferImageCopy));
            CMD_CHK(); D.CmdCopyBufferToImage(cb, sb, di, l, (uint32_t)n, rg); break; }
        case SPATHA_CMD_COPY_IMAGE_TO_BUFFER: {
            VkImage si = HV(SPATHA_T_IMG, VkImage, r_u64(&br));
            VkImageLayout l = xl((VkImageLayout)r_u64(&br));
            VkBuffer db = HV(SPATHA_T_BUF, VkBuffer, r_u64(&br)); uint64_t n = r_u64(&br);
            const void *rg = r_arr(&br, n, sizeof(VkBufferImageCopy));
            CMD_CHK(); D.CmdCopyImageToBuffer(cb, si, l, db, (uint32_t)n, rg); break; }
        case SPATHA_CMD_COPY_BUFFER: {
            VkBuffer sb = HV(SPATHA_T_BUF, VkBuffer, r_u64(&br));
            VkBuffer db = HV(SPATHA_T_BUF, VkBuffer, r_u64(&br)); uint64_t n = r_u64(&br);
            const void *rg = r_arr(&br, n, sizeof(VkBufferCopy));
            CMD_CHK(); D.CmdCopyBuffer(cb, sb, db, (uint32_t)n, rg); break; }
        case SPATHA_CMD_COPY_IMAGE: {
            VkImage si = HV(SPATHA_T_IMG, VkImage, r_u64(&br)); VkImageLayout sl = xl((VkImageLayout)r_u64(&br));
            VkImage di = HV(SPATHA_T_IMG, VkImage, r_u64(&br)); VkImageLayout dl = xl((VkImageLayout)r_u64(&br));
            uint64_t n = r_u64(&br); const void *rg = r_arr(&br, n, sizeof(VkImageCopy));
            CMD_CHK(); D.CmdCopyImage(cb, si, sl, di, dl, (uint32_t)n, rg); break; }
        case SPATHA_CMD_BLIT_IMAGE: {
            VkImage si = HV(SPATHA_T_IMG, VkImage, r_u64(&br)); VkImageLayout sl = xl((VkImageLayout)r_u64(&br));
            VkImage di = HV(SPATHA_T_IMG, VkImage, r_u64(&br)); VkImageLayout dl = xl((VkImageLayout)r_u64(&br));
            uint64_t n = r_u64(&br); const void *rg = r_arr(&br, n, sizeof(VkImageBlit));
            VkFilter f = (VkFilter)r_u64(&br);
            CMD_CHK(); D.CmdBlitImage(cb, si, sl, di, dl, (uint32_t)n, rg, f); break; }
        case SPATHA_CMD_CLEAR_COLOR_IMAGE: {
            VkImage im = HV(SPATHA_T_IMG, VkImage, r_u64(&br)); VkImageLayout l = xl((VkImageLayout)r_u64(&br));
            RD_STRUCT(br, VkClearColorValue, col);
            uint64_t n = r_u64(&br); const void *rg = r_arr(&br, n, sizeof(VkImageSubresourceRange));
            CMD_CHK(); D.CmdClearColorImage(cb, im, l, &col, (uint32_t)n, rg); break; }
        case SPATHA_CMD_BIND_VERTEX_BUFFERS: {
            uint32_t first = (uint32_t)r_u64(&br); uint64_t n = r_u64(&br);
            const uint64_t *ids = r_arr(&br, n, 8); const VkDeviceSize *offs = r_arr(&br, n, 8);
            CMD_CHK();
            VkBuffer *bs = ar_alloc(c, n * sizeof(*bs) + 8);
            for (uint64_t i = 0; i < n; i++) bs[i] = HV(SPATHA_T_BUF, VkBuffer, ids[i]);
            CMD_CHK(); D.CmdBindVertexBuffers(cb, first, (uint32_t)n, bs, offs); break; }
        case SPATHA_CMD_BIND_INDEX_BUFFER: {
            VkBuffer b = HV(SPATHA_T_BUF, VkBuffer, r_u64(&br)); VkDeviceSize o = r_u64(&br);
            VkIndexType t = (VkIndexType)r_u64(&br);
            CMD_CHK(); D.CmdBindIndexBuffer(cb, b, o, t); break; }
        case SPATHA_CMD_PUSH_CONSTANTS: {
            VkPipelineLayout lay = HV(SPATHA_T_PLAYOUT, VkPipelineLayout, r_u64(&br));
            VkShaderStageFlags sf = (VkShaderStageFlags)r_u64(&br);
            uint32_t off = (uint32_t)r_u64(&br); uint64_t sz = r_u64(&br);
            const void *data = r_blob(&br, sz);
            CMD_CHK(); D.CmdPushConstants(cb, lay, sf, off, (uint32_t)sz, data); break; }
        case SPATHA_CMD_SET_LINE_WIDTH: {
            uint64_t v = r_u64(&br); float f; uint32_t u = (uint32_t)v; memcpy(&f, &u, 4);
            CMD_CHK(); D.CmdSetLineWidth(cb, f); break; }
        case SPATHA_CMD_SET_BLEND_CONSTANTS: {
            const float *f = r_blob(&br, 16); CMD_CHK(); D.CmdSetBlendConstants(cb, f); break; }
        case SPATHA_CMD_SET_DEPTH_BIAS: {
            const float *f = r_blob(&br, 12); CMD_CHK(); D.CmdSetDepthBias(cb, f[0], f[1], f[2]); break; }
        case SPATHA_CMD_FILL_BUFFER: {
            VkBuffer b = HV(SPATHA_T_BUF, VkBuffer, r_u64(&br));
            VkDeviceSize o = r_u64(&br), sz = r_u64(&br); uint32_t dt = (uint32_t)r_u64(&br);
            CMD_CHK(); D.CmdFillBuffer(cb, b, o, sz, dt); break; }
#include "spathad_cmds.inc"
        default: ok = 0; break;
        }
cmd_done:;
    }
    vr = D.EndCommandBuffer(cb);
    if (!ok) vr = VK_ERROR_UNKNOWN;
out:;
    ar_free(c);
    return reply_res(c, m, vr);
}

/* ---- submit / sincronizacion ---- */
static int h_queue_submit(struct client *c, const struct spatha_msg *m, const uint8_t *p) {
    struct rbuf r; r_init(&r, p, m->len);
    uint64_t dev = r_u64(&r), qid = r_u64(&r), fid = r_u64(&r), n = r_u64(&r);
    VkResult vr = VK_ERROR_UNKNOWN;
    V.bad = 0;
    if (r.err || !V.dev || dev != V.dev_id || n > 256) goto out;
    VkQueue q = HV(SPATHA_T_QUEUE, VkQueue, qid);
    VkFence f = HV(SPATHA_T_FENCE, VkFence, fid);
    VkSubmitInfo *si = ar_alloc(c, n * sizeof(*si) + 8);
    for (uint64_t i = 0; i < n; i++) {
        uint64_t nw = r_u64(&r); const uint64_t *wid = r_arr(&r, nw, 8);
        const VkPipelineStageFlags *ws = r_arr(&r, nw, 4);
        uint64_t nc = r_u64(&r); const uint64_t *cid = r_arr(&r, nc, 8);
        uint64_t ns = r_u64(&r); const uint64_t *sid = r_arr(&r, ns, 8);
        uint64_t has_tl = r_u64(&r), nwv = 0, nsv = 0;
        const uint64_t *wv = NULL, *sv = NULL;
        if (has_tl) {
            nwv = r_u64(&r); wv = r_arr(&r, nwv, 8);
            nsv = r_u64(&r); sv = r_arr(&r, nsv, 8);
        }
        CHK();
        VkSemaphore *wsem = ar_alloc(c, nw * sizeof(*wsem) + 8);
        VkCommandBuffer *cbs = ar_alloc(c, nc * sizeof(*cbs) + 8);
        VkSemaphore *ssem = ar_alloc(c, ns * sizeof(*ssem) + 8);
        for (uint64_t k = 0; k < nw; k++) wsem[k] = HV(SPATHA_T_SEM, VkSemaphore, wid[k]);
        for (uint64_t k = 0; k < nc; k++) cbs[k] = HV(SPATHA_T_CMDBUF, VkCommandBuffer, cid[k]);
        for (uint64_t k = 0; k < ns; k++) ssem[k] = HV(SPATHA_T_SEM, VkSemaphore, sid[k]);
        CHK();
        VkTimelineSemaphoreSubmitInfo *tl = NULL;
        if (has_tl) {
            tl = ar_alloc(c, sizeof(*tl));
            tl->sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
            tl->waitSemaphoreValueCount = (uint32_t)nwv;   tl->pWaitSemaphoreValues = wv;
            tl->signalSemaphoreValueCount = (uint32_t)nsv; tl->pSignalSemaphoreValues = sv;
        }
        si[i] = (VkSubmitInfo){ .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = tl,
            .waitSemaphoreCount = (uint32_t)nw, .pWaitSemaphores = wsem, .pWaitDstStageMask = ws,
            .commandBufferCount = (uint32_t)nc, .pCommandBuffers = cbs,
            .signalSemaphoreCount = (uint32_t)ns, .pSignalSemaphores = ssem };
    }
    CHK();
    vr = D.QueueSubmit(q, (uint32_t)n, si, f);
out:;
    ar_free(c);
    return reply_res(c, m, vr);
}

static int h_wait_idle(struct client *c, const struct spatha_msg *m, const uint8_t *p) {
    struct rbuf r; r_init(&r, p, m->len);
    uint64_t dev = r_u64(&r), qid = r_u64(&r);
    VkResult vr = VK_ERROR_UNKNOWN;
    V.bad = 0;
    if (!r.err && V.dev && dev == V.dev_id) {
        if (m->op == SPATHA_OP_DEVICE_WAIT_IDLE) vr = D.DeviceWaitIdle(V.dev);
        else { VkQueue q = HV(SPATHA_T_QUEUE, VkQueue, qid); if (!V.bad) vr = D.QueueWaitIdle(q); }
    }
    return reply_res(c, m, vr);
}

static int h_fences(struct client *c, const struct spatha_msg *m, const uint8_t *p) {
    struct rbuf r; r_init(&r, p, m->len);
    uint64_t dev = r_u64(&r), n = r_u64(&r);
    VkResult vr = VK_ERROR_UNKNOWN;
    V.bad = 0;
    const uint64_t *ids = (n && n <= 1024) ? r_arr(&r, n, 8) : NULL;
    if (r.err || !V.dev || dev != V.dev_id || !ids) goto out;
    VkFence *fs = ar_alloc(c, n * sizeof(*fs));
    for (uint64_t i = 0; i < n; i++) fs[i] = HV(SPATHA_T_FENCE, VkFence, ids[i]);
    CHK();
    if (m->op == SPATHA_OP_WAIT_FENCES) {
        uint64_t all = r_u64(&r), to = r_u64(&r);
        CHK();
        vr = D.WaitForFences(V.dev, (uint32_t)n, fs, all ? VK_TRUE : VK_FALSE, to);
    } else if (m->op == SPATHA_OP_RESET_FENCES) vr = D.ResetFences(V.dev, (uint32_t)n, fs);
    else vr = D.GetFenceStatus(V.dev, fs[0]);
out:;
    ar_free(c);
    return reply_res(c, m, vr);
}

/* ---- swapchain emulado ---- */
static int find_mem_type(struct client *c, uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags pref) {
    for (int pass = 0; pass < 2; pass++) {
        VkMemoryPropertyFlags need = pass == 0 ? (want | pref) : want;
        for (uint32_t i = 0; i < V.mp.memoryTypeCount; i++)
            if ((bits & (1u << i)) && (V.mp.memoryTypes[i].propertyFlags & need) == need) return (int)i;
    }
    return -1;
}

static int h_create_swapchain(struct client *c, const struct spatha_msg *m, const uint8_t *p) {
    struct rbuf r; r_init(&r, p, m->len);
    uint64_t dev = r_u64(&r);
    VkFormat fmt = (VkFormat)r_u64(&r);
    uint32_t w = (uint32_t)r_u64(&r), h = (uint32_t)r_u64(&r);
    VkImageUsageFlags usage = (VkImageUsageFlags)r_u64(&r);
    uint32_t want = (uint32_t)r_u64(&r);
    VkResult vr = VK_ERROR_UNKNOWN;
    struct wbuf wb; w_init(&wb);
    struct sc *s = NULL;
    VkDevice d = V.dev;
    uint64_t sidx = 0;

    if (r.err || !d || dev != V.dev_id || !w || !h || w > 16384 || h > 16384) goto out;
    if (fmt != VK_FORMAT_B8G8R8A8_UNORM && fmt != VK_FORMAT_B8G8R8A8_SRGB) { vr = VK_ERROR_FORMAT_NOT_SUPPORTED; goto out; }
    for (int i = 0; i < MAX_SC; i++) if (!V.scs[i].used) { s = &V.scs[i]; sidx = (uint64_t)i + 1; break; }
    if (!s) { vr = VK_ERROR_TOO_MANY_OBJECTS; goto out; }
    memset(s, 0, sizeof(*s));
    s->used = 1; s->fmt = fmt; s->w = w; s->h = h;
    s->n = want < 2 ? 2 : (want > MAX_SC_IMG ? MAX_SC_IMG : want);

    uint32_t target_n = s->n;
    s->n = 0;
    for (uint32_t i = 0; i < target_n; i++) {
        VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
            .format = fmt, .extent = { w, h, 1 }, .mipLevels = 1, .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
        VkImage im = VK_NULL_HANDLE;
        vr = D.CreateImage(d, &ci, NULL, &im);
        if (vr != VK_SUCCESS) goto fail;
        s->img_id[i] = obj_add(c, SPATHA_T_IMG, H2U(im), 0, 0);
        s->n = i + 1;
        VkMemoryRequirements mr; D.GetImageMemoryRequirements(d, im, &mr);
        int mt = find_mem_type(c, mr.memoryTypeBits, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mt < 0) { vr = VK_ERROR_OUT_OF_DEVICE_MEMORY; goto fail; }
        VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)mt };
        vr = D.AllocateMemory(d, &ai, NULL, &s->img_mem[i]);
        if (vr != VK_SUCCESS) goto fail;
        vr = D.BindImageMemory(d, im, s->img_mem[i], 0);
        if (vr != VK_SUCCESS) goto fail;
    }
    {
        VkBufferCreateInfo bc = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = (VkDeviceSize)w * h * 4, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
        vr = D.CreateBuffer(d, &bc, NULL, &s->rb);
        if (vr != VK_SUCCESS) goto fail;
        VkMemoryRequirements mr; D.GetBufferMemoryRequirements(d, s->rb, &mr);
        int mt = find_mem_type(c, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        if (mt < 0) { vr = VK_ERROR_OUT_OF_HOST_MEMORY; goto fail; }
        s->rb_coherent = (V.mp.memoryTypes[mt].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
        VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)mt };
        vr = D.AllocateMemory(d, &ai, NULL, &s->rb_mem);
        if (vr != VK_SUCCESS) goto fail;
        vr = D.BindBufferMemory(d, s->rb, s->rb_mem, 0);
        if (vr != VK_SUCCESS) goto fail;
        void *mp = NULL;
        vr = D.MapMemory(d, s->rb_mem, 0, VK_WHOLE_SIZE, 0, &mp);
        if (vr != VK_SUCCESS) goto fail;
        s->rb_map = mp;
    }
    {
        VkCommandPoolCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = V.gq_family };
        vr = D.CreateCommandPool(d, &pi, NULL, &s->pool);
        if (vr != VK_SUCCESS) goto fail;
        VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = s->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
        vr = D.AllocateCommandBuffers(d, &cai, &s->cb);
        if (vr != VK_SUCCESS) goto fail;
        VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        vr = D.CreateFence(d, &fi, NULL, &s->fence);
        if (vr != VK_SUCCESS) goto fail;
    }
    w_u64(&wb, sidx); w_u64(&wb, s->n);
    for (uint32_t i = 0; i < s->n; i++) w_u64(&wb, s->img_id[i]);
    DBG("create_swapchain %ux%u fmt=%d n=%u -> sc=%llu", w, h, (int)fmt, s->n, (unsigned long long)sidx);
    vr = VK_SUCCESS;
    goto out;
fail:
    sc_destroy(c, s);
out:;
    int rc = reply_data(c, m, vr, wb.p, wb.len);
    w_free(&wb);
    return rc;
}

static struct sc *sc_get(struct client *c, uint64_t id) {
    return (id >= 1 && id <= MAX_SC && V.scs[id - 1].used) ? &V.scs[id - 1] : NULL;
}

static int h_destroy_swapchain(struct client *c, const struct spatha_msg *m, const uint8_t *p) {
    struct rbuf r; r_init(&r, p, m->len);
    uint64_t dev = r_u64(&r), id = r_u64(&r);
    struct sc *s = sc_get(c, id);
    if (r.err || !V.dev || dev != V.dev_id || !s) return reply_res(c, m, VK_ERROR_UNKNOWN);
    sc_destroy(c, s);
    return reply_res(c, m, VK_SUCCESS);
}

static int h_acquire(struct client *c, const struct spatha_msg *m, const uint8_t *p) {
    struct rbuf r; r_init(&r, p, m->len);
    uint64_t dev = r_u64(&r), id = r_u64(&r), to = r_u64(&r), semid = r_u64(&r), fid = r_u64(&r);
    struct sc *s = sc_get(c, id);
    VkResult vr = VK_ERROR_UNKNOWN;
    uint64_t idx = 0;
    V.bad = 0;
    (void)to;
    if (!r.err && V.dev && dev == V.dev_id && s) {
        VkSemaphore sem = HV(SPATHA_T_SEM, VkSemaphore, semid);
        VkFence f = HV(SPATHA_T_FENCE, VkFence, fid);
        if (!V.bad) {
            idx = s->next; s->next = (s->next + 1) % s->n;
            if (sem || f) {
                VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                    .signalSemaphoreCount = sem ? 1 : 0, .pSignalSemaphores = &sem };
                vr = D.QueueSubmit(V.gq, 1, &si, f);
            } else vr = VK_SUCCESS;
        }
    }
    return reply_data(c, m, vr, &idx, 8);
}

static int h_present(struct client *c, const struct spatha_msg *m, const uint8_t *p) {
    struct rbuf r; r_init(&r, p, m->len);
    uint64_t dev = r_u64(&r), qid = r_u64(&r), id = r_u64(&r), idx = r_u64(&r), nw = r_u64(&r);
    struct sc *s = sc_get(c, id);
    VkResult vr = VK_ERROR_UNKNOWN;
    uint8_t *out = NULL; size_t outlen = 0;
    V.bad = 0;
    const uint64_t *wid = (nw <= 64) ? r_arr(&r, nw, 8) : NULL;
    if (r.err || !V.dev || dev != V.dev_id || !s || idx >= s->n || !wid) goto done;
    VkQueue q = HV(SPATHA_T_QUEUE, VkQueue, qid);
    VkSemaphore *ws = ar_alloc(c, nw * sizeof(*ws) + 8);
    VkPipelineStageFlags *st = ar_alloc(c, nw * sizeof(*st) + 8);
    for (uint64_t i = 0; i < nw; i++) { ws[i] = HV(SPATHA_T_SEM, VkSemaphore, wid[i]); st[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT; }
    VkImage im = HV(SPATHA_T_IMG, VkImage, s->img_id[idx]);
    if (V.bad) goto done;

    VkCommandBuffer cb = s->cb;
    D.ResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    D.BeginCommandBuffer(cb, &bi);
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = im, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    D.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkBufferImageCopy rg = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                             .imageExtent = { s->w, s->h, 1 } };
    D.CmdCopyImageToBuffer(cb, im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s->rb, 1, &rg);
    VkBufferMemoryBarrier bb = { .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = s->rb, .size = VK_WHOLE_SIZE };
    D.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &bb, 0, NULL);
    D.EndCommandBuffer(cb);

    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = (uint32_t)nw, .pWaitSemaphores = ws, .pWaitDstStageMask = st,
        .commandBufferCount = 1, .pCommandBuffers = &cb };
    vr = D.QueueSubmit(q, 1, &si, s->fence);
    if (vr != VK_SUCCESS) goto done;
    vr = D.WaitForFences(V.dev, 1, &s->fence, VK_TRUE, 10ull * 1000000000ull);
    D.ResetFences(V.dev, 1, &s->fence);
    if (vr != VK_SUCCESS) goto done;
    if (!s->rb_coherent) {
        VkMappedMemoryRange mr = { .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = s->rb_mem,
                                   .size = VK_WHOLE_SIZE };
        D.InvalidateMappedMemoryRanges(V.dev, 1, &mr);
    }
    size_t pix = (size_t)s->w * s->h * 4;
    outlen = 16 + pix;
    out = malloc(outlen);
    if (!out) oom();
    uint64_t dims[2] = { s->w, s->h };
    memcpy(out, dims, 16);
    memcpy(out + 16, s->rb_map, pix);
done:;
    ar_free(c);
    int rc = reply_data(c, m, vr, out, outlen);
    free(out);
    return rc;
}

static int dispatch_op(struct client *c, const struct spatha_msg *m, const uint8_t *payload);

static int dispatch(struct client *c, const struct spatha_msg *m, const uint8_t *payload) {
    c->n_msgs++;
    DBG("fd=%d op=0x%02x req=%u len=%u", c->fd, m->op, m->req_id, m->len);

    uint8_t *aligned = NULL;
    if (m->len && ((uintptr_t)payload & 7)) {
        aligned = malloc(m->len);
        if (!aligned) oom();
        memcpy(aligned, payload, m->len);
        payload = aligned;
    }
    int rc = dispatch_op(c, m, payload);
    free(aligned);
    return rc;
}

static int dispatch_op(struct client *c, const struct spatha_msg *m, const uint8_t *payload) {

    switch (m->op) {
    case SPATHA_OP_PING:
        return spatha_send(c->fd, SPATHA_OP_PONG, m->req_id, NULL, 0);
    case SPATHA_OP_QUIT:
        return -ECONNRESET;
    case SPATHA_OP_VK_CREATE_INSTANCE:
        return h_create_instance(c, m, payload);
    case SPATHA_OP_VK_DESTROY_INSTANCE:
        return h_destroy_instance(c, m, payload);
    case SPATHA_OP_VK_ENUMERATE_PHYSICAL_DEVICES:
        return h_enum_pd(c, m, payload);
    case SPATHA_OP_VK_GET_PHYSICAL_DEVICE_PROPERTIES:
        return h_get_pd_props(c, m, payload);
    case SPATHA_OP_VK_ENUMERATE_INSTANCE_EXTENSION_PROPERTIES:
        return h_enum_ext(c, m);
    case SPATHA_OP_VK_GET_PHYSICAL_DEVICE_QUEUE_FAMILY_PROPERTIES:
        return h_get_pd_qf(c, m, payload);
    case SPATHA_OP_GET_PD_PROPS2: case SPATHA_OP_GET_PD_FEATURES: case SPATHA_OP_GET_PD_MEMPROPS:
    case SPATHA_OP_GET_PD_FORMAT_PROPS: case SPATHA_OP_GET_PD_IMAGE_FORMAT_PROPS:
        return h_pd_query(c, m, payload);
    case SPATHA_OP_GET_PD_CHAIN:         return h_pd_chain(c, m, payload);
    case SPATHA_OP_CREATE_DEVICE:        return h_create_device(c, m, payload);
    case SPATHA_OP_DESTROY_DEVICE:       return h_destroy_device(c, m, payload);
    case SPATHA_OP_CREATE:               return h_create(c, m, payload);
    case SPATHA_OP_DESTROY:              return h_destroy(c, m, payload);
    case SPATHA_OP_CREATE_GFX_PIPELINES: return h_create_gfx(c, m, payload);
    case SPATHA_OP_ALLOC_DESC_SETS:      return h_alloc_dsets(c, m, payload);
    case SPATHA_OP_FREE_DESC_SETS:       return h_free_dsets(c, m, payload);
    case SPATHA_OP_UPDATE_DESC_SETS:     return h_update_dsets(c, m, payload);
    case SPATHA_OP_SEM_OP:               return h_sem_op(c, m, payload);
    case SPATHA_OP_EVENT_OP:             return h_event_op(c, m, payload);
    case SPATHA_OP_GET_QUERY_RESULTS:    return h_query_results(c, m, payload);
    case SPATHA_OP_RESET_QUERY_POOL:     return h_reset_query(c, m, payload);
    case SPATHA_OP_PCACHE_OP:            return h_pcache_op(c, m, payload);
    case SPATHA_OP_CREATE_COMPUTE_PIPELINES: return h_create_compute(c, m, payload);
    case SPATHA_OP_RESET_POOL:           return h_reset_pool(c, m, payload);
    case SPATHA_OP_DSL_SUPPORT:          return h_dsl_support(c, m, payload);
    case SPATHA_OP_CREATE_RENDERPASS2:   return h_create_rp2(c, m, payload);
    case SPATHA_OP_ALLOC_MEMORY:         return h_alloc_memory(c, m, payload);
    case SPATHA_OP_BIND_MEMORY:          return h_bind_memory(c, m, payload);
    case SPATHA_OP_GET_MEM_REQS:         return h_get_mem_reqs(c, m, payload);
    case SPATHA_OP_MEM_WRITE:            return h_mem_write(c, m, payload);
    case SPATHA_OP_MEM_READ:             return h_mem_read(c, m, payload);
    case SPATHA_OP_GET_SUBRES_LAYOUT:    return h_subres_layout(c, m, payload);
    case SPATHA_OP_GET_BUFFER_ADDR:      return h_buffer_addr(c, m, payload);
    case SPATHA_OP_ALLOC_CMDBUFS:        return h_alloc_cmdbufs(c, m, payload);
    case SPATHA_OP_FREE_CMDBUFS:         return h_free_cmdbufs(c, m, payload);
    case SPATHA_OP_CMDBUF_REPLAY:        return h_cmdbuf_replay(c, m, payload);
    case SPATHA_OP_QUEUE_SUBMIT:         return h_queue_submit(c, m, payload);
    case SPATHA_OP_QUEUE_WAIT_IDLE: case SPATHA_OP_DEVICE_WAIT_IDLE:
        return h_wait_idle(c, m, payload);
    case SPATHA_OP_WAIT_FENCES: case SPATHA_OP_RESET_FENCES: case SPATHA_OP_GET_FENCE_STATUS:
        return h_fences(c, m, payload);
    case SPATHA_OP_CREATE_SWAPCHAIN:     return h_create_swapchain(c, m, payload);
    case SPATHA_OP_DESTROY_SWAPCHAIN:    return h_destroy_swapchain(c, m, payload);
    case SPATHA_OP_ACQUIRE_NEXT_IMAGE:   return h_acquire(c, m, payload);
    case SPATHA_OP_QUEUE_PRESENT:        return h_present(c, m, payload);
    default:
        return reply_error(c, m->req_id, EINVAL);
    }
}

/* ---- framing / loop ---- */

static int process(struct client *c) {
    size_t off = 0;
    while (c->len - off >= sizeof(struct spatha_msg)) {
        struct spatha_msg m;
        memcpy(&m, c->buf + off, sizeof(m));
        if (m.magic != SPATHA_MAGIC || m.len > SPATHA_MAX_PAYLOAD) {
            DBG("fd=%d framing invalido", c->fd);
            return -EPROTO;
        }
        size_t total = sizeof(m) + m.len;
        if (c->len - off < total) {
            if (off + total > c->cap) {
                if (off) { memmove(c->buf, c->buf + off, c->len - off); c->len -= off; off = 0; }
                if (total > c->cap) {
                    uint8_t *nb = realloc(c->buf, total);
                    if (!nb) return -ENOMEM;
                    c->buf = nb; c->cap = total;
                }
            }
            break;
        }
        int rc = dispatch(c, &m, c->buf + off + sizeof(m));
        if (rc < 0) return rc;
        off += total;
    }
    if (off) {
        memmove(c->buf, c->buf + off, c->len - off);
        c->len -= off;
    }
    return 0;
}

static int on_readable(struct client *c) {
    if (c->len == c->cap) return -EPROTO;
    ssize_t r = recv(c->fd, c->buf + c->len, c->cap - c->len, MSG_DONTWAIT);
    if (r == 0) return -ECONNRESET;
    if (r < 0) return (errno == EAGAIN || errno == EINTR) ? 0 : -errno;
    c->len += (size_t)r;
    return process(c);
}

static void accept_client(int lfd) {
    int fd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
    if (fd < 0) return;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].fd < 0) {
            g_clients[i].buf = malloc(BUF_INIT);
            if (!g_clients[i].buf) break;
            g_clients[i].cap = BUF_INIT;
            g_clients[i].fd = fd;
            DBG("cliente fd=%d (slot %d)", fd, i);
            return;
        }
    }
    DBG("sin slots; rechazando fd=%d", fd);
    close(fd);
}

int main(void) {
    const char *path = spatha_sock_path();
    g_debug = getenv("SPATHA_DEBUG") != NULL;

    if (load_vulkan() < 0) return 1;

    for (int i = 0; i < MAX_CLIENTS; i++) g_clients[i].fd = -1;

    struct sigaction sa = { .sa_handler = on_signal };
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    int probe = spatha_connect(path, 200);
    if (probe >= 0) {
        close(probe);
        fprintf(stderr, "[spathad] ya hay un daemon en %s\n", path);
        return 1;
    }
    unlink(path);

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    if (strlen(path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "[spathad] path demasiado largo\n");
        return 1;
    }
    strcpy(addr.sun_path, path);

    int lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (lfd < 0) { perror("socket"); return 1; }
    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    chmod(path, 0666);
    if (listen(lfd, 16) < 0) { perror("listen"); return 1; }

    printf("[spathad] listening on %s\n", path);
    fflush(stdout);

    struct pollfd pfds[MAX_CLIENTS + 1];
    while (!g_stop) {
        int n = 0, map[MAX_CLIENTS + 1];
        pfds[n] = (struct pollfd){ lfd, POLLIN, 0 }; map[n++] = -1;
        for (int i = 0; i < MAX_CLIENTS; i++)
            if (g_clients[i].fd >= 0) {
                pfds[n] = (struct pollfd){ g_clients[i].fd, POLLIN, 0 };
                map[n++] = i;
            }

        if (poll(pfds, (nfds_t)n, -1) < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }
        for (int k = 0; k < n; k++) {
            if (!pfds[k].revents) continue;
            if (map[k] < 0) { accept_client(lfd); continue; }
            struct client *c = &g_clients[map[k]];
            if (on_readable(c) < 0 || (pfds[k].revents & (POLLERR | POLLNVAL)))
                client_close(c);
        }
    }

    for (int i = 0; i < MAX_CLIENTS; i++)
        if (g_clients[i].fd >= 0) client_close(&g_clients[i]);
    close(lfd);
    unlink(path);
    printf("[spathad] bye\n");
    return 0;
}

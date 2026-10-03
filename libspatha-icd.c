/*
 * Spatha ICD stub — Hito 2: forwarding de CreateInstance,
 * EnumeratePhysicalDevices y GetPhysicalDeviceProperties al daemon.
 *
 * Modo daemon (env SPATHA_ICD_DAEMON):
 *   off     (default) no toca el socket; Hito 1 puro.
 *   try     intenta conectar + forwardear; si falla, sigue como Hito 1
 *           (instancia local, 0 physical devices).
 *   require si falla la conexion o el CreateInstance remoto ->
 *           VK_ERROR_INITIALIZATION_FAILED
 *           (el loader cae al siguiente ICD, p.ej. llvmpipe).
 * Debug: SPATHA_DEBUG=1 imprime a stderr.
 */
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdalign.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <vulkan/vulkan.h>
#include <vulkan/vk_icd.h>
#include "proto.h"
#include "wire.h"
#include "chain.h"

static void xw(struct wbuf *w, uint32_t type, const void *pnext);
static VkResult dev_features_write(struct wbuf *w, const VkDeviceCreateInfo *ci);
static uint64_t mem_alloc_flags(const VkMemoryAllocateInfo *ai);

#define SPATHA_EXPORT __attribute__((visibility("default")))
#define SPATHA_ICD_INTERFACE_VERSION 7
#define SPATHA_CONNECT_TIMEOUT_MS    2000

#define DBG(...) do { if (getenv("SPATHA_DEBUG")) { \
    fprintf(stderr, "[spatha-icd] " __VA_ARGS__); fputc('\n', stderr); } } while (0)

enum daemon_mode { MODE_OFF, MODE_TRY, MODE_REQUIRE };

static enum daemon_mode get_mode(void) {
    const char *m = getenv("SPATHA_ICD_DAEMON");
    if (!m) return MODE_OFF;
    if (!strcmp(m, "require")) return MODE_REQUIRE;
    if (!strcmp(m, "try"))     return MODE_TRY;
    return MODE_OFF;
}

struct spatha_physical_device;

struct spatha_instance {
    VK_LOADER_DATA loader_data;    /* DEBE ser el primer campo */
    VkAllocationCallbacks alloc;   /* copia; valida si has_alloc */
    int has_alloc;

    pthread_mutex_t lock;          /* serializa RPC sobre el socket */
    int sock;                      /* -1 si no hay conexion */
    uint32_t next_req;
    uint32_t remote_id;            /* instance_id en el daemon; 0 = sin remoto */

    pthread_mutex_t pd_lock;       /* protege el cache de physical devices */
    int pds_valid;
    uint32_t n_pds;
    struct spatha_physical_device *pds[SPATHA_MAX_PD];
};

/* Handle dispatchable: igual, VK_LOADER_DATA primero. */
struct spatha_physical_device {
    VK_LOADER_DATA loader_data;
    uint32_t remote_id;
    struct spatha_instance *inst;

    /* cache de queue families (el loader pregunta dos veces); lo protege inst->pd_lock */
    int qf_valid;
    uint32_t n_qf;
    VkQueueFamilyProperties qf[SPATHA_MAX_QF];

    /* caches Hito 4 (los protege inst->pd_lock) */
    int props_valid;  VkPhysicalDeviceProperties props;
    int mp_valid;     VkPhysicalDeviceMemoryProperties mp;
    int feat_valid;   VkPhysicalDeviceFeatures feat;
};

/* ---- alocacion (respeta VkAllocationCallbacks) ---- */

static void *raw_alloc(const VkAllocationCallbacks *a, size_t sz) {
    if (a && a->pfnAllocation) {
        void *p = a->pfnAllocation(a->pUserData, sz, alignof(max_align_t),
                                   VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
        if (p) memset(p, 0, sz);
        return p;
    }
    return calloc(1, sz);
}

static void raw_free(const VkAllocationCallbacks *a, void *p) {
    if (a && a->pfnFree) a->pfnFree(a->pUserData, p);
    else free(p);
}

static const VkAllocationCallbacks *inst_cb(const struct spatha_instance *i) {
    return i->has_alloc ? &i->alloc : NULL;
}

static struct spatha_instance *inst_alloc(const VkAllocationCallbacks *a) {
    struct spatha_instance *i = raw_alloc(a, sizeof(*i));
    if (!i) return NULL;
    if (a && a->pfnAllocation) { i->alloc = *a; i->has_alloc = 1; }
    i->sock = -1;
    pthread_mutex_init(&i->lock, NULL);
    pthread_mutex_init(&i->pd_lock, NULL);
    return i;
}

static void inst_free(struct spatha_instance *i) {
    VkAllocationCallbacks cb = i->alloc;
    int has = i->has_alloc;
    pthread_mutex_destroy(&i->lock);
    pthread_mutex_destroy(&i->pd_lock);
    raw_free(has ? &cb : NULL, i);
}

/* ---- conexion y RPC ---- */

static void drop_conn(struct spatha_instance *inst) {
    if (inst->sock >= 0) close(inst->sock);
    inst->sock = -1;
}

static int daemon_handshake(struct spatha_instance *inst) {
    int fd = spatha_connect(spatha_sock_path(), SPATHA_CONNECT_TIMEOUT_MS);
    if (fd < 0) { DBG("connect %s: %s", spatha_sock_path(), strerror(-fd)); return fd; }

    uint32_t req = inst->next_req++;
    struct spatha_msg r;
    int rc = spatha_send(fd, SPATHA_OP_PING, req, NULL, 0);
    if (!rc) rc = spatha_recv(fd, &r, NULL, 0);
    if (!rc && (r.op != SPATHA_OP_PONG || r.req_id != req)) rc = -EPROTO;
    if (rc) { DBG("handshake: %s", strerror(-rc)); close(fd); return rc; }

    inst->sock = fd;
    DBG("daemon conectado (fd=%d)", fd);
    return 0;
}

/* Un request/response. `resp` debe tener cap >= 4 (cabe un OP_ERROR).
 * Error de transporte/protocolo => se cierra la conexion (stream no confiable);
 * las llamadas siguientes fallan rapido con -ENOTCONN. */
static int rpc(struct spatha_instance *inst, uint32_t op,
               const void *req, uint32_t req_len,
               void *resp, uint32_t cap, uint32_t *resp_len)
{
    struct spatha_msg h;
    uint32_t id;
    int rc;

    pthread_mutex_lock(&inst->lock);
    if (inst->sock < 0) {
        rc = -ENOTCONN;
    } else {
        id = inst->next_req++;
        rc = spatha_send(inst->sock, op, id, req, req_len);
        if (!rc) rc = spatha_recv(inst->sock, &h, resp, cap);
        if (!rc && (h.req_id != id || (h.op != op && h.op != SPATHA_OP_ERROR)))
            rc = -EPROTO;
        if (rc) {
            DBG("rpc op=0x%02x: %s (conexion cerrada)", op, strerror(-rc));
            drop_conn(inst);
        } else if (h.op == SPATHA_OP_ERROR) {
            rc = -EIO;                 /* error de aplicacion; conexion sana */
        } else if (resp_len) {
            *resp_len = h.len;
        }
    }
    pthread_mutex_unlock(&inst->lock);
    return rc;
}

/* ---- entry points ---- */

static VkResult fetch_extensions(void);
static VkResult translate_inst_exts(uint32_t n, const char *const *req,
                                    char out[][SPATHA_EXT_NAME_MAX], uint32_t *n_out);

static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkCreateInstance(const VkInstanceCreateInfo *ci,
                      const VkAllocationCallbacks *alloc,
                      VkInstance *out)
{
    if (!ci || !out || ci->sType != VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO)
        return VK_ERROR_INITIALIZATION_FAILED;
    *out = VK_NULL_HANDLE;

    /* (No devolvemos INCOMPATIBLE_DRIVER por apiVersion: la spec lo prohibe
     * en implementaciones >= 1.1.)
     * Extensiones: las pedidas por la app (vista del contenedor) se traducen
     * a nombres del host. Sin daemon no hay extensiones que ofrecer. */
    char host_names[SPATHA_MAX_INST_EXT][SPATHA_EXT_NAME_MAX];
    uint32_t n_host = 0;
    if (ci->enabledExtensionCount > 0) {
        if (get_mode() == MODE_OFF) return VK_ERROR_EXTENSION_NOT_PRESENT;
        fetch_extensions();
        VkResult tr = translate_inst_exts(ci->enabledExtensionCount,
                                          ci->ppEnabledExtensionNames, host_names, &n_host);
        if (tr != VK_SUCCESS) {
            DBG("extension no disponible (result=%d)", tr);
            return tr;
        }
    }

    struct spatha_instance *inst = inst_alloc(alloc);
    if (!inst) return VK_ERROR_OUT_OF_HOST_MEMORY;
    set_loader_magic_value(inst);

    enum daemon_mode mode = get_mode();
    if (mode != MODE_OFF) {
        int ok = 0;
        if (daemon_handshake(inst) == 0) {
            struct spatha_resp_create_instance r;
            uint32_t rl = 0;
            uint8_t creq[4 + SPATHA_MAX_INST_EXT * SPATHA_EXT_NAME_MAX];
            uint32_t creq_len = 0;
            if (n_host) {
                memcpy(creq, &n_host, 4);
                for (uint32_t i = 0; i < n_host; i++)
                    memcpy(creq + 4 + (size_t)i * SPATHA_EXT_NAME_MAX, host_names[i],
                           SPATHA_EXT_NAME_MAX);
                creq_len = 4 + n_host * SPATHA_EXT_NAME_MAX;
            }
            int rc = rpc(inst, SPATHA_OP_VK_CREATE_INSTANCE, n_host ? creq : NULL, creq_len,
                         &r, sizeof(r), &rl);
            if (!rc && rl == sizeof(r) && r.vk_result == VK_SUCCESS && r.instance_id) {
                inst->remote_id = r.instance_id;
                ok = 1;
            } else {
                DBG("CreateInstance remoto fallo (rc=%d result=%d)", rc, rc ? 0 : r.vk_result);
                drop_conn(inst);
            }
        }
        if (!ok && mode == MODE_REQUIRE) {
            inst_free(inst);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        /* modo try + fallo: sigue como Hito 1 (remote_id = 0) */
    }

    *out = (VkInstance)inst;
    DBG("instance %p creada (remote_id=%u)", (void *)inst, inst->remote_id);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
stub_vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks *alloc)
{
    (void)alloc; /* usamos la copia guardada en create */
    struct spatha_instance *inst = (struct spatha_instance *)instance;
    if (!inst) return;

    if (inst->remote_id) {
        int32_t res;
        uint32_t rl = 0;
        rpc(inst, SPATHA_OP_VK_DESTROY_INSTANCE, &inst->remote_id, 4, &res, sizeof(res), &rl);
    }
    if (inst->sock >= 0) {
        spatha_send(inst->sock, SPATHA_OP_QUIT, inst->next_req++, NULL, 0);
        drop_conn(inst);
    }
    /* Vulkan no tiene destroy para physical devices: los liberamos aca. */
    for (uint32_t i = 0; i < inst->n_pds; i++)
        raw_free(inst_cb(inst), inst->pds[i]);

    DBG("instance %p destruida", (void *)inst);
    inst_free(inst);
}

/* ====================================================================
 * Extensiones de instancia.
 * El contenedor ve la lista del host SIN extensiones de superficie/display/debug
 * (no existen alli), mas VK_KHR_surface y VK_KHR_xcb_surface, que implementa
 * este ICD: la presentacion es por readback (el daemon copia la imagen a un
 * buffer, el ICD la dibuja en la ventana X con xcb_put_image). Esas dos
 * extensiones NO se reenvian al host.
 * ==================================================================== */
#define EXT_SURFACE "VK_KHR_surface"
#define EXT_XCB     "VK_KHR_xcb_surface"

static pthread_mutex_t g_ext_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_ext_valid = 0;
static uint32_t g_ext_count = 0;
static VkExtensionProperties *g_ext_props = NULL;
static uint32_t g_vis_count = 0;
static VkExtensionProperties *g_vis_props = NULL;

static int ext_hidden(const char *n) {
    return strstr(n, "surface") || strstr(n, "display") || strstr(n, "debug_") ||
           strstr(n, "swapchain") || strstr(n, "acquire_") || strstr(n, "present");
}

static void build_visible_locked(void) {
    free(g_vis_props); g_vis_props = NULL; g_vis_count = 0;
    g_vis_props = malloc((g_ext_count + 2) * sizeof(VkExtensionProperties));
    if (!g_vis_props) return;
    VkExtensionProperties x;
    memset(&x, 0, sizeof(x)); strcpy(x.extensionName, EXT_SURFACE); x.specVersion = 25;
    g_vis_props[g_vis_count++] = x;
    memset(&x, 0, sizeof(x)); strcpy(x.extensionName, EXT_XCB); x.specVersion = 6;
    g_vis_props[g_vis_count++] = x;
    for (uint32_t i = 0; i < g_ext_count; i++)
        if (!ext_hidden(g_ext_props[i].extensionName))
            g_vis_props[g_vis_count++] = g_ext_props[i];
}

static VkResult fetch_extensions(void) {
    pthread_mutex_lock(&g_ext_lock);
    if (g_ext_valid) { pthread_mutex_unlock(&g_ext_lock); return VK_SUCCESS; }
    int fd = spatha_connect(spatha_sock_path(), SPATHA_CONNECT_TIMEOUT_MS);
    if (fd < 0) {
        DBG("ext: connect %s: %s", spatha_sock_path(), strerror(-fd));
        pthread_mutex_unlock(&g_ext_lock);
        return VK_SUCCESS;
    }
    uint32_t req_id = 1;
    int rc = spatha_send(fd, SPATHA_OP_VK_ENUMERATE_INSTANCE_EXTENSION_PROPERTIES, req_id, NULL, 0);
    if (!rc) {
        static uint8_t buf[64 * 1024];
        struct spatha_msg h;
        rc = spatha_recv(fd, &h, buf, sizeof(buf));
        if (!rc && h.op == SPATHA_OP_VK_ENUMERATE_INSTANCE_EXTENSION_PROPERTIES &&
            h.req_id == req_id && h.len >= sizeof(struct spatha_resp_enum_ext)) {
            struct spatha_resp_enum_ext hdr;
            memcpy(&hdr, buf, sizeof(hdr));
            if (hdr.vk_result == VK_SUCCESS &&
                hdr.count <= (sizeof(buf) - sizeof(hdr)) / sizeof(VkExtensionProperties) &&
                h.len == sizeof(hdr) + hdr.count * sizeof(VkExtensionProperties)) {
                g_ext_count = hdr.count;
                if (hdr.count > 0) {
                    g_ext_props = malloc(hdr.count * sizeof(VkExtensionProperties));
                    if (g_ext_props) memcpy(g_ext_props, buf + sizeof(hdr), hdr.count * sizeof(VkExtensionProperties));
                    else g_ext_count = 0;
                }
                g_ext_valid = 1;
                build_visible_locked();
                DBG("ext: %u extension(es) remotas cacheadas", g_ext_count);
            }
        }
    }
    close(fd);
    pthread_mutex_unlock(&g_ext_lock);
    return VK_SUCCESS;
}

static int host_has_ext(const char *name) {
    pthread_mutex_lock(&g_ext_lock);
    int r = 0;
    for (uint32_t i = 0; i < g_ext_count; i++)
        if (!strncmp(g_ext_props[i].extensionName, name, VK_MAX_EXTENSION_NAME_SIZE)) r = 1;
    pthread_mutex_unlock(&g_ext_lock);
    return r;
}

/* surface/xcb las resuelve este ICD (no van al host); el resto debe existir en el host. */
static VkResult translate_inst_exts(uint32_t n, const char *const *req,
                                    char out[][SPATHA_EXT_NAME_MAX], uint32_t *n_out)
{
    *n_out = 0;
    for (uint32_t i = 0; i < n; i++) {
        const char *want = req ? req[i] : NULL;
        if (!want) return VK_ERROR_EXTENSION_NOT_PRESENT;
        if (!strcmp(want, EXT_SURFACE) || !strcmp(want, EXT_XCB)) continue;
        if (ext_hidden(want) || strlen(want) >= SPATHA_EXT_NAME_MAX || !host_has_ext(want))
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        int dup = 0;
        for (uint32_t j = 0; j < *n_out; j++) if (!strcmp(out[j], want)) dup = 1;
        if (dup) continue;
        if (*n_out >= SPATHA_MAX_INST_EXT) return VK_ERROR_EXTENSION_NOT_PRESENT;
        memset(out[*n_out], 0, SPATHA_EXT_NAME_MAX);
        strcpy(out[(*n_out)++], want);
    }
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkEnumerateInstanceExtensionProperties(const char *layer, uint32_t *count, VkExtensionProperties *props)
{
    if (layer) return VK_ERROR_LAYER_NOT_PRESENT;
    if (!count) return VK_ERROR_INITIALIZATION_FAILED;
    if (get_mode() == MODE_OFF) { *count = 0; return VK_SUCCESS; }
    fetch_extensions();
    pthread_mutex_lock(&g_ext_lock);
    uint32_t total = g_vis_count;
    if (!props) { *count = total; pthread_mutex_unlock(&g_ext_lock); return VK_SUCCESS; }
    uint32_t n = *count < total ? *count : total;
    if (g_vis_props && n) memcpy(props, g_vis_props, n * sizeof(VkExtensionProperties));
    pthread_mutex_unlock(&g_ext_lock);
    *count = n;
    return n < total ? VK_INCOMPLETE : VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkEnumerateInstanceLayerProperties(uint32_t *count, VkLayerProperties *props)
{ (void)props; if (!count) return VK_ERROR_INITIALIZATION_FAILED; *count = 0; return VK_SUCCESS; }

static VKAPI_ATTR VkResult VKAPI_CALL stub_vkEnumerateInstanceVersion(uint32_t *v)
{ if (!v) return VK_ERROR_INITIALIZATION_FAILED; *v = VK_MAKE_API_VERSION(0, 1, 2, 0); return VK_SUCCESS; }

/* ====================================================================
 * RPC estructurado (ops del Hito 4): respuesta = {int32 result, int32 pad} + datos.
 * ==================================================================== */
static VkResult vcall(struct spatha_instance *inst, uint32_t op, const struct wbuf *w,
                      void *resp, size_t cap, size_t *rlen)
{
    uint32_t rl = 0;
    if (w && (w->err || w->len > SPATHA_MAX_PAYLOAD)) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (cap > SPATHA_MAX_PAYLOAD) cap = SPATHA_MAX_PAYLOAD;
    int rc = rpc(inst, op, w ? w->p : NULL, w ? (uint32_t)w->len : 0, resp, (uint32_t)cap, &rl);
    if (rc || rl < 8) return VK_ERROR_DEVICE_LOST;
    if (rlen) *rlen = rl;
    int32_t r; memcpy(&r, resp, 4);
    return (VkResult)r;
}

#define TOH(T, id) ((T)(uintptr_t)(id))
#define ID(h)      ((uint64_t)(uintptr_t)(h))

/* ---- physical devices ---- */
static int pds_fetch_locked(struct spatha_instance *inst) {
    if (inst->pds_valid) return 1;
    if (!inst->remote_id || inst->sock < 0) return 0;
    uint8_t buf[sizeof(struct spatha_resp_enum_pd) + SPATHA_MAX_PD * 4];
    uint32_t rl = 0;
    int rc = rpc(inst, SPATHA_OP_VK_ENUMERATE_PHYSICAL_DEVICES, &inst->remote_id, 4, buf, sizeof(buf), &rl);
    if (rc || rl < sizeof(struct spatha_resp_enum_pd)) return 0;
    struct spatha_resp_enum_pd hdr; memcpy(&hdr, buf, sizeof(hdr));
    if ((hdr.vk_result != VK_SUCCESS && hdr.vk_result != VK_INCOMPLETE) ||
        hdr.count > SPATHA_MAX_PD || rl != sizeof(hdr) + hdr.count * 4) return 0;
    for (uint32_t i = 0; i < hdr.count; i++) {
        struct spatha_physical_device *pd = raw_alloc(inst_cb(inst), sizeof(*pd));
        if (!pd) break;
        set_loader_magic_value(pd);
        memcpy(&pd->remote_id, buf + sizeof(hdr) + i * 4, 4);
        pd->inst = inst;
        inst->pds[inst->n_pds++] = pd;
    }
    inst->pds_valid = 1;
    DBG("%u physical device(s) remotos", inst->n_pds);
    return 1;
}

static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkEnumeratePhysicalDevices(VkInstance instance, uint32_t *n, VkPhysicalDevice *out)
{
    struct spatha_instance *inst = (struct spatha_instance *)instance;
    if (!inst || !n) return VK_ERROR_INITIALIZATION_FAILED;
    pthread_mutex_lock(&inst->pd_lock);
    uint32_t total = pds_fetch_locked(inst) ? inst->n_pds : 0;
    VkResult r = VK_SUCCESS;
    if (!out) *n = total;
    else {
        uint32_t k = *n < total ? *n : total;
        for (uint32_t i = 0; i < k; i++) out[i] = (VkPhysicalDevice)inst->pds[i];
        if (k < total) r = VK_INCOMPLETE;
        *n = k;
    }
    pthread_mutex_unlock(&inst->pd_lock);
    return r;
}

static int pd_props_locked(struct spatha_physical_device *pd) {
    if (pd->props_valid) return 1;
    uint8_t buf[4 + sizeof(VkPhysicalDeviceProperties)];
    uint32_t rl = 0;
    int rc = rpc(pd->inst, SPATHA_OP_VK_GET_PHYSICAL_DEVICE_PROPERTIES, &pd->remote_id, 4, buf, sizeof(buf), &rl);
    int32_t vr = 0;
    if (!rc && rl == sizeof(buf)) memcpy(&vr, buf, 4);
    if (rc || rl != sizeof(buf) || vr != 0) return 0;
    memcpy(&pd->props, buf + 4, sizeof(pd->props));
    pd->props_valid = 1;
    return 1;
}

static VKAPI_ATTR void VKAPI_CALL
stub_vkGetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties *p)
{
    if (!p) return;
    memset(p, 0, sizeof(*p));
    struct spatha_physical_device *pd = (struct spatha_physical_device *)physicalDevice;
    if (!pd || !pd->inst) return;
    pthread_mutex_lock(&pd->inst->pd_lock);
    if (pd_props_locked(pd)) {
        *p = pd->props;
        uint32_t hv = pd->props.apiVersion;
        uint32_t hmaj = VK_API_VERSION_MAJOR(hv), hmin = VK_API_VERSION_MINOR(hv);
        if (hmaj > 1 || (hmaj == 1 && hmin >= 2))
            p->apiVersion = VK_MAKE_API_VERSION(0, 1, 2, VK_API_VERSION_PATCH(hv));
        DBG("pd_props: %s api=%u.%u.%u deviceType=%d", p->deviceName,
            VK_API_VERSION_MAJOR(p->apiVersion), VK_API_VERSION_MINOR(p->apiVersion),
            VK_API_VERSION_PATCH(p->apiVersion), (int)p->deviceType);
    }
    pthread_mutex_unlock(&pd->inst->pd_lock);
}

static int qf_locked(struct spatha_physical_device *pd) {
    if (pd->qf_valid) return 1;
    uint8_t buf[sizeof(struct spatha_resp_enum_qf) + SPATHA_MAX_QF * sizeof(VkQueueFamilyProperties)];
    uint32_t rl = 0;
    int rc = rpc(pd->inst, SPATHA_OP_VK_GET_PHYSICAL_DEVICE_QUEUE_FAMILY_PROPERTIES, &pd->remote_id, 4, buf, sizeof(buf), &rl);
    if (rc || rl < sizeof(struct spatha_resp_enum_qf)) return 0;
    struct spatha_resp_enum_qf hdr; memcpy(&hdr, buf, sizeof(hdr));
    if (hdr.vk_result != VK_SUCCESS || hdr.count > SPATHA_MAX_QF ||
        rl != sizeof(hdr) + hdr.count * sizeof(VkQueueFamilyProperties)) return 0;
    memcpy(pd->qf, buf + sizeof(hdr), hdr.count * sizeof(VkQueueFamilyProperties));
    pd->n_qf = hdr.count;
    pd->qf_valid = 1;
    return 1;
}

static VKAPI_ATTR void VKAPI_CALL
stub_vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice physicalDevice, uint32_t *n, VkQueueFamilyProperties *p)
{
    if (!n) return;
    struct spatha_physical_device *pd = (struct spatha_physical_device *)physicalDevice;
    if (!pd || !pd->inst) { *n = 0; return; }
    pthread_mutex_lock(&pd->inst->pd_lock);
    uint32_t total = qf_locked(pd) ? pd->n_qf : 0;
    DBG("qf stub: total=%u p=%p", total, (void*)p);
    if (!p) *n = total;
    else {
        uint32_t k = *n < total ? *n : total;
        if (k) memcpy(p, pd->qf, k * sizeof(VkQueueFamilyProperties));
        *n = k;
    }
    pthread_mutex_unlock(&pd->inst->pd_lock);
}

static VKAPI_ATTR void VKAPI_CALL
stub_vkGetPhysicalDeviceFeatures(VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures *f)
{
    if (!f) return;
    memset(f, 0, sizeof(*f));
    struct spatha_physical_device *pd = (struct spatha_physical_device *)physicalDevice;
    if (!pd || !pd->inst) return;
    pthread_mutex_lock(&pd->inst->pd_lock);
    if (!pd->feat_valid) {
        struct wbuf w; w_init(&w); w_u64(&w, pd->remote_id);
        alignas(8) uint8_t resp[8 + 256]; size_t rl = 0;
        if (vcall(pd->inst, SPATHA_OP_GET_PD_FEATURES, &w, resp, sizeof(resp), &rl) == VK_SUCCESS &&
            rl >= 8 + sizeof(pd->feat)) {
            memcpy(&pd->feat, resp + 8, sizeof(pd->feat)); pd->feat_valid = 1;
        }
        w_free(&w);
    }
    if (pd->feat_valid) *f = pd->feat;
    pthread_mutex_unlock(&pd->inst->pd_lock);
}


static int pd_mp_locked(struct spatha_physical_device *pd) {
    if (pd->mp_valid) return 1;
    struct wbuf w; w_init(&w); w_u64(&w, pd->remote_id);
    alignas(8) uint8_t resp[8 + sizeof(VkPhysicalDeviceMemoryProperties) + 8]; size_t rl = 0;
    if (vcall(pd->inst, SPATHA_OP_GET_PD_MEMPROPS, &w, resp, sizeof(resp), &rl) == VK_SUCCESS &&
        rl >= 8 + sizeof(pd->mp)) {
        memcpy(&pd->mp, resp + 8, sizeof(pd->mp)); pd->mp_valid = 1;
    }
    w_free(&w);
    return pd->mp_valid;
}

static VKAPI_ATTR void VKAPI_CALL
stub_vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice physicalDevice, VkPhysicalDeviceMemoryProperties *p)
{
    if (!p) return;
    memset(p, 0, sizeof(*p));
    struct spatha_physical_device *pd = (struct spatha_physical_device *)physicalDevice;
    if (!pd || !pd->inst) return;
    pthread_mutex_lock(&pd->inst->pd_lock);
    if (pd_mp_locked(pd)) *p = pd->mp;
    pthread_mutex_unlock(&pd->inst->pd_lock);
}

static VKAPI_ATTR void VKAPI_CALL
stub_vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice physicalDevice, VkFormat fmt, VkFormatProperties *p)
{
    if (!p) return;
    memset(p, 0, sizeof(*p));
    struct spatha_physical_device *pd = (struct spatha_physical_device *)physicalDevice;
    if (!pd || !pd->inst) return;
    struct wbuf w; w_init(&w); w_u64(&w, pd->remote_id); w_u64(&w, (uint64_t)fmt);
    alignas(8) uint8_t resp[64]; size_t rl = 0;
    if (vcall(pd->inst, SPATHA_OP_GET_PD_FORMAT_PROPS, &w, resp, sizeof(resp), &rl) == VK_SUCCESS &&
        rl >= 8 + sizeof(*p))
        memcpy(p, resp + 8, sizeof(*p));
    w_free(&w);
}

static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkGetPhysicalDeviceImageFormatProperties(VkPhysicalDevice physicalDevice, VkFormat fmt, VkImageType type,
        VkImageTiling tiling, VkImageUsageFlags usage, VkImageCreateFlags flags, VkImageFormatProperties *p)
{
    struct spatha_physical_device *pd = (struct spatha_physical_device *)physicalDevice;
    if (!p) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    memset(p, 0, sizeof(*p));
    if (!pd || !pd->inst) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    struct wbuf w; w_init(&w);
    w_u64(&w, pd->remote_id); w_u64(&w, (uint64_t)fmt); w_u64(&w, (uint64_t)type);
    w_u64(&w, (uint64_t)tiling); w_u64(&w, (uint64_t)usage); w_u64(&w, (uint64_t)flags);
    alignas(8) uint8_t resp[8 + sizeof(VkImageFormatProperties) + 8]; size_t rl = 0;
    VkResult vr = vcall(pd->inst, SPATHA_OP_GET_PD_IMAGE_FORMAT_PROPS, &w, resp, sizeof(resp), &rl);
    if (vr == VK_SUCCESS && rl >= 8 + sizeof(*p)) memcpy(p, resp + 8, sizeof(*p));
    w_free(&w);
    return vr;
}

static VKAPI_ATTR void VKAPI_CALL
stub_vkGetPhysicalDeviceSparseImageFormatProperties(VkPhysicalDevice pd, VkFormat fmt, VkImageType type,
        VkSampleCountFlagBits samples, VkImageUsageFlags usage, VkImageTiling tiling, uint32_t *n,
        VkSparseImageFormatProperties *p)
{ (void)pd; (void)fmt; (void)type; (void)samples; (void)usage; (void)tiling; (void)p; if (n) *n = 0; }


static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkEnumerateDeviceLayerProperties(VkPhysicalDevice pd, uint32_t *n, VkLayerProperties *p)
{ (void)pd; (void)p; if (n) *n = 0; return VK_SUCCESS; }
/* ====================================================================
 * Device, queues, objetos
 * ==================================================================== */
struct spatha_device;

struct spatha_queue {
    VK_LOADER_DATA loader_data;
    struct spatha_device *dev;
    uint32_t remote_id, family, index;
};

struct spatha_mem {                 /* VkDeviceMemory */
    uint64_t id, size;
    uint32_t type;
    int host_visible;
    uint8_t *shadow, *synced;       /* copia local mapeada + ultimo contenido enviado */
    int mapped;
    uint64_t map_off, map_size;
};

struct spatha_device {
    VK_LOADER_DATA loader_data;     /* primer campo */
    struct spatha_instance *inst;
    struct spatha_physical_device *pd;
    VkAllocationCallbacks alloc;
    int has_alloc;
    uint32_t remote_id;
    struct spatha_queue *queues;
    uint32_t nq;
    VkPhysicalDeviceMemoryProperties mp;

    pthread_mutex_t lock;           /* protege mems[] */
    struct spatha_mem **mems;
    uint32_t n_mems, cap_mems;
};

struct spatha_cmdbuf {
    VK_LOADER_DATA loader_data;
    struct spatha_device *dev;
    uint32_t remote_id;
    VkCommandBufferUsageFlags flags;
    VkCommandBufferLevel level;
    struct wbuf rec;                /* stream de comandos grabados */
    int recording;
    VkCommandBufferInheritanceInfo inh;
    int has_inh;
};

static void rpc_small_destroy(struct spatha_device *d, uint32_t type, uint64_t id) {
    struct wbuf w; w_init(&w);
    w_u64(&w, d->remote_id); w_u64(&w, type); w_u64(&w, id);
    alignas(8) uint8_t resp[16]; size_t rl;
    vcall(d->inst, SPATHA_OP_DESTROY, &w, resp, sizeof(resp), &rl);
    w_free(&w);
}

/* w ya tiene [dev][type][payload]; devuelve el id nuevo. */
static VkResult rpc_create(struct spatha_device *d, struct wbuf *w, uint64_t *id) {
    alignas(8) uint8_t resp[32]; size_t rl = 0;
    VkResult r = vcall(d->inst, SPATHA_OP_CREATE, w, resp, sizeof(resp), &rl);
    w_free(w);
    if (r != VK_SUCCESS) return r;
    if (rl < 16) return VK_ERROR_DEVICE_LOST;
    memcpy(id, resp + 8, 8);
    return VK_SUCCESS;
}
static void cbegin(struct wbuf *w, struct spatha_device *d, uint32_t type) {
    w_init(w); w_u64(w, d->remote_id); w_u64(w, type);
}

static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkCreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo *ci,
                    const VkAllocationCallbacks *a, VkDevice *out)
{
    struct spatha_physical_device *pd = (struct spatha_physical_device *)physicalDevice;
    if (!pd || !ci || !out) return VK_ERROR_INITIALIZATION_FAILED;
    *out = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) {
        const char *n = ci->ppEnabledExtensionNames[i];
        if (!n || strlen(n) >= SPATHA_EXT_NAME_MAX) return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    if (!pd->inst->remote_id) return VK_ERROR_INITIALIZATION_FAILED;

    struct wbuf w; w_init(&w);
    w_u64(&w, pd->remote_id);
    w_u64(&w, ci->queueCreateInfoCount);
    uint64_t total = 0;
    for (uint32_t i = 0; i < ci->queueCreateInfoCount; i++) {
        const VkDeviceQueueCreateInfo *q = &ci->pQueueCreateInfos[i];
        w_u64(&w, q->queueFamilyIndex); w_u64(&w, q->queueCount);
        w_raw(&w, q->pQueuePriorities, (size_t)q->queueCount * 4);
        total += q->queueCount;
    }
    VkResult fr = dev_features_write(&w, ci);
    if (fr != VK_SUCCESS) { w_free(&w); return fr; }

    uint8_t *resp = malloc(8 + 16 + total * 8 + 64);
    if (!resp) { w_free(&w); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    size_t rl = 0;
    VkResult vr = vcall(pd->inst, SPATHA_OP_CREATE_DEVICE, &w, resp, 8 + 16 + total * 8 + 64, &rl);
    w_free(&w);
    if (vr != VK_SUCCESS) { free(resp); return vr; }
    struct rbuf r; r_init(&r, resp + 8, rl - 8);
    uint64_t dev_id = r_u64(&r), nq = r_u64(&r);
    if (r.err || nq != total) { free(resp); return VK_ERROR_DEVICE_LOST; }

    struct spatha_device *d = raw_alloc(a, sizeof(*d));
    if (!d) { free(resp); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    if (a && a->pfnAllocation) { d->alloc = *a; d->has_alloc = 1; }
    set_loader_magic_value(d);
    d->inst = pd->inst; d->pd = pd; d->remote_id = (uint32_t)dev_id;
    pthread_mutex_init(&d->lock, NULL);
    d->queues = calloc(nq ? nq : 1, sizeof(*d->queues));
    d->nq = (uint32_t)nq;
    uint32_t k = 0;
    for (uint32_t i = 0; i < ci->queueCreateInfoCount; i++)
        for (uint32_t j = 0; j < ci->pQueueCreateInfos[i].queueCount; j++, k++) {
            struct spatha_queue *q = &d->queues[k];
            set_loader_magic_value(q);
            q->dev = d; q->remote_id = (uint32_t)r_u64(&r);
            q->family = ci->pQueueCreateInfos[i].queueFamilyIndex; q->index = j;
        }
    free(resp);
    pthread_mutex_lock(&pd->inst->pd_lock);
    pd_mp_locked(pd);
    d->mp = pd->mp;
    pthread_mutex_unlock(&pd->inst->pd_lock);
    *out = (VkDevice)d;
    DBG("device %p creado (remote_id=%u, %u queue(s))", (void *)d, d->remote_id, d->nq);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
stub_vkDestroyDevice(VkDevice device, const VkAllocationCallbacks *a)
{
    (void)a;
    struct spatha_device *d = (struct spatha_device *)device;
    if (!d) return;
    struct wbuf w; w_init(&w); w_u64(&w, d->remote_id);
    alignas(8) uint8_t resp[16]; size_t rl;
    vcall(d->inst, SPATHA_OP_DESTROY_DEVICE, &w, resp, sizeof(resp), &rl);
    w_free(&w);
    for (uint32_t i = 0; i < d->n_mems; i++) { free(d->mems[i]->shadow); free(d->mems[i]->synced); free(d->mems[i]); }
    free(d->mems); free(d->queues);
    pthread_mutex_destroy(&d->lock);
    VkAllocationCallbacks cb = d->alloc; int has = d->has_alloc;
    raw_free(has ? &cb : NULL, d);
}

static VKAPI_ATTR void VKAPI_CALL
stub_vkGetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue *out)
{
    struct spatha_device *d = (struct spatha_device *)device;
    *out = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < d->nq; i++)
        if (d->queues[i].family == family && d->queues[i].index == index) { *out = (VkQueue)&d->queues[i]; return; }
}

/* ---- memoria: shadow local + sincronizacion en puntos de submit/unmap/flush ---- */
static int mem_send(struct spatha_device *d, struct spatha_mem *m, uint64_t off, uint64_t size) {
    const uint64_t CH = 8u * 1024 * 1024;
    while (size) {
        uint64_t n = size < CH ? size : CH;
        struct wbuf w; w_init(&w);
        w_u64(&w, d->remote_id); w_u64(&w, m->id); w_u64(&w, off); w_u64(&w, n);
        w_raw(&w, m->shadow + off, n);
        alignas(8) uint8_t resp[16]; size_t rl;
        VkResult r = vcall(d->inst, SPATHA_OP_MEM_WRITE, &w, resp, sizeof(resp), &rl);
        w_free(&w);
        if (r != VK_SUCCESS) return -1;
        memcpy(m->synced + off, m->shadow + off, n);
        off += n; size -= n;
    }
    return 0;
}

/* Envia solo el tramo [primer byte distinto, ultimo] del rango. */
static void mem_sync_range(struct spatha_device *d, struct spatha_mem *m, uint64_t off, uint64_t size) {
    if (!m->shadow || !size) return;
    const uint8_t *a = m->shadow + off, *b = m->synced + off;
    uint64_t lo = 0, hi = size;
    while (lo < hi && a[lo] == b[lo]) lo++;
    if (lo == hi) return;
    while (hi > lo && a[hi - 1] == b[hi - 1]) hi--;
    mem_send(d, m, off + lo, hi - lo);
}

static void dev_flush_all(struct spatha_device *d) {
    pthread_mutex_lock(&d->lock);
    for (uint32_t i = 0; i < d->n_mems; i++) {
        struct spatha_mem *m = d->mems[i];
        if (m->mapped) mem_sync_range(d, m, m->map_off, m->map_size);
    }
    pthread_mutex_unlock(&d->lock);
}

static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkAllocateMemory(VkDevice device, const VkMemoryAllocateInfo *ai, const VkAllocationCallbacks *a, VkDeviceMemory *out)
{
    (void)a;
    struct spatha_device *d = (struct spatha_device *)device;
    struct wbuf w; w_init(&w);
    w_u64(&w, d->remote_id); w_u64(&w, ai->allocationSize); w_u64(&w, ai->memoryTypeIndex);
    w_u64(&w, mem_alloc_flags(ai));
    alignas(8) uint8_t resp[32]; size_t rl = 0;
    VkResult vr = vcall(d->inst, SPATHA_OP_ALLOC_MEMORY, &w, resp, sizeof(resp), &rl);
    w_free(&w);
    if (vr != VK_SUCCESS) return vr;
    if (rl < 16) return VK_ERROR_DEVICE_LOST;
    struct spatha_mem *m = calloc(1, sizeof(*m));
    if (!m) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memcpy(&m->id, resp + 8, 8);
    m->size = ai->allocationSize; m->type = ai->memoryTypeIndex;
    m->host_visible = ai->memoryTypeIndex < d->mp.memoryTypeCount &&
        (d->mp.memoryTypes[ai->memoryTypeIndex].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    pthread_mutex_lock(&d->lock);
    if (d->n_mems == d->cap_mems) {
        uint32_t nc = d->cap_mems ? d->cap_mems * 2 : 16;
        struct spatha_mem **nm = realloc(d->mems, nc * sizeof(*nm));
        if (!nm) { pthread_mutex_unlock(&d->lock); rpc_small_destroy(d, SPATHA_T_MEM, m->id); free(m); return VK_ERROR_OUT_OF_HOST_MEMORY; }
        d->mems = nm; d->cap_mems = nc;
    }
    d->mems[d->n_mems++] = m;
    pthread_mutex_unlock(&d->lock);
    *out = TOH(VkDeviceMemory, m);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
stub_vkFreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks *a)
{
    (void)a;
    struct spatha_device *d = (struct spatha_device *)device;
    struct spatha_mem *m = (struct spatha_mem *)(uintptr_t)memory;
    if (!m) return;
    pthread_mutex_lock(&d->lock);
    for (uint32_t i = 0; i < d->n_mems; i++)
        if (d->mems[i] == m) { d->mems[i] = d->mems[--d->n_mems]; break; }
    pthread_mutex_unlock(&d->lock);
    rpc_small_destroy(d, SPATHA_T_MEM, m->id);
    free(m->shadow); free(m->synced); free(m);
}

static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkMapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize off, VkDeviceSize size, VkMemoryMapFlags f, void **pp)
{
    (void)device; (void)f;
    struct spatha_mem *m = (struct spatha_mem *)(uintptr_t)memory;
    if (!m || !m->host_visible || m->mapped || off >= m->size) return VK_ERROR_MEMORY_MAP_FAILED;
    if (size == VK_WHOLE_SIZE) size = m->size - off;
    if (size > m->size - off) return VK_ERROR_MEMORY_MAP_FAILED;
    if (!m->shadow) {
        m->shadow = calloc(1, m->size); m->synced = calloc(1, m->size);
        if (!m->shadow || !m->synced) { free(m->shadow); free(m->synced); m->shadow = m->synced = NULL; return VK_ERROR_MEMORY_MAP_FAILED; }
    }
    m->mapped = 1; m->map_off = off; m->map_size = size;
    *pp = m->shadow + off;
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
stub_vkUnmapMemory(VkDevice device, VkDeviceMemory memory)
{
    struct spatha_device *d = (struct spatha_device *)device;
    struct spatha_mem *m = (struct spatha_mem *)(uintptr_t)memory;
    if (!m || !m->mapped) return;
    mem_sync_range(d, m, m->map_off, m->map_size);
    m->mapped = 0;
}

static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkFlushMappedMemoryRanges(VkDevice device, uint32_t n, const VkMappedMemoryRange *r)
{
    struct spatha_device *d = (struct spatha_device *)device;
    for (uint32_t i = 0; i < n; i++) {
        struct spatha_mem *m = (struct spatha_mem *)(uintptr_t)r[i].memory;
        if (!m || !m->shadow || r[i].offset >= m->size) continue;
        uint64_t sz = r[i].size == VK_WHOLE_SIZE ? m->size - r[i].offset : r[i].size;
        if (sz > m->size - r[i].offset) sz = m->size - r[i].offset;
        mem_sync_range(d, m, r[i].offset, sz);
    }
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkInvalidateMappedMemoryRanges(VkDevice device, uint32_t n, const VkMappedMemoryRange *r)
{
    struct spatha_device *d = (struct spatha_device *)device;
    for (uint32_t i = 0; i < n; i++) {
        struct spatha_mem *m = (struct spatha_mem *)(uintptr_t)r[i].memory;
        if (!m || !m->shadow || r[i].offset >= m->size) continue;
        uint64_t off = r[i].offset;
        uint64_t sz = r[i].size == VK_WHOLE_SIZE ? m->size - off : r[i].size;
        if (sz > m->size - off) sz = m->size - off;
        dev_flush_all(d);                       /* que la GPU vea las escrituras pendientes */
        while (sz) {
            uint64_t ch = sz < 8u * 1024 * 1024 ? sz : 8u * 1024 * 1024;
            struct wbuf w; w_init(&w);
            w_u64(&w, d->remote_id); w_u64(&w, m->id); w_u64(&w, off); w_u64(&w, ch);
            uint8_t *resp = malloc(8 + ch + 8);
            if (!resp) { w_free(&w); return VK_ERROR_OUT_OF_HOST_MEMORY; }
            size_t rl = 0;
            VkResult vr = vcall(d->inst, SPATHA_OP_MEM_READ, &w, resp, 8 + ch + 8, &rl);
            w_free(&w);
            if (vr != VK_SUCCESS || rl < 8 + ch) { free(resp); return VK_ERROR_DEVICE_LOST; }
            memcpy(m->shadow + off, resp + 8, ch);
            memcpy(m->synced + off, resp + 8, ch);
            free(resp);
            off += ch; sz -= ch;
        }
    }
    return VK_SUCCESS;
}

static VkResult rpc_memreqs(struct spatha_device *d, uint32_t kind, uint64_t oid, VkMemoryRequirements *mr) {
    struct wbuf w; w_init(&w);
    w_u64(&w, d->remote_id); w_u64(&w, kind); w_u64(&w, oid);
    alignas(8) uint8_t resp[8 + sizeof(VkMemoryRequirements) + 8]; size_t rl = 0;
    VkResult vr = vcall(d->inst, SPATHA_OP_GET_MEM_REQS, &w, resp, sizeof(resp), &rl);
    w_free(&w);
    memset(mr, 0, sizeof(*mr));
    if (vr == VK_SUCCESS && rl >= 8 + sizeof(*mr)) memcpy(mr, resp + 8, sizeof(*mr));
    return vr;
}
static VkResult rpc_bind(struct spatha_device *d, uint32_t kind, uint64_t oid, VkDeviceMemory mem, VkDeviceSize off) {
    struct spatha_mem *m = (struct spatha_mem *)(uintptr_t)mem;
    struct wbuf w; w_init(&w);
    w_u64(&w, d->remote_id); w_u64(&w, kind); w_u64(&w, oid); w_u64(&w, m ? m->id : 0); w_u64(&w, off);
    alignas(8) uint8_t resp[16]; size_t rl;
    VkResult vr = vcall(d->inst, SPATHA_OP_BIND_MEMORY, &w, resp, sizeof(resp), &rl);
    w_free(&w);
    return vr;
}

static VKAPI_ATTR void VKAPI_CALL
stub_vkGetBufferMemoryRequirements(VkDevice device, VkBuffer b, VkMemoryRequirements *mr)
{ rpc_memreqs((struct spatha_device *)device, SPATHA_T_BUF, ID(b), mr); }
static VKAPI_ATTR void VKAPI_CALL
stub_vkGetImageMemoryRequirements(VkDevice device, VkImage i, VkMemoryRequirements *mr)
{ rpc_memreqs((struct spatha_device *)device, SPATHA_T_IMG, ID(i), mr); }
static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkBindBufferMemory(VkDevice device, VkBuffer b, VkDeviceMemory m, VkDeviceSize o)
{ return rpc_bind((struct spatha_device *)device, SPATHA_T_BUF, ID(b), m, o); }
static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkBindImageMemory(VkDevice device, VkImage i, VkDeviceMemory m, VkDeviceSize o)
{ return rpc_bind((struct spatha_device *)device, SPATHA_T_IMG, ID(i), m, o); }

static VKAPI_ATTR void VKAPI_CALL
stub_vkGetImageSubresourceLayout(VkDevice device, VkImage image, const VkImageSubresource *sr, VkSubresourceLayout *out)
{
    struct spatha_device *d = (struct spatha_device *)device;
    struct wbuf w; w_init(&w);
    w_u64(&w, d->remote_id); w_u64(&w, ID(image)); w_raw(&w, sr, sizeof(*sr));
    alignas(8) uint8_t resp[8 + sizeof(VkSubresourceLayout) + 8]; size_t rl = 0;
    memset(out, 0, sizeof(*out));
    if (vcall(d->inst, SPATHA_OP_GET_SUBRES_LAYOUT, &w, resp, sizeof(resp), &rl) == VK_SUCCESS &&
        rl >= 8 + sizeof(*out)) memcpy(out, resp + 8, sizeof(*out));
    w_free(&w);
}

/* ---- creadores / destructores simples ---- */
#define DEV(d) struct spatha_device *dv = (struct spatha_device *)(d)

static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkCreateSemaphore(VkDevice d, const VkSemaphoreCreateInfo *ci, const VkAllocationCallbacks *a, VkSemaphore *out)
{ (void)a; DEV(d); struct wbuf w; cbegin(&w, dv, SPATHA_T_SEM); xw(&w, SPATHA_T_SEM, ci->pNext); uint64_t id;
  VkResult r = rpc_create(dv, &w, &id); if (r == VK_SUCCESS) *out = TOH(VkSemaphore, id); return r; }
static VKAPI_ATTR void VKAPI_CALL
stub_vkDestroySemaphore(VkDevice d, VkSemaphore s, const VkAllocationCallbacks *a)
{ (void)a; if (s) rpc_small_destroy((struct spatha_device *)d, SPATHA_T_SEM, ID(s)); }

static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkCreateFence(VkDevice d, const VkFenceCreateInfo *ci, const VkAllocationCallbacks *a, VkFence *out)
{ (void)a; DEV(d); struct wbuf w; cbegin(&w, dv, SPATHA_T_FENCE); w_u64(&w, ci->flags); uint64_t id;
  VkResult r = rpc_create(dv, &w, &id); if (r == VK_SUCCESS) *out = TOH(VkFence, id); return r; }
static VKAPI_ATTR void VKAPI_CALL
stub_vkDestroyFence(VkDevice d, VkFence f, const VkAllocationCallbacks *a)
{ (void)a; if (f) rpc_small_destroy((struct spatha_device *)d, SPATHA_T_FENCE, ID(f)); }

static VkResult fence_op(struct spatha_device *d, uint32_t op, uint32_t n, const VkFence *f, int all, uint64_t to) {
    struct wbuf w; w_init(&w);
    w_u64(&w, d->remote_id); w_u64(&w, n);
    for (uint32_t i = 0; i < n; i++) w_u64(&w, ID(f[i]));
    if (op == SPATHA_OP_WAIT_FENCES) { w_u64(&w, all); w_u64(&w, to); }
    alignas(8) uint8_t resp[16]; size_t rl;
    VkResult r = vcall(d->inst, op, &w, resp, sizeof(resp), &rl);
    w_free(&w);
    return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkResetFences(VkDevice d, uint32_t n, const VkFence *f)
{ return fence_op((struct spatha_device *)d, SPATHA_OP_RESET_FENCES, n, f, 0, 0); }
static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkGetFenceStatus(VkDevice d, VkFence f)
{ return fence_op((struct spatha_device *)d, SPATHA_OP_GET_FENCE_STATUS, 1, &f, 0, 0); }
static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkWaitForFences(VkDevice d, uint32_t n, const VkFence *f, VkBool32 all, uint64_t to)
{ return fence_op((struct spatha_device *)d, SPATHA_OP_WAIT_FENCES, n, f, all, to); }

static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkCreateBuffer(VkDevice d, const VkBufferCreateInfo *ci, const VkAllocationCallbacks *a, VkBuffer *out)
{ (void)a; DEV(d); struct wbuf w; cbegin(&w, dv, SPATHA_T_BUF);
  w_raw(&w, ci, sizeof(*ci));
  w_raw(&w, ci->pQueueFamilyIndices, (size_t)ci->queueFamilyIndexCount * 4);
  uint64_t id; VkResult r = rpc_create(dv, &w, &id); if (r == VK_SUCCESS) *out = TOH(VkBuffer, id); return r; }
static VKAPI_ATTR void VKAPI_CALL
stub_vkDestroyBuffer(VkDevice d, VkBuffer b, const VkAllocationCallbacks *a)
{ (void)a; if (b) rpc_small_destroy((struct spatha_device *)d, SPATHA_T_BUF, ID(b)); }

static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkCreateImage(VkDevice d, const VkImageCreateInfo *ci, const VkAllocationCallbacks *a, VkImage *out)
{ (void)a; DEV(d); struct wbuf w; cbegin(&w, dv, SPATHA_T_IMG); xw(&w, SPATHA_T_IMG, ci->pNext);
  w_raw(&w, ci, sizeof(*ci));
  w_raw(&w, ci->pQueueFamilyIndices, (size_t)ci->queueFamilyIndexCount * 4);
  uint64_t id; VkResult r = rpc_create(dv, &w, &id); if (r == VK_SUCCESS) *out = TOH(VkImage, id); return r; }
static VKAPI_ATTR void VKAPI_CALL
stub_vkDestroyImage(VkDevice d, VkImage i, const VkAllocationCallbacks *a)
{ (void)a; if (i) rpc_small_destroy((struct spatha_device *)d, SPATHA_T_IMG, ID(i)); }

static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkCreateImageView(VkDevice d, const VkImageViewCreateInfo *ci, const VkAllocationCallbacks *a, VkImageView *out)
{ (void)a; DEV(d); struct wbuf w; cbegin(&w, dv, SPATHA_T_VIEW); xw(&w, SPATHA_T_VIEW, ci->pNext); w_raw(&w, ci, sizeof(*ci));
  uint64_t id; VkResult r = rpc_create(dv, &w, &id); if (r == VK_SUCCESS) *out = TOH(VkImageView, id); return r; }
static VKAPI_ATTR void VKAPI_CALL
stub_vkDestroyImageView(VkDevice d, VkImageView v, const VkAllocationCallbacks *a)
{ (void)a; if (v) rpc_small_destroy((struct spatha_device *)d, SPATHA_T_VIEW, ID(v)); }

static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkCreateSampler(VkDevice d, const VkSamplerCreateInfo *ci, const VkAllocationCallbacks *a, VkSampler *out)
{ (void)a; DEV(d); struct wbuf w; cbegin(&w, dv, SPATHA_T_SAMPLER); xw(&w, SPATHA_T_SAMPLER, ci->pNext); w_raw(&w, ci, sizeof(*ci));
  uint64_t id; VkResult r = rpc_create(dv, &w, &id); if (r == VK_SUCCESS) *out = TOH(VkSampler, id); return r; }
static VKAPI_ATTR void VKAPI_CALL
stub_vkDestroySampler(VkDevice d, VkSampler s, const VkAllocationCallbacks *a)
{ (void)a; if (s) rpc_small_destroy((struct spatha_device *)d, SPATHA_T_SAMPLER, ID(s)); }

static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkCreateShaderModule(VkDevice d, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a, VkShaderModule *out)
{ (void)a; DEV(d); struct wbuf w; cbegin(&w, dv, SPATHA_T_SHADER);
  w_u64(&w, ci->codeSize); w_raw(&w, ci->pCode, ci->codeSize);
  uint64_t id; VkResult r = rpc_create(dv, &w, &id); if (r == VK_SUCCESS) *out = TOH(VkShaderModule, id); return r; }
static VKAPI_ATTR void VKAPI_CALL
stub_vkDestroyShaderModule(VkDevice d, VkShaderModule s, const VkAllocationCallbacks *a)
{ (void)a; if (s) rpc_small_destroy((struct spatha_device *)d, SPATHA_T_SHADER, ID(s)); }

static VKAPI_ATTR void VKAPI_CALL
stub_vkDestroyDescriptorSetLayout(VkDevice d, VkDescriptorSetLayout l, const VkAllocationCallbacks *a)
{ (void)a; if (l) rpc_small_destroy((struct spatha_device *)d, SPATHA_T_DSL, ID(l)); }

static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkCreateDescriptorPool(VkDevice d, const VkDescriptorPoolCreateInfo *ci, const VkAllocationCallbacks *a, VkDescriptorPool *out)
{ (void)a; DEV(d); struct wbuf w; cbegin(&w, dv, SPATHA_T_DPOOL);
  w_raw(&w, ci, sizeof(*ci)); w_raw(&w, ci->pPoolSizes, (size_t)ci->poolSizeCount * sizeof(VkDescriptorPoolSize));
  uint64_t id; VkResult r = rpc_create(dv, &w, &id); if (r == VK_SUCCESS) *out = TOH(VkDescriptorPool, id); return r; }
static VKAPI_ATTR void VKAPI_CALL
stub_vkDestroyDescriptorPool(VkDevice d, VkDescriptorPool p, const VkAllocationCallbacks *a)
{ (void)a; if (p) rpc_small_destroy((struct spatha_device *)d, SPATHA_T_DPOOL, ID(p)); }

static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkCreatePipelineLayout(VkDevice d, const VkPipelineLayoutCreateInfo *ci, const VkAllocationCallbacks *a, VkPipelineLayout *out)
{ (void)a; DEV(d); struct wbuf w; cbegin(&w, dv, SPATHA_T_PLAYOUT);
  w_raw(&w, ci, sizeof(*ci));
  for (uint32_t i = 0; i < ci->setLayoutCount; i++) w_u64(&w, ID(ci->pSetLayouts[i]));
  w_raw(&w, ci->pPushConstantRanges, (size_t)ci->pushConstantRangeCount * sizeof(VkPushConstantRange));
  uint64_t id; VkResult r = rpc_create(dv, &w, &id); if (r == VK_SUCCESS) *out = TOH(VkPipelineLayout, id); return r; }
static VKAPI_ATTR void VKAPI_CALL
stub_vkDestroyPipelineLayout(VkDevice d, VkPipelineLayout l, const VkAllocationCallbacks *a)
{ (void)a; if (l) rpc_small_destroy((struct spatha_device *)d, SPATHA_T_PLAYOUT, ID(l)); }

static VKAPI_ATTR void VKAPI_CALL
stub_vkDestroyPipelineCache(VkDevice d, VkPipelineCache c, const VkAllocationCallbacks *a)
{ (void)a; if (c) rpc_small_destroy((struct spatha_device *)d, SPATHA_T_PCACHE, ID(c)); }

static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkCreateRenderPass(VkDevice d, const VkRenderPassCreateInfo *ci, const VkAllocationCallbacks *a, VkRenderPass *out)
{ (void)a; DEV(d); struct wbuf w; cbegin(&w, dv, SPATHA_T_RPASS); xw(&w, SPATHA_T_RPASS, ci->pNext);
  w_raw(&w, ci, sizeof(*ci));
  w_raw(&w, ci->pAttachments, (size_t)ci->attachmentCount * sizeof(VkAttachmentDescription));
  w_raw(&w, ci->pSubpasses, (size_t)ci->subpassCount * sizeof(VkSubpassDescription));
  for (uint32_t i = 0; i < ci->subpassCount; i++) {
      const VkSubpassDescription *s = &ci->pSubpasses[i];
      w_raw(&w, s->pInputAttachments, (size_t)s->inputAttachmentCount * sizeof(VkAttachmentReference));
      w_raw(&w, s->pColorAttachments, (size_t)s->colorAttachmentCount * sizeof(VkAttachmentReference));
      if (s->pResolveAttachments) w_raw(&w, s->pResolveAttachments, (size_t)s->colorAttachmentCount * sizeof(VkAttachmentReference));
      if (s->pDepthStencilAttachment) w_raw(&w, s->pDepthStencilAttachment, sizeof(VkAttachmentReference));
      w_raw(&w, s->pPreserveAttachments, (size_t)s->preserveAttachmentCount * 4);
  }
  w_raw(&w, ci->pDependencies, (size_t)ci->dependencyCount * sizeof(VkSubpassDependency));
  uint64_t id; VkResult r = rpc_create(dv, &w, &id); if (r == VK_SUCCESS) *out = TOH(VkRenderPass, id); return r; }
static VKAPI_ATTR void VKAPI_CALL
stub_vkDestroyRenderPass(VkDevice d, VkRenderPass p, const VkAllocationCallbacks *a)
{ (void)a; if (p) rpc_small_destroy((struct spatha_device *)d, SPATHA_T_RPASS, ID(p)); }

static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkCreateFramebuffer(VkDevice d, const VkFramebufferCreateInfo *ci, const VkAllocationCallbacks *a, VkFramebuffer *out)
{ (void)a; DEV(d); struct wbuf w; cbegin(&w, dv, SPATHA_T_FB);
  w_raw(&w, ci, sizeof(*ci));
  for (uint32_t i = 0; i < ci->attachmentCount; i++) w_u64(&w, ID(ci->pAttachments[i]));
  uint64_t id; VkResult r = rpc_create(dv, &w, &id); if (r == VK_SUCCESS) *out = TOH(VkFramebuffer, id); return r; }
static VKAPI_ATTR void VKAPI_CALL
stub_vkDestroyFramebuffer(VkDevice d, VkFramebuffer f, const VkAllocationCallbacks *a)
{ (void)a; if (f) rpc_small_destroy((struct spatha_device *)d, SPATHA_T_FB, ID(f)); }

static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkCreateCommandPool(VkDevice d, const VkCommandPoolCreateInfo *ci, const VkAllocationCallbacks *a, VkCommandPool *out)
{ (void)a; DEV(d); struct wbuf w; cbegin(&w, dv, SPATHA_T_CMDPOOL); w_raw(&w, ci, sizeof(*ci));
  uint64_t id; VkResult r = rpc_create(dv, &w, &id); if (r == VK_SUCCESS) *out = TOH(VkCommandPool, id); return r; }
static VKAPI_ATTR void VKAPI_CALL
stub_vkDestroyCommandPool(VkDevice d, VkCommandPool p, const VkAllocationCallbacks *a)
{ (void)a; if (p) rpc_small_destroy((struct spatha_device *)d, SPATHA_T_CMDPOOL, ID(p)); }
/* Nota: los spatha_cmdbuf de un pool destruido se liberan con el device (no hay lista por pool);
 * es una fuga acotada. vkFreeCommandBuffers si los libera. */

/* ---- pipelines ---- */
static void w_opt(struct wbuf *w, const void *p, size_t sz) {
    w_u64(w, p != NULL);
    if (p) w_raw(w, p, sz);
}

static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkCreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t n,
        const VkGraphicsPipelineCreateInfo *cis, const VkAllocationCallbacks *a, VkPipeline *out)
{
    (void)a;
    struct spatha_device *d = (struct spatha_device *)device;
    struct wbuf w; w_init(&w);
    w_u64(&w, d->remote_id); w_u64(&w, ID(cache)); w_u64(&w, n);
    for (uint32_t k = 0; k < n; k++) {
        const VkGraphicsPipelineCreateInfo *ci = &cis[k];
        w_raw(&w, ci, sizeof(*ci));
        w_raw(&w, ci->pStages, (size_t)ci->stageCount * sizeof(VkPipelineShaderStageCreateInfo));
        for (uint32_t i = 0; i < ci->stageCount; i++) {
            const VkPipelineShaderStageCreateInfo *s = &ci->pStages[i];
            uint64_t nl = strlen(s->pName);
            w_u64(&w, nl); w_raw(&w, s->pName, nl);
            if (s->pSpecializationInfo) {
                const VkSpecializationInfo *sp = s->pSpecializationInfo;
                w_u64(&w, 1); w_raw(&w, sp, sizeof(*sp));
                w_raw(&w, sp->pMapEntries, (size_t)sp->mapEntryCount * sizeof(VkSpecializationMapEntry));
                w_raw(&w, sp->pData, sp->dataSize);
            } else w_u64(&w, 0);
        }
        const VkPipelineVertexInputStateCreateInfo *vi = ci->pVertexInputState;
        w_opt(&w, vi, sizeof(*vi));
        if (vi) {
            w_raw(&w, vi->pVertexBindingDescriptions, (size_t)vi->vertexBindingDescriptionCount * sizeof(VkVertexInputBindingDescription));
            w_raw(&w, vi->pVertexAttributeDescriptions, (size_t)vi->vertexAttributeDescriptionCount * sizeof(VkVertexInputAttributeDescription));
        }
        w_opt(&w, ci->pInputAssemblyState, sizeof(VkPipelineInputAssemblyStateCreateInfo));
        w_opt(&w, ci->pTessellationState, sizeof(VkPipelineTessellationStateCreateInfo));
        const VkPipelineViewportStateCreateInfo *vp = ci->pViewportState;
        w_opt(&w, vp, sizeof(*vp));
        if (vp) {
            w_u64(&w, vp->pViewports != NULL);
            if (vp->pViewports) w_raw(&w, vp->pViewports, (size_t)vp->viewportCount * sizeof(VkViewport));
            w_u64(&w, vp->pScissors != NULL);
            if (vp->pScissors) w_raw(&w, vp->pScissors, (size_t)vp->scissorCount * sizeof(VkRect2D));
        }
        w_opt(&w, ci->pRasterizationState, sizeof(VkPipelineRasterizationStateCreateInfo));
        const VkPipelineMultisampleStateCreateInfo *ms = ci->pMultisampleState;
        w_opt(&w, ms, sizeof(*ms));
        if (ms) {
            w_u64(&w, ms->pSampleMask != NULL);
            if (ms->pSampleMask) w_raw(&w, ms->pSampleMask, (size_t)((ms->rasterizationSamples + 31) / 32) * 4);
        }
        w_opt(&w, ci->pDepthStencilState, sizeof(VkPipelineDepthStencilStateCreateInfo));
        const VkPipelineColorBlendStateCreateInfo *cb = ci->pColorBlendState;
        w_opt(&w, cb, sizeof(*cb));
        if (cb) w_raw(&w, cb->pAttachments, (size_t)cb->attachmentCount * sizeof(VkPipelineColorBlendAttachmentState));
        const VkPipelineDynamicStateCreateInfo *dy = ci->pDynamicState;
        w_opt(&w, dy, sizeof(*dy));
        if (dy) w_raw(&w, dy->pDynamicStates, (size_t)dy->dynamicStateCount * sizeof(VkDynamicState));
    }
    for (uint32_t i = 0; i < n; i++) out[i] = VK_NULL_HANDLE;
    uint8_t *resp = malloc(8 + (size_t)n * 8 + 64);
    if (!resp) { w_free(&w); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    size_t rl = 0;
    VkResult vr = vcall(d->inst, SPATHA_OP_CREATE_GFX_PIPELINES, &w, resp, 8 + (size_t)n * 8 + 64, &rl);
    w_free(&w);
    if (vr == VK_SUCCESS) {
        if (rl < 8 + (size_t)n * 8) vr = VK_ERROR_DEVICE_LOST;
        else for (uint32_t i = 0; i < n; i++) { uint64_t id; memcpy(&id, resp + 8 + i * 8, 8); out[i] = TOH(VkPipeline, id); }
    }
    free(resp);
    return vr;
}
static VKAPI_ATTR void VKAPI_CALL
stub_vkDestroyPipeline(VkDevice d, VkPipeline p, const VkAllocationCallbacks *a)
{ (void)a; if (p) rpc_small_destroy((struct spatha_device *)d, SPATHA_T_PIPE, ID(p)); }

/* ---- descriptor sets ---- */


static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkFreeDescriptorSets(VkDevice device, VkDescriptorPool pool, uint32_t n, const VkDescriptorSet *sets)
{
    struct spatha_device *d = (struct spatha_device *)device;
    struct wbuf w; w_init(&w);
    w_u64(&w, d->remote_id); w_u64(&w, ID(pool)); w_u64(&w, n);
    for (uint32_t i = 0; i < n; i++) w_u64(&w, ID(sets[i]));
    alignas(8) uint8_t resp[16]; size_t rl;
    VkResult r = vcall(d->inst, SPATHA_OP_FREE_DESC_SETS, &w, resp, sizeof(resp), &rl);
    w_free(&w);
    return r;
}

/* ---- sincronizacion de queues ---- */
static VKAPI_ATTR VkResult VKAPI_CALL stub_vkQueueWaitIdle(VkQueue queue)
{
    struct spatha_queue *q = (struct spatha_queue *)queue;
    struct wbuf w; w_init(&w); w_u64(&w, q->dev->remote_id); w_u64(&w, q->remote_id);
    alignas(8) uint8_t resp[16]; size_t rl;
    VkResult r = vcall(q->dev->inst, SPATHA_OP_QUEUE_WAIT_IDLE, &w, resp, sizeof(resp), &rl);
    w_free(&w); return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL stub_vkDeviceWaitIdle(VkDevice device)
{
    struct spatha_device *d = (struct spatha_device *)device;
    struct wbuf w; w_init(&w); w_u64(&w, d->remote_id); w_u64(&w, 0);
    alignas(8) uint8_t resp[16]; size_t rl;
    VkResult r = vcall(d->inst, SPATHA_OP_DEVICE_WAIT_IDLE, &w, resp, sizeof(resp), &rl);
    w_free(&w); return r;
}
/* ====================================================================
 * Command buffers: se graba un stream local y se reproduce en el host
 * al terminar (vkEndCommandBuffer).
 * ==================================================================== */
static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkAllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo *ai, VkCommandBuffer *out)
{
    struct spatha_device *d = (struct spatha_device *)device;
    uint32_t n = ai->commandBufferCount;
    struct wbuf w; w_init(&w);
    w_u64(&w, d->remote_id); w_u64(&w, ID(ai->commandPool)); w_u64(&w, ai->level); w_u64(&w, n);
    uint8_t *resp = malloc(8 + (size_t)n * 8 + 64);
    if (!resp) { w_free(&w); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    size_t rl = 0;
    VkResult vr = vcall(d->inst, SPATHA_OP_ALLOC_CMDBUFS, &w, resp, 8 + (size_t)n * 8 + 64, &rl);
    w_free(&w);
    for (uint32_t i = 0; i < n; i++) out[i] = VK_NULL_HANDLE;
    if (vr == VK_SUCCESS && rl < 8 + (size_t)n * 8) vr = VK_ERROR_DEVICE_LOST;
    if (vr == VK_SUCCESS) {
        for (uint32_t i = 0; i < n; i++) {
            struct spatha_cmdbuf *c = calloc(1, sizeof(*c));
            uint64_t id; memcpy(&id, resp + 8 + i * 8, 8);
            if (!c) {      /* deshacer lo asignado */
                for (uint32_t k = 0; k < i; k++) { free(out[k]); out[k] = VK_NULL_HANDLE; }
                vr = VK_ERROR_OUT_OF_HOST_MEMORY; break;
            }
            set_loader_magic_value(c);
            c->dev = d; c->remote_id = (uint32_t)id;
            out[i] = (VkCommandBuffer)c;
        }
    }
    free(resp);
    return vr;
}

static VKAPI_ATTR void VKAPI_CALL
stub_vkFreeCommandBuffers(VkDevice device, VkCommandPool pool, uint32_t n, const VkCommandBuffer *cbs)
{
    (void)pool;
    struct spatha_device *d = (struct spatha_device *)device;
    struct wbuf w; w_init(&w);
    uint64_t cnt = 0;
    for (uint32_t i = 0; i < n; i++) if (cbs[i]) cnt++;
    if (!cnt) return;
    w_u64(&w, d->remote_id); w_u64(&w, cnt);
    for (uint32_t i = 0; i < n; i++) if (cbs[i]) w_u64(&w, ((struct spatha_cmdbuf *)cbs[i])->remote_id);
    alignas(8) uint8_t resp[16]; size_t rl;
    vcall(d->inst, SPATHA_OP_FREE_CMDBUFS, &w, resp, sizeof(resp), &rl);
    w_free(&w);
    for (uint32_t i = 0; i < n; i++)
        if (cbs[i]) { struct spatha_cmdbuf *c = (struct spatha_cmdbuf *)cbs[i]; w_free(&c->rec); free(c); }
}


static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkResetCommandBuffer(VkCommandBuffer cb, VkCommandBufferResetFlags f)
{
    (void)f;
    struct spatha_cmdbuf *c = (struct spatha_cmdbuf *)cb;
    w_free(&c->rec); c->recording = 0;
    return VK_SUCCESS;
}

/* Cada vkCmd* construye su cuerpo y lo anade al stream como [cmd][nbytes][cuerpo]. */
#define CMD(cb) struct spatha_cmdbuf *cmdb = (struct spatha_cmdbuf *)(cb); struct wbuf b; w_init(&b)
#define CMD_DONE(code) do { w_u64(&cmdb->rec, (code)); w_u64(&cmdb->rec, b.len); \
    w_raw(&cmdb->rec, b.p, b.len); w_free(&b); } while (0)

static VKAPI_ATTR void VKAPI_CALL
stub_vkCmdBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo *bi, VkSubpassContents sc)
{ CMD(cb); w_raw(&b, bi, sizeof(*bi));
  w_raw(&b, bi->pClearValues, (size_t)bi->clearValueCount * sizeof(VkClearValue));
  w_u64(&b, sc); CMD_DONE(SPATHA_CMD_BEGIN_RENDER_PASS); }
static VKAPI_ATTR void VKAPI_CALL stub_vkCmdEndRenderPass(VkCommandBuffer cb)
{ CMD(cb); CMD_DONE(SPATHA_CMD_END_RENDER_PASS); }
static VKAPI_ATTR void VKAPI_CALL stub_vkCmdNextSubpass(VkCommandBuffer cb, VkSubpassContents sc)
{ CMD(cb); w_u64(&b, sc); CMD_DONE(SPATHA_CMD_NEXT_SUBPASS); }
static VKAPI_ATTR void VKAPI_CALL
stub_vkCmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p)
{ CMD(cb); w_u64(&b, bp); w_u64(&b, ID(p)); CMD_DONE(SPATHA_CMD_BIND_PIPELINE); }
static VKAPI_ATTR void VKAPI_CALL
stub_vkCmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout l, uint32_t first,
                             uint32_t n, const VkDescriptorSet *sets, uint32_t nd, const uint32_t *dyn)
{ CMD(cb); w_u64(&b, bp); w_u64(&b, ID(l)); w_u64(&b, first); w_u64(&b, n);
  for (uint32_t i = 0; i < n; i++) w_u64(&b, ID(sets[i]));
  w_u64(&b, nd); w_raw(&b, dyn, (size_t)nd * 4); CMD_DONE(SPATHA_CMD_BIND_DESC_SETS); }
static VKAPI_ATTR void VKAPI_CALL
stub_vkCmdSetViewport(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkViewport *v)
{ CMD(cb); w_u64(&b, first); w_u64(&b, n); w_raw(&b, v, (size_t)n * sizeof(*v)); CMD_DONE(SPATHA_CMD_SET_VIEWPORT); }
static VKAPI_ATTR void VKAPI_CALL
stub_vkCmdSetScissor(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkRect2D *v)
{ CMD(cb); w_u64(&b, first); w_u64(&b, n); w_raw(&b, v, (size_t)n * sizeof(*v)); CMD_DONE(SPATHA_CMD_SET_SCISSOR); }
static VKAPI_ATTR void VKAPI_CALL
stub_vkCmdDraw(VkCommandBuffer cb, uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi)
{ CMD(cb); w_u64(&b, vc); w_u64(&b, ic); w_u64(&b, fv); w_u64(&b, fi); CMD_DONE(SPATHA_CMD_DRAW); }
static VKAPI_ATTR void VKAPI_CALL
stub_vkCmdDrawIndexed(VkCommandBuffer cb, uint32_t xc, uint32_t ic, uint32_t fx, int32_t vo, uint32_t fi)
{ CMD(cb); w_u64(&b, xc); w_u64(&b, ic); w_u64(&b, fx); w_u64(&b, (uint64_t)(int64_t)vo); w_u64(&b, fi);
  CMD_DONE(SPATHA_CMD_DRAW_INDEXED); }
static VKAPI_ATTR void VKAPI_CALL
stub_vkCmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags ss, VkPipelineStageFlags ds, VkDependencyFlags df,
        uint32_t nm, const VkMemoryBarrier *mb, uint32_t nb, const VkBufferMemoryBarrier *bb,
        uint32_t ni, const VkImageMemoryBarrier *ib)
{ CMD(cb); w_u64(&b, ss); w_u64(&b, ds); w_u64(&b, df);
  w_u64(&b, nm); w_raw(&b, mb, (size_t)nm * sizeof(*mb));
  w_u64(&b, nb); w_raw(&b, bb, (size_t)nb * sizeof(*bb));
  w_u64(&b, ni); w_raw(&b, ib, (size_t)ni * sizeof(*ib)); CMD_DONE(SPATHA_CMD_PIPELINE_BARRIER); }
static VKAPI_ATTR void VKAPI_CALL
stub_vkCmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer sb, VkImage di, VkImageLayout l, uint32_t n, const VkBufferImageCopy *r)
{ CMD(cb); w_u64(&b, ID(sb)); w_u64(&b, ID(di)); w_u64(&b, l); w_u64(&b, n);
  w_raw(&b, r, (size_t)n * sizeof(*r)); CMD_DONE(SPATHA_CMD_COPY_BUFFER_TO_IMAGE); }
static VKAPI_ATTR void VKAPI_CALL
stub_vkCmdCopyImageToBuffer(VkCommandBuffer cb, VkImage si, VkImageLayout l, VkBuffer db, uint32_t n, const VkBufferImageCopy *r)
{ CMD(cb); w_u64(&b, ID(si)); w_u64(&b, l); w_u64(&b, ID(db)); w_u64(&b, n);
  w_raw(&b, r, (size_t)n * sizeof(*r)); CMD_DONE(SPATHA_CMD_COPY_IMAGE_TO_BUFFER); }
static VKAPI_ATTR void VKAPI_CALL
stub_vkCmdCopyBuffer(VkCommandBuffer cb, VkBuffer sb, VkBuffer db, uint32_t n, const VkBufferCopy *r)
{ CMD(cb); w_u64(&b, ID(sb)); w_u64(&b, ID(db)); w_u64(&b, n);
  w_raw(&b, r, (size_t)n * sizeof(*r)); CMD_DONE(SPATHA_CMD_COPY_BUFFER); }
static VKAPI_ATTR void VKAPI_CALL
stub_vkCmdCopyImage(VkCommandBuffer cb, VkImage si, VkImageLayout sl, VkImage di, VkImageLayout dl, uint32_t n, const VkImageCopy *r)
{ CMD(cb); w_u64(&b, ID(si)); w_u64(&b, sl); w_u64(&b, ID(di)); w_u64(&b, dl); w_u64(&b, n);
  w_raw(&b, r, (size_t)n * sizeof(*r)); CMD_DONE(SPATHA_CMD_COPY_IMAGE); }
static VKAPI_ATTR void VKAPI_CALL
stub_vkCmdBlitImage(VkCommandBuffer cb, VkImage si, VkImageLayout sl, VkImage di, VkImageLayout dl, uint32_t n, const VkImageBlit *r, VkFilter f)
{ CMD(cb); w_u64(&b, ID(si)); w_u64(&b, sl); w_u64(&b, ID(di)); w_u64(&b, dl); w_u64(&b, n);
  w_raw(&b, r, (size_t)n * sizeof(*r)); w_u64(&b, f); CMD_DONE(SPATHA_CMD_BLIT_IMAGE); }
static VKAPI_ATTR void VKAPI_CALL
stub_vkCmdClearColorImage(VkCommandBuffer cb, VkImage im, VkImageLayout l, const VkClearColorValue *c, uint32_t n, const VkImageSubresourceRange *r)
{ CMD(cb); w_u64(&b, ID(im)); w_u64(&b, l); w_raw(&b, c, sizeof(*c)); w_u64(&b, n);
  w_raw(&b, r, (size_t)n * sizeof(*r)); CMD_DONE(SPATHA_CMD_CLEAR_COLOR_IMAGE); }
static VKAPI_ATTR void VKAPI_CALL
stub_vkCmdBindVertexBuffers(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer *bs, const VkDeviceSize *offs)
{ CMD(cb); w_u64(&b, first); w_u64(&b, n);
  for (uint32_t i = 0; i < n; i++) w_u64(&b, ID(bs[i]));
  for (uint32_t i = 0; i < n; i++) { w_u64(&b, offs[i]); }
  CMD_DONE(SPATHA_CMD_BIND_VERTEX_BUFFERS); }
static VKAPI_ATTR void VKAPI_CALL
stub_vkCmdBindIndexBuffer(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off, VkIndexType t)
{ CMD(cb); w_u64(&b, ID(buf)); w_u64(&b, off); w_u64(&b, t); CMD_DONE(SPATHA_CMD_BIND_INDEX_BUFFER); }
static VKAPI_ATTR void VKAPI_CALL
stub_vkCmdPushConstants(VkCommandBuffer cb, VkPipelineLayout l, VkShaderStageFlags sf, uint32_t off, uint32_t sz, const void *data)
{ CMD(cb); w_u64(&b, ID(l)); w_u64(&b, sf); w_u64(&b, off); w_u64(&b, sz); w_raw(&b, data, sz);
  CMD_DONE(SPATHA_CMD_PUSH_CONSTANTS); }
static VKAPI_ATTR void VKAPI_CALL stub_vkCmdSetLineWidth(VkCommandBuffer cb, float lw)
{ CMD(cb); uint32_t u; memcpy(&u, &lw, 4); w_u64(&b, u); CMD_DONE(SPATHA_CMD_SET_LINE_WIDTH); }
static VKAPI_ATTR void VKAPI_CALL stub_vkCmdSetBlendConstants(VkCommandBuffer cb, const float c[4])
{ CMD(cb); w_raw(&b, c, 16); CMD_DONE(SPATHA_CMD_SET_BLEND_CONSTANTS); }
static VKAPI_ATTR void VKAPI_CALL stub_vkCmdSetDepthBias(VkCommandBuffer cb, float a, float c, float s)
{ CMD(cb); float f[3] = { a, c, s }; w_raw(&b, f, 12); CMD_DONE(SPATHA_CMD_SET_DEPTH_BIAS); }
static VKAPI_ATTR void VKAPI_CALL
stub_vkCmdFillBuffer(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off, VkDeviceSize sz, uint32_t data)
{ CMD(cb); w_u64(&b, ID(buf)); w_u64(&b, off); w_u64(&b, sz); w_u64(&b, data); CMD_DONE(SPATHA_CMD_FILL_BUFFER); }

/* ====================================================================
 * WSI: surface xcb (la crea el loader) + swapchain emulado.
 * libxcb se carga con dlopen y sus tipos se declaran aqui: no hace falta
 * libxcb-dev en el contenedor.
 * ==================================================================== */
typedef struct { VkIcdSurfaceBase base; void *connection; uint32_t window; } SpathaSurfaceXcb;
typedef struct { unsigned int sequence; } sx_cookie;
typedef struct { uint8_t response_type, depth; uint16_t sequence; uint32_t length, root;
                 int16_t x, y; uint16_t width, height, border_width; uint8_t pad0[2]; } sx_geom_reply;

static struct {
    void *lib;
    sx_cookie (*get_geometry)(void *, uint32_t);
    sx_geom_reply *(*get_geometry_reply)(void *, sx_cookie, void *);
    uint32_t (*generate_id)(void *);
    sx_cookie (*create_gc)(void *, uint32_t, uint32_t, uint32_t, const void *);
    sx_cookie (*free_gc)(void *, uint32_t);
    sx_cookie (*put_image)(void *, uint8_t, uint32_t, uint32_t, uint16_t, uint16_t, int16_t, int16_t,
                           uint8_t, uint8_t, uint32_t, const uint8_t *);
    int (*flush)(void *);
    uint32_t (*max_req_len)(void *);
    void *(*request_check)(void *, sx_cookie);
} X;

/* ---- MIT-SHM opcional: memfd + xcb_shm_attach_fd (SHM 1.2). Si algo falla en
 * cualquier punto se vuelve a xcb_put_image. SPATHA_NOSHM=1 lo desactiva. ---- */
typedef struct { uint8_t response_type, shared_pixmaps; uint16_t sequence; uint32_t length;
                 uint16_t major, minor, uid, gid; uint8_t pixmap_format, pad[15]; } sx_shm_ver_reply;
static struct {
    void *lib;
    sx_cookie (*query_version)(void *);
    sx_shm_ver_reply *(*query_version_reply)(void *, sx_cookie, void *);
    sx_cookie (*attach_fd_checked)(void *, uint32_t, int, uint8_t);
    sx_cookie (*put_image_checked)(void *, uint32_t, uint32_t, uint16_t, uint16_t, uint16_t, uint16_t,
                                   uint16_t, uint16_t, int16_t, int16_t, uint8_t, uint8_t, uint8_t,
                                   uint32_t, uint32_t);
    sx_cookie (*put_image)(void *, uint32_t, uint32_t, uint16_t, uint16_t, uint16_t, uint16_t,
                           uint16_t, uint16_t, int16_t, int16_t, uint8_t, uint8_t, uint8_t,
                           uint32_t, uint32_t);
    sx_cookie (*detach)(void *, uint32_t);
} XS;
static pthread_mutex_t g_x_lock = PTHREAD_MUTEX_INITIALIZER;

static void *dl_try(const char *const *names) {
    for (int i = 0; names[i]; i++) {
        void *h = dlopen(names[i], RTLD_NOW | RTLD_GLOBAL);
        if (h) return h;
    }
    return NULL;
}

static int x_load(void) {
    pthread_mutex_lock(&g_x_lock);
    if (!X.lib) {
        static const char *const cand[] = { "libxcb.so.1", "libxcb.so", NULL };
        void *l = dl_try(cand);
        if (!l) { DBG("dlopen libxcb.so.1: %s", dlerror()); pthread_mutex_unlock(&g_x_lock); return 0; }
        X.get_geometry = dlsym(l, "xcb_get_geometry");
        X.get_geometry_reply = dlsym(l, "xcb_get_geometry_reply");
        X.generate_id = dlsym(l, "xcb_generate_id");
        X.create_gc = dlsym(l, "xcb_create_gc");
        X.free_gc = dlsym(l, "xcb_free_gc");
        X.put_image = dlsym(l, "xcb_put_image");
        X.flush = dlsym(l, "xcb_flush");
        X.max_req_len = dlsym(l, "xcb_get_maximum_request_length");
        X.request_check = dlsym(l, "xcb_request_check");
        if (!X.get_geometry || !X.get_geometry_reply || !X.generate_id || !X.create_gc || !X.free_gc ||
            !X.put_image || !X.flush || !X.max_req_len) { DBG("libxcb incompleta"); pthread_mutex_unlock(&g_x_lock); return 0; }
        X.lib = l;
    }
    pthread_mutex_unlock(&g_x_lock);
    return 1;
}

static SpathaSurfaceXcb *surf_xcb(VkSurfaceKHR s) {
    SpathaSurfaceXcb *x = (SpathaSurfaceXcb *)(uintptr_t)s;
    return (x && x->base.platform == VK_ICD_WSI_PLATFORM_XCB) ? x : NULL;
}

/* Loader >=1.4.3xx: pide al ICD su propia superficie; sin esto SurfaceSupport = FALSE */
typedef struct { VkStructureType sType; const void *pNext; VkFlags flags; void *connection; uint32_t window; } SpathaXcbCI;

static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkCreateXcbSurfaceKHR(VkInstance instance, const SpathaXcbCI *ci, const VkAllocationCallbacks *alloc, VkSurfaceKHR *out)
{
    (void)instance; (void)alloc;
    if (!ci || !out) return VK_ERROR_INITIALIZATION_FAILED;
    SpathaSurfaceXcb *s = calloc(1, sizeof(*s));
    if (!s) return VK_ERROR_OUT_OF_HOST_MEMORY;
    s->base.platform = VK_ICD_WSI_PLATFORM_XCB;
    s->connection = ci->connection;
    s->window = ci->window;
    *out = (VkSurfaceKHR)(uintptr_t)s;
    DBG("CreateXcbSurfaceKHR -> %p", (void *)s);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
stub_vkDestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks *alloc)
{
    (void)instance; (void)alloc;
    free(surf_xcb(surface));
}

static int x_geometry(SpathaSurfaceXcb *s, uint32_t *w, uint32_t *h, uint8_t *depth) {
    if (!x_load()) return 0;
    sx_geom_reply *g = X.get_geometry_reply(s->connection, X.get_geometry(s->connection, s->window), NULL);
    if (!g) return 0;
    *w = g->width; *h = g->height; if (depth) *depth = g->depth;
    free(g);
    return 1;
}

static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkGetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice physicalDevice, uint32_t family, VkSurfaceKHR surface, VkBool32 *sup)
{
    struct spatha_physical_device *pd = (struct spatha_physical_device *)physicalDevice;
    if (!sup) return VK_ERROR_INITIALIZATION_FAILED;
    *sup = VK_FALSE;
    DBG("surface_support: family=%u surf=%p platform=%d", family, (void*)(uintptr_t)surface, surface ? (int)((SpathaSurfaceXcb *)(uintptr_t)surface)->base.platform : -1);
    if (!pd || !pd->inst || !surf_xcb(surface)) return VK_SUCCESS;
    pthread_mutex_lock(&pd->inst->pd_lock);
    int ok = qf_locked(pd);
    if (ok && family < pd->n_qf && (pd->qf[family].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)))
        *sup = VK_TRUE;
    pthread_mutex_unlock(&pd->inst->pd_lock);
    DBG("surface_support family=%u -> %u", family, (unsigned)*sup);
    return ok ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED;
}

static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice pd, VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR *c)
{
    (void)pd;
    SpathaSurfaceXcb *s = surf_xcb(surface);
    if (!s || !c) return VK_ERROR_SURFACE_LOST_KHR;
    uint32_t w = 0, h = 0;
    if (!x_geometry(s, &w, &h, NULL)) return VK_ERROR_SURFACE_LOST_KHR;
    memset(c, 0, sizeof(*c));
    c->minImageCount = 2; c->maxImageCount = 8;
    c->currentExtent = (VkExtent2D){ w, h };
    c->minImageExtent = (VkExtent2D){ 1, 1 };
    c->maxImageExtent = (VkExtent2D){ 16384, 16384 };
    c->maxImageArrayLayers = 1;
    c->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    c->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    c->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    c->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                             VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice pd, VkSurfaceKHR surface, uint32_t *n, VkSurfaceFormatKHR *f)
{
    (void)pd;
    if (!surf_xcb(surface) || !n) return VK_ERROR_SURFACE_LOST_KHR;
    static const VkSurfaceFormatKHR fm[2] = {
        { VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
        { VK_FORMAT_B8G8R8A8_SRGB,  VK_COLOR_SPACE_SRGB_NONLINEAR_KHR } };
    if (!f) { *n = 2; return VK_SUCCESS; }
    uint32_t k = *n < 2 ? *n : 2;
    memcpy(f, fm, k * sizeof(fm[0]));
    *n = k;
    return k < 2 ? VK_INCOMPLETE : VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice pd, VkSurfaceKHR surface, uint32_t *n, VkPresentModeKHR *m)
{
    (void)pd;
    if (!surf_xcb(surface) || !n) return VK_ERROR_SURFACE_LOST_KHR;
    static const VkPresentModeKHR pm[2] = { VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR };
    if (!m) { *n = 2; return VK_SUCCESS; }
    uint32_t k = *n < 2 ? *n : 2;
    memcpy(m, pm, k * sizeof(pm[0]));
    *n = k;
    return k < 2 ? VK_INCOMPLETE : VK_SUCCESS;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL
stub_vkGetPhysicalDeviceXcbPresentationSupportKHR(VkPhysicalDevice pd, uint32_t family, void *conn, uint32_t visual)
{ (void)pd; (void)family; (void)conn; (void)visual; return VK_TRUE; }


static int xs_load(void) {
    pthread_mutex_lock(&g_x_lock);
    if (!XS.lib) {
        static const char *const cand[] = { "libxcb-shm.so.0", "libxcb-shm.so", NULL };
        void *l = dl_try(cand);
        if (l) {
            XS.query_version = dlsym(l, "xcb_shm_query_version");
            XS.query_version_reply = dlsym(l, "xcb_shm_query_version_reply");
            XS.attach_fd_checked = dlsym(l, "xcb_shm_attach_fd_checked");
            XS.put_image_checked = dlsym(l, "xcb_shm_put_image_checked");
            XS.put_image = dlsym(l, "xcb_shm_put_image");
            XS.detach = dlsym(l, "xcb_shm_detach");
            if (XS.query_version && XS.query_version_reply && XS.attach_fd_checked &&
                XS.put_image_checked && XS.put_image && XS.detach && X.request_check) XS.lib = l;
        }
        if (!XS.lib) DBG("MIT-SHM: libxcb-shm no disponible");
    }
    pthread_mutex_unlock(&g_x_lock);
    return XS.lib != NULL;
}

struct spatha_swapchain {
    struct spatha_device *dev;
    uint64_t remote_id;
    SpathaSurfaceXcb *surf;
    uint32_t gc, w, h, n, next;
    uint8_t depth;
    VkFormat fmt;
    uint64_t img_id[8];
    uint8_t *frame;                 /* buffer de respuesta de present (malloc, o NULL con SHM) */
    size_t frame_cap;
    int nshm, shm_cur, shm_verified; /* 2 segmentos memfd en ping-pong */
    uint8_t *shm_buf[2];
    uint32_t shm_seg[2];
};

static void shm_teardown(struct spatha_swapchain *sc) {
    void *conn = sc->surf->connection;
    for (int i = 0; i < sc->nshm; i++) {
        if (XS.lib && X.lib) XS.detach(conn, sc->shm_seg[i]);
        munmap(sc->shm_buf[i], sc->frame_cap);
        sc->shm_buf[i] = NULL;
    }
    if (sc->nshm && X.lib) X.flush(conn);
    sc->nshm = 0;
}

/* Devuelve 1 si quedaron 2 segmentos listos. Deja el swapchain intacto si falla. */
static int shm_setup(struct spatha_swapchain *sc) {
    if (getenv("SPATHA_NOSHM") || !xs_load()) return 0;
    void *conn = sc->surf->connection;
    sx_shm_ver_reply *vr = XS.query_version_reply(conn, XS.query_version(conn), NULL);
    int okv = vr && vr->major == 1 && vr->minor >= 2;   /* attach_fd llego en SHM 1.2 */
    DBG("MIT-SHM %s (%u.%u)", okv ? "ok" : "sin fd-passing", vr ? vr->major : 0, vr ? vr->minor : 0);
    free(vr);
    if (!okv) return 0;
    for (int i = 0; i < 2; i++) {
        int fd = (int)syscall(SYS_memfd_create, "spatha-frame", 1u /* MFD_CLOEXEC */);
        if (fd < 0) { DBG("memfd_create: %s", strerror(errno)); goto fail; }
        if (ftruncate(fd, (off_t)sc->frame_cap) < 0) { close(fd); goto fail; }
        void *m = mmap(NULL, sc->frame_cap, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        int xfd = (m == MAP_FAILED) ? -1 : dup(fd);   /* xcb se queda con el fd que le pasamos */
        close(fd);
        if (xfd < 0) { if (m != MAP_FAILED) munmap(m, sc->frame_cap); goto fail; }
        uint32_t seg = X.generate_id(conn);
        void *err = X.request_check(conn, XS.attach_fd_checked(conn, seg, xfd, 1));
        if (err) { free(err); munmap(m, sc->frame_cap); DBG("shm_attach_fd rechazado"); goto fail; }
        sc->shm_buf[i] = m; sc->shm_seg[i] = seg; sc->nshm = i + 1;
    }
    return 1;
fail:
    shm_teardown(sc);
    return 0;
}

static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *ci, const VkAllocationCallbacks *a, VkSwapchainKHR *out)
{
    (void)a;
    struct spatha_device *d = (struct spatha_device *)device;
    SpathaSurfaceXcb *s = surf_xcb(ci->surface);
    if (!s) return VK_ERROR_SURFACE_LOST_KHR;
    if (!x_load()) return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t w = ci->imageExtent.width, h = ci->imageExtent.height;
    uint32_t cw = 0, ch = 0; uint8_t depth = 24;
    if (!x_geometry(s, &cw, &ch, &depth)) return VK_ERROR_SURFACE_LOST_KHR;
    if (!w || !h) return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t want = ci->minImageCount < 2 ? 2 : (ci->minImageCount > 8 ? 8 : ci->minImageCount);

    struct wbuf wb; w_init(&wb);
    w_u64(&wb, d->remote_id); w_u64(&wb, ci->imageFormat); w_u64(&wb, w); w_u64(&wb, h);
    w_u64(&wb, ci->imageUsage); w_u64(&wb, want);
    alignas(8) uint8_t resp[8 + 16 + 8 * 8 + 32]; size_t rl = 0;
    VkResult vr = vcall(d->inst, SPATHA_OP_CREATE_SWAPCHAIN, &wb, resp, sizeof(resp), &rl);
    w_free(&wb);
    if (vr != VK_SUCCESS) return vr;
    struct rbuf r; r_init(&r, resp + 8, rl - 8);
    uint64_t sid = r_u64(&r), n = r_u64(&r);
    if (r.err || n < 1 || n > 8) return VK_ERROR_DEVICE_LOST;

    struct spatha_swapchain *sc = calloc(1, sizeof(*sc));
    if (!sc) return VK_ERROR_OUT_OF_HOST_MEMORY;
    sc->dev = d; sc->remote_id = sid; sc->surf = s; sc->w = w; sc->h = h; sc->n = (uint32_t)n;
    sc->depth = depth; sc->fmt = ci->imageFormat;
    for (uint64_t i = 0; i < n; i++) sc->img_id[i] = r_u64(&r);
    sc->frame_cap = 8 + 16 + (size_t)w * h * 4 + 64;
    sc->gc = X.generate_id(s->connection);
    X.create_gc(s->connection, sc->gc, s->window, 0, NULL);
    int use_shm = shm_setup(sc);
    if (!use_shm) sc->frame = malloc(sc->frame_cap);
    if (!use_shm && !sc->frame) {
        struct wbuf dw; w_init(&dw); w_u64(&dw, d->remote_id); w_u64(&dw, sid);
        alignas(8) uint8_t rs[16]; size_t l2; vcall(d->inst, SPATHA_OP_DESTROY_SWAPCHAIN, &dw, rs, sizeof(rs), &l2);
        w_free(&dw); free(sc);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    *out = TOH(VkSwapchainKHR, sc);
    DBG("swapchain %ux%u n=%u fmt=%d depth=%u", w, h, sc->n, (int)sc->fmt, sc->depth);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
stub_vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks *a)
{
    (void)a;
    struct spatha_device *d = (struct spatha_device *)device;
    struct spatha_swapchain *sc = (struct spatha_swapchain *)(uintptr_t)swapchain;
    if (!sc) return;
    struct wbuf w; w_init(&w); w_u64(&w, d->remote_id); w_u64(&w, sc->remote_id);
    alignas(8) uint8_t resp[16]; size_t rl;
    vcall(d->inst, SPATHA_OP_DESTROY_SWAPCHAIN, &w, resp, sizeof(resp), &rl);
    w_free(&w);
    if (X.lib) { X.free_gc(sc->surf->connection, sc->gc); X.flush(sc->surf->connection); }
    shm_teardown(sc);
    free(sc->frame); free(sc);
}

static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkGetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain, uint32_t *n, VkImage *out)
{
    (void)device;
    struct spatha_swapchain *sc = (struct spatha_swapchain *)(uintptr_t)swapchain;
    if (!sc || !n) return VK_ERROR_INITIALIZATION_FAILED;
    if (!out) { *n = sc->n; return VK_SUCCESS; }
    uint32_t k = *n < sc->n ? *n : sc->n;
    for (uint32_t i = 0; i < k; i++) out[i] = TOH(VkImage, sc->img_id[i]);
    *n = k;
    return k < sc->n ? VK_INCOMPLETE : VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
                           VkSemaphore sem, VkFence fence, uint32_t *idx)
{
    struct spatha_device *d = (struct spatha_device *)device;
    struct spatha_swapchain *sc = (struct spatha_swapchain *)(uintptr_t)swapchain;
    if (!sc || !idx) return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t cw = 0, ch = 0;
    if (!x_geometry(sc->surf, &cw, &ch, NULL)) return VK_ERROR_SURFACE_LOST_KHR;
    if (cw != sc->w || ch != sc->h) return VK_ERROR_OUT_OF_DATE_KHR;
    struct wbuf w; w_init(&w);
    w_u64(&w, d->remote_id); w_u64(&w, sc->remote_id); w_u64(&w, timeout); w_u64(&w, ID(sem)); w_u64(&w, ID(fence));
    alignas(8) uint8_t resp[32]; size_t rl = 0;
    VkResult vr = vcall(d->inst, SPATHA_OP_ACQUIRE_NEXT_IMAGE, &w, resp, sizeof(resp), &rl);
    w_free(&w);
    if (vr != VK_SUCCESS) return vr;
    if (rl < 16) return VK_ERROR_DEVICE_LOST;
    uint64_t i; memcpy(&i, resp + 8, 8);
    *idx = (uint32_t)i;
    return VK_SUCCESS;
}

static VkResult present_one(struct spatha_queue *q, struct spatha_swapchain *sc, uint32_t idx,
                            uint32_t nwait, const VkSemaphore *wait)
{
    struct spatha_device *d = q->dev;
    struct wbuf w; w_init(&w);
    w_u64(&w, d->remote_id); w_u64(&w, q->remote_id); w_u64(&w, sc->remote_id); w_u64(&w, idx); w_u64(&w, nwait);
    for (uint32_t i = 0; i < nwait; i++) w_u64(&w, ID(wait[i]));
    size_t rl = 0;
    uint8_t *buf = sc->nshm ? sc->shm_buf[sc->shm_cur] : sc->frame;
    VkResult vr = vcall(d->inst, SPATHA_OP_QUEUE_PRESENT, &w, buf, sc->frame_cap, &rl);
    w_free(&w);
    if (vr != VK_SUCCESS) return vr;
    size_t need = 8 + 16 + (size_t)sc->w * sc->h * 4;
    if (rl < need) return VK_ERROR_DEVICE_LOST;
    const uint8_t *px = buf + 8 + 16;
    void *conn = sc->surf->connection;
    if (sc->nshm) {
        uint32_t seg = sc->shm_seg[sc->shm_cur];
        if (!sc->shm_verified) {
            void *err = X.request_check(conn, XS.put_image_checked(conn, sc->surf->window, sc->gc,
                (uint16_t)sc->w, (uint16_t)sc->h, 0, 0, (uint16_t)sc->w, (uint16_t)sc->h, 0, 0,
                sc->depth, 2, 0, seg, 8 + 16));
            if (!err) { sc->shm_verified = 1; DBG("MIT-SHM activo"); sc->shm_cur ^= 1; return VK_SUCCESS; }
            free(err);
            DBG("shm_put_image rechazado: vuelvo a put_image");
            uint8_t *keep = malloc(sc->frame_cap);
            if (!keep) return VK_ERROR_OUT_OF_HOST_MEMORY;
            memcpy(keep, buf, need);
            shm_teardown(sc);
            sc->frame = keep;
            px = keep + 8 + 16;
        } else {
            XS.put_image(conn, sc->surf->window, sc->gc, (uint16_t)sc->w, (uint16_t)sc->h, 0, 0,
                         (uint16_t)sc->w, (uint16_t)sc->h, 0, 0, sc->depth, 2, 0, seg, 8 + 16);
            X.flush(conn);
            sc->shm_cur ^= 1;
            return VK_SUCCESS;
        }
    }
    size_t stride = (size_t)sc->w * 4;
    size_t maxb = (size_t)X.max_req_len(conn) * 4;
    size_t rows = maxb > 64 + stride ? (maxb - 64) / stride : 1;
    if (rows < 1) rows = 1;
    for (uint32_t y = 0; y < sc->h; y += (uint32_t)rows) {
        uint32_t nr = sc->h - y < rows ? sc->h - y : (uint32_t)rows;
        X.put_image(conn, 2 /* ZPixmap */, sc->surf->window, sc->gc, (uint16_t)sc->w, (uint16_t)nr,
                    0, (int16_t)y, 0, sc->depth, (uint32_t)(nr * stride), px + (size_t)y * stride);
    }
    X.flush(conn);
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL
stub_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pi)
{
    struct spatha_queue *q = (struct spatha_queue *)queue;
    VkResult overall = VK_SUCCESS;
    for (uint32_t i = 0; i < pi->swapchainCount; i++) {
        struct spatha_swapchain *sc = (struct spatha_swapchain *)(uintptr_t)pi->pSwapchains[i];
        VkResult r = sc ? present_one(q, sc, pi->pImageIndices[i],
                                      i == 0 ? pi->waitSemaphoreCount : 0, pi->pWaitSemaphores)
                        : VK_ERROR_SURFACE_LOST_KHR;
        if (pi->pResults) pi->pResults[i] = r;
        if (r != VK_SUCCESS && overall == VK_SUCCESS) overall = r;
    }
    return overall;
}

/* ====================================================================
 * Dispatch por nombre + exports del ICD
 * ==================================================================== */

#include "icd_v12.inc"

#define FUNCS(X) \
  X(CreateInstance) X(EnumerateInstanceExtensionProperties) X(EnumerateInstanceLayerProperties) \
  X(EnumerateInstanceVersion) X(DestroyInstance) X(EnumeratePhysicalDevices) \
  X(GetPhysicalDeviceProperties) X(GetPhysicalDeviceQueueFamilyProperties) X(GetPhysicalDeviceFeatures) X(GetPhysicalDeviceFeatures2) X(GetPhysicalDeviceProperties2) \
  X(GetPhysicalDeviceMemoryProperties) X(GetPhysicalDeviceFormatProperties) \
  X(GetPhysicalDeviceImageFormatProperties) X(GetPhysicalDeviceSparseImageFormatProperties) \
  X(EnumerateDeviceExtensionProperties) X(EnumerateDeviceLayerProperties) \
  X(CreateDevice) X(DestroyDevice) X(GetDeviceQueue) \
  X(GetPhysicalDeviceSurfaceSupportKHR) X(GetPhysicalDeviceSurfaceCapabilitiesKHR) \
  X(GetPhysicalDeviceSurfaceFormatsKHR) X(GetPhysicalDeviceSurfacePresentModesKHR) \
  X(GetPhysicalDeviceXcbPresentationSupportKHR) X(CreateXcbSurfaceKHR) X(DestroySurfaceKHR) \
  X(CreateSwapchainKHR) X(DestroySwapchainKHR) X(GetSwapchainImagesKHR) \
  X(AcquireNextImageKHR) X(QueuePresentKHR) \
  X(AllocateMemory) X(FreeMemory) X(MapMemory) X(UnmapMemory) X(FlushMappedMemoryRanges) \
  X(InvalidateMappedMemoryRanges) X(GetBufferMemoryRequirements) X(GetImageMemoryRequirements) \
  X(BindBufferMemory) X(BindImageMemory) X(GetImageSubresourceLayout) \
  X(CreateSemaphore) X(DestroySemaphore) X(CreateFence) X(DestroyFence) X(ResetFences) \
  X(GetFenceStatus) X(WaitForFences) X(CreateBuffer) X(DestroyBuffer) X(CreateImage) X(DestroyImage) \
  X(CreateImageView) X(DestroyImageView) X(CreateSampler) X(DestroySampler) \
  X(CreateShaderModule) X(DestroyShaderModule) X(CreateDescriptorSetLayout) \
  X(DestroyDescriptorSetLayout) X(CreateDescriptorPool) X(DestroyDescriptorPool) \
  X(CreatePipelineLayout) X(DestroyPipelineLayout) X(CreatePipelineCache) X(DestroyPipelineCache) \
  X(CreateRenderPass) X(DestroyRenderPass) X(CreateFramebuffer) X(DestroyFramebuffer) \
  X(CreateCommandPool) X(DestroyCommandPool) X(CreateGraphicsPipelines) X(DestroyPipeline) \
  X(AllocateDescriptorSets) X(FreeDescriptorSets) X(UpdateDescriptorSets) \
  X(QueueSubmit) X(QueueWaitIdle) X(DeviceWaitIdle) \
  X(AllocateCommandBuffers) X(FreeCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer) \
  X(ResetCommandBuffer) X(CmdBeginRenderPass) X(CmdEndRenderPass) X(CmdNextSubpass) \
  X(CmdBindPipeline) X(CmdBindDescriptorSets) X(CmdSetViewport) X(CmdSetScissor) X(CmdDraw) \
  X(CmdDrawIndexed) X(CmdPipelineBarrier) X(CmdCopyBufferToImage) X(CmdCopyImageToBuffer) \
  X(CmdCopyBuffer) X(CmdCopyImage) X(CmdBlitImage) X(CmdClearColorImage) X(CmdBindVertexBuffers) \
  X(CmdBindIndexBuffer) X(CmdPushConstants) X(CmdSetLineWidth) X(CmdSetBlendConstants) \
  X(CmdSetDepthBias) X(CmdFillBuffer) \
  NEWFUNCS(X)

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL stub_vkGetDeviceProcAddr(VkDevice dev, const char *name);
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL stub_vkGetInstanceProcAddr(VkInstance i, const char *name);

struct fn_entry { const char *name; PFN_vkVoidFunction fn; };
static const struct fn_entry g_fns[] = {
#define X(n) { "vk" #n, (PFN_vkVoidFunction)stub_vk##n },
    FUNCS(X)
#undef X
    { "vkGetDeviceProcAddr", (PFN_vkVoidFunction)stub_vkGetDeviceProcAddr },
    { "vkGetInstanceProcAddr", (PFN_vkVoidFunction)stub_vkGetInstanceProcAddr },
};

static PFN_vkVoidFunction lookup_name(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof(g_fns) / sizeof(g_fns[0]); i++)
        if (!strcmp(g_fns[i].name, name)) return g_fns[i].fn;
    for (size_t i = 0; i < sizeof(g_alias) / sizeof(g_alias[0]); i++)
        if (!strcmp(g_alias[i].name, name)) return g_alias[i].fn;
    return NULL;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL stub_vkGetDeviceProcAddr(VkDevice dev, const char *name)
{ (void)dev; return lookup_name(name); }
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL stub_vkGetInstanceProcAddr(VkInstance i, const char *name)
{ (void)i; return lookup_name(name); }

SPATHA_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName)
{ (void)instance; return lookup_name(pName); }

SPATHA_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetPhysicalDeviceProcAddr(VkInstance instance, const char *pName)
{ (void)instance; return lookup_name(pName); }

SPATHA_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *pSupportedVersion)
{
    if (!pSupportedVersion) return VK_ERROR_INITIALIZATION_FAILED;
    if (*pSupportedVersion < 4) return VK_ERROR_INCOMPATIBLE_DRIVER;
    if (*pSupportedVersion > SPATHA_ICD_INTERFACE_VERSION) *pSupportedVersion = SPATHA_ICD_INTERFACE_VERSION;
    return VK_SUCCESS;
}

/*
 * probe5 — base de la opcion "buffer compartido + copia".
 * Valida, en el host (Termux), la cadena que va a usar spathad:
 *
 *   AImageReader (da un ANativeWindow, sin SurfaceView)
 *     -> vkCreateAndroidSurfaceKHR (Mali)
 *     -> swapchain real -> clear + present
 *     -> AImageReader_acquireLatestImage -> lectura CPU de los pixeles
 *
 * Si el pixel leido coincide con el color clareado en cada frame, la base
 * funciona y el resto es protocolo (surface/swapchain/frame sobre el socket).
 *
 * Compilar (Termux):  pkg install vulkan-headers
 *   clang -O2 -Wall -Wextra -o probe5 probe5.c -ldl
 * Usar:  ./probe5 [ancho alto]        (default 640 360)
 *        SPATHA_VK_LIB=/ruta/libvulkan.so ./probe5
 *
 * Las funciones de libmediandk se resuelven con dlsym (no hace falta el
 * sysroot de media ni -lmediandk; newWithUsage es API 26, con fallback).
 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_ANDROID_KHR 1
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

typedef struct AImageReader AImageReader;
typedef struct AImage AImage;

#define AIMG_FORMAT_RGBA_8888  0x1
#define HB_USAGE_CPU_READ_OFTEN 3ull
#define HB_USAGE_GPU_COLOR_OUTPUT (1ull << 9)

typedef int (*fn_new_usage)(int32_t, int32_t, int32_t, uint64_t, int32_t, AImageReader **);
typedef int (*fn_new)(int32_t, int32_t, int32_t, int32_t, AImageReader **);
typedef int (*fn_get_window)(AImageReader *, struct ANativeWindow **);
typedef int (*fn_acquire)(AImageReader *, AImage **);
typedef int (*fn_plane_data)(const AImage *, int, uint8_t **, int *);
typedef int (*fn_plane_int)(const AImage *, int, int32_t *);
typedef int (*fn_img_int)(const AImage *, int32_t *);
typedef void (*fn_img_delete)(AImage *);
typedef void (*fn_rd_delete)(AImageReader *);

static fn_new_usage   m_new_usage;
static fn_new         m_new;
static fn_get_window  m_get_window;
static fn_acquire     m_acquire_latest;
static fn_plane_data  m_plane_data;
static fn_plane_int   m_row_stride, m_pixel_stride;
static fn_img_int     m_width, m_height;
static fn_img_delete  m_img_delete;
static fn_rd_delete   m_rd_delete;

#define VKFN(n) static PFN_##n n
VKFN(vkCreateInstance); VKFN(vkDestroyInstance);
VKFN(vkEnumeratePhysicalDevices); VKFN(vkGetPhysicalDeviceProperties);
VKFN(vkGetPhysicalDeviceQueueFamilyProperties);
VKFN(vkGetPhysicalDeviceSurfaceSupportKHR);
VKFN(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
VKFN(vkGetPhysicalDeviceSurfaceFormatsKHR);
VKFN(vkCreateAndroidSurfaceKHR); VKFN(vkDestroySurfaceKHR);
VKFN(vkCreateDevice); VKFN(vkDestroyDevice); VKFN(vkGetDeviceProcAddr);
VKFN(vkGetDeviceQueue);
VKFN(vkCreateSwapchainKHR); VKFN(vkDestroySwapchainKHR);
VKFN(vkGetSwapchainImagesKHR); VKFN(vkAcquireNextImageKHR); VKFN(vkQueuePresentKHR);
VKFN(vkCreateCommandPool); VKFN(vkDestroyCommandPool);
VKFN(vkAllocateCommandBuffers);
VKFN(vkBeginCommandBuffer); VKFN(vkEndCommandBuffer);
VKFN(vkCmdPipelineBarrier); VKFN(vkCmdClearColorImage);
VKFN(vkQueueSubmit); VKFN(vkQueueWaitIdle); VKFN(vkDeviceWaitIdle);
VKFN(vkCreateFence); VKFN(vkDestroyFence);
VKFN(vkWaitForFences); VKFN(vkResetFences);

static PFN_vkGetInstanceProcAddr gipa;

static void die(const char *what) { fprintf(stderr, "[probe5] FALLO: %s\n", what); exit(1); }
#define NEED(p) do { if (!(p)) die("no se pudo resolver " #p); } while (0)
#define VKOK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
    fprintf(stderr, "[probe5] FALLO: %s -> %d (linea %d)\n", #x, (int)r_, __LINE__); exit(1); } } while (0)

#define LOAD_G(n)       do { n = (PFN_##n)gipa(VK_NULL_HANDLE, #n); NEED(n); } while (0)
#define LOAD_I(n)       do { n = (PFN_##n)gipa(inst, #n); NEED(n); } while (0)
#define LOAD_D(n)       do { n = (PFN_##n)vkGetDeviceProcAddr(dev, #n); NEED(n); } while (0)

static void *sym(void *lib, const char *name) { return dlsym(lib, name); }

int main(int argc, char **argv) {
    int W = argc > 2 ? atoi(argv[1]) : 640;
    int H = argc > 2 ? atoi(argv[2]) : 360;
    if (W <= 0 || H <= 0) die("tamano invalido");

    /* ---------- ImageReader (consumidor) ---------- */
    void *media = dlopen("libmediandk.so", RTLD_NOW | RTLD_LOCAL);
    if (!media) { fprintf(stderr, "dlopen libmediandk: %s\n", dlerror()); return 1; }
    m_new_usage      = (fn_new_usage)sym(media, "AImageReader_newWithUsage");
    m_new            = (fn_new)sym(media, "AImageReader_new");
    m_get_window     = (fn_get_window)sym(media, "AImageReader_getWindow");
    m_acquire_latest = (fn_acquire)sym(media, "AImageReader_acquireLatestImage");
    m_plane_data     = (fn_plane_data)sym(media, "AImage_getPlaneData");
    m_row_stride     = (fn_plane_int)sym(media, "AImage_getPlaneRowStride");
    m_pixel_stride   = (fn_plane_int)sym(media, "AImage_getPlanePixelStride");
    m_width          = (fn_img_int)sym(media, "AImage_getWidth");
    m_height         = (fn_img_int)sym(media, "AImage_getHeight");
    m_img_delete     = (fn_img_delete)sym(media, "AImage_delete");
    m_rd_delete      = (fn_rd_delete)sym(media, "AImageReader_delete");
    NEED(m_get_window); NEED(m_acquire_latest); NEED(m_plane_data);
    NEED(m_row_stride); NEED(m_pixel_stride); NEED(m_width); NEED(m_height);
    NEED(m_img_delete); NEED(m_rd_delete);

    AImageReader *rd = NULL;
    int ms;
    if (m_new_usage)
        ms = m_new_usage(W, H, AIMG_FORMAT_RGBA_8888,
                         HB_USAGE_CPU_READ_OFTEN | HB_USAGE_GPU_COLOR_OUTPUT, 4, &rd);
    else if (m_new)
        ms = m_new(W, H, AIMG_FORMAT_RGBA_8888, 4, &rd);
    else { die("sin AImageReader_new*"); return 1; }
    if (ms || !rd) { fprintf(stderr, "[probe5] AImageReader_new*: media_status=%d\n", ms); return 1; }

    struct ANativeWindow *win = NULL;
    if (m_get_window(rd, &win) || !win) die("AImageReader_getWindow");
    printf("[probe5] ImageReader %dx%d RGBA8888 ok (%s) window=%p\n", W, H,
           m_new_usage ? "newWithUsage" : "new", (void *)win);

    /* ---------- Vulkan real ---------- */
    const char *vkpath = getenv("SPATHA_VK_LIB");
    if (!vkpath || !*vkpath) vkpath = "/system/lib64/libvulkan.so";
    void *vk = dlopen(vkpath, RTLD_NOW | RTLD_LOCAL);
    if (!vk) { fprintf(stderr, "dlopen %s: %s\n", vkpath, dlerror()); return 1; }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(vk, "vkGetInstanceProcAddr");
    NEED(gipa);
    LOAD_G(vkCreateInstance);

    const char *iext[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME };
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                             .pApplicationName = "probe5", .apiVersion = VK_API_VERSION_1_0 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                 .pApplicationInfo = &ai,
                                 .enabledExtensionCount = 2, .ppEnabledExtensionNames = iext };
    VkInstance inst = VK_NULL_HANDLE;
    VKOK(vkCreateInstance(&ici, NULL, &inst));
    printf("[probe5] instance ok (VK_KHR_surface + VK_KHR_android_surface)\n");

    LOAD_I(vkDestroyInstance); LOAD_I(vkEnumeratePhysicalDevices);
    LOAD_I(vkGetPhysicalDeviceProperties); LOAD_I(vkGetPhysicalDeviceQueueFamilyProperties);
    LOAD_I(vkGetPhysicalDeviceSurfaceSupportKHR); LOAD_I(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    LOAD_I(vkGetPhysicalDeviceSurfaceFormatsKHR);
    LOAD_I(vkCreateAndroidSurfaceKHR); LOAD_I(vkDestroySurfaceKHR);
    LOAD_I(vkCreateDevice); LOAD_I(vkGetDeviceProcAddr);

    uint32_t npd = 1;
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    VkResult er = vkEnumeratePhysicalDevices(inst, &npd, &pd);
    if ((er != VK_SUCCESS && er != VK_INCOMPLETE) || npd == 0) die("sin physical devices");
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    printf("[probe5] GPU: %s api=%u.%u.%u\n", props.deviceName,
           VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
           VK_API_VERSION_PATCH(props.apiVersion));

    /* ---------- surface sobre el ImageReader ---------- */
    VkAndroidSurfaceCreateInfoKHR sci = { .sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
                                          .window = win };
    VkSurfaceKHR surf = VK_NULL_HANDLE;
    VKOK(vkCreateAndroidSurfaceKHR(inst, &sci, NULL, &surf));
    printf("[probe5] vkCreateAndroidSurfaceKHR sobre el ImageReader: ok\n");

    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
    VkQueueFamilyProperties *qp = calloc(nq ? nq : 1, sizeof(*qp));
    if (!qp) die("calloc");
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp);
    uint32_t qf = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++) {
        VkBool32 sup = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surf, &sup);
        if (sup && (qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) { qf = i; break; }
    }
    free(qp);
    if (qf == UINT32_MAX) die("sin queue family graphics+present");

    /* ---------- device + swapchain ---------- */
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                    .queueFamilyIndex = qf, .queueCount = 1, .pQueuePriorities = &prio };
    const char *dext[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                               .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
                               .enabledExtensionCount = 1, .ppEnabledExtensionNames = dext };
    VkDevice dev = VK_NULL_HANDLE;
    VKOK(vkCreateDevice(pd, &dci, NULL, &dev));

    LOAD_D(vkDestroyDevice); LOAD_D(vkGetDeviceQueue);
    LOAD_D(vkCreateSwapchainKHR); LOAD_D(vkDestroySwapchainKHR);
    LOAD_D(vkGetSwapchainImagesKHR); LOAD_D(vkAcquireNextImageKHR); LOAD_D(vkQueuePresentKHR);
    LOAD_D(vkCreateCommandPool); LOAD_D(vkDestroyCommandPool); LOAD_D(vkAllocateCommandBuffers);
    LOAD_D(vkBeginCommandBuffer); LOAD_D(vkEndCommandBuffer);
    LOAD_D(vkCmdPipelineBarrier); LOAD_D(vkCmdClearColorImage);
    LOAD_D(vkQueueSubmit); LOAD_D(vkQueueWaitIdle); LOAD_D(vkDeviceWaitIdle);
    LOAD_D(vkCreateFence); LOAD_D(vkDestroyFence); LOAD_D(vkWaitForFences); LOAD_D(vkResetFences);

    VkQueue q;
    vkGetDeviceQueue(dev, qf, 0, &q);

    VkSurfaceCapabilitiesKHR caps;
    VKOK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surf, &caps));
    printf("[probe5] caps: images %u..%u extent=%ux%u usage=0x%x alpha=0x%x\n",
           caps.minImageCount, caps.maxImageCount,
           caps.currentExtent.width, caps.currentExtent.height,
           caps.supportedUsageFlags, caps.supportedCompositeAlpha);
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) die("surface sin TRANSFER_DST");

    uint32_t nf = 0;
    VKOK(vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &nf, NULL));
    VkSurfaceFormatKHR *fm = calloc(nf ? nf : 1, sizeof(*fm));
    if (!fm) die("calloc");
    VKOK(vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &nf, fm));
    int fi = -1;
    for (uint32_t i = 0; i < nf; i++) {
        printf("[probe5]   formato %u: fmt=%d cs=%d\n", i, fm[i].format, fm[i].colorSpace);
        if (fi < 0 && fm[i].format == VK_FORMAT_R8G8B8A8_UNORM) fi = (int)i;
    }
    if (fi < 0) die("la surface no ofrece R8G8B8A8_UNORM (el ImageReader es RGBA8888)");

    VkCompositeAlphaFlagBitsKHR alpha = 0;
    const VkCompositeAlphaFlagBitsKHR order[] = {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR };
    for (unsigned i = 0; i < sizeof(order) / sizeof(order[0]); i++)
        if (caps.supportedCompositeAlpha & order[i]) { alpha = order[i]; break; }
    if (!alpha) die("sin composite alpha soportado");

    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xFFFFFFFFu) { ext.width = (uint32_t)W; ext.height = (uint32_t)H; }

    uint32_t want = caps.minImageCount < 3 ? 3 : caps.minImageCount;
    if (caps.maxImageCount && want > caps.maxImageCount) want = caps.maxImageCount;

    VkSwapchainCreateInfoKHR sc_ci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = surf,
        .minImageCount = want, .imageFormat = fm[fi].format, .imageColorSpace = fm[fi].colorSpace,
        .imageExtent = ext, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform, .compositeAlpha = alpha,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE };
    free(fm);
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    VKOK(vkCreateSwapchainKHR(dev, &sc_ci, NULL, &sc));

    uint32_t nimg = 0;
    VKOK(vkGetSwapchainImagesKHR(dev, sc, &nimg, NULL));
    VkImage *imgs = calloc(nimg ? nimg : 1, sizeof(*imgs));
    if (!imgs) die("calloc");
    VKOK(vkGetSwapchainImagesKHR(dev, sc, &nimg, imgs));
    printf("[probe5] swapchain ok: %u imagenes %ux%u\n", nimg, ext.width, ext.height);

    /* ---------- command buffer + fence ---------- */
    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                    .queueFamilyIndex = qf };
    VkCommandPool pool;
    VKOK(vkCreateCommandPool(dev, &pci, NULL, &pool));
    VkCommandBufferAllocateInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                        .commandBufferCount = 1 };
    VkCommandBuffer cb;
    VKOK(vkAllocateCommandBuffers(dev, &cbi, &cb));
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence;
    VKOK(vkCreateFence(dev, &fci, NULL, &fence));

    /* ---------- frames: rojo, verde, azul, rojo, ... ---------- */
    static const uint8_t expect[3][4] = { {255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255} };
    const int NFRAMES = 6;
    int bad = 0;
    for (int f = 0; f < NFRAMES; f++) {
        uint32_t idx = 0;
        VKOK(vkResetFences(dev, 1, &fence));
        VkResult ar = vkAcquireNextImageKHR(dev, sc, 1000000000ull, VK_NULL_HANDLE, fence, &idx);
        if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) {
            fprintf(stderr, "[probe5] FALLO: vkAcquireNextImageKHR -> %d (frame %d)\n", (int)ar, f);
            return 1;
        }
        VKOK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 1000000000ull));

        VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        VKOK(vkBeginCommandBuffer(cb, &bi));
        VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkImageMemoryBarrier b = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = imgs[idx], .subresourceRange = rng };
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, NULL, 0, NULL, 1, &b);
        VkClearColorValue cc;
        cc.float32[0] = (f % 3 == 0) ? 1.0f : 0.0f;
        cc.float32[1] = (f % 3 == 1) ? 1.0f : 0.0f;
        cc.float32[2] = (f % 3 == 2) ? 1.0f : 0.0f;
        cc.float32[3] = 1.0f;
        vkCmdClearColorImage(cb, imgs[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &rng);
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = 0;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             0, 0, NULL, 0, NULL, 1, &b);
        VKOK(vkEndCommandBuffer(cb));

        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                            .commandBufferCount = 1, .pCommandBuffers = &cb };
        VKOK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
        VKOK(vkQueueWaitIdle(q));   /* sin semaforos: render terminado antes del present */

        VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                                .swapchainCount = 1, .pSwapchains = &sc, .pImageIndices = &idx };
        VkResult pr = vkQueuePresentKHR(q, &pi);
        if (pr != VK_SUCCESS && pr != VK_SUBOPTIMAL_KHR) {
            fprintf(stderr, "[probe5] FALLO: vkQueuePresentKHR -> %d (frame %d)\n", (int)pr, f);
            return 1;
        }

        /* consumidor: ultimo frame disponible, leido por CPU */
        AImage *img = NULL;
        for (int t = 0; t < 200; t++) {
            if (m_acquire_latest(rd, &img) == 0 && img) break;
            img = NULL;
            usleep(10000);
        }
        if (!img) { printf("[probe5] frame %d: el ImageReader no recibio frame\n", f); bad = 1; continue; }

        int32_t iw = 0, ih = 0, rs = 0, ps = 0;
        uint8_t *data = NULL;
        int dlen = 0;
        m_width(img, &iw); m_height(img, &ih);
        m_row_stride(img, 0, &rs); m_pixel_stride(img, 0, &ps);
        int mr = m_plane_data(img, 0, &data, &dlen);
        if (mr || !data || ps != 4) {
            printf("[probe5] frame %d: getPlaneData media_status=%d data=%p pixel_stride=%d\n",
                   f, mr, (void *)data, ps);
            bad = 1;
        } else {
            const uint8_t *px = data + (size_t)(ih / 2) * (size_t)rs + (size_t)(iw / 2) * 4;
            const uint8_t *e = expect[f % 3];
            int ok = px[0] == e[0] && px[1] == e[1] && px[2] == e[2] && px[3] == e[3];
            printf("[probe5] frame %d: %dx%d stride=%d pixel(centro)=%02x%02x%02x%02x esperado=%02x%02x%02x%02x %s\n",
                   f, iw, ih, rs, px[0], px[1], px[2], px[3], e[0], e[1], e[2], e[3],
                   ok ? "OK" : "MAL");
            if (!ok) bad = 1;
        }
        m_img_delete(img);
    }

    vkDeviceWaitIdle(dev);
    vkDestroyFence(dev, fence, NULL);
    vkDestroyCommandPool(dev, pool, NULL);
    vkDestroySwapchainKHR(dev, sc, NULL);
    free(imgs);
    vkDestroyDevice(dev, NULL);
    vkDestroySurfaceKHR(inst, surf, NULL);
    vkDestroyInstance(inst, NULL);
    m_rd_delete(rd);

    puts(bad ? "[probe5] FAIL" : "[probe5] OK");
    return bad;
}

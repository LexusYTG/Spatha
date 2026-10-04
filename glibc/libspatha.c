/* Cliente de prueba (host): PINGs + Hito 2 completo contra el daemon. */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include "proto.h"

static int call(int fd, uint32_t op, uint32_t req, const void *rq, uint32_t rql,
                void *rs, uint32_t cap, uint32_t *rsl) {
    struct spatha_msg h;
    int rc = spatha_send(fd, op, req, rq, rql);
    if (!rc) rc = spatha_recv(fd, &h, rs, cap);
    if (rc) { fprintf(stderr, "op 0x%02x: %s\n", op, strerror(-rc)); return 1; }
    if (h.op != op || h.req_id != req) { fprintf(stderr, "op 0x%02x: respuesta inesperada op=0x%02x\n", op, h.op); return 1; }
    if (rsl) *rsl = h.len;
    return 0;
}

int main(void) {
    int fd = spatha_connect(spatha_sock_path(), 2000);
    if (fd < 0) { fprintf(stderr, "connect: %s\n", strerror(-fd)); return 1; }
    printf("[spatha] connected\n");

    int bad = 0;
    for (uint32_t i = 0; i < 3; i++) {
        struct spatha_msg h;
        if (spatha_send(fd, SPATHA_OP_PING, i, NULL, 0) || spatha_recv(fd, &h, NULL, 0) ||
            h.op != SPATHA_OP_PONG) { fputs("PING fallo\n", stderr); bad = 1; }
    }
    if (!bad) puts("[spatha] 3x PING ok");

    struct spatha_resp_enum_ext xh;
    uint8_t xb[64 * 1024];
    uint32_t xl;
    if (call(fd, SPATHA_OP_VK_ENUMERATE_INSTANCE_EXTENSION_PROPERTIES, 5, NULL, 0, xb, sizeof xb, &xl)) return 1;
    if (xl < sizeof xh) { fputs("ext: respuesta corta\n", stderr); return 1; }
    memcpy(&xh, xb, sizeof(xh));
    printf("[spatha] ext: result=%d count=%u\n", xh.vk_result, xh.count);
    if (xl != sizeof(xh) + xh.count * sizeof(VkExtensionProperties)) { fputs("ext: largo inconsistente\n", stderr); return 1; }
    for (uint32_t i = 0; i < xh.count && i < 5; i++) {
        VkExtensionProperties p;
        memcpy(&p, xb + sizeof(xh) + i * sizeof(p), sizeof(p));
        printf("[spatha]   %s (v%u)\n", p.extensionName, p.specVersion);
    }
    bad |= xh.vk_result != 0 || xh.count == 0;

    struct spatha_resp_create_instance ci; uint32_t rl;
    if (call(fd, SPATHA_OP_VK_CREATE_INSTANCE, 10, NULL, 0, &ci, sizeof ci, &rl)) return 1;
    printf("[spatha] create: result=%d id=%u\n", ci.vk_result, ci.instance_id);
    if (ci.vk_result || !ci.instance_id) return 1;

    uint8_t eb[sizeof(struct spatha_resp_enum_pd) + SPATHA_MAX_PD * 4];
    if (call(fd, SPATHA_OP_VK_ENUMERATE_PHYSICAL_DEVICES, 11, &ci.instance_id, 4, eb, sizeof eb, &rl)) return 1;
    struct spatha_resp_enum_pd eh; memcpy(&eh, eb, sizeof eh);
    printf("[spatha] enum: result=%d count=%u\n", eh.vk_result, eh.count);

    for (uint32_t i = 0; i < eh.count; i++) {
        uint32_t pd; memcpy(&pd, eb + sizeof eh + i * 4, 4);
        uint8_t pb[4 + sizeof(VkPhysicalDeviceProperties)];
        if (call(fd, SPATHA_OP_VK_GET_PHYSICAL_DEVICE_PROPERTIES, 12 + i, &pd, 4, pb, sizeof pb, &rl)) return 1;
        VkPhysicalDeviceProperties p; memcpy(&p, pb + 4, sizeof p);
        printf("[spatha] pd %u: %s api=%u.%u.%u vid=0x%x did=0x%x\n", pd, p.deviceName,
               VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion),
               VK_API_VERSION_PATCH(p.apiVersion), p.vendorID, p.deviceID);
        printf("[spatha] pd %u: deviceType=%u\n", pd, (unsigned)p.deviceType);

        uint8_t qb[sizeof(struct spatha_resp_enum_qf) + SPATHA_MAX_QF * sizeof(VkQueueFamilyProperties)];
        if (call(fd, SPATHA_OP_VK_GET_PHYSICAL_DEVICE_QUEUE_FAMILY_PROPERTIES, 40 + i, &pd, 4, qb, sizeof qb, &rl)) return 1;
        struct spatha_resp_enum_qf qh;
        if (rl < sizeof qh) { fputs("qf: respuesta corta\n", stderr); return 1; }
        memcpy(&qh, qb, sizeof qh);
        printf("[spatha] pd %u queue families: result=%d count=%u\n", pd, qh.vk_result, qh.count);
        if (rl != sizeof(qh) + qh.count * sizeof(VkQueueFamilyProperties)) { fputs("qf: largo inconsistente\n", stderr); return 1; }
        int has_gfx = 0;
        for (uint32_t j = 0; j < qh.count; j++) {
            VkQueueFamilyProperties q;
            memcpy(&q, qb + sizeof(qh) + j * sizeof(q), sizeof(q));
            printf("[spatha]   qf %u: flags=0x%x queues=%u\n", j, q.queueFlags, q.queueCount);
            if (q.queueFlags & VK_QUEUE_GRAPHICS_BIT) has_gfx = 1;
        }
        bad |= qh.vk_result != 0 || !qh.count || !has_gfx;
    }

    int32_t dr;
    if (call(fd, SPATHA_OP_VK_DESTROY_INSTANCE, 20, &ci.instance_id, 4, &dr, sizeof dr, &rl)) return 1;
    printf("[spatha] destroy: result=%d\n", dr);
    bad |= dr != 0 || eh.count == 0;

    /* Surfaces: CreateInstance con extensiones (nombres del HOST) */
    uint8_t cq[4 + 2 * SPATHA_EXT_NAME_MAX];
    memset(cq, 0, sizeof cq);
    uint32_t two = 2;
    memcpy(cq, &two, 4);
    strcpy((char *)cq + 4, "VK_KHR_surface");
    strcpy((char *)cq + 4 + SPATHA_EXT_NAME_MAX, "VK_KHR_android_surface");
    struct spatha_resp_create_instance c2;
    if (call(fd, SPATHA_OP_VK_CREATE_INSTANCE, 30, cq, sizeof cq, &c2, sizeof c2, &rl)) return 1;
    printf("[spatha] create+ext (surface, android_surface): result=%d id=%u\n", c2.vk_result, c2.instance_id);
    bad |= c2.vk_result != 0 || !c2.instance_id;
    if (c2.instance_id) {
        if (call(fd, SPATHA_OP_VK_DESTROY_INSTANCE, 31, &c2.instance_id, 4, &dr, sizeof dr, &rl)) return 1;
        bad |= dr != 0;
    }

    spatha_send(fd, SPATHA_OP_QUIT, 99, NULL, 0);
    close(fd);
    puts(bad ? "[spatha] FAIL" : "[spatha] OK");
    return bad;
}

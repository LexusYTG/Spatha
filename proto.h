#ifndef SPATHA_PROTO_H
#define SPATHA_PROTO_H

#include <stddef.h>
#include <stdint.h>

#define SPATHA_SOCK_PATH   "/tmp/spatha.sock"
#define SPATHA_MAGIC       0x53505441u
#define SPATHA_MAX_PAYLOAD (16u * 1024u * 1024u)  /* frames de swapchain y escrituras de memoria */

enum spatha_op {
    SPATHA_OP_QUIT = 0x00,
    SPATHA_OP_PING = 0x01,
    SPATHA_OP_PONG = 0x02,
    SPATHA_OP_ERROR = 0x03,  /* payload: uint32 errno-style code */

    SPATHA_OP_VK_CREATE_INSTANCE                = 0x10,
    SPATHA_OP_VK_DESTROY_INSTANCE               = 0x11,
    SPATHA_OP_VK_ENUMERATE_PHYSICAL_DEVICES     = 0x12,
    SPATHA_OP_VK_GET_PHYSICAL_DEVICE_PROPERTIES = 0x13,
    SPATHA_OP_VK_ENUMERATE_INSTANCE_EXTENSION_PROPERTIES = 0x14,
    SPATHA_OP_VK_GET_PHYSICAL_DEVICE_QUEUE_FAMILY_PROPERTIES = 0x15,

    SPATHA_OP_GET_PD_FEATURES          = 0x20,
    SPATHA_OP_GET_PD_MEMPROPS          = 0x21,
    SPATHA_OP_GET_PD_FORMAT_PROPS      = 0x22,
    SPATHA_OP_GET_PD_IMAGE_FORMAT_PROPS= 0x23,
    SPATHA_OP_CREATE_DEVICE            = 0x24,
    SPATHA_OP_DESTROY_DEVICE           = 0x25,
    SPATHA_OP_GET_PD_PROPS2            = 0x26,
    SPATHA_OP_GET_PD_CHAIN             = 0x27,

    SPATHA_OP_CREATE                   = 0x30,
    SPATHA_OP_DESTROY                  = 0x31,
    SPATHA_OP_CREATE_GFX_PIPELINES     = 0x32,
    SPATHA_OP_ALLOC_DESC_SETS          = 0x33,
    SPATHA_OP_FREE_DESC_SETS           = 0x34,
    SPATHA_OP_UPDATE_DESC_SETS         = 0x35,
    SPATHA_OP_SEM_OP                   = 0x36,
    SPATHA_OP_EVENT_OP                 = 0x37,
    SPATHA_OP_GET_QUERY_RESULTS        = 0x38,
    SPATHA_OP_RESET_QUERY_POOL         = 0x39,
    SPATHA_OP_PCACHE_OP                = 0x3A,
    SPATHA_OP_CREATE_COMPUTE_PIPELINES = 0x3B,
    SPATHA_OP_RESET_POOL               = 0x3C,
    SPATHA_OP_DSL_SUPPORT              = 0x3D,
    SPATHA_OP_CREATE_RENDERPASS2       = 0x3E,

    SPATHA_OP_ALLOC_MEMORY             = 0x40,
    SPATHA_OP_BIND_MEMORY              = 0x41,
    SPATHA_OP_GET_MEM_REQS             = 0x42,
    SPATHA_OP_MEM_WRITE                = 0x43,
    SPATHA_OP_MEM_READ                 = 0x44,
    SPATHA_OP_GET_SUBRES_LAYOUT        = 0x45,
    SPATHA_OP_GET_BUFFER_ADDR          = 0x46,

    SPATHA_OP_ALLOC_CMDBUFS            = 0x50,
    SPATHA_OP_FREE_CMDBUFS             = 0x51,
    SPATHA_OP_RESET_CMDBUF             = 0x52,
    SPATHA_OP_CMDBUF_REPLAY            = 0x53,

    SPATHA_OP_QUEUE_SUBMIT             = 0x60,
    SPATHA_OP_QUEUE_WAIT_IDLE          = 0x61,
    SPATHA_OP_DEVICE_WAIT_IDLE         = 0x62,
    SPATHA_OP_WAIT_FENCES              = 0x63,
    SPATHA_OP_RESET_FENCES             = 0x64,
    SPATHA_OP_GET_FENCE_STATUS         = 0x65,

    SPATHA_OP_CREATE_SWAPCHAIN         = 0x70,
    SPATHA_OP_DESTROY_SWAPCHAIN        = 0x71,
    SPATHA_OP_ACQUIRE_NEXT_IMAGE       = 0x72,
    SPATHA_OP_QUEUE_PRESENT            = 0x73,
};

enum spatha_obj {
    SPATHA_T_NONE = 0, SPATHA_T_DEV, SPATHA_T_QUEUE, SPATHA_T_CMDPOOL, SPATHA_T_CMDBUF,
    SPATHA_T_MEM, SPATHA_T_BUF, SPATHA_T_IMG, SPATHA_T_VIEW, SPATHA_T_SAMPLER,
    SPATHA_T_SHADER, SPATHA_T_DSL, SPATHA_T_DPOOL, SPATHA_T_DSET, SPATHA_T_PLAYOUT,
    SPATHA_T_PCACHE, SPATHA_T_RPASS, SPATHA_T_FB, SPATHA_T_PIPE, SPATHA_T_SEM,
    SPATHA_T_FENCE,
    SPATHA_T_EVENT, SPATHA_T_QPOOL, SPATHA_T_BVIEW,
};

#define SPATHA_MAX_PD 8
#define SPATHA_MAX_INST_EXT 32
#define SPATHA_MAX_QF 8
#define SPATHA_EXT_NAME_MAX 256

struct spatha_resp_create_instance { int32_t vk_result; uint32_t instance_id; };
struct spatha_resp_enum_pd         { int32_t vk_result; uint32_t count; };
struct spatha_resp_enum_ext        { int32_t vk_result; uint32_t count; };
struct spatha_resp_enum_qf         { int32_t vk_result; uint32_t count; };

struct spatha_msg {
    uint32_t magic;
    uint32_t op;
    uint32_t len;
    uint32_t req_id;
};

const char *spatha_sock_path(void);

int spatha_connect(const char *path, int timeout_ms);
int spatha_write_all(int fd, const void *buf, size_t n);
int spatha_read_all(int fd, void *buf, size_t n);
int spatha_send(int fd, uint32_t op, uint32_t req_id,
                const void *payload, uint32_t len);
int spatha_recv(int fd, struct spatha_msg *hdr, void *payload, uint32_t cap);

#endif

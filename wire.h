#ifndef SPATHA_WIRE_H
#define SPATHA_WIRE_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct wbuf { uint8_t *p; size_t len, cap; int err; };

static inline void w_init(struct wbuf *w) { memset(w, 0, sizeof(*w)); }
static inline void w_free(struct wbuf *w) { free(w->p); memset(w, 0, sizeof(*w)); }

static inline void w_raw(struct wbuf *w, const void *d, size_t n) {
    size_t m = (n + 7) & ~(size_t)7;
    if (w->err) return;
    if (w->len + m > w->cap) {
        size_t nc = w->cap ? w->cap : 256;
        while (nc < w->len + m) nc *= 2;
        uint8_t *np = realloc(w->p, nc);
        if (!np) { w->err = 1; return; }
        w->p = np; w->cap = nc;
    }
    if (n) memcpy(w->p + w->len, d, n);
    if (m > n) memset(w->p + w->len + n, 0, m - n);
    w->len += m;
}
static inline void w_u64(struct wbuf *w, uint64_t v) { w_raw(w, &v, 8); }

struct rbuf { const uint8_t *p; size_t len, off; int err; };

static inline void r_init(struct rbuf *r, const void *p, size_t len) {
    r->p = p; r->len = len; r->off = 0; r->err = 0;
}
static inline uint64_t r_u64(struct rbuf *r) {
    uint64_t v = 0;
    if (r->err || r->off + 8 > r->len) { r->err = 1; return 0; }
    memcpy(&v, r->p + r->off, 8);
    r->off += 8;
    return v;
}
static inline const void *r_blob(struct rbuf *r, size_t n) {
    size_t m = (n + 7) & ~(size_t)7;
    if (r->err || n > r->len || m < n || r->off + m > r->len) { r->err = 1; return NULL; }
    const void *q = r->p + r->off;
    r->off += m;
    return q;
}
static inline const void *r_arr(struct rbuf *r, uint64_t count, size_t esz) {
    if (esz && count > r->len / esz) { r->err = 1; return NULL; }
    return r_blob(r, (size_t)count * esz);
}

enum spatha_cmd {
    SPATHA_CMD_BEGIN_RENDER_PASS = 1, SPATHA_CMD_END_RENDER_PASS, SPATHA_CMD_NEXT_SUBPASS,
    SPATHA_CMD_BIND_PIPELINE, SPATHA_CMD_BIND_DESC_SETS, SPATHA_CMD_SET_VIEWPORT,
    SPATHA_CMD_SET_SCISSOR, SPATHA_CMD_DRAW, SPATHA_CMD_DRAW_INDEXED,
    SPATHA_CMD_PIPELINE_BARRIER, SPATHA_CMD_COPY_BUFFER_TO_IMAGE,
    SPATHA_CMD_COPY_IMAGE_TO_BUFFER, SPATHA_CMD_COPY_BUFFER, SPATHA_CMD_COPY_IMAGE,
    SPATHA_CMD_BLIT_IMAGE, SPATHA_CMD_CLEAR_COLOR_IMAGE, SPATHA_CMD_BIND_VERTEX_BUFFERS,
    SPATHA_CMD_BIND_INDEX_BUFFER, SPATHA_CMD_PUSH_CONSTANTS, SPATHA_CMD_SET_LINE_WIDTH,
    SPATHA_CMD_SET_BLEND_CONSTANTS, SPATHA_CMD_SET_DEPTH_BIAS, SPATHA_CMD_FILL_BUFFER,
    SPATHA_CMD_DISPATCH, SPATHA_CMD_DISPATCH_INDIRECT, SPATHA_CMD_DISPATCH_BASE,
    SPATHA_CMD_DRAW_INDIRECT, SPATHA_CMD_DRAW_INDEXED_INDIRECT,
    SPATHA_CMD_DRAW_INDIRECT_COUNT, SPATHA_CMD_DRAW_INDEXED_INDIRECT_COUNT,
    SPATHA_CMD_CLEAR_DEPTH_STENCIL, SPATHA_CMD_CLEAR_ATTACHMENTS, SPATHA_CMD_RESOLVE_IMAGE,
    SPATHA_CMD_UPDATE_BUFFER, SPATHA_CMD_SET_DEPTH_BOUNDS, SPATHA_CMD_SET_STENCIL_COMPARE,
    SPATHA_CMD_SET_STENCIL_WRITE, SPATHA_CMD_SET_STENCIL_REF, SPATHA_CMD_SET_EVENT,
    SPATHA_CMD_RESET_EVENT, SPATHA_CMD_WAIT_EVENTS, SPATHA_CMD_EXECUTE_COMMANDS,
    SPATHA_CMD_RESET_QUERY_POOL, SPATHA_CMD_BEGIN_QUERY, SPATHA_CMD_END_QUERY,
    SPATHA_CMD_WRITE_TIMESTAMP, SPATHA_CMD_COPY_QUERY_RESULTS,
};
#endif

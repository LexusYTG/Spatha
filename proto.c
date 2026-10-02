#define _GNU_SOURCE
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include "proto.h"

const char *spatha_sock_path(void) {
    const char *p = getenv("SPATHA_SOCK");
    return (p && *p) ? p : SPATHA_SOCK_PATH;
}

int spatha_connect(const char *path, int timeout_ms) {
    struct sockaddr_un addr;
    if (!path || strlen(path) >= sizeof(addr.sun_path))
        return -ENAMETOOLONG;

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -errno;

    if (timeout_ms > 0) {
        struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, path);

    int rc;
    do { rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr)); }
    while (rc < 0 && errno == EINTR);
    if (rc < 0) {
        int e = errno;
        close(fd);
        return -e;
    }
    return fd;
}

int spatha_write_all(int fd, const void *buf, size_t n) {
    const uint8_t *p = buf;
    while (n) {
        /* MSG_NOSIGNAL: un daemon muerto no debe matar a la app con SIGPIPE */
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return -ETIMEDOUT;
            return -errno;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

int spatha_read_all(int fd, void *buf, size_t n) {
    uint8_t *p = buf;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return -ETIMEDOUT;
            return -errno;
        }
        if (r == 0) return -ECONNRESET; /* EOF */
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

int spatha_send(int fd, uint32_t op, uint32_t req_id,
                const void *payload, uint32_t len) {
    if (len > SPATHA_MAX_PAYLOAD) return -EMSGSIZE;
    if (len && !payload) return -EINVAL;

    struct spatha_msg h = { SPATHA_MAGIC, op, len, req_id };
    int rc = spatha_write_all(fd, &h, sizeof(h));
    if (rc == 0 && len)
        rc = spatha_write_all(fd, payload, len);
    return rc;
}

int spatha_recv(int fd, struct spatha_msg *hdr, void *payload, uint32_t cap) {
    int rc = spatha_read_all(fd, hdr, sizeof(*hdr));
    if (rc) return rc;
    if (hdr->magic != SPATHA_MAGIC || hdr->len > SPATHA_MAX_PAYLOAD)
        return -EPROTO;
    if (hdr->len > cap)
        return -EMSGSIZE; /* stream desincronizado: el caller debe cerrar */
    if (hdr->len)
        rc = spatha_read_all(fd, payload, hdr->len);
    return rc;
}

// Interactive single-shot client.
//
// Sends one command (built from argv) and prints the typed response.
// Wire format (matches src/server.cpp):
//   request:  [u32 total_len][u32 nstr] (repeat: [u32 slen][bytes])
//   response: [u32 total_len][typed payload]
// Tags:
//   0=NIL  1=ERR(code:i32, len:u32, msg)  2=STR(len:u32, bytes)
//   3=INT(i64)  4=DBL(f64)  5=ARR(len:u32, then len typed elements)

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <string>
#include <vector>


static void msg(const char *m) { fprintf(stderr, "%s\n", m); }
static void die(const char *m) { fprintf(stderr, "[%d] %s\n", errno, m); abort(); }

static int32_t read_full(int fd, char *buf, size_t n) {
    while (n > 0) {
        ssize_t rv = read(fd, buf, n);
        if (rv <= 0) return -1;
        assert((size_t)rv <= n);
        n -= (size_t)rv;
        buf += rv;
    }
    return 0;
}

static int32_t write_all(int fd, const char *buf, size_t n) {
    while (n > 0) {
        ssize_t rv = write(fd, buf, n);
        if (rv <= 0) return -1;
        assert((size_t)rv <= n);
        n -= (size_t)rv;
        buf += rv;
    }
    return 0;
}

// Match server-side cap so `keys` against a populated DB doesn't get rejected.
const size_t k_max_msg = 32u << 20;

static int32_t send_req(int fd, const std::vector<std::string> &cmd) {
    uint32_t len = 4;
    for (const auto &s : cmd) len += 4 + s.size();
    if (len > k_max_msg) return -1;

    std::vector<char> wbuf(4 + len);
    memcpy(&wbuf[0], &len, 4);
    uint32_t n = (uint32_t)cmd.size();
    memcpy(&wbuf[4], &n, 4);
    size_t cur = 8;
    for (const auto &s : cmd) {
        uint32_t p = (uint32_t)s.size();
        memcpy(&wbuf[cur], &p, 4);
        memcpy(&wbuf[cur + 4], s.data(), s.size());
        cur += 4 + s.size();
    }
    return write_all(fd, wbuf.data(), 4 + len);
}

enum {
    TAG_NIL = 0, TAG_ERR = 1, TAG_STR = 2, TAG_INT = 3, TAG_DBL = 4, TAG_ARR = 5,
};

static int32_t print_response(const uint8_t *data, size_t size) {
    if (size < 1) { msg("bad response"); return -1; }
    switch (data[0]) {
    case TAG_NIL:
        printf("(nil)\n");
        return 1;
    case TAG_ERR: {
        if (size < 1 + 8) { msg("bad response"); return -1; }
        int32_t code = 0; uint32_t len = 0;
        memcpy(&code, &data[1], 4);
        memcpy(&len, &data[1 + 4], 4);
        if (size < 1 + 8 + len) { msg("bad response"); return -1; }
        printf("(err) %d %.*s\n", code, len, &data[1 + 8]);
        return 1 + 8 + len;
    }
    case TAG_STR: {
        if (size < 1 + 4) { msg("bad response"); return -1; }
        uint32_t len = 0;
        memcpy(&len, &data[1], 4);
        if (size < 1 + 4 + len) { msg("bad response"); return -1; }
        printf("(str) %.*s\n", len, &data[1 + 4]);
        return 1 + 4 + len;
    }
    case TAG_INT: {
        if (size < 1 + 8) { msg("bad response"); return -1; }
        int64_t val = 0;
        memcpy(&val, &data[1], 8);
        printf("(int) %ld\n", val);
        return 1 + 8;
    }
    case TAG_DBL: {
        if (size < 1 + 8) { msg("bad response"); return -1; }
        double val = 0;
        memcpy(&val, &data[1], 8);
        printf("(dbl) %g\n", val);
        return 1 + 8;
    }
    case TAG_ARR: {
        if (size < 1 + 4) { msg("bad response"); return -1; }
        uint32_t len = 0;
        memcpy(&len, &data[1], 4);
        printf("(arr) len=%u\n", len);
        size_t arr_bytes = 1 + 4;
        for (uint32_t i = 0; i < len; ++i) {
            int32_t rv = print_response(&data[arr_bytes], size - arr_bytes);
            if (rv < 0) return rv;
            arr_bytes += (size_t)rv;
        }
        printf("(arr) end\n");
        return (int32_t)arr_bytes;
    }
    default:
        msg("bad response");
        return -1;
    }
}

static int32_t read_res(int fd) {
    char hdr[4];
    errno = 0;
    if (read_full(fd, hdr, 4)) {
        if (errno == 0) msg("EOF"); else msg("read() error");
        return -1;
    }
    uint32_t len = 0;
    memcpy(&len, hdr, 4);
    if (len > k_max_msg) { msg("too long"); return -1; }

    std::vector<char> rbuf(len);
    if (read_full(fd, rbuf.data(), len)) { msg("read() error"); return -1; }

    int32_t rv = print_response((uint8_t *)rbuf.data(), len);
    if (rv > 0 && (uint32_t)rv != len) { msg("bad response"); rv = -1; }
    return rv;
}

int main(int argc, char **argv) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) die("socket()");

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = ntohs(1234);
    addr.sin_addr.s_addr = ntohl(INADDR_LOOPBACK);
    if (connect(fd, (const struct sockaddr *)&addr, sizeof(addr))) die("connect");

    std::vector<std::string> cmd;
    for (int i = 1; i < argc; ++i) cmd.push_back(argv[i]);
    if (send_req(fd, cmd) == 0) read_res(fd);

    close(fd);
    return 0;
}

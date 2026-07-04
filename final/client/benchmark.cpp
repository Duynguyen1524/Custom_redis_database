// Benchmark for the toy redis server.
//
// Measures:
//   - throughput (ops/sec) per command type (SET, GET, DEL)
//   - latency distribution (p50/p90/p99/max) per command type, in microseconds
//
// Knobs (all optional; have sensible defaults):
//   -h <host>        server host         (default 127.0.0.1)
//   -p <port>        server port         (default 1234)
//   -c <conns>       concurrent conns    (default 50)
//   -n <ops>         total ops per phase (default 100000)
//   -P <pipeline>    requests in flight per conn (default 16)
//   -d <bytes>       value size for SET  (default 16)
//   -k <prefix>      key prefix          (default "k")
//
// Each connection is driven by its own thread. Keys are partitioned across
// threads (each thread "owns" key indices [tid*chunk, (tid+1)*chunk)) so SET,
// GET, and DEL phases all hit existing keys deterministically without any
// coordination between threads. This is intentional: it isolates the server's
// per-op cost from any client-side contention.
//
// Latency is measured by timestamping every Pth request we *send* and every
// reply we *finish parsing*, taking their difference. Sampling (rather than
// every request) keeps the timestamp arrays bounded and avoids skewing the
// timing of the hot path.

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <algorithm>
#include <atomic>
#include <string>
#include <vector>


// ----- wire helpers (matches server) ----------------------------------------

enum {
    TAG_NIL = 0, TAG_ERR = 1, TAG_STR = 2, TAG_INT = 3, TAG_DBL = 4, TAG_ARR = 5,
};

static int32_t read_full(int fd, char *buf, size_t n) {
    while (n > 0) {
        ssize_t rv = read(fd, buf, n);
        if (rv <= 0) return -1;
        n -= (size_t)rv;
        buf += rv;
    }
    return 0;
}

static int32_t write_all(int fd, const char *buf, size_t n) {
    while (n > 0) {
        ssize_t rv = write(fd, buf, n);
        if (rv <= 0) return -1;
        n -= (size_t)rv;
        buf += rv;
    }
    return 0;
}

// Append-encode one request into `out`.
static void encode_req(std::vector<char> &out, const std::vector<std::string> &cmd) {
    uint32_t len = 4;
    for (const auto &s : cmd) len += 4 + s.size();

    size_t base = out.size();
    out.resize(base + 4 + len);
    char *p = out.data() + base;

    memcpy(p, &len, 4); p += 4;
    uint32_t n = (uint32_t)cmd.size();
    memcpy(p, &n, 4); p += 4;
    for (const auto &s : cmd) {
        uint32_t sl = (uint32_t)s.size();
        memcpy(p, &sl, 4); p += 4;
        memcpy(p, s.data(), s.size()); p += s.size();
    }
}

// Skip a single typed value at data[0..size). Returns bytes consumed, -1 on err.
static int32_t skip_value(const uint8_t *data, size_t size) {
    if (size < 1) return -1;
    switch (data[0]) {
    case TAG_NIL: return 1;
    case TAG_INT: case TAG_DBL:
        return (size < 1 + 8) ? -1 : 1 + 8;
    case TAG_STR: {
        if (size < 5) return -1;
        uint32_t l = 0; memcpy(&l, data + 1, 4);
        return (size < 5 + l) ? -1 : (int32_t)(5 + l);
    }
    case TAG_ERR: {
        if (size < 9) return -1;
        uint32_t l = 0; memcpy(&l, data + 5, 4);
        return (size < 9 + l) ? -1 : (int32_t)(9 + l);
    }
    case TAG_ARR: {
        if (size < 5) return -1;
        uint32_t n = 0; memcpy(&n, data + 1, 4);
        size_t off = 5;
        for (uint32_t i = 0; i < n; ++i) {
            int32_t r = skip_value(data + off, size - off);
            if (r < 0) return -1;
            off += (size_t)r;
        }
        return (int32_t)off;
    }
    default: return -1;
    }
}

// Read one full response (header + body). Returns 0 ok, -1 err.
static int32_t read_one_response(int fd, std::vector<char> &buf) {
    char hdr[4];
    if (read_full(fd, hdr, 4)) return -1;
    uint32_t len = 0; memcpy(&len, hdr, 4);
    buf.resize(len);
    if (len && read_full(fd, buf.data(), len)) return -1;
    int32_t consumed = skip_value((const uint8_t *)buf.data(), len);
    if (consumed < 0 || (uint32_t)consumed != len) return -1;
    return 0;
}

// ----- timing ----------------------------------------------------------------

static uint64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// ----- per-thread driver -----------------------------------------------------

struct Args {
    const char *host = "127.0.0.1";
    int port = 1234;
    int conns = 50;
    int ops_per_phase = 100000;
    int pipeline = 16;
    int value_size = 16;
    std::string key_prefix = "k";
};

enum Phase { PHASE_SET, PHASE_GET, PHASE_DEL };
static const char *phase_name(Phase p) {
    return p == PHASE_SET ? "SET" : p == PHASE_GET ? "GET" : "DEL";
}

struct ThreadCtx {
    int tid = 0;
    const Args *args = nullptr;
    Phase phase;
    int start_idx = 0;
    int n_ops = 0;
    std::vector<uint64_t> latencies_ns;  // one sample per fully-drained pipeline window
    uint64_t total_ns = 0;
};

static int connect_to(const char *host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        struct hostent *he = gethostbyname(host);
        if (!he) { close(fd); return -1; }
        memcpy(&addr.sin_addr, he->h_addr, he->h_length);
    }
    if (connect(fd, (const struct sockaddr *)&addr, sizeof(addr))) {
        close(fd); return -1;
    }
    return fd;
}

static void *thread_main(void *p) {
    ThreadCtx *ctx = (ThreadCtx *)p;
    const Args &A = *ctx->args;

    int fd = connect_to(A.host, A.port);
    if (fd < 0) { fprintf(stderr, "connect failed for tid=%d\n", ctx->tid); return nullptr; }

    std::string value(A.value_size, 'x');
    std::vector<char> wbuf, rbuf;
    wbuf.reserve(64 * 1024);

    uint64_t t_start = now_ns();
    int sent = 0, recv = 0;
    uint64_t window_start_ns = 0;
    bool window_open = false;

    while (recv < ctx->n_ops) {
        int target_inflight = std::min(A.pipeline, ctx->n_ops - recv);
        if (sent - recv < target_inflight) {
            wbuf.clear();
            int batch = target_inflight - (sent - recv);
            if (!window_open) {
                window_start_ns = now_ns();
                window_open = true;
            }
            for (int i = 0; i < batch; ++i) {
                int idx = ctx->start_idx + sent + i;
                std::string key = A.key_prefix + std::to_string(idx);
                std::vector<std::string> cmd;
                if (ctx->phase == PHASE_SET)      cmd = {"set", key, value};
                else if (ctx->phase == PHASE_GET) cmd = {"get", key};
                else                              cmd = {"del", key};
                encode_req(wbuf, cmd);
            }
            if (write_all(fd, wbuf.data(), wbuf.size())) {
                fprintf(stderr, "write failed tid=%d\n", ctx->tid);
                break;
            }
            sent += batch;
        }
        if (read_one_response(fd, rbuf)) {
            fprintf(stderr, "read failed tid=%d\n", ctx->tid);
            break;
        }
        recv++;
        if (window_open && (sent - recv) == 0) {
            ctx->latencies_ns.push_back(now_ns() - window_start_ns);
            window_open = false;
        }
    }
    ctx->total_ns = now_ns() - t_start;
    close(fd);
    return nullptr;
}

// ----- stats -----------------------------------------------------------------

static uint64_t pct(std::vector<uint64_t> &v, double q) {
    if (v.empty()) return 0;
    size_t i = (size_t)(q * (v.size() - 1));
    std::nth_element(v.begin(), v.begin() + i, v.end());
    return v[i];
}

struct PhaseResult {
    const char *name;
    int total_ops;
    double seconds;
    double ops_per_sec;
    uint64_t lat_p50_us, lat_p90_us, lat_p99_us, lat_max_us;
};

static PhaseResult run_phase(const Args &A, Phase phase) {
    int chunk = A.ops_per_phase / A.conns;
    int total = chunk * A.conns;

    std::vector<ThreadCtx> ctxs(A.conns);
    std::vector<pthread_t> tids(A.conns);

    uint64_t t0 = now_ns();
    for (int i = 0; i < A.conns; ++i) {
        ctxs[i].tid = i;
        ctxs[i].args = &A;
        ctxs[i].phase = phase;
        ctxs[i].start_idx = i * chunk;
        ctxs[i].n_ops = chunk;
        pthread_create(&tids[i], nullptr, thread_main, &ctxs[i]);
    }
    for (int i = 0; i < A.conns; ++i) pthread_join(tids[i], nullptr);
    uint64_t t1 = now_ns();

    std::vector<uint64_t> all_lat_us;
    for (auto &c : ctxs)
        for (auto v : c.latencies_ns)
            // a window of P pipelined ops took v ns; per-op latency = v/P
            all_lat_us.push_back((v / (uint64_t)A.pipeline) / 1000);

    PhaseResult r;
    r.name = phase_name(phase);
    r.total_ops = total;
    r.seconds = (t1 - t0) / 1e9;
    r.ops_per_sec = total / r.seconds;
    r.lat_p50_us = pct(all_lat_us, 0.50);
    r.lat_p90_us = pct(all_lat_us, 0.90);
    r.lat_p99_us = pct(all_lat_us, 0.99);
    r.lat_max_us = all_lat_us.empty() ? 0 : *std::max_element(all_lat_us.begin(), all_lat_us.end());
    return r;
}

// ----- main ------------------------------------------------------------------

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [-h host] [-p port] [-c conns] [-n ops] [-P pipeline] [-d value_size] [-k key_prefix]\n"
        "Defaults: host=127.0.0.1 port=1234 conns=50 ops=100000 pipeline=16 value_size=16 prefix=k\n",
        prog);
}

int main(int argc, char **argv) {
    Args A;
    int opt;
    while ((opt = getopt(argc, argv, "h:p:c:n:P:d:k:H")) != -1) {
        switch (opt) {
        case 'h': A.host = optarg; break;
        case 'p': A.port = atoi(optarg); break;
        case 'c': A.conns = atoi(optarg); break;
        case 'n': A.ops_per_phase = atoi(optarg); break;
        case 'P': A.pipeline = atoi(optarg); break;
        case 'd': A.value_size = atoi(optarg); break;
        case 'k': A.key_prefix = optarg; break;
        case 'H': default: usage(argv[0]); return 1;
        }
    }
    if (A.conns <= 0 || A.ops_per_phase <= 0 || A.pipeline <= 0) {
        usage(argv[0]); return 1;
    }
    if (A.ops_per_phase % A.conns != 0) {
        A.ops_per_phase = (A.ops_per_phase / A.conns) * A.conns;
    }

    fprintf(stderr,
        "config: host=%s port=%d conns=%d ops/phase=%d pipeline=%d value=%dB prefix=%s\n",
        A.host, A.port, A.conns, A.ops_per_phase, A.pipeline, A.value_size, A.key_prefix.c_str());

    PhaseResult rs = run_phase(A, PHASE_SET);
    PhaseResult rg = run_phase(A, PHASE_GET);
    PhaseResult rd = run_phase(A, PHASE_DEL);

    fprintf(stderr,
        "\n%-4s  %10s  %10s  %10s  %8s  %8s  %8s  %8s\n",
        "op", "ops", "seconds", "ops/sec", "p50_us", "p90_us", "p99_us", "max_us");
    for (auto *r : {&rs, &rg, &rd}) {
        fprintf(stderr, "%-4s  %10d  %10.3f  %10.0f  %8lu  %8lu  %8lu  %8lu\n",
            r->name, r->total_ops, r->seconds, r->ops_per_sec,
            (unsigned long)r->lat_p50_us, (unsigned long)r->lat_p90_us,
            (unsigned long)r->lat_p99_us, (unsigned long)r->lat_max_us);
    }

    // CSV on stdout, easy to redirect into a file
    printf("op,ops,seconds,ops_per_sec,p50_us,p90_us,p99_us,max_us\n");
    for (auto *r : {&rs, &rg, &rd}) {
        printf("%s,%d,%.6f,%.2f,%lu,%lu,%lu,%lu\n",
            r->name, r->total_ops, r->seconds, r->ops_per_sec,
            (unsigned long)r->lat_p50_us, (unsigned long)r->lat_p90_us,
            (unsigned long)r->lat_p99_us, (unsigned long)r->lat_max_us);
    }
    return 0;
}

// Fast FIO-compatible zipf trace generator for piping into cache_sim.
// Replicates FIO's zipf_next() algorithm (Gray, Gonzalez, Grafe method).
//
// Usage: ./gen_fio_zipf [options] | ./cache_sim /dev/stdin <cache_size> --trace_format csv ...
//
// Options:
//   --theta <f>        Zipf exponent (default: 0.9)
//   --size_gib <n>     Address space in GiB (default: 3300)
//   --io_size_gib <n>  IO amount per job in GiB (default: 3584)
//   --jobs <n>         Number of jobs (default: 4)
//   --bs <n>           Block size in bytes (default: 4096)
//   --seed <n>         Base seed; jobs use seed, seed+1, ... (default: 12345)

#include <cstdio>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <random>
#include <algorithm>

// Approximate H(n, theta) = sum_{k=1}^{n} k^{-theta}
// Exact for first 10000 terms, then Euler-Maclaurin integral for the rest.
static double zeta_approx(uint64_t n, double theta) {
    double s = 0;
    uint64_t exact = std::min<uint64_t>(n, 10000);
    for (uint64_t i = 1; i <= exact; i++)
        s += pow((double)i, -theta);
    if (n > exact) {
        double omt = 1.0 - theta;
        s += (pow(n + 0.5, omt) - pow(exact + 0.5, omt)) / omt;
    }
    return s;
}

struct ZipfGen {
    uint64_t n;
    double theta, alpha, eta, zetan, zeta2_val;
    std::mt19937_64 rng;
    std::uniform_real_distribution<double> udist;
    uint64_t rand_off;

    ZipfGen(uint64_t nranges, double theta_, uint32_t seed)
        : n(nranges), theta(theta_), udist(0.0, 1.0), rng(seed)
    {
        rand_off = rng() % n;
        zeta2_val = zeta_approx(2, theta);
        zetan = zeta_approx(n, theta);
        alpha = 1.0 / (1.0 - theta);
        eta = (1.0 - pow(2.0 / n, 1.0 - theta)) / (1.0 - zeta2_val / zetan);
        fprintf(stderr, "ZipfGen: n=%lu theta=%.2f zetan=%.4f alpha=%.2f eta=%.6f rand_off=%lu\n",
                n, theta, zetan, alpha, eta, rand_off);
    }

    inline uint64_t next() {
        double u = udist(rng);
        double uz = u * zetan;
        uint64_t val;
        if (uz < 1.0) {
            val = 1;
        } else if (uz < 1.0 + pow(0.5, theta)) {
            val = 2;
        } else {
            double base = eta * u - eta + 1.0;
            // pow(base, alpha) — alpha=10 for theta=0.9, optimize if integer
            double result;
            int ialpha = (int)alpha;
            if (fabs(alpha - ialpha) < 1e-9 && ialpha <= 20) {
                result = 1.0;
                double b = base;
                int e = ialpha;
                while (e > 0) {
                    if (e & 1) result *= b;
                    b *= b;
                    e >>= 1;
                }
            } else {
                result = pow(base, alpha);
            }
            val = 1 + (uint64_t)(n * result);
        }
        if (val < 1) val = 1;
        if (val > n) val = n;
        val--;
        return (val + rand_off) % n;
    }
};

// Fast uint64 to decimal string; returns pointer to start of string within buf.
static char* u64_to_str(uint64_t v, char* end) {
    if (v == 0) { *--end = '0'; return end; }
    while (v) { *--end = '0' + (v % 10); v /= 10; }
    return end;
}

int main(int argc, char* argv[]) {
    double theta = 0.9;
    uint64_t size_gib = 3300;
    uint64_t io_size_gib = 3584;
    int num_jobs = 4;
    int bs = 4096;
    uint32_t base_seed = 12345;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--theta") && i+1 < argc) theta = atof(argv[++i]);
        else if (!strcmp(argv[i], "--size_gib") && i+1 < argc) size_gib = atoll(argv[++i]);
        else if (!strcmp(argv[i], "--io_size_gib") && i+1 < argc) io_size_gib = atoll(argv[++i]);
        else if (!strcmp(argv[i], "--jobs") && i+1 < argc) num_jobs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bs") && i+1 < argc) bs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i+1 < argc) base_seed = atoi(argv[++i]);
        else { fprintf(stderr, "Unknown arg: %s\n", argv[i]); return 1; }
    }

    uint64_t num_blocks = size_gib * (1ULL << 30) / bs;
    uint64_t ios_per_job = io_size_gib * (1ULL << 30) / bs;
    uint64_t total_ios = ios_per_job * num_jobs;

    fprintf(stderr, "Generating: theta=%.2f size=%luGiB io_size=%luGiB/job bs=%d jobs=%d\n",
            theta, size_gib, io_size_gib, bs, num_jobs);
    fprintf(stderr, "  num_blocks=%lu ios_per_job=%lu total_ios=%lu\n",
            num_blocks, ios_per_job, total_ios);

    ZipfGen* gens[16];
    for (int j = 0; j < num_jobs; j++)
        gens[j] = new ZipfGen(num_blocks, theta, base_seed + j);

    // 4 MB output buffer for fast pipe throughput
    static char outbuf[4 * 1024 * 1024];
    setvbuf(stdout, outbuf, _IOFBF, sizeof(outbuf));

    // Working buffer for line construction
    static char wbuf[4 * 1024 * 1024];
    int wpos = 0;

    uint64_t written[16] = {};
    uint64_t ts = 0;
    const int BATCH = 10000;
    uint64_t progress_interval = total_ios / 100;
    uint64_t next_progress = progress_interval;

    char tmp[24];

    while (true) {
        bool done = true;
        for (int j = 0; j < num_jobs; j++) {
            if (written[j] >= ios_per_job) continue;
            done = false;
            int n = (int)std::min<uint64_t>(BATCH, ios_per_job - written[j]);
            for (int i = 0; i < n; i++) {
                uint64_t block = gens[j]->next();
                uint64_t offset = (uint64_t)block * bs;

                // Build line: "0,W,<offset>,4096,<ts>\n"
                memcpy(wbuf + wpos, "0,W,", 4); wpos += 4;

                char* p = u64_to_str(offset, tmp + 22);
                int len = (int)((tmp + 22) - p);
                memcpy(wbuf + wpos, p, len); wpos += len;

                memcpy(wbuf + wpos, ",4096,", 6); wpos += 6;

                p = u64_to_str(ts, tmp + 22);
                len = (int)((tmp + 22) - p);
                memcpy(wbuf + wpos, p, len); wpos += len;

                wbuf[wpos++] = '\n';
                ts++;

                if (wpos > (int)sizeof(wbuf) - 128) {
                    fwrite(wbuf, 1, wpos, stdout);
                    wpos = 0;
                }
            }
            written[j] += n;
        }
        if (done) break;

        if (ts >= next_progress) {
            fprintf(stderr, "\r  progress: %lu / %lu (%.1f%%)",
                    ts, total_ios, 100.0 * ts / total_ios);
            next_progress += progress_interval;
        }
    }

    if (wpos > 0) fwrite(wbuf, 1, wpos, stdout);
    fflush(stdout);

    fprintf(stderr, "\r  done: %lu IOs generated\n", ts);

    for (int j = 0; j < num_jobs; j++) delete gens[j];
    return 0;
}

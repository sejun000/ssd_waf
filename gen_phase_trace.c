// Phase-shift workload generator (csv4col: W,offset,size,ts)
// A(5TB) -> B(5TB) -> A -> B, 20TB total:
//   constant background: 10% of bytes, 256KiB sequential cycling over 200GB
//     (rewrite interval = 2TB host >> cache reach: pure-flush component)
//   Phase A main: 90% uniform 16KiB over 200GB  (1.25x cache 160GB)
//   Phase B main: 15% uniform 16KiB over 8GB (hot bait, always absorbable)
//              + 75% uniform 16KiB over 1TB   (beyond reach at any util)
// Unique set ~= 200+8+1000+200 GB = 1.408TB (fits cold 1.6TB, ~80-88% util)
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

static uint64_t rng_s = 88172645463325252ULL;
static inline uint64_t rnd(void) {
    rng_s ^= rng_s << 13; rng_s ^= rng_s >> 7; rng_s ^= rng_s << 17;
    return rng_s;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <out.csv> [total_TB=20] [phase_TB=5]\n", argv[0]); return 1; }
    const uint64_t TB = 1000000000000ULL;
    uint64_t total = (argc > 2 ? strtoull(argv[2], 0, 10) : 20) * TB;
    uint64_t phase = (argc > 3 ? strtoull(argv[3], 0, 10) : 5) * TB;

    const uint64_t WR = 16384, WS = 262144;          // random / seq write sizes
    const uint64_t A_BASE = 0,              A_SLOTS = 200000000000ULL / WR;
    const uint64_t H_BASE = 200000000000ULL, H_SLOTS = 8000000000ULL / WR;
    const uint64_t B_BASE = 208000000000ULL, B_SLOTS = 1000000000000ULL / WR;
    const uint64_t S_BASE = 1208000000000ULL, S_SLOTS = 200000000000ULL / WS;

    FILE *out = fopen(argv[1], "w");
    if (!out) { perror("open"); return 1; }
    setvbuf(out, malloc(1 << 24), _IOFBF, 1 << 24);

    uint64_t host = 0, seq_ptr = 0, rand_bytes = 0, ts = 1600000000000000ULL;
    uint64_t st_a = 0, st_h = 0, st_b = 0, st_s = 0, lines = 0;

    while (host < total) {
        uint64_t off, sz;
        if (rand_bytes >= 9 * WS) {                  // 1 seq write per 9x its bytes -> 10%
            off = S_BASE + seq_ptr * WS; sz = WS;
            seq_ptr = (seq_ptr + 1) % S_SLOTS;
            rand_bytes -= 9 * WS; st_s += sz;
        } else {
            int in_b = (host / phase) & 1;           // phase 0=A,1=B alternating
            uint64_t r = rnd();
            if (!in_b)            { off = A_BASE + (r % A_SLOTS) * WR; st_a += WR; }
            else if (r % 6 == 0)  { off = H_BASE + (rnd() % H_SLOTS) * WR; st_h += WR; }  // 15/90
            else                  { off = B_BASE + (rnd() % B_SLOTS) * WR; st_b += WR; }
            sz = WR; rand_bytes += WR;
        }
        fprintf(out, "W,%llu,%llu,%llu\n",
                (unsigned long long)off, (unsigned long long)sz, (unsigned long long)ts);
        host += sz; ts += 8; lines++;
    }
    fclose(out);
    fprintf(stderr, "lines=%llu total=%.3fTB A=%.3fTB hot=%.3fTB bulk=%.3fTB seq=%.3fTB\n",
            (unsigned long long)lines, host / 1e12, st_a / 1e12, st_h / 1e12, st_b / 1e12, st_s / 1e12);
    return 0;
}

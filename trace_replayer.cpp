/*
 * trace_replay_aio.cpp – replay write‑only traces with libaio and
 *                        periodically log NAND/host writes for WAF analysis.
 *
 * build:
 *   g++ -O2 -std=c++17 -laio -o trace_replay_aio trace_replay_aio.cpp trace_parser.cpp
 *
 * usage (root privileges required):
 *   sudo ./trace_replay_aio <trace file> <target disk file> <trace format(csv|blktrace)> \
 *                          <output csv file> <proc name> [queue_depth]
 *   e.g. sudo ./trace_replay_aio alibaba.csv /dev/nvme3n1 csv waf.output nvmev0 32
 *
 * Notes
 * -----
 * • Only W / WS events are issued exactly to the target device; *all data is
 *   discarded*.
 * • The CSV contains three columns with no header:
 *     1) cumulative logical bytes written by the replayer
 *     2) NAND‑level bytes  (cat /proc/<proc>/nand_write_count * 4096)
 *     3) host‑level bytes  (cat /proc/<proc>/write_count      * 4096)
 *   A new row is appended every 1 GiB of additional logical I/O.
 */

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <libaio.h>

#include <iostream>
#include <fstream>
#include <vector>
#include <string>

#include "trace_parser.h"

#define BLOCK_SIZE 4096
static_assert((BLOCK_SIZE & (BLOCK_SIZE - 1)) == 0, "BLOCK_SIZE must be power‑of‑2");

static inline off_t align_up(off_t v, off_t a) { return (v + a - 1) & ~(a - 1); }
static inline off_t align_down(off_t v, off_t a) { return v & ~(a - 1); }

static inline unsigned long long read_counter(const std::string &path)
{
    std::ifstream f(path);
    unsigned long long v = 0;
    if (f) f >> v;
    return v;
}

int main(int argc, char **argv)
{
    if (argc < 7) {
        fprintf(stderr,
                "Usage: %s <trace file> <target disk file> <trace format(csv|blktrace)> "
                "<output csv file> <proc name> [queue_depth]\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    /* ----------------------------------------------------------------- */
    const char *trace_file      = argv[1];
    const char *disk_file       = argv[2];
    const char *trace_format    = argv[3];
    const char *csv_file_path   = argv[4];
    const std::string proc_name = argv[5];
    unsigned qd                 = (argc >= 7 ? std::stoul(argv[6]) : 1);
    if (qd == 0 || qd > 4096) {
        fprintf(stderr, "queue_depth must be 1..4096\n");
        return EXIT_FAILURE;
    }

    std::ofstream csv_out(csv_file_path);
    if (!csv_out) { perror("open csv"); return EXIT_FAILURE; }

    /* ----------------------------------------------------------------- */
    ITraceParser *parser = createTraceParser(trace_format);
    std::ifstream infile(trace_file);
    if (!infile) { perror("open trace"); return EXIT_FAILURE; }

    /* target device + libaio context ---------------------------------- */
    int fd = open(disk_file, O_WRONLY | O_DIRECT);
    if (fd < 0) { perror("open target"); return EXIT_FAILURE; }

    io_context_t ctx{};
    if (io_setup(qd, &ctx) < 0) { perror("io_setup"); close(fd); return EXIT_FAILURE; }

    /* single zero‑pattern buffer reused for all I/O -------------------- */
    void *buf;
    constexpr size_t BUF_BYTES = 256ULL * 1024ULL * 1024ULL; /* 256 MiB */
    if (posix_memalign(&buf, BLOCK_SIZE, BUF_BYTES)) {
        perror("posix_memalign"); io_destroy(ctx); close(fd); return EXIT_FAILURE; }
    memset(buf, 0, BUF_BYTES);

    /* ----------------------------------------------------------------- */
    std::vector<struct iocb>      iocbs(qd);
    std::vector<struct iocb *>    iocb_ptrs(qd);
    std::vector<struct io_event>  evs(qd);

    size_t   inflight       = 0;
    uint64_t total_wbytes   = 0;   /* cumulative logical bytes written */
    uint64_t next_dump      = 1ULL << 30; /* 1 GiB */
    uint64_t line_cnt       = 0;
    std::string line;

    const uint64_t write_limit = 15000ULL * 1024ULL * 1024ULL * 1024ULL; /* 15000 GiB */
    uint64_t       write_bytes = 0;

    auto reap = [&](unsigned min) {
        if (!inflight) return;
        int nr = io_getevents(ctx, min, inflight, evs.data(), nullptr);
        if (nr < 0) { perror("io_getevents"); exit(EXIT_FAILURE); }
        inflight -= nr;
    };

    /* ------------------------------- replay main loop ---------------- */
    while (std::getline(infile, line)) {
        ParsedRow p = parser->parseTrace(line);
        ++line_cnt;
        if (p.dev_id.empty() || !(p.op_type == "W" || p.op_type == "WS"))
            continue;

        off_t pos  = align_down(p.lba_offset, BLOCK_SIZE);
        off_t end  = align_up(pos + p.lba_size, BLOCK_SIZE);
        size_t sz  = end - pos;
        if (write_bytes + sz > write_limit) break;
        write_bytes += sz;

        if (sz > BUF_BYTES) { fprintf(stderr, "I/O > BUF_BYTES not supported\n"); break; }
        /*
        struct iocb &iocb = iocbs[inflight];
        io_prep_pwrite(&iocb, fd, buf, sz, pos);
        iocb_ptrs[inflight] = &iocb;
        ++inflight;
        total_wbytes += sz;

        if (inflight == qd) reap(1);

        if (inflight && io_submit(ctx, 1, &iocb_ptrs[inflight - 1]) != 1) {
            perror("io_submit"); break; }
        */

        /* reserve a slot first */
        unsigned idx = inflight;                 // ① 고정

        struct iocb &iocb = iocbs[idx];
        io_prep_pwrite(&iocb, fd, buf, sz, pos);
        iocb_ptrs[idx] = &iocb;

        /* ② 먼저 제출 */
        if (io_submit(ctx, 1, &iocb_ptrs[idx]) != 1) {
            perror("io_submit");
            break;               // error: 루프 종료 또는 continue
        }

        /* ③ 성공한 뒤 inflight 증가 */
        ++inflight;

        /* ④ 깊이 가득 차면 event 수확 */
        if (inflight == qd)
            reap(1);

        /* ---- periodic WAF sampling every 1 GiB of logical writes ---- */
        if (write_bytes >= next_dump) {
            const std::string base = "/proc/" + proc_name + "/";
            //printf("WAF sampling: %s\n", base.c_str());
            unsigned long long nand_cnt  = read_counter(base + "nand_write_count");
            unsigned long long host_cnt  = read_counter(base + "write_count");
            //printf("nand_cnt=%llu, host_cnt=%llu\n", nand_cnt, host_cnt);
            csv_out << write_bytes << ','
                    << (nand_cnt * BLOCK_SIZE) << ','
                    << (host_cnt * BLOCK_SIZE) << '\n';
            csv_out.flush();
            next_dump += (1ULL << 30);
        }

        if (line_cnt % 1000000ULL == 0)
            printf("[%.2f GB written, lines=%lu]\n", write_bytes / 1e9, line_cnt);
    }

    /* wait for remaining I/Os ---------------------------------------- */
    reap(0);

    /* ----------------------------------------------------------------- */
    free(buf);
    io_destroy(ctx);
    close(fd);
    csv_out.close();

    printf("Done. lines=%lu, written=%.2f GB\n", line_cnt, total_wbytes / 1e9);
    return EXIT_SUCCESS;
}

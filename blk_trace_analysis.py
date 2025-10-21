import argparse
import subprocess
import trace_parser
from datetime import datetime
from concurrent.futures import ThreadPoolExecutor, as_completed  # ← 추가

def estimate_device_size(file_path, trace_format):
    """트레이스를 한 번 훑어서 유니크한 LBA 개수를 세어 디바이스 크기를 근사"""
    unique_lbas = set()
    
    with open(file_path, 'r') as f:
        max_value = 0
        line_count_limit = 10000000
        line_count = 0
        for row in f:
            if (line_count >= line_count_limit):
                break
            line_count += 1
            if (line_count % 10000 == 0):
                print ("line_count: ", line_count, " max_value: ", max_value)
            parsed_row = trace_parser.parse_trace(row, trace_format)
            #print ("parsed_row: ", parsed_row)
            if parsed_row is None:
                continue  # 유효하지 않은 데이터 건너뛰기
            dev_id, op_type, lba_offset, lba_size, timestamp = parsed_row
            max_value = max(max_value, int(lba_offset) + int(lba_size))
            #unique_lbas.add((dev_id, lba_offset))
    
    return max_value  # 블록 크기를 4KB로 가정하고 크기 근사

def run_cache_analysis(trace_file, device_size, rw_policy='all', trace_format='csv', cache_policy="LRU", valid_rate=''):
    """캐시 크기를 1%, 5%, 10%, 15%, 20%, 25%, 30%로 변경하며 실행"""
    #cache_ratios = [0.18]
    cache_ratios = [0.18]
    if (valid_rate == ''):
        valid_ratio_ranges = [0.8]
    else:
        front = [float(x) for x in valid_rate.split(',')][0]
        back = [float(x) for x in valid_rate.split(',')][1]
        valid_ratio_ranges = [float(x)/100.0 for x in range(int(front*100), int(back*100)+1, 1)]

    # ---- 병렬화: valid_ratio 단위로 20개 동시 실행 ----
    def _run_for_valid_ratio(valid_ratio):
        for ratio in cache_ratios:
            cache_size = int(device_size * ratio)
            input_cache_policy = cache_policy
            if (ratio == 0):
                input_cache_policy = "NO_CACHE"
            print(f"\nRunning analysis with cache size: {cache_size} bytes ({ratio*100:.5f}% trace_format {trace_format})")
            print (f"./cache_sim {trace_file} {cache_size} --rw_policy {rw_policy} --trace_format {trace_format} --cache_policy {input_cache_policy} --cache_trace /mnt/nvme2n2/{cache_policy}_{ratio}.trace --cold_trace /mnt/nvme2n2/{cache_policy}_{ratio}.cold.trace --cold_capacity {int(device_size * 1.07)} --waf_log_file {cache_policy}_{ratio}.waf.log")
            ts = datetime.now().strftime('%y%m%d_%H%M%S')
            waf_log_file = f"{cache_policy}_{ratio}_{ts}.waf.log"
            if (valid_rate != ''):
                subprocess.run([
                    "./cache_sim", trace_file, str(cache_size),
                    "--rw_policy", rw_policy,
                    "--trace_format", trace_format,
                    "--cache_policy", input_cache_policy,
                    "--cache_trace", "/mnt/nvme2n2/"+ cache_policy + "_" + str(ratio) + ".trace",
                    "--cold_trace", "/mnt/nvme2n2/"+ cache_policy + "_" + str(ratio) + ".cold.trace",
                    "--cold_capacity", str(int(device_size * 1.07)),
                    "--waf_log_file", waf_log_file,
                    "--valid_ratio", str(valid_ratio),
                    "--stat_log_file", "dp."+str(valid_ratio)
                ])
            else:
                subprocess.run([
                    "./cache_sim", trace_file, str(cache_size),
                    "--rw_policy", rw_policy,
                    "--trace_format", trace_format,
                    "--cache_policy", input_cache_policy,
                    "--cache_trace", "/mnt/nvme2n2/"+ cache_policy + "_" + str(ratio) + ".trace",
                    "--cold_trace", "/mnt/nvme2n2/"+ cache_policy + "_" + str(ratio) + ".cold.trace",
                    "--cold_capacity", str(int(device_size * 1.07)),
                    "--waf_log_file", waf_log_file
                ])

    with ThreadPoolExecutor(max_workers=20) as ex:
        futures = [ex.submit(_run_for_valid_ratio, vr) for vr in valid_ratio_ranges]
        for _ in as_completed(futures):
            pass
    # -----------------------------------------------

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Automate Block Cache Analysis for Different Cache Sizes")
    parser.add_argument("trace_file", type=str, help="Path to the block trace file")
    
    parser.add_argument("--rw_policy", type=str, choices=['all', 'write-only', 'read-only'], default='all', help="Cache policy: all (default) or write-only")
    parser.add_argument("--cache_policy", type=str, default='all', help="Cache policy: all (default) or write-only")
    parser.add_argument("--trace_format", type=str, choices=['csv', 'blktrace'], default='csv', help="Trace format: csv (default) or blktrace")
    parser.add_argument("--valid_rate", type=str, default='', help="Valid rate threshold for LOG_GREEDY_COST_BENEFIT_11 policy (two decimal number with comma)")
    #parser.add_argument("--device_size", type=int, default=3841362697216, help="Device size in bytes")
    parser.add_argument("--device_size", type=int, default=501861437440, help="Device size in bytes")
    args = parser.parse_args()
    estimated_device_size = args.device_size
    print(f"Estimated Device Size: {estimated_device_size} bytes")
    run_cache_analysis(args.trace_file, estimated_device_size, args.rw_policy, args.trace_format, args.cache_policy, args.valid_rate)

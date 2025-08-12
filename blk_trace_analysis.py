import argparse
import subprocess
import trace_parser
from datetime import datetime

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

def run_cache_analysis(trace_file, device_size, rw_policy='all', trace_format='csv', cache_policy="LRU"):
    """캐시 크기를 1%, 5%, 10%, 15%, 20%, 25%, 30%로 변경하며 실행"""
    #cache_ratios = [0.023, 0.047, 0.105, 0.176]
    #cache_ratios = [0.023255814,0.023809524,0.024390244,0.025,0.051282051,0.052631579,0.054054054,0.055555556,0.085714286,0.088235294,0.090909091,0.09375,0.129032258,0.133333333,0.137931034,0.142857143]
    #cache_ratios = [0.0116, 0.024, 0.038, 0.054] # paper
    #cache_ratios = [0.070, 0.090]
    cache_ratios = [0.023]
    #cache_ratios = [0.0116, 0.023, 0.038, 0.054, 0.070, 0.090] # paper
    #cache_ratios = [0.038, 0.054]
    #cache_ratios = [0, 0.054]
    #cache_ratios = [0.054]
    #cache_ratios = [0.075, 0.085, 0.095, 0.105]
    #cache_ratios = [0.05618, 0.075, 0.085, 0.095, 0.105]
    #cache_ratios = [0.105]
    #cache_ratios = [0.05618, 0.075, 0.085, 0.095, 0.105]
    #cache_ratios = [0.0123, 0.0164, 0.0186, 0.021, 0.023]
    #cache_ratios = [0.105]
    #cache_ratios = [0.01642, 0.01862, 0.023]
    
    for ratio in cache_ratios:
        cache_size = int(device_size * ratio)
        input_cache_policy = cache_policy
        if (ratio == 0):
            input_cache_policy = "NO_CACHE"
        print(f"\nRunning analysis with cache size: {cache_size} bytes ({ratio*100:.5f}% trace_format {trace_format})")
        # print command line 
        #print("["./cache_sim", trace_file, str(cache_size), "--rw_policy", rw_policy, "--trace_format", trace_format, "--cache_policy", input_cache_policy, "--cache_trace", "/mnt/nvme2n2/"+ cache_policy + "_" + str(ratio) + ".trace", \
        #                "--cold_trace", "/mnt/nvme2n2/"+ cache_policy + "_" + str(ratio) + ".cold.trace", "--cold_capacity", str(int(device_size * 1.07)), "--waf_log_file", waf_log_file]")
        print (f"./cache_sim {trace_file} {cache_size} --rw_policy {rw_policy} --trace_format {trace_format} --cache_policy {input_cache_policy} --cache_trace /mnt/nvme2n2/{cache_policy}_{ratio}.trace --cold_trace /mnt/nvme2n2/{cache_policy}_{ratio}.cold.trace --cold_capacity {int(device_size * 1.07)} --waf_log_file {cache_policy}_{ratio}.waf.log")
        # run command line
        ts = datetime.now().strftime('%y%m%d_%H%M%S')
        waf_log_file = f"{cache_policy}_{ratio}_{ts}.waf.log"
        
        subprocess.run(["./cache_sim", trace_file, str(cache_size), "--rw_policy", rw_policy, "--trace_format", trace_format, "--cache_policy", input_cache_policy, "--cache_trace", "/mnt/nvme2n2/"+ cache_policy + "_" + str(ratio) + ".trace", \
                        "--cold_trace", "/mnt/nvme2n2/"+ cache_policy + "_" + str(ratio) + ".cold.trace", "--cold_capacity", str(int(device_size * 1.07)), "--waf_log_file", waf_log_file])

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Automate Block Cache Analysis for Different Cache Sizes")
    parser.add_argument("trace_file", type=str, help="Path to the block trace file")
    
    parser.add_argument("--rw_policy", type=str, choices=['all', 'write-only', 'read-only'], default='all', help="Cache policy: all (default) or write-only")
    parser.add_argument("--cache_policy", type=str, default='all', help="Cache policy: all (default) or write-only")
    parser.add_argument("--trace_format", type=str, choices=['csv', 'blktrace'], default='csv', help="Trace format: csv (default) or blktrace")
    #parser.add_argument("--device_size", type=int, default=130599218053120, help="Device size in bytes")
    #parser.add_argument("--device_size", type=int, default=17491254312960, help="Device size in bytes")
    #parser.add_argument("--device_size", type=int, default=21083994456064, help="Device size in bytes")
    #parser.add_argument("--device_size", type=int, default=21083994456064, help="Device size in bytes")
    #parser.add_argument("--device_size", type=int, default=512_000_000_000, help="Device size in bytes")
    #parser.add_argument("--device_size", type=int, default=8_377_333_710_848, help="Device size in bytes")
    #parser.add_argument("--device_size", type=int, default=5003636899840, help="Device size in bytes")
    #parser.add_argument("--device_size", type=int, default=137010594691, help="Device size in bytes")
    parser.add_argument("--device_size", type=int, default=3841362697216, help="Device size in bytes")
    args = parser.parse_args()
    #estimated_device_size = estimate_device_size(args.trace_file, args.trace_format)
    estimated_device_size = args.device_size
    print(f"Estimated Device Size: {estimated_device_size} bytes")
    run_cache_analysis(args.trace_file, estimated_device_size, args.rw_policy, args.trace_format, args.cache_policy)


import argparse

import trace_parser
from collections import OrderedDict
def lru_cache_policy(cache, lba_offset, lba_size, cache_size, block_size):
    """LRU 정책: 새로운 요청이 들어오면 블록 단위로 캐시를 관리하며 오래된 항목을 제거"""
    new_blocks = set(range(lba_offset // block_size, (lba_offset + lba_size) // block_size + 1))
    while len(cache) + len(new_blocks) > cache_size // block_size:
        
        cache.popitem(last=False)  # 가장 오래된 항목 제거
    
    for block in new_blocks:
        #print (block)
        cache[block] = True  # 새로운 블록 추가

def check_cache_hit(cache, lba_offset, lba_size, block_size):
    """요청된 LBA 범위와 캐시 내 범위를 비교하여 부분 hit/miss 확인"""
    request_blocks = set(range(lba_offset // block_size, (lba_offset + lba_size) // block_size + 1))
    hit_blocks = request_blocks.intersection(cache.keys())
    
    # Hit된 블록을 최신으로 갱신 (LRU 업데이트)
    hit_bytes = 0
    for block in hit_blocks:
        left_offset = max(block * block_size, lba_offset)
        right_offset = min((block + 1) * block_size, lba_offset + lba_size)
        hit_bytes += right_offset - left_offset
        cache.move_to_end(block)
    return hit_bytes
    #return len(hit_blocks) * block_size

def calc_hit_ratio(read_hit_size, total_read_size, write_hit_size, total_write_size):
    """Read/Write Cache Hit Ratio 계산"""
    read_hit_ratio = (read_hit_size / total_read_size) * 100 if total_read_size > 0 else 0
    write_hit_ratio = (write_hit_size / total_write_size) * 100 if total_write_size > 0 else 0
    return read_hit_ratio, write_hit_ratio


def analyze_block_trace(file_path, cache_size, block_size, policy='all', trace_format='csv'):
    """블록 트레이스 파일을 분석하여 cache hit ratio 계산"""
    cache = OrderedDict()  # LRU 구현을 위해 OrderedDict 사용
    total_read, read_hits, total_read_size, read_hit_size = 0, 0, 0, 0
    total_write, write_hits, total_write_size, write_hit_size = 0, 0, 0, 0
    
    with open(file_path, 'r') as f:
        line_count_limit = 10000000
        line_count = 0
        for line in f:
            if line_count >= line_count_limit:
                break
            line_count += 1
            if line_count % 10000 == 0:
                print(f"line_count: {line_count}")
                read_hit_ratio, write_hit_ratio = calc_hit_ratio(read_hit_size, total_read_size, write_hit_size, total_write_size)
                print(f"Intermediate Read Cache Hit Ratio: {read_hit_ratio:.2f}%")
                print(f"Intermediate Write Cache Hit Ratio: {write_hit_ratio:.2f}%")
                print(f"cache_len = {len(cache)} max_cache_size = {cache_size // block_size}")
            
            parsed_row = trace_parser.parse_trace(line, trace_format)
            if parsed_row is None:
                continue  # 유효하지 않은 데이터 건너뛰기
            
            dev_id, op_type, lba_offset, lba_size, timestamp = parsed_row
            
            hit_size = check_cache_hit(cache, lba_offset, lba_size, block_size, op_type)
            #print (op_type)
            if op_type == 'R' or op_type == 'RS':
                total_read += 1
                total_read_size += lba_size
                read_hit_size += hit_size
                hit_status = 'HIT' if hit_size > 0 else 'MISS'
                
                if policy == 'all':  # 모든 접근을 캐시에 올림
                    lru_cache_policy(cache, lba_offset, lba_size, cache_size, block_size)
                #print(f"{timestamp}, READ, {lba_offset}-{lba_offset+lba_size}, {hit_status}, HIT_SIZE={hit_size}")
                
            elif op_type == 'W' or op_type == 'WS':
                total_write += 1
                total_write_size += lba_size
                write_hit_size += hit_size
                hit_status = 'HIT' if hit_size > 0 else 'MISS'
                if policy in ('all', 'write-only'):
                    lru_cache_policy(cache, lba_offset, lba_size, cache_size, block_size)
                #print(f"{timestamp}, WRITE, {lba_offset}-{lba_offset+lba_size}, {hit_status}, HIT_SIZE={hit_size}")
                
    read_hit_ratio, write_hit_ratio = calc_hit_ratio(read_hit_size, total_read_size, write_hit_size, total_write_size)
    print("\nFinal Cache Hit Ratios:")
    print(f"Read Cache Hit Ratio: {read_hit_ratio:.2f}%")
    print(f"Write Cache Hit Ratio: {write_hit_ratio:.2f}%")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Block Cache Analysis")
    parser.add_argument("trace_file", type=str, help="Path to the block trace file")
    parser.add_argument("cache_size", type=int, help="Cache size in bytes")
    parser.add_argument("--block_size", type=int, default=65536, help="block size in bytes")
    #parser.add_argument("--policy", type=str, choices=['all', 'write-only'], default='all', help="Cache policy: all (default) or write-only")
    parser.add_argument("--trace_format", type=str, choices=['csv', 'blktrace'], default='csv', help="Trace format: csv (default) or blktrace")
    
    args = parser.parse_args()
    analyze_block_trace(args.trace_file, args.cache_size, args.block_size, 'all', args.trace_format)
    analyze_block_trace(args.trace_file, args.cache_size, args.block_size, 'write-only', args.trace_format)
    analyze_block_trace(args.trace_file, args.cache_size, args.block_size, 'read-only', args.trace_format)

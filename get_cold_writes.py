#!/usr/bin/env python3
import argparse

def process_final(final_block, intermediate_blocks, th):
    """
    final_block: dict with keys "cache_write_size" and "cold_tier_write_size" from Final Stats
    intermediate_blocks: list of dicts from preceding Intermediate Stats blocks (순서대로 등장한 순서)
    th: threshold for cold_tier_write_size (정수)
    
    intermediate_blocks 중, cold_tier_write_size가 th를 최초로 초과한 블록을 specific로 간주.
    만약 specific를 찾지 못하면 None을 반환.
    """
    specific = None
    for block in intermediate_blocks:
        if "cache_write_size" in block and block["cache_write_size"] > th:
            specific = block
            break
    if specific is None:
        return None
    # 필요한 키가 존재하는지 확인
    if "cache_write_size" not in final_block or "cache_write_size" not in specific:
        return None
    num = final_block["cold_tier_write_size"] - specific["cold_tier_write_size"]
    den = final_block["cache_write_size"] - specific["cache_write_size"]

    print (final_block["cold_tier_write_size"], final_block["cache_write_size"], specific["cold_tier_write_size"], specific["cache_write_size"])
    if den == 0:
        return None
    return num / den

def main():
    parser = argparse.ArgumentParser(description="Calculate cold_write_ratio for each Final Stats block based on a specific Intermediate Stats threshold.")
    parser.add_argument("--file", required=True, help="Path to the trace file")
    parser.add_argument("--th", type=int, required=True, help="Threshold for cold_tier_write_size to determine specific block")
    args = parser.parse_args()
    
    th = args.th

    # 상태 변수:
    # current_block: 현재 파싱 중인 블록의 값을 저장하는 dict
    # state: 현재 블록의 종류 ("Intermediate" 또는 "Final")
    # intermediate_blocks: 최근 사이클의 Intermediate Stats 블록들 (나타난 순서대로)
    state = None
    current_block = {}
    intermediate_blocks = []
    
    with open(args.file, 'r') as f:
        for line in f:
            line = line.strip()
            if not line:
                continue

            # 블록 구분 헤더 처리
            if line.startswith("Intermediate Stats"):
                # 만약 이전 블록이 Intermediate Stats였으면 current_block을 저장
                if state == "Intermediate" and current_block:
                    intermediate_blocks.append(current_block)
                elif state == "Final" and current_block:
                    # 만약 연속해서 Final Stats가 나오면, 처리 후 초기화
                    ratio = process_final(current_block, intermediate_blocks, th)
                    if ratio is not None:
                        print(f"cold_write_ratio: {ratio:.4f}")
                    else:
                        print("cold_write_ratio: N/A (specific intermediate not found or denominator zero)")
                    intermediate_blocks = []  # Final 후에는 다음 사이클을 위해 초기화
                state = "Intermediate"
                current_block = {}
                continue

            if line.startswith("Final Stats"):
                # Final Stats 시작 전에, 만약 바로 전에 Intermediate 블록이 있었다면 저장
                if state == "Intermediate" and current_block:
                    intermediate_blocks.append(current_block)
                elif state == "Final" and current_block:
                    # 연속된 Final Stats 처리 (드물지만)
                    ratio = process_final(current_block, intermediate_blocks, th)
                    if ratio is not None:
                        print(f"cold_write_ratio: {ratio:.4f}")
                    else:
                        print("cold_write_ratio: N/A (specific intermediate not found or denominator zero)")
                    intermediate_blocks = []
                state = "Final"
                current_block = {}
                continue

            # 값 파싱: 필요한 값은 "cache_write_size"와 "cold_tier_write_size"
            if "cache_write_size" in line:
                try:
                    value = int(line.split('=')[1].strip())
                    current_block["cache_write_size"] = value
                except Exception as e:
                    print("Error parsing cache_write_size:", e)
            elif "cold_tier_write_size" in line:
                try:
                    value = int(line.split('=')[1].strip())
                    current_block["cold_tier_write_size"] = value
                except Exception as e:
                    print("Error parsing cold_tier_write_size:", e)
    
    # 파일 끝에 도달했을 때, 만약 마지막 블록이 Final Stats면 처리
    if state == "Final" and current_block:
        ratio = process_final(current_block, intermediate_blocks, th)
        if ratio is not None:
            print(f"cold_write_ratio: {ratio:.4f}")
        else:
            print("cold_write_ratio: N/A (specific intermediate not found or denominator zero)")

if __name__ == "__main__":
    main()

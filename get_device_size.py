import argparse

def get_total_device_size(index_list, filename):
    # 파일을 읽어 인덱스별 device size를 딕셔너리로 생성
    size_map = {}
    with open(filename, 'r') as f:
        for line in f:
            idx_str, size_str = line.strip().split(',')
            size_map[int(idx_str)] = int(size_str)
    
    # 입력받은 인덱스에 해당하는 device size의 합을 계산
    return sum(size_map[i] for i in index_list)

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="CSV 파일에서 인덱스별 device size의 총합을 계산합니다.")
    parser.add_argument("indices", help="쉼표로 구분된 인덱스 리스트 (예: 2,3,5,8,10)")
    parser.add_argument("filename", help="CSV 파일명 (예: devices.csv)")
    args = parser.parse_args()

    # 문자열로 전달된 인덱스 리스트를 정수 리스트로 변환
    index_list = list(map(int, args.indices.split(',')))
    total_size = get_total_device_size(index_list, args.filename)
    print(f"총 device size: {total_size}")

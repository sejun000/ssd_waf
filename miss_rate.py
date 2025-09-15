import pandas as pd
import matplotlib.pyplot as plt
import matplotlib.ticker as mticker
import io
import argparse
from matplotlib.ticker import MultipleLocator

# 1. C++ 프로그램의 CSV 출력을 여기에 붙여넣으세요.
# (위 C++ 예제 코드의 실행 결과 예시)

# 2. C++ 코드에 입력했던 전체 데이터셋 크기를 동일하게 입력하세요.
total_dataset_size = 10 # 블록 단위

# 3. 데이터 파싱 및 가공
try:

    # with argparse, you can also pass the CSV data and total dataset size as command line arguments
    parser = argparse.ArgumentParser(description='Process cache miss rate data.')
    parser.add_argument('--file', type=str, help='CSV file', required=True)
    parser.add_argument('--total_size', type=int, help='Total dataset size in blocks', required=True)
    args = parser.parse_args()
    csv_file = args.file
    with open(csv_file, 'r') as f:
        csv_output = f.read()
    total_dataset_size = args.total_size
    # 문자열 데이터를 파일처럼 읽어서 DataFrame으로 변환
    df = pd.read_csv(io.StringIO(csv_output))

    # 가로축 계산: 캐시 크기 / 전체 데이터셋 크기
    df['CacheSizeRatio'] = df['CacheSize(blocks)'] / total_dataset_size * 4096
    print(df)
    
    # 세로축: Miss Rate (이미 비율로 계산되어 있음)
    df['MissRateRatio'] = df['MissRate(%)'] / 100.0

    # 4. 그래프 생성
    plt.style.use('seaborn-v0_8-whitegrid')
    fig, ax = plt.subplots(figsize=(10, 6))

    ax.plot(df['CacheSizeRatio'], df['MissRateRatio'], marker='o', linestyle='-', markersize=6)

    # 5. 그래프 서식 설정
    ax.set_title('Cache Miss Curve by Dataset Size Ratio', fontsize=16)
    ax.set_xlabel('Cache Size Ratio (Cache Size / Total Dataset Size)', fontsize=12)
    ax.set_ylabel('Cache Miss Ratio', fontsize=12)

    # x축과 y축을 퍼센트(%) 형식으로 표시
    ax.xaxis.set_major_formatter(mticker.PercentFormatter(xmax=1.0))
    ax.yaxis.set_major_formatter(mticker.PercentFormatter(xmax=1.0))

    # 축 범위 설정 (0% ~ 100%)
    ax.xaxis.set_major_locator(MultipleLocator(0.1))
    ax.yaxis.set_major_locator(MultipleLocator(0.1))
    ax.set_xlim(0, 1)
    
    ax.set_ylim(0, 1)
    ax.grid(True, which='both', linestyle='--', linewidth=0.5)

    # 그래프 파일로 저장 및 출력
    plt.tight_layout()
    plt.savefig('mrc_graph.png', dpi=300)
    plt.show()
    
    print("그래프가 'mrc_graph.png' 파일로 성공적으로 저장되었습니다.")

except Exception as e:
    print(f"오류가 발생했습니다: {e}")
    print("입력된 CSV 데이터 형식을 확인해주세요. (예: CacheSize,MissRate)")
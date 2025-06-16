import csv

lba_unit = 512

def parse_trace(row, type):
    if type == 'blktrace':
        return parse_blktrace(row)
    else:
        return parse_csv_trace(row)


def parse_csv_trace(input_row):
    row = input_row.split(',')
    if len(row) < 5:
        return None
    return row[0], row[1], row[2], int(row[3]), row[4]  # (dev_id, op_type, lba_offset, lba_size, timestamp)

def parse_blktrace(input_row):
    row = input_row.split()
    if len(row) < 10:
        return None
    dev_id = row[0]  # 디바이스 ID    
    op_type = row[6]
    try:
        #op_type = 'R' if row[6] == 'R' else 'W'  # R 또는 W 판별
        lba_offset = int(row[7])  # LBA Offset
        lba_size = int(row[9])   # 블록 크기를 512B로 변환
        timestamp = row[3]  # Timestamp
    except:
        return None
    return dev_id, op_type, lba_offset * lba_unit, lba_size * lba_unit, timestamp

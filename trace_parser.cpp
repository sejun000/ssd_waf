#include "trace_parser.h"
#include <sstream>
#include <vector>
#include <cstdlib>

// lba 단위 상수 (blktrace의 경우에 사용)
static const long long lba_unit = 512;

// 문자열을 delimiter 기준으로 분리하는 헬퍼 함수
static std::vector<std::string> splitString(const std::string &str, char delimiter) {
    std::vector<std::string> tokens;
    std::istringstream iss(str);
    std::string token;
    while (std::getline(iss, token, delimiter)) {
        if (!token.empty()) {  // 빈 문자열이면 추가하지 않음
            tokens.push_back(token);
        }
    }
    return tokens;
}

// CSV 파서 구현
ParsedRow CsvTraceParser::parseTrace(const std::string &line) {
    ParsedRow result;
    // 콤마(,)로 분리
    std::vector<std::string> tokens = splitString(line, ',');
    if (tokens.size() < 5) {
        return ParsedRow();  // 빈 구조체 반환 (dev_id가 빈 문자열이면 파싱 실패로 간주)
    }
    result.dev_id = tokens[0];
    result.op_type = tokens[1];
    try {
        // CSV에서는 lba_offset은 문자열 그대로 int 변환, lba_size는 int, timestamp는 double로 변환
        result.lba_offset = std::stoll(tokens[2]);
        result.lba_size = std::stoi(tokens[3]);
        result.timestamp = std::stod(tokens[4]);
    } catch (...) {
        return ParsedRow();
    }
    return result;
}

// blktrace 파서 구현
ParsedRow BlktraceParser::parseTrace(const std::string &line) {
    ParsedRow result;
    // 공백 문자로 분리
    std::vector<std::string> tokens = splitString(line, ' ');
    if (tokens.size() < 10) {
        return ParsedRow();
    }
    result.dev_id = tokens[0];  // 디바이스 ID
    // blktrace: only process Q (queue) events to avoid duplicates (Q→G→I→D→C)
    if (tokens[5] != "Q")
        return ParsedRow();
    result.op_type = tokens[6];
    try {
        long long lba_off = std::stoll(tokens[7]);
        int lba_sz = std::stoi(tokens[9]);
        result.lba_offset = lba_off * lba_unit;
        result.lba_size = lba_sz * lba_unit;
        result.timestamp = std::stod(tokens[3]);
    } catch (...) {
        return ParsedRow();
    }
    return result;
}

// Tencent 파서 구현: Timestamp,Offset(sectors),Size(sectors),IOType,VolumeID
ParsedRow TencentTraceParser::parseTrace(const std::string &line) {
    ParsedRow result;
    std::vector<std::string> tokens = splitString(line, ',');
    if (tokens.size() < 5) {
        return ParsedRow();
    }
    try {
        result.timestamp = std::stod(tokens[0]);
        long long offset_sectors = std::stoll(tokens[1]);
        long long size_sectors = std::stoll(tokens[2]);
        int io_type = std::stoi(tokens[3]);
        result.dev_id = tokens[4];
        // IOType: 1=Write, 0=Read
        result.op_type = (io_type == 1) ? "W" : "R";
        result.lba_offset = offset_sectors * 512;
        result.lba_size = size_sectors * 512;
    } catch (...) {
        return ParsedRow();
    }
    return result;
}

// Factory 함수: 타입에 따라 적절한 파서 객체 생성
ITraceParser* createTraceParser(const std::string &type) {
    if (type == "blktrace") {
        printf("BlktraceParser\n");
        return new BlktraceParser();
    } else if (type == "tencent") {
        printf("TencentTraceParser\n");
        return new TencentTraceParser();
    } else {
        return new CsvTraceParser();
    }
}

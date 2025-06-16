#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <cstdlib>   // for exit()

// 1일(하루)을 µs(마이크로초)로 환산
static const long long DAY_IN_US = 86400000000LL;

// 1백만 라인마다 출력
static const long long LINES_PER_REPORT = 1000000LL;

/**
 * 문자열을 구분자(delimiter)로 split하여 vector<string>으로 반환
 */
std::vector<std::string> split(const std::string &line, char delimiter) {
    std::vector<std::string> result;
    std::stringstream ss(line);
    std::string token;
    while (std::getline(ss, token, delimiter)) {
        result.push_back(token);
    }
    return result;
}

/**
 * device_info.csv 로부터 device_id -> capacity(바이트) 정보를 읽어서 반환
 */
std::unordered_map<int, long long> loadDeviceInfo(const std::string &deviceInfoFile) {
    std::unordered_map<int, long long> capacities;
    std::ifstream ifs(deviceInfoFile);
    if (!ifs.is_open()) {
        std::cerr << "Error: cannot open device info file: " << deviceInfoFile << std::endl;
        std::exit(1);
    }

    std::string line;
    while (std::getline(ifs, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        // 예: "0,536870912000"
        auto tokens = split(line, ',');
        if (tokens.size() < 2) {
            continue; // 형식 불량 라인 무시
        }
        int devId = std::stoi(tokens[0]);
        long long capacity = std::stoll(tokens[1]);
        capacities[devId] = capacity;
    }

    ifs.close();
    return capacities;
}

/**
 * 모든 SSD에 대한 (devId, DWPD)를 계산하여, devId 오름차순 정렬 후 반환.
 *  - duration_days = (last_ts - start_ts) / DAY_IN_US
 *  - DWPD = (totalWrites[devId] / capacity) / duration_days
 */
std::vector<std::pair<int,double>> calcAllDWPD(
    const std::unordered_map<int, long long> &totalWrites,
    const std::unordered_map<int, long long> &capacities,
    long long start_ts, long long last_ts)
{
    long long duration_us = last_ts - start_ts;
    if (duration_us <= 0) {
        duration_us = 1;
    }
    double duration_days = static_cast<double>(duration_us) / static_cast<double>(DAY_IN_US);

    std::vector<std::pair<int,double>> results;
    results.reserve(capacities.size());

    for (auto &kv : capacities) {
        int devId = kv.first;
        long long cap = kv.second;
        if (cap <= 0) {
            // 용량 0 이하 -> 계산 불가 -> 스킵
            continue;
        }
        long long writtenBytes = 0;
        auto it = totalWrites.find(devId);
        if (it != totalWrites.end()) {
            writtenBytes = it->second;
        }
        double dwpd = (static_cast<double>(writtenBytes) / (double)cap) / duration_days;
        results.push_back({devId, dwpd});
    }
    // SSD ID 오름차순 정렬
    std::sort(results.begin(), results.end(),
              [](auto &a, auto &b){ return a.first < b.first; });
    return results;
}

/**
 * DWPD와 함께, 4가지 구간별 그룹핑도 함께 출력.
 *  - 0.1 미만, 0.1~0.33, 0.33~1, 1 이상
 *  - 매번(중간 + 최종) 동일한 포맷으로 출력
 */
void printDWPDandGroups(long long lineCount,
                        const std::unordered_map<int, long long> &totalWrites,
                        const std::unordered_map<int, long long> &capacities,
                        long long start_ts, long long last_ts)
{
    // (1) 헤더
    std::cout << "\n=== DWPD (" << lineCount << " lines) ===" << std::endl;

    // (2) SSD별 (devId, DWPD)
    auto dwpdList = calcAllDWPD(totalWrites, capacities, start_ts, last_ts);

    // 2-1) SSD별 DWPD 목록
    for (auto &p : dwpdList) {
        int devId = p.first;
        double dwpdVal = p.second;
        std::cout << "SSD " << devId << " DWPD = " << dwpdVal << std::endl;
    }
    std::cout << "----------------------------------" << std::endl;

    // (3) 그룹핑
    std::vector<std::pair<int,double>> groupA; // < 0.1
    std::vector<std::pair<int,double>> groupB; // [0.1 ~ 0.33)
    std::vector<std::pair<int,double>> groupC; // [0.33 ~ 1)
    std::vector<std::pair<int,double>> groupD; // [1 ~ ]

    for (auto &p : dwpdList) {
        double val = p.second;
        if (val < 0.1) {
            groupA.push_back(p);
        } else if (val < 0.33) {  // 0.1 <= val < 0.33
            groupB.push_back(p);
        } else if (val < 1.0) {   // 0.33 <= val < 1
            groupC.push_back(p);
        } else {
            groupD.push_back(p);  // 1 이상
        }
    }

    // 그룹 출력 함수
    auto printGroup = [&](const std::string &groupName,
                          const std::vector<std::pair<int,double>> &group) {
        std::cout << groupName << " (" << group.size() << "개)" << std::endl;
        for (auto &pp : group) {
            std::cout << "  SSD " << pp.first << " => " << pp.second << std::endl;
        }
    };

    std::cout << "\n--- DWPD 그룹핑 결과 ---" << std::endl;
    printGroup("  (<0.1)", groupA);
    printGroup("  (0.1 ~ 0.33)", groupB);
    printGroup("  (0.33 ~ 1)", groupC);
    printGroup("  (>=1)", groupD);

    std::cout << "----------------------------------" << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <device_info.csv> <trace_file.csv>" << std::endl;
        return 1;
    }

    std::string deviceInfoFile = argv[1];
    std::string traceFile = argv[2];

    // (1) 디바이스 정보 로드
    std::unordered_map<int, long long> capacities = loadDeviceInfo(deviceInfoFile);

    // (2) 트레이스 파일 열기
    std::ifstream ifs(traceFile);
    if (!ifs.is_open()) {
        std::cerr << "Error: cannot open trace file: " << traceFile << std::endl;
        return 1;
    }

    // 디바이스별 누적 쓰기량
    std::unordered_map<int, long long> totalWrites;

    long long lineCount = 0;
    long long start_ts = -1;
    long long last_ts = -1;

    std::string line;
    while (std::getline(ifs, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        auto tokens = split(line, ',');
        if (tokens.size() < 5) {
            // 형식 오류 -> 스킵
            continue;
        }

        int devId = std::stoi(tokens[0]);      // device id
        std::string rwFlag = tokens[1];        // R or W
        // long long offset = std::stoll(tokens[2]); // 필요시 사용
        long long size = std::stoll(tokens[3]);
        long long ts   = std::stoll(tokens[4]);

        if (start_ts < 0) {
            start_ts = ts;
        }
        last_ts = ts;

        // 쓰기면 누적
        if (rwFlag == "W") {
            totalWrites[devId] += size;
        }

        lineCount++;

        // (3) 매 1백만 줄마다 출력 (4가지 구간 그룹핑 포함)
        if (lineCount % LINES_PER_REPORT == 0) {
            printDWPDandGroups(lineCount, totalWrites, capacities, start_ts, last_ts);
        }
    }
    ifs.close();

    // (4) 마지막에 최종 출력 (여기도 동일하게 그룹핑 포함)
    if (lineCount > 0) {
        printDWPDandGroups(lineCount, totalWrites, capacities, start_ts, last_ts);
    } else {
        std::cout << "No valid lines processed." << std::endl;
    }

    return 0;
}

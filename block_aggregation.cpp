#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <cstdlib>
#include <map>

/**
 * 간단한 split 함수: 주어진 문자열(line)을 delimiter로 잘라서 vector<string>으로 반환
 */
std::vector<std::string> split(const std::string &line, char delimiter) {
    std::vector<std::string> result;
    std::stringstream ss(line);
    std::string token;
    while (std::getline(ss, token, delimiter)) {
        if (!token.empty()) {
            result.push_back(token);
        }
    }
    return result;
}

/**
 * device_info.csv에서 device_id -> capacity를 읽어온다.
 *  예) "0,536870912000"
 */
std::unordered_map<int, long long> loadDeviceInfo(const std::string &filename) {
    std::unordered_map<int, long long> capacities;
    std::ifstream ifs(filename);
    if (!ifs.is_open()) {
        std::cerr << "Error: cannot open device info file: " << filename << std::endl;
        std::exit(1);
    }

    std::string line;
    while (std::getline(ifs, line)) {
        // 공백, 주석 등 무시
        if (line.empty() || line[0] == '#') {
            continue;
        }
        auto tokens = split(line, ',');
        if (tokens.size() < 2) {
            continue; // 잘못된 형식 무시
        }
        int devId = std::stoi(tokens[0]);
        long long capacity = std::stoll(tokens[1]);
        capacities[devId] = capacity;
    }
    ifs.close();
    return capacities;
}

int main(int argc, char* argv[])
{
    if (argc < 5) {
        std::cerr << "Usage: " << argv[0]
                  << " <device_info.csv> <trace_file.csv> <device_list> <output_trace_file.csv>\n"
                  << "  ex) " << argv[0] << " device_info.csv trace_file.csv \"0,3,5,6,9\" output_traced_file.csv\n";
        return 1;
    }

    std::string deviceInfoFile = argv[1];
    std::string traceFile = argv[2];
    std::string deviceListStr = argv[3];  // 예: "0,3,5,6,9"
    std::string outputTraceFile = argv[4];

    // 1) device_info.csv에서 (devID -> capacity) 맵 읽기
    auto capacities = loadDeviceInfo(deviceInfoFile);
    // 2) 사용자가 지정한 device ID 리스트 파싱
    //    예: "0,3,5,6,9" -> [0,3,5,6,9]
    std::vector<int> chosenDevs;
    {
        auto devTokens = split(deviceListStr, ',');
        for (auto &dstr : devTokens) {
            chosenDevs.push_back(std::stoi(dstr));
        }
    }

    // 3) 지정된 순서대로 prefix sum 계산 (device별 시작 LBA)
    //    예) prefixMap[0] = 0
    //         prefixMap[3] = capacity(0)
    //         prefixMap[5] = capacity(0)+capacity(3)
    //         ...
    long long prefix = 0;
    std::unordered_map<int, long long> prefixMap;
    for (auto dev : chosenDevs) {
        if (capacities.find(dev) == capacities.end()) {
            std::cerr << "Warning: device " << dev
                      << " not found in device_info.csv. Capacity=0 assumed.\n";
            prefixMap[dev] = prefix; // 또는 스킵
        } else {
            prefixMap[dev] = prefix;
            prefix += capacities[dev];
        }
    }
    printf ("Selected Capcity : %lld\n", prefix);
    //return 0;
    // 4) trace_file.csv 를 열고, 해당 device들에 대한 레코드만 골라서
    //    오프셋을 prefixMap[devID]만큼 shift하여 출력
    std::ifstream ifs(traceFile);
    if (!ifs.is_open()) {
        std::cerr << "Error: cannot open trace file: " << traceFile << std::endl;
        return 1;
    }

    // 결과를 표준출력으로 보낸다.
    // 포맷: "0,R,NewOffset,Size,Timestamp"
    // 여기서 device ID는 "0" (또는 원하는 ID)로 통일
    const std::string unifiedDevId = "0"; // 마치 하나의 디바이스처럼 보이게

    std::string line;
    long long baseTimestamp = 0; // 첫 행의 timestamp
    bool firstLine = true;

    std::ios::sync_with_stdio(false); 
    std::cin.tie(nullptr);  

    std::ofstream ofs;                       // ① 파일 스트림
    std::ostream* out = &std::cout;          // ② 기본은 stdout
    ofs.open(outputTraceFile, std::ios::out | std::ios::trunc);
    if (!ofs.is_open()) {
        std::cerr << "Error: cannot open output file: "
                  << outputTraceFile << '\n';
        return 1;
    }
    out = &ofs;
    
    while (std::getline(ifs, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        auto tokens = split(line, ',');
        if (tokens.size() < 5) {
            // 잘못된 형식 -> 스킵
            continue;
        }

        int devId = std::stoi(tokens[0]);
        std::string rw = tokens[1];      // R or W
        long long offset = std::stoll(tokens[2]);
        long long size   = std::stoll(tokens[3]);
        long long ts     = std::stoll(tokens[4]);
        if(firstLine) {
            baseTimestamp = ts;
            firstLine = false;
        }
        // 첫 행의 timestamp로부터 10일을 초과하면 중단 (정렬되어 있다고 가정)
      //  if (ts - baseTimestamp > 86400 * 1000ULL * 1000ULL) {
       //     break;
       // }
        // 해당 devId가 사용자 지정 리스트에 있는지 확인
        if (prefixMap.find(devId) == prefixMap.end()) {
            // 포함되지 않은 device -> 스킵
            continue;
        }

        // offset을 prefixMap[devId]만큼 shift
        long long newOffset = offset + prefixMap[devId];

        // 새 로우 출력 (device ID 통합 = 0)
        // 형식: "0,R,newOffset,size,timestamp"
        *out << unifiedDevId << ','
            << rw           << ','
            << newOffset    << ','
            << size         << ','
            << ts           << '\n';
    }

    ifs.close();
    ofs.close();
    return 0;
}
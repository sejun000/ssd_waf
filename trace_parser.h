#ifndef TRACE_PARSER_H
#define TRACE_PARSER_H

#include <string>
#include "cache_sim.h"  // ParsedRow 구조체 선언 포함

// 추상 인터페이스: Trace Parser
class ITraceParser {
public:
    virtual ParsedRow parseTrace(const std::string &line) = 0;
    virtual ~ITraceParser() {}
};

// CSV 형식 트레이스 파서를 위한 구현 클래스
class CsvTraceParser : public ITraceParser {
public:
    ParsedRow parseTrace(const std::string &line) override;
};

// blktrace 형식 트레이스 파서를 위한 구현 클래스
class BlktraceParser : public ITraceParser {
public:
    ParsedRow parseTrace(const std::string &line) override;
};

// Factory 함수: 파서 타입("csv" 또는 "blktrace")에 따라 적절한 파서 객체를 생성
ITraceParser* createTraceParser(const std::string &type);

#endif // TRACE_PARSER_H

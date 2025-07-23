#pragma once
#include <list>

class Segment;

class EvictPolicy {
public:
    virtual ~EvictPolicy() = default;

    /* victim 선택 */
    virtual Segment* choose_segment() = 0;

    /* 세그먼트 추가 / 제거 */
    virtual void add(Segment* seg)       = 0;
    virtual void remove(Segment* seg)    = 0;

    /* valid_cnt 가 변했을 때 호출 */
    virtual void update(Segment* seg)    = 0;
};

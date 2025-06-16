#pragma once
#include <vector>
#include <stack>

class DummyAllocator {
private:
    size_t blockSize;
    size_t totalSize;
    size_t maxBlocks;
    std::vector<bool> allocated; // 각 블록이 할당되었는지 추적
    std::stack<size_t> freeList;

public:
    // 생성자: 전체 사이즈(byte 단위)를 입력받아 풀 초기화
    DummyAllocator(size_t totalSizeInBytes, size_t blockSizeInBytes);
            // alloc: 사용 가능한 64K 블록의 id를 반환. 할당 실패 시 -1 반환
    size_t alloc();

    // free: id에 해당하는 블록을 해제. id가 할당되지 않았거나 범위를 벗어나면 false 반환
    bool free(size_t id);
};

#include <vector>
#include <cassert>
#include <stack>
#include <stdio.h>
#include "allocator.h"


DummyAllocator::DummyAllocator(size_t totalSizeInBytes, size_t blockSizeInBytes)
    : totalSize(totalSizeInBytes)
{
    // 전체 블록 개수 계산
    blockSize = blockSizeInBytes;
    maxBlocks = totalSize / blockSize;
    printf("maxBlocks = %ld %ld\n", totalSize, maxBlocks);
    // 블록 할당 상태 벡터 초기화
    allocated.resize(maxBlocks, false);

    // 초기에는 모든 블록이 비어있으므로,
    // 스택(freeList)에 id를 넣어 둠
    // (O(1) pop/push가 가능)
    for (size_t i = static_cast<size_t>(maxBlocks) - 1; i >= 0; --i) {
        freeList.push(i);
        if (i == 0) break;
    }
}

// alloc: 64K 블록 하나를 할당하고, 그 블록의 id 반환
// 할당 불가능하면 -1 반환
size_t DummyAllocator::alloc() {
    // 더 이상 할당할 블록이 없으면 -1 리턴
    if (freeList.empty()) {
        assert(false);
        return -1;
    }
    
    // freeList의 top에서 하나 꺼내서 할당 처리
    size_t id = freeList.top();
    freeList.pop();
    allocated[id] = true;
    return id;
}

// free: id에 해당하는 블록을 해제
// 유효하지 않은 id거나 이미 해제된 상태면 false 반환
bool DummyAllocator::free(size_t id) {
    // id 범위가 유효한지 확인
    if (id < 0 || static_cast<size_t>(id) >= maxBlocks) {
        printf("id is invalid %ld %ld\n", id, maxBlocks);
        assert(false);
        return false;
    }
    // 이미 해제되어 있다면 false 반환
    if (!allocated[id]) {
        printf("id is already freed\n");
        assert(false);
        return false;
    }

    // 해제 처리
    allocated[id] = false;
    freeList.push(id);
    return true;
}
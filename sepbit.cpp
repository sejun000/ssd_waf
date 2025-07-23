#include <iostream>
#include <cfloat>
#include "sepbit.h"
#include "fifo.h"
#include "metadata.h"

SepBIT::SepBIT() {
    mAvgLifespan = DBL_MAX;
    mClassNumOfLastCollectedSegment = 0;
    mLba2Fifo = new FIFO();
    mMetadata = new Metadata();
}

int SepBIT::Classify(uint64_t blockAddr, bool isGcAppend, uint64_t global_timestamp) {
  if (!isGcAppend) {
    uint64_t lifespan = mLba2Fifo->Query(blockAddr);
    if (lifespan != UINT64_MAX && lifespan < mAvgLifespan) {
      //printf("0 Classify: %lu, lifespan: %lu, avg lifespan: %f\n", blockAddr, lifespan, mAvgLifespan);
      return 0;
    } else {
      //printf("1 Classify: %lu, lifespan: %lu, avg lifespan: %f\n", blockAddr, lifespan, mAvgLifespan);
      return 1;
    }
  } else {
    if (mClassNumOfLastCollectedSegment == 0) {
      return 2;
    } else {
      uint64_t age = global_timestamp - mMetadata->Query(blockAddr);
      if (age < 4 * mAvgLifespan) {
        return 3;
      } else if (age < 16 * mAvgLifespan) {
        return 4;
      } else {
        return 5;
      }
    }
  }
}

void SepBIT::CollectSegment(Segment *segment, uint64_t global_timestamp) {
  static int totLifespan = 0;
  static int nCollects = 0;
  if (segment->get_class_num() == 0) {
    totLifespan += global_timestamp - segment->get_create_time();
    nCollects += 1;
  }
  if (nCollects == 16) {
    mAvgLifespan = 1.0 * totLifespan / nCollects;
    nCollects = 0;
    totLifespan = 0;
    //std::cout << "AvgLifespan: " << mAvgLifespan << std::endl;
  }

  mClassNumOfLastCollectedSegment = segment->get_class_num();
}

void SepBIT::Append(uint64_t blockAddr, uint64_t global_timestamp, void *arg) {
  uint64_t valid_blocks = reinterpret_cast<uint64_t>(arg);
  global_timestamp += 1;
  mLba2Fifo->Update(blockAddr, mAvgLifespan, valid_blocks);
  mMetadata->Update(blockAddr, global_timestamp);
}

void SepBIT::GcAppend(uint64_t blockAddr) {
}

#include <string>
#include <iostream>
#include "src/storage_adapter/storage_adapter.h"
#include "src/storage_adapter/local_adapter.h"
#include "src/storage_adapter/null_adapter.h"

class StorageAdapterFactory {
  public:
    static StorageAdapter *GetInstance(std::string type) {
      if (type == "Local") {
        return new LocalAdapter();
      } else if (type == "Null") {
        return new NullAdapter();
      } else {
        std::cerr << "Unknown StorageAdapter type: " << type << ", defaulting to Null" << std::endl;
      }
      return new NullAdapter();
    }
};

// MCLA GPU renderer -- a thread's time. See thread_load.h.

#include "thread_load.h"

namespace mcla::gpu {

namespace {
thread_local ThreadLoad t_load;
}

ThreadLoad& CurrentThreadLoad() {
  return t_load;
}

}  // namespace mcla::gpu

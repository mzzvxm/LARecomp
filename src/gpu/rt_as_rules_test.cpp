#include "rt_as_rules.h"

#include <cstdio>

namespace as = mcla::gpu::rt_as;

int main() {
  unsigned checks = 0;
  unsigned failures = 0;
  const auto check = [&](bool pass, const char* what) {
    ++checks;
    if (!pass) {
      ++failures;
      std::printf("FAIL %s\n", what);
    }
  };

  for (uint64_t reported = 1; reported <= 1536; ++reported) {
    const uint64_t rounded = ((reported + 255) / 256) * 256;
    check(as::CompactBytes(1024, reported) == (rounded < 1024 ? rounded : 0), "rounding and strict savings");
  }
  check(as::CompactBytes(1024, 0) == 0, "zero query result");
  check(as::CompactBytes(UINT64_MAX, UINT64_MAX - 7) == 0, "rounding overflow");
  check(as::CompactBytes(0, 256) == 0, "zero original");
  check(as::ReservationBytes(1) == 65536, "minimum allocation charge");
  check(as::ReservationBytes(65537) == 131072, "round allocation up");
  check(as::ReservationBytes(as::kCompactionHeadroom) == as::kCompactionHeadroom, "full cap fits");
  check(as::ReservationBytes(as::kCompactionHeadroom + 1) == 0, "oversize original");
  check(as::ReservationBytes(UINT64_MAX) == 0, "allocation overflow");
  check(!as::CanReserve(0, 0), "zero reservation");
  check(!as::CanReserve(UINT64_MAX, 1), "held byte overflow");
  check(as::CanReserve(as::kCompactionHeadroom - 65536, 512), "last slot fits");
  check(!as::CanReserve(as::kCompactionHeadroom - 65536 + 1, 512), "physical alignment counts against headroom");
  check(!as::CanReserve(as::kCompactionHeadroom, 512), "old BLAS still retiring");
  check(as::CanReserve(as::kCompactionHeadroom - 65536, 512), "submission retirement allows retry");
  check(!as::MatchesGeneration(0, 0), "empty generation");
  check(as::MatchesGeneration(41, 41), "same generation");
  check(!as::MatchesGeneration(41, 42), "reloaded sector or recycled range");

  as::CompactionMemory memory;
  memory.BeginCopy(65536, 256);
  memory.BeginCopy(65536, 512);
  check(memory.reserved == 131072 && memory.extra == 768, "two copies in flight");
  memory.Publish(1024, 256);
  check(memory.reserved == 131072 && memory.extra == 1536, "published original remains charged");
  memory.CancelCopy(65536, 512);
  check(memory.reserved == 65536 && memory.extra == 1024, "eviction discards settled copy");
  memory.RetireOriginal(65536, 1024);
  check(memory.reserved == 0 && memory.extra == 0, "last referencing frame releases the charge");
  memory.BeginCopy(65536, 512);
  memory.CancelCopy(65536, 512);
  check(memory.reserved == 0 && memory.extra == 0, "failed submission can retry after restart");

  using as::CopyResult;
  for (bool settled : {false, true}) {
    for (bool complete : {false, true}) {
      for (bool same : {false, true}) {
        for (bool enabled : {false, true}) {
          if (complete && !settled) continue;
          const auto expected = !settled ? CopyResult::kWait : !same ? CopyResult::kDrop :
                                complete && enabled ? CopyResult::kPublish : CopyResult::kRetry;
          check(as::FinishCopy(settled, complete, same, enabled) == expected, "copy readiness and cancellation matrix");
        }
      }
    }
  }

  struct Instance {
    uint32_t instanceID = 7, instanceMask = 0x80, instanceContributionToHitGroupIndex = 3;
    bool cullDisable = true;
    struct Transform { float m[3][4] = {{1, 0, 0, 10}, {0, 1, 0, 20}, {0, 0, 1, 30}}; } transform;
  };
  Instance instance;
  const as::InstanceKey city = as::KeyOf(instance, 9);
  instance.instanceID = 0;
  instance.instanceMask = 1;
  const as::InstanceKey car = as::KeyOf(instance, 9);
  const std::array reference{car, city};
  auto edited = reference;
  const auto reuse = [&] { return as::CanReuse(true, true, false, false, reference, edited); };
  check(reuse(), "same ordered descriptors and generations");
  check(!as::CanReuse(false, true, false, false, reference, edited), "A/B disabled");
  check(!as::CanReuse(true, false, false, false, reference, edited), "abandoned or in flight");
  check(!as::CanReuse(true, true, true, false, reference, edited), "build flags changed");
  check(!as::CanReuse(true, true, false, false, {}, {}), "empty scene");
  check(!as::CanReuse(true, true, false, false, reference, std::span(edited).first(1)), "instance removed");
  check(!as::CanReuse(true, true, false, false, std::span(reference).first(1), edited), "instance added");
  edited = {city, car};
  check(!reuse(), "instance order changed");
  edited = reference;
  ++edited[1].generation;
  check(!reuse(), "compact address published");
  edited = reference;
  ++edited[0].generation;
  check(!reuse(), "car allocation reused");
  edited = reference;
  edited[1].id ^= 1;
  check(!reuse(), "instance ID changed");
  edited = reference;
  edited[1].mask ^= 1;
  check(!reuse(), "mask changed");
  edited = reference;
  edited[1].hit_group ^= 1;
  check(!reuse(), "hit group changed");
  edited = reference;
  edited[1].cull_disabled = false;
  check(!reuse(), "cull mode changed");
  instance.transform.m[2][3] += 1;
  edited = {as::KeyOf(instance, 9), city};
  check(!reuse(), "camera transform changed");
  edited[0].generation = 0;
  check(!as::CanReuse(true, true, false, false, edited, edited), "missing generation");

  std::printf("%u checks, %u failures\n", checks, failures);
  return failures != 0;
}

#include "hitch_rules.h"
#include "pipeline_warm_order.h"

#include <cstdio>
#include <vector>

namespace {

int checks = 0;
int failures = 0;
#define CHECK(condition) do { \
  ++checks; \
  if (!(condition)) { \
    ++failures; \
    std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
  } \
} while (false)

void TestCadence() {
  using mcla::gpu::hitch::Cadence;
  Cadence cadence;
  CHECK(!cadence.Observe(0));
  CHECK(!cadence.Observe(50));
  CHECK(cadence.Observe(50.1));
  for (int i = 0; i < 6; ++i) CHECK(!cadence.Observe(10));
  CHECK(cadence.median_ms() == 10);
  CHECK(cadence.Observe(20.1));
  CHECK(!cadence.Observe(20));
  for (int i = 0; i < 64; ++i) cadence.Observe(30);
  CHECK(cadence.median_ms() == 30);
  CHECK(!cadence.Observe(50));
  CHECK(cadence.Observe(50.1));

  Cadence transition;
  for (int i = 0; i < 64; ++i) transition.Observe(10);
  for (int i = 0; i < 31; ++i) transition.Observe(20);
  CHECK(transition.median_ms() == 10);
  CHECK(!transition.Observe(20));
  CHECK(transition.median_ms() == 20);

  Cadence even;
  for (int i = 0; i < 4; ++i) {
    even.Observe(10);
    even.Observe(20);
  }
  CHECK(even.median_ms() == 15);
}

void TestRateLimit() {
  mcla::gpu::hitch::RateLimit limit;
  for (int i = 0; i < 8; ++i) CHECK(limit.Allow(1000));
  CHECK(!limit.Allow(1999));
  CHECK(!limit.Allow(1999));
  CHECK(limit.Allow(2000));
  CHECK(limit.TakeSuppressed() == 2);
  CHECK(limit.TakeSuppressed() == 0);
}

void TestPipelineWarmup() {
  using namespace mcla::gpu::pipeline_warm_order;
  constexpr auto refused = [] { return false; };
  constexpr auto allowed = [] { return true; };
  CHECK(!Learn(false, 500'000'000, refused));
  CHECK(!Learn(true, 29'999'999, refused));
  CHECK(Learn(true, 30'000'000, refused));
  CHECK(!Learn(true, 30'000'000, allowed));

  struct Record {
    size_t index;
    uint64_t ns;
  };
  std::vector<Record> records;
  for (size_t i = 0; i < 200; ++i) {
    records.push_back({i, i == 0 || i > 128 ? i + 100 : 0});
  }
  const auto cost = [](const Record& r) { return r.ns; };
  const auto order = Order(records, cost);
  CHECK(order.size() == records.size());
  for (size_t i = 0; i < 128; ++i) CHECK(order[i] == &records[i]);
  for (size_t i = 0; i < 64; ++i) CHECK(order[i + 128] == &records[199 - i]);
  for (size_t i = 192; i < 200; ++i) CHECK(order[i] == &records[i - 64]);
  CHECK(Order(std::vector<Record>{}, cost).empty());

  for (Record& r : records) r.ns = 0;
  records[129].ns = records[131].ns = 100;
  const auto tied = Order(records, cost);
  CHECK(tied[128] == &records[129]);
  CHECK(tied[129] == &records[131]);
  CHECK(tied[130] == &records[128]);
  CHECK(tied[131] == &records[130]);
}

}  // namespace

int main() {
  TestCadence();
  TestRateLimit();
  TestPipelineWarmup();
  std::printf("hitch_rules_test: %d checks, %d failures\n", checks, failures);
  return failures != 0;
}

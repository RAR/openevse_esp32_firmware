#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "../../src/lite/lite_random.h"

TEST_CASE("fills exactly len bytes via injected backend") {
  uint8_t buf[8] = {0};
  lite_random_set_backend([](uint8_t *b, size_t n){ for (size_t i=0;i<n;i++) b[i]=0xA5; });
  lite_random_bytes(buf, sizeof buf);
  for (uint8_t b : buf) CHECK(b == 0xA5);
}
TEST_CASE("zero length is a no-op and does not crash") {
  lite_random_set_backend([](uint8_t*, size_t){});
  lite_random_bytes(nullptr, 0);
  CHECK(true);
}

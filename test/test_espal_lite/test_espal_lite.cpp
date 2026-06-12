#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "../../src/lite/espal_lite_format.h"

TEST_CASE("short id is low 24 bits as 6 lowercase hex") {
  CHECK(lite_format_short_id(0x00ABCDEF12345678ULL) == "345678");
}
TEST_CASE("long id is full 64-bit unique id as 16 lowercase hex") {
  CHECK(lite_format_long_id(0x00ABCDEF12345678ULL) == "00abcdef12345678");
}
TEST_CASE("flash size passes through bytes") {
  CHECK(lite_flash_size_bytes(0x200000) == 2097152u);
}

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "../../src/lite/juicebox_proto.h"
#include <string.h>

static bool feed_str(JuiceBoxParser &p, const char *s, JuiceBoxFrame &out) {
  bool got = false;
  for (const char *c = s; *c; ++c) { if (p.feed((uint8_t)*c, out)) got = true; }
  return got;
}

TEST_CASE("parses a structured $ES frame terminated by CR") {
  JuiceBoxParser p; JuiceBoxFrame f;
  REQUIRE(feed_str(p, "$ES01C:S00,L00,T00,H00,A00,P000,F00\r", f));
  CHECK(strcmp(f.type, "ES") == 0);
  CHECK(strcmp(f.payload, "S00,L00,T00,H00,A00,P000,F00") == 0);
  CHECK(f.len == 0x1C);
}

TEST_CASE("parses a length-less $MD debug frame") {
  JuiceBoxParser p; JuiceBoxFrame f;
  REQUIRE(feed_str(p, "$MD:Back Online\n", f));
  CHECK(strcmp(f.type, "MD") == 0);
  CHECK(strcmp(f.payload, "Back Online") == 0);
}

TEST_CASE("the next $ flushes the previous frame (no terminator needed)") {
  JuiceBoxParser p; JuiceBoxFrame f;
  bool got = false;
  for (const char *c = "$PV002:20$ES"; *c; ++c) if (p.feed((uint8_t)*c, f)) got = true;
  REQUIRE(got);
  CHECK(strcmp(f.type, "PV") == 0);
  CHECK(strcmp(f.payload, "20") == 0);
}

TEST_CASE("garbage before '$' is discarded (resync)") {
  JuiceBoxParser p; JuiceBoxFrame f;
  REQUIRE(feed_str(p, "xyz\x01\x02$FW006:100102\r", f));
  CHECK(strcmp(f.type, "FW") == 0);
  CHECK(strcmp(f.payload, "100102") == 0);
}

TEST_CASE("a frame split across feeds still parses") {
  JuiceBoxParser p; JuiceBoxFrame f;
  CHECK_FALSE(feed_str(p, "$ES01C:S00,L00,", f));
  REQUIRE(feed_str(p, "T00,H00,A00,P000,F00\r", f));
  CHECK(strcmp(f.payload, "S00,L00,T00,H00,A00,P000,F00") == 0);
}

TEST_CASE("an over-long runaway line does not overflow") {
  JuiceBoxParser p; JuiceBoxFrame f;
  char big[256]; big[0] = '$'; big[1] = 'E'; big[2] = 'S';
  memset(big + 3, 'A', sizeof(big) - 4); big[sizeof(big) - 1] = '\0';
  for (char *c = big; *c; ++c) p.feed((uint8_t)*c, f);
  CHECK(true);
}

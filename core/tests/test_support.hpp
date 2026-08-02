#pragma once

#include <cstdio>
#include <cstring>

namespace wds::chart_editor::test {

inline int& failure_count() {
  static int count = 0;
  return count;
}

inline void check(bool condition, const char* expr, const char* file, int line) {
  if (!condition) {
    std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expr);
    ++failure_count();
  }
}

}  // namespace wds::chart_editor::test

#define CHECK(expr) ::wds::chart_editor::test::check((expr), #expr, __FILE__, __LINE__)

#define CHECK_EQ(a, b) \
  ::wds::chart_editor::test::check((a) == (b), #a " == " #b, __FILE__, __LINE__)

#define CHECK_NE(a, b) \
  ::wds::chart_editor::test::check((a) != (b), #a " != " #b, __FILE__, __LINE__)

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace wds::common {

// Marketing versions used by update checks. Only these two spellings parse:
//   a.b.c-stable
//   a.b.c-beta.d
// Legacy forms such as "1.0.0-beta1" or a bare "1.0.0" are rejected.
struct AppVersion {
  int major = 0;
  int minor = 0;
  int patch = 0;
  bool stable = false;
  int beta = 0;
};

std::optional<AppVersion> parse_app_version(std::string_view text);
int compare_app_version(const AppVersion& a, const AppVersion& b);
std::string format_app_version(const AppVersion& version);

}  // namespace wds::common

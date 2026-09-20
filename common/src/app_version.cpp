#include "wds/common/app_version.hpp"

#include <cctype>
#include <limits>
#include <string>

namespace wds::common {
namespace {

bool parse_nonneg_int(std::string_view& text, int& out) {
  if (text.empty() || !std::isdigit(static_cast<unsigned char>(text.front()))) return false;
  long long value = 0;
  std::size_t consumed = 0;
  while (consumed < text.size() && std::isdigit(static_cast<unsigned char>(text[consumed]))) {
    value = value * 10 + (text[consumed] - '0');
    if (value > std::numeric_limits<int>::max()) return false;
    ++consumed;
  }
  out = static_cast<int>(value);
  text.remove_prefix(consumed);
  return true;
}

bool consume(std::string_view& text, std::string_view token) {
  if (text.size() < token.size() || text.substr(0, token.size()) != token) return false;
  text.remove_prefix(token.size());
  return true;
}

}  // namespace

std::optional<AppVersion> parse_app_version(std::string_view text) {
  AppVersion version;
  if (!parse_nonneg_int(text, version.major) || !consume(text, ".")) return std::nullopt;
  if (!parse_nonneg_int(text, version.minor) || !consume(text, ".")) return std::nullopt;
  if (!parse_nonneg_int(text, version.patch) || !consume(text, "-")) return std::nullopt;
  if (consume(text, "stable")) {
    if (!text.empty()) return std::nullopt;
    version.stable = true;
    version.beta = 0;
    return version;
  }
  if (!consume(text, "beta.")) return std::nullopt;
  if (!parse_nonneg_int(text, version.beta) || !text.empty()) return std::nullopt;
  version.stable = false;
  return version;
}

int compare_app_version(const AppVersion& a, const AppVersion& b) {
  if (a.major != b.major) return a.major < b.major ? -1 : 1;
  if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
  if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
  if (a.stable != b.stable) return a.stable ? 1 : -1;
  if (a.beta != b.beta) return a.beta < b.beta ? -1 : 1;
  return 0;
}

std::string format_app_version(const AppVersion& version) {
  std::string text = std::to_string(version.major) + "." + std::to_string(version.minor) + "." +
                     std::to_string(version.patch);
  if (version.stable) {
    text += "-stable";
  } else {
    text += "-beta.";
    text += std::to_string(version.beta);
  }
  return text;
}

}  // namespace wds::common

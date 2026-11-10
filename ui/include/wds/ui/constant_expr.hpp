#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace wds::ui {

inline constexpr int kConstantExprMaxDecimals = 6;
// Chip labels count digits and the decimal point toward this budget; sign and
// trailing unit (e.g. "x") are extra.
inline constexpr int kSpeedLabelSlotBudget = 4;

// Four-function + parentheses constant expression. Shunting-yard + RPN stacks,
// no recursion. On success the value is rounded to kConstantExprMaxDecimals.
std::optional<double> eval_constant_expr(std::string_view text);

std::string format_trimmed_decimal(double value, int max_decimals = kConstantExprMaxDecimals);
std::string format_slot_budget_decimal(double value, int max_slots = kSpeedLabelSlotBudget);

}  // namespace wds::ui

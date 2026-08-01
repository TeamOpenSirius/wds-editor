#include <wds/core/chart_session.hpp>

namespace wds::chart_editor {

ChartSession::ChartSession() { new_empty(); }

ChartSlot& ChartSession::new_empty(MusicTiming timing, std::string path) {
  ChartSlot slot;
  slot.path = std::move(path);
  slot.document = ChartDocument(timing);
  charts_.push_back(std::move(slot));
  active_index_ = charts_.size() - 1;
  return charts_.back();
}

size_t ChartSession::add_chart(ChartDocument document, std::string path) {
  ChartSlot slot;
  slot.path = std::move(path);
  slot.document = std::move(document);
  charts_.push_back(std::move(slot));
  return charts_.size() - 1;
}

bool ChartSession::switch_chart(size_t index) {
  if (index >= charts_.size()) return false;
  active_index_ = index;
  return true;
}

ChartSlot* ChartSession::active() noexcept {
  return charts_.empty() ? nullptr : &charts_[active_index_];
}
const ChartSlot* ChartSession::active() const noexcept {
  return charts_.empty() ? nullptr : &charts_[active_index_];
}
ChartDocument* ChartSession::document() noexcept {
  ChartSlot* slot = active();
  return slot ? &slot->document : nullptr;
}
const ChartDocument* ChartSession::document() const noexcept {
  const ChartSlot* slot = active();
  return slot ? &slot->document : nullptr;
}
EditHistory* ChartSession::history() noexcept {
  ChartSlot* slot = active();
  return slot ? &slot->history : nullptr;
}
const EditHistory* ChartSession::history() const noexcept {
  const ChartSlot* slot = active();
  return slot ? &slot->history : nullptr;
}

}  // namespace wds::chart_editor

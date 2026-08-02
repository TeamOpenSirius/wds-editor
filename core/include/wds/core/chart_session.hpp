#pragma once

#include <wds/core/edit_history.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace wds::chart_editor {

struct ChartSlot {
  std::string path;
  ChartDocument document;
  EditHistory history;
};

class ChartSession {
 public:
  ChartSession();
  ChartSlot& new_empty(MusicTiming timing = {}, std::string path = {});
  size_t add_chart(ChartDocument document, std::string path = {});
  bool switch_chart(size_t index);
  size_t chart_count() const noexcept { return charts_.size(); }
  size_t active_index() const noexcept { return active_index_; }
  ChartSlot* active() noexcept;
  const ChartSlot* active() const noexcept;
  ChartDocument* document() noexcept;
  const ChartDocument* document() const noexcept;
  EditHistory* history() noexcept;
  const EditHistory* history() const noexcept;
  std::vector<ChartSlot>& charts() noexcept { return charts_; }
  const std::vector<ChartSlot>& charts() const noexcept { return charts_; }

 private:
  std::vector<ChartSlot> charts_;
  size_t active_index_ = 0;
};

}  // namespace wds::chart_editor

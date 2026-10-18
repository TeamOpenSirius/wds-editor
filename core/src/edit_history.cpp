#include <wds/core/edit_history.hpp>

#include <algorithm>

namespace wds::chart_editor {

CompositeCommand::CompositeCommand(std::string label) : label_(std::move(label)) {}
void CompositeCommand::add(std::unique_ptr<IEditCommand> command) {
  if (command) commands_.push_back(std::move(command));
}
bool CompositeCommand::execute(ChartDocument& doc) {
  size_t executed = 0;
  for (; executed < commands_.size(); ++executed) {
    if (!commands_[executed]->execute(doc)) {
      while (executed > 0) commands_[--executed]->undo(doc);
      return false;
    }
  }
  return true;
}
bool CompositeCommand::undo(ChartDocument& doc) {
  // Mirror execute(): on partial failure, re-apply already-undone children so the
  // document never stays half-rolled-back while EditHistory keeps the command.
  size_t undone = 0;
  for (; undone < commands_.size(); ++undone) {
    const size_t idx = commands_.size() - 1 - undone;
    if (!commands_[idx]->undo(doc)) {
      while (undone > 0) {
        --undone;
        const size_t redo_idx = commands_.size() - 1 - undone;
        commands_[redo_idx]->execute(doc);
      }
      return false;
    }
  }
  return true;
}
std::string CompositeCommand::label() const { return label_; }
size_t CompositeCommand::size_bytes() const {
  size_t total = sizeof(*this) + label_.capacity() +
                 commands_.capacity() * sizeof(std::unique_ptr<IEditCommand>);
  for (const auto& c : commands_) total += c->size_bytes();
  return total;
}

SetNotesCommand::SetNotesCommand(std::vector<NotationNote> before, std::vector<NotationNote> after,
                                 std::string label)
    : before_(std::move(before)), after_(std::move(after)), label_(std::move(label)) {}
bool SetNotesCommand::execute(ChartDocument& doc) { return doc.set_notes(after_); }
bool SetNotesCommand::undo(ChartDocument& doc) { return doc.set_notes(before_); }
std::string SetNotesCommand::label() const { return label_; }
size_t SetNotesCommand::size_bytes() const {
  return sizeof(*this) + (before_.capacity() + after_.capacity()) * sizeof(NotationNote) +
         label_.capacity();
}

SetTimingCommand::SetTimingCommand(MusicTiming before, MusicTiming after, std::string label)
    : before_(std::move(before)), after_(std::move(after)), label_(std::move(label)) {}
bool SetTimingCommand::execute(ChartDocument& doc) { return doc.set_timing(after_); }
bool SetTimingCommand::undo(ChartDocument& doc) { return doc.set_timing(before_); }
std::string SetTimingCommand::label() const { return label_; }
size_t SetTimingCommand::size_bytes() const {
  const auto heap = [](const MusicTiming& t) {
    return t.points.capacity() * sizeof(TimingPoint) + t.prefix_ms.capacity() * sizeof(double);
  };
  return sizeof(*this) + heap(before_) + heap(after_) + label_.capacity();
}

SetScrollSpeedPointsCommand::SetScrollSpeedPointsCommand(
    std::vector<ScrollSpeedPoint> before, std::vector<ScrollSpeedPoint> after, std::string label)
    : before_(std::move(before)), after_(std::move(after)), label_(std::move(label)) {}
bool SetScrollSpeedPointsCommand::execute(ChartDocument& doc) {
  return doc.set_scroll_speed_points(after_);
}
bool SetScrollSpeedPointsCommand::undo(ChartDocument& doc) {
  return doc.set_scroll_speed_points(before_);
}
std::string SetScrollSpeedPointsCommand::label() const { return label_; }
size_t SetScrollSpeedPointsCommand::size_bytes() const {
  return sizeof(*this) + (before_.capacity() + after_.capacity()) * sizeof(ScrollSpeedPoint) +
         label_.capacity();
}

SetCompatibilityProfileCommand::SetCompatibilityProfileCommand(
    ChartCompatibilityProfile before, ChartCompatibilityProfile after)
    : before_(before), after_(after) {}
bool SetCompatibilityProfileCommand::execute(ChartDocument& doc) {
  return doc.set_compatibility_profile(after_);
}
bool SetCompatibilityProfileCommand::undo(ChartDocument& doc) {
  return doc.set_compatibility_profile(before_);
}
std::string SetCompatibilityProfileCommand::label() const { return "Set compatibility profile"; }

AddNotesCommand::AddNotesCommand(std::vector<NotationNote> notes, std::string label)
    : notes_(std::move(notes)), label_(std::move(label)) {}
bool AddNotesCommand::execute(ChartDocument& doc) {
  // One bulk add (single sort / index rebuild). Ids are recorded back into notes_
  // so redo re-adds with the same ids; on failure the doc is left untouched.
  std::vector<int32_t> ids;
  const bool ok = doc.add_notes(notes_, &ids);
  for (size_t i = 0; i < ids.size() && i < notes_.size(); ++i) {
    notes_[i].id = ids[i];
  }
  return ok;
}
bool AddNotesCommand::undo(ChartDocument& doc) {
  std::vector<int32_t> ids;
  ids.reserve(notes_.size());
  for (const auto& note : notes_) {
    ids.push_back(note.id);
  }
  return doc.remove_notes(ids);
}
std::string AddNotesCommand::label() const { return label_; }
size_t AddNotesCommand::size_bytes() const {
  return sizeof(*this) + notes_.capacity() * sizeof(NotationNote) + label_.capacity();
}

RemoveNotesCommand::RemoveNotesCommand(std::vector<NotationNote> notes, std::string label)
    : notes_(std::move(notes)), label_(std::move(label)) {}
bool RemoveNotesCommand::execute(ChartDocument& doc) {
  std::vector<int32_t> ids;
  ids.reserve(notes_.size());
  for (const auto& note : notes_) {
    ids.push_back(note.id);
  }
  return doc.remove_notes(ids);
}
bool RemoveNotesCommand::undo(ChartDocument& doc) { return doc.add_notes(notes_); }
std::string RemoveNotesCommand::label() const { return label_; }
size_t RemoveNotesCommand::size_bytes() const {
  return sizeof(*this) + notes_.capacity() * sizeof(NotationNote) + label_.capacity();
}

UpdateNotesCommand::UpdateNotesCommand(std::unordered_map<int32_t, NotePair> changes,
                                       std::string label)
    : changes_(std::move(changes)), label_(std::move(label)) {}
bool UpdateNotesCommand::apply(ChartDocument& doc, bool after) {
  std::vector<NoteUpdate> updates;
  updates.reserve(changes_.size());
  for (const auto& [id, pair] : changes_) {
    updates.push_back({id, after ? pair.second : pair.first});
  }
  return doc.apply_note_updates(updates);
}
bool UpdateNotesCommand::execute(ChartDocument& doc) { return apply(doc, true); }
bool UpdateNotesCommand::undo(ChartDocument& doc) { return apply(doc, false); }
std::string UpdateNotesCommand::label() const { return label_; }
size_t UpdateNotesCommand::size_bytes() const {
  // Node = pair<const int32_t, NotePair> + next pointer / hash overhead.
  using Node = std::pair<const int32_t, NotePair>;
  return sizeof(*this) + changes_.size() * (sizeof(Node) + 2 * sizeof(void*)) +
         changes_.bucket_count() * sizeof(void*) + label_.capacity();
}

bool EditHistory::execute(std::unique_ptr<IEditCommand> cmd, ChartDocument& doc) {
  if (!cmd || !cmd->execute(doc)) return false;
  Entry entry;
  entry.bytes = cmd->size_bytes();
  entry.command = std::move(cmd);
  undo_.push_back(std::move(entry));
  redo_.clear();
  trim_undo();
  return true;
}
bool EditHistory::undo(ChartDocument& doc) {
  if (undo_.empty() || !undo_.back().command->undo(doc)) return false;
  redo_.push_back(std::move(undo_.back()));
  undo_.pop_back();
  return true;
}
bool EditHistory::redo(ChartDocument& doc) {
  if (redo_.empty() || !redo_.back().command->execute(doc)) return false;
  undo_.push_back(std::move(redo_.back()));
  redo_.pop_back();
  return true;
}
void EditHistory::clear() { undo_.clear(); redo_.clear(); }

size_t EditHistory::undo_bytes() const noexcept {
  size_t total = 0;
  for (const auto& e : undo_) total += e.bytes;
  return total;
}
size_t EditHistory::redo_bytes() const noexcept {
  size_t total = 0;
  for (const auto& e : redo_) total += e.bytes;
  return total;
}
void EditHistory::set_limits(EditHistoryLimits limits) {
  limits_ = limits;
  trim_undo();
}

void EditHistory::trim_undo() {
  size_t total = undo_bytes();
  while (undo_.size() > limits_.min_keep &&
         (undo_.size() > limits_.max_entries || total > limits_.max_bytes)) {
    total -= std::min(total, undo_.front().bytes);
    undo_.pop_front();
  }
}

}  // namespace wds::chart_editor

#include <wds/core/edit_history.hpp>

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
  for (size_t i = commands_.size(); i > 0; --i)
    if (!commands_[i - 1]->undo(doc)) return false;
  return true;
}
std::string CompositeCommand::label() const { return label_; }

SetNotesCommand::SetNotesCommand(std::vector<NotationNote> before, std::vector<NotationNote> after,
                                 std::string label)
    : before_(std::move(before)), after_(std::move(after)), label_(std::move(label)) {}
bool SetNotesCommand::execute(ChartDocument& doc) { return doc.set_notes(after_); }
bool SetNotesCommand::undo(ChartDocument& doc) { return doc.set_notes(before_); }
std::string SetNotesCommand::label() const { return label_; }

AddNotesCommand::AddNotesCommand(std::vector<NotationNote> notes, std::string label)
    : notes_(std::move(notes)), label_(std::move(label)) {}
bool AddNotesCommand::execute(ChartDocument& doc) {
  size_t added = 0;
  for (; added < notes_.size(); ++added) {
    auto note = notes_[added];
    if (note.id >= 0 && doc.find_note(note.id).has_value()) {
      while (added > 0) doc.remove_note(notes_[--added].id);
      return false;
    }
    const int32_t id = doc.add_note(note);
    if (id < 0) {
      while (added > 0) doc.remove_note(notes_[--added].id);
      return false;
    }
    notes_[added].id = id;
  }
  return true;
}
bool AddNotesCommand::undo(ChartDocument& doc) {
  size_t removed = 0;
  for (; removed < notes_.size(); ++removed) {
    if (!doc.remove_note(notes_[removed].id)) {
      while (removed > 0) doc.add_note(notes_[--removed]);
      return false;
    }
  }
  return true;
}
std::string AddNotesCommand::label() const { return label_; }

RemoveNotesCommand::RemoveNotesCommand(std::vector<NotationNote> notes, std::string label)
    : notes_(std::move(notes)), label_(std::move(label)) {}
bool RemoveNotesCommand::execute(ChartDocument& doc) {
  size_t removed = 0;
  for (; removed < notes_.size(); ++removed) {
    if (!doc.remove_note(notes_[removed].id)) {
      while (removed > 0) doc.add_note(notes_[--removed]);
      return false;
    }
  }
  return true;
}
bool RemoveNotesCommand::undo(ChartDocument& doc) {
  size_t added = 0;
  for (; added < notes_.size(); ++added) {
    const auto& note = notes_[added];
    if (doc.find_note(note.id).has_value() || doc.add_note(note) < 0) {
      while (added > 0) doc.remove_note(notes_[--added].id);
      return false;
    }
  }
  return true;
}
std::string RemoveNotesCommand::label() const { return label_; }

UpdateNotesCommand::UpdateNotesCommand(std::unordered_map<int32_t, NotePair> changes,
                                       std::string label)
    : changes_(std::move(changes)), label_(std::move(label)) {}
bool UpdateNotesCommand::apply(ChartDocument& doc, bool after) {
  std::vector<int32_t> applied;
  for (const auto& [id, pair] : changes_) {
    if (!doc.update_note(id, after ? pair.second : pair.first)) {
      for (int32_t prior : applied) {
        const auto& prior_pair = changes_.at(prior);
        doc.update_note(prior, after ? prior_pair.first : prior_pair.second);
      }
      return false;
    }
    applied.push_back(id);
  }
  return true;
}
bool UpdateNotesCommand::execute(ChartDocument& doc) { return apply(doc, true); }
bool UpdateNotesCommand::undo(ChartDocument& doc) { return apply(doc, false); }
std::string UpdateNotesCommand::label() const { return label_; }

bool EditHistory::execute(std::unique_ptr<IEditCommand> cmd, ChartDocument& doc) {
  if (!cmd || !cmd->execute(doc)) return false;
  undo_.push_back(std::move(cmd));
  redo_.clear();
  return true;
}
bool EditHistory::undo(ChartDocument& doc) {
  if (undo_.empty() || !undo_.back()->undo(doc)) return false;
  redo_.push_back(std::move(undo_.back()));
  undo_.pop_back();
  return true;
}
bool EditHistory::redo(ChartDocument& doc) {
  if (redo_.empty() || !redo_.back()->execute(doc)) return false;
  undo_.push_back(std::move(redo_.back()));
  redo_.pop_back();
  return true;
}
void EditHistory::clear() { undo_.clear(); redo_.clear(); }

}  // namespace wds::chart_editor

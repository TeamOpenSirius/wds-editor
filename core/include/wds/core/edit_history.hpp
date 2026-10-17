#pragma once

#include <wds/core/notation.hpp>

#include <cstddef>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wds::chart_editor {

class IEditCommand {
 public:
  virtual ~IEditCommand() = default;
  virtual bool execute(ChartDocument& doc) = 0;
  virtual bool undo(ChartDocument& doc) = 0;
  virtual std::string label() const = 0;
  // Estimated heap + object bytes this command retains while it sits in the
  // history. Used by EditHistory to cap memory; an estimate, not exact.
  // Evaluated once when the command is pushed, so it must not change afterwards.
  virtual size_t size_bytes() const { return 64; }
};

class CompositeCommand final : public IEditCommand {
 public:
  explicit CompositeCommand(std::string label = "Composite");
  void add(std::unique_ptr<IEditCommand> command);
  bool execute(ChartDocument& doc) override;
  bool undo(ChartDocument& doc) override;
  std::string label() const override;
  size_t size_bytes() const override;

 private:
  std::string label_;
  std::vector<std::unique_ptr<IEditCommand>> commands_;
};

class SetNotesCommand final : public IEditCommand {
 public:
  SetNotesCommand(std::vector<NotationNote> before, std::vector<NotationNote> after,
                  std::string label = "Set notes");
  bool execute(ChartDocument& doc) override;
  bool undo(ChartDocument& doc) override;
  std::string label() const override;
  size_t size_bytes() const override;

 private:
  // Full before/after copies. A diff representation was deliberately not used:
  // set_notes() is independent of the current document state, so a diff would
  // add a "document must equal the recorded state" precondition.
  std::vector<NotationNote> before_;
  std::vector<NotationNote> after_;
  std::string label_;
};

class SetTimingCommand final : public IEditCommand {
 public:
  SetTimingCommand(MusicTiming before, MusicTiming after, std::string label = "Set timing");
  bool execute(ChartDocument& doc) override;
  bool undo(ChartDocument& doc) override;
  std::string label() const override;
  size_t size_bytes() const override;

 private:
  MusicTiming before_;
  MusicTiming after_;
  std::string label_;
};

class SetScrollSpeedPointsCommand final : public IEditCommand {
 public:
  SetScrollSpeedPointsCommand(std::vector<ScrollSpeedPoint> before,
                             std::vector<ScrollSpeedPoint> after,
                             std::string label = "Set scroll speed");
  bool execute(ChartDocument& doc) override;
  bool undo(ChartDocument& doc) override;
  std::string label() const override;
  size_t size_bytes() const override;

 private:
  std::vector<ScrollSpeedPoint> before_;
  std::vector<ScrollSpeedPoint> after_;
  std::string label_;
};

class SetCompatibilityProfileCommand final : public IEditCommand {
 public:
  SetCompatibilityProfileCommand(ChartCompatibilityProfile before,
                                 ChartCompatibilityProfile after);
  bool execute(ChartDocument& doc) override;
  bool undo(ChartDocument& doc) override;
  std::string label() const override;

 private:
  ChartCompatibilityProfile before_;
  ChartCompatibilityProfile after_;
};

class AddNotesCommand final : public IEditCommand {
 public:
  explicit AddNotesCommand(std::vector<NotationNote> notes, std::string label = "Add notes");
  bool execute(ChartDocument& doc) override;
  bool undo(ChartDocument& doc) override;
  std::string label() const override;
  size_t size_bytes() const override;

 private:
  std::vector<NotationNote> notes_;
  std::string label_;
};

class RemoveNotesCommand final : public IEditCommand {
 public:
  explicit RemoveNotesCommand(std::vector<NotationNote> notes,
                              std::string label = "Remove notes");
  bool execute(ChartDocument& doc) override;
  bool undo(ChartDocument& doc) override;
  std::string label() const override;
  size_t size_bytes() const override;

 private:
  std::vector<NotationNote> notes_;
  std::string label_;
};

class UpdateNotesCommand final : public IEditCommand {
 public:
  using NotePair = std::pair<NotationNote, NotationNote>;
  explicit UpdateNotesCommand(std::unordered_map<int32_t, NotePair> changes,
                              std::string label = "Update notes");
  bool execute(ChartDocument& doc) override;
  bool undo(ChartDocument& doc) override;
  std::string label() const override;
  size_t size_bytes() const override;

 private:
  bool apply(ChartDocument& doc, bool after);
  std::unordered_map<int32_t, NotePair> changes_;
  std::string label_;
};

// Undo history cap: after each execute() the oldest undo entries are dropped while
// the count exceeds kMaxUndoEntries or the estimated retained bytes exceed
// kMaxUndoBytes, but the newest kMinUndoEntriesKept entries are always kept
// (so a single huge edit can still be undone). The redo stack is cleared by
// execute() and only ever holds entries that came out of the (capped) undo stack.
inline constexpr size_t kMaxUndoEntries = 1000;
inline constexpr size_t kMaxUndoBytes = size_t{512} * 1024 * 1024;
inline constexpr size_t kMinUndoEntriesKept = 32;

struct EditHistoryLimits {
  size_t max_entries = kMaxUndoEntries;
  size_t max_bytes = kMaxUndoBytes;
  size_t min_keep = kMinUndoEntriesKept;
};

class EditHistory {
 public:
  EditHistory() = default;
  explicit EditHistory(EditHistoryLimits limits) : limits_(limits) {}

  bool execute(std::unique_ptr<IEditCommand> cmd, ChartDocument& doc);
  bool undo(ChartDocument& doc);
  bool redo(ChartDocument& doc);
  void clear();
  bool can_undo() const noexcept { return !undo_.empty(); }
  bool can_redo() const noexcept { return !redo_.empty(); }

  size_t undo_count() const noexcept { return undo_.size(); }
  size_t redo_count() const noexcept { return redo_.size(); }
  // Sum of the size_bytes() estimates of the undo / redo entries.
  size_t undo_bytes() const noexcept;
  size_t redo_bytes() const noexcept;
  const EditHistoryLimits& limits() const noexcept { return limits_; }
  void set_limits(EditHistoryLimits limits);

 private:
  struct Entry {
    std::unique_ptr<IEditCommand> command;
    size_t bytes = 0;
  };

  void trim_undo();

  std::deque<Entry> undo_;
  std::deque<Entry> redo_;
  EditHistoryLimits limits_;
};

}  // namespace wds::chart_editor

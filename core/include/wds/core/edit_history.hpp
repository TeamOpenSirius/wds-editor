#pragma once

#include <wds/core/notation.hpp>

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
};

class CompositeCommand final : public IEditCommand {
 public:
  explicit CompositeCommand(std::string label = "Composite");
  void add(std::unique_ptr<IEditCommand> command);
  bool execute(ChartDocument& doc) override;
  bool undo(ChartDocument& doc) override;
  std::string label() const override;

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

 private:
  std::vector<NotationNote> before_;
  std::vector<NotationNote> after_;
  std::string label_;
};

class AddNotesCommand final : public IEditCommand {
 public:
  explicit AddNotesCommand(std::vector<NotationNote> notes, std::string label = "Add notes");
  bool execute(ChartDocument& doc) override;
  bool undo(ChartDocument& doc) override;
  std::string label() const override;

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

 private:
  bool apply(ChartDocument& doc, bool after);
  std::unordered_map<int32_t, NotePair> changes_;
  std::string label_;
};

class EditHistory {
 public:
  bool execute(std::unique_ptr<IEditCommand> cmd, ChartDocument& doc);
  bool undo(ChartDocument& doc);
  bool redo(ChartDocument& doc);
  void clear();
  bool can_undo() const noexcept { return !undo_.empty(); }
  bool can_redo() const noexcept { return !redo_.empty(); }

 private:
  std::vector<std::unique_ptr<IEditCommand>> undo_;
  std::vector<std::unique_ptr<IEditCommand>> redo_;
};

}  // namespace wds::chart_editor

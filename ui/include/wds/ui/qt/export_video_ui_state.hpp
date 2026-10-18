#pragma once

#include <QString>

namespace wds::ui {

enum class ExportVideoPhase : int {
  NeedEnv = 0,
  Downloading,
  NeedRetry,
  Ready,
  Encoding,
  Paused,
  Done,
};

struct ExportVideoUiState {
  ExportVideoPhase phase = ExportVideoPhase::NeedEnv;
  bool env_ok = false;
  bool software_only = false;
  QString hint;
  QString eta = QStringLiteral("—");
  int progress = 0;

  bool can_close() const noexcept {
    switch (phase) {
      case ExportVideoPhase::Downloading:
      case ExportVideoPhase::Encoding:
      case ExportVideoPhase::Paused:
        return false;
      default:
        return true;
    }
  }

  bool resolution_enabled() const noexcept { return phase == ExportVideoPhase::Ready; }

  bool generate_enabled() const noexcept { return phase == ExportVideoPhase::Ready; }

  bool cancel_enabled() const noexcept {
    return phase == ExportVideoPhase::NeedEnv || phase == ExportVideoPhase::NeedRetry ||
           phase == ExportVideoPhase::Ready;
  }

  bool show_env_row() const noexcept {
    if (phase == ExportVideoPhase::NeedEnv || phase == ExportVideoPhase::Downloading ||
        phase == ExportVideoPhase::NeedRetry) {
      return true;
    }
    return phase == ExportVideoPhase::Ready && software_only;
  }

  bool show_env_button() const noexcept {
    return phase == ExportVideoPhase::NeedEnv || phase == ExportVideoPhase::Downloading ||
           phase == ExportVideoPhase::NeedRetry;
  }

  QString env_button_text() const {
    switch (phase) {
      case ExportVideoPhase::Downloading:
        return QStringLiteral("中断");
      case ExportVideoPhase::NeedRetry:
        return QStringLiteral("重试");
      default:
        return QStringLiteral("下载");
    }
  }

  QString left_button_text() const {
    switch (phase) {
      case ExportVideoPhase::Encoding:
        return QStringLiteral("暂停");
      case ExportVideoPhase::Paused:
        return QStringLiteral("继续");
      default:
        return QStringLiteral("取消");
    }
  }

  QString right_button_text() const {
    switch (phase) {
      case ExportVideoPhase::Encoding:
      case ExportVideoPhase::Paused:
        return QStringLiteral("打断");
      case ExportVideoPhase::Done:
        return QStringLiteral("关闭");
      default:
        return QStringLiteral("生成");
    }
  }

  bool left_enabled() const noexcept {
    if (phase == ExportVideoPhase::Downloading) return false;
    if (phase == ExportVideoPhase::Done) return false;
    if (phase == ExportVideoPhase::Encoding || phase == ExportVideoPhase::Paused) return true;
    return cancel_enabled();
  }

  bool right_enabled() const noexcept {
    if (phase == ExportVideoPhase::NeedEnv || phase == ExportVideoPhase::Downloading ||
        phase == ExportVideoPhase::NeedRetry) {
      return false;
    }
    return true;
  }

  void enter_need_env() {
    phase = ExportVideoPhase::NeedEnv;
    env_ok = false;
    software_only = false;
    hint = QStringLiteral("生成视频需要先下载环境");
    progress = 0;
    eta = QStringLiteral("—");
  }

  void enter_downloading() {
    phase = ExportVideoPhase::Downloading;
    hint = QStringLiteral("生成视频需要先下载环境");
    progress = 0;
  }

  void enter_retry(const QString& message) {
    phase = ExportVideoPhase::NeedRetry;
    env_ok = false;
    hint = message.isEmpty() ? QStringLiteral("环境下载失败") : message;
    progress = 0;
    eta = QStringLiteral("—");
  }

  void enter_ready(bool soft) {
    phase = ExportVideoPhase::Ready;
    env_ok = true;
    software_only = soft;
    hint = soft ? QStringLiteral("当前 ffmpeg 只能使用软件编码，生成速度可能会变慢。") : QString();
    progress = 0;
    eta = QStringLiteral("—");
  }

  void enter_encoding() {
    phase = ExportVideoPhase::Encoding;
    progress = 0;
  }

  void enter_paused() { phase = ExportVideoPhase::Paused; }

  void enter_done() {
    phase = ExportVideoPhase::Done;
    progress = 100;
    eta = QStringLiteral("—");
  }
};

}  // namespace wds::ui

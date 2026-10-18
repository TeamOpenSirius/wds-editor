#pragma once

#include <QProcess>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace wds::ui {

inline bool write_all_stdin(QProcess* proc, const char* data, qint64 n) {
  if (proc == nullptr || proc->state() != QProcess::Running || data == nullptr || n < 0) {
    return false;
  }
  qint64 off = 0;
  // QIODevice::waitForBytesWritten returns after ≥1 byte. write() of a whole
  // 4K BGRA frame (~33 MiB) would sit in Qt's userspace buffer; at 60 fps that
  // grows by gigabytes per second until the encoder catches up.
  constexpr qint64 kChunk = 256 * 1024;
  constexpr qint64 kMaxQueued = 4 * 1024 * 1024;
  auto drain = [&](qint64 max_queued, int timeout_ms) {
    while (proc->bytesToWrite() > max_queued) {
      if (proc->state() != QProcess::Running) return false;
      if (!proc->waitForBytesWritten(timeout_ms)) return false;
    }
    return true;
  };
  while (off < n) {
    if (!drain(kMaxQueued, 15000)) return false;
    const qint64 chunk = std::min(kChunk, n - off);
    const qint64 w = proc->write(data + off, chunk);
    if (w < 0) return false;
    if (w == 0) {
      if (!proc->waitForBytesWritten(5000)) return false;
      continue;
    }
    off += w;
  }
  return drain(0, 30000);
}

inline bool write_all_stdin(QProcess* proc, const std::uint8_t* data, std::size_t n) {
  return write_all_stdin(proc, reinterpret_cast<const char*>(data), static_cast<qint64>(n));
}

inline bool write_all_stdin(QProcess* proc, const std::vector<std::uint8_t>& data) {
  return write_all_stdin(proc, data.data(), data.size());
}

}  // namespace wds::ui

#include "wds/common/crash_handler.hpp"
#include "wds/common/crash_input_journal.hpp"
#include "wds/common/log.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)

int main() {
  std::printf("wds_crash_handler_tests skipped on Windows\n");
  return 0;
}

#else

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <atomic>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <thread>

namespace {

int failures = 0;

#define CHECK(cond)                                                           \
  do {                                                                        \
    if (!(cond)) {                                                            \
      std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #cond, __FILE__,     \
                   __LINE__);                                                 \
      ++failures;                                                             \
    }                                                                         \
  } while (0)

std::string read_file(const char* path) {
  std::string out;
  if (path == nullptr) return out;
  FILE* fp = std::fopen(path, "rb");
  if (fp == nullptr) return out;
  char buf[4096];
  std::size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0) {
    out.append(buf, n);
  }
  std::fclose(fp);
  return out;
}

std::vector<std::string> list_crash_txt(const char* dir) {
  std::vector<std::string> out;
  if (dir == nullptr) return out;
  DIR* d = ::opendir(dir);
  if (d == nullptr) return out;
  while (const dirent* ent = ::readdir(d)) {
    const char* name = ent->d_name;
    const std::size_t n = std::strlen(name);
    if (std::strncmp(name, "crash-", 6) == 0 && n >= 4 &&
        std::strcmp(name + n - 4, ".txt") == 0) {
      out.push_back(std::string(dir) + "/" + name);
    }
  }
  ::closedir(d);
  return out;
}

bool path_exists(const char* path) {
  struct stat st {};
  return path != nullptr && ::stat(path, &st) == 0;
}

void disable_core_dumps() {
  struct rlimit rl {};
  rl.rlim_cur = 0;
  rl.rlim_max = 0;
  (void)::setrlimit(RLIMIT_CORE, &rl);
}

void child_prepare(const char* dir) {
  (void)::setenv("WDS_CRASH_NO_DIALOG", "1", 1);
  (void)::setenv("WDS_CRASH_LOG_DIR", dir, 1);
  disable_core_dumps();
  wds::common::install_crash_handlers();
  WDS_LOG("wds-crash-test-log-line\n");
  WDS_LOG("wds-crash-test-second\n");
  wds::common::journal_begin_event(wds::common::CrashInputKind::Click, 1.0f, 2.0f, 0.0f, 0.0f, 0, 0,
                                   1, 0);
  wds::common::journal_end_event();
  wds::common::crash_set_context(wds::common::CrashContextField::ProjectPath,
                                 "/tmp/x.wdsproject");
}

#if defined(__GNUC__)
__attribute__((noinline))
#endif
// Must NOT be a tail call: at -O3 a trailing `fp()` becomes a jump and the
// recursion turns into an infinite loop that never overflows. Using `pad`
// after the recursive call keeps every frame alive.
int force_stack_overflow(int depth) {
  volatile char pad[65536];
  pad[0] = static_cast<char>(depth);
  pad[sizeof(pad) - 1] = 2;
  int (*volatile fp)(int) = force_stack_overflow;
  const int r = fp(depth + 1);
  pad[1] = static_cast<char>(r);
  return r + pad[0] + pad[1] + pad[sizeof(pad) - 1];
}

enum class CrashCase { Segv, Abort, Terminate, Overflow, Clean, Term, Killed };

void child_run(CrashCase c, const char* dir) {
  child_prepare(dir);
  switch (c) {
    case CrashCase::Segv: {
      volatile int* p = nullptr;
      *p = 1;
      break;
    }
    case CrashCase::Abort:
      std::abort();
      break;
    case CrashCase::Terminate:
      throw std::runtime_error("boom");
    case CrashCase::Overflow:
      (void)force_stack_overflow(0);
      break;
    case CrashCase::Clean:
      wds::common::mark_clean_exit();
      ::_exit(0);
    case CrashCase::Term:
      // Requested shutdown: the termination handler must drop the sentinel.
      ::raise(SIGTERM);
      break;
    case CrashCase::Killed:
      // Simulates SIGKILL / power loss: nothing gets to run, sentinel stays.
      ::_exit(3);
  }
  ::_exit(125);
}

int wait_child(pid_t pid) {
  int st = 0;
  if (::waitpid(pid, &st, 0) < 0) return -1;
  return st;
}

bool probe_previous(const char* dir) {
  const pid_t pid = ::fork();
  if (pid == 0) {
    (void)::setenv("WDS_CRASH_NO_DIALOG", "1", 1);
    (void)::setenv("WDS_CRASH_LOG_DIR", dir, 1);
    char buf[1024] = {};
    const bool crashed = wds::common::previous_session_crashed(buf, sizeof(buf));
    ::_exit(crashed && buf[0] != '\0' ? 1 : 0);
  }
  const int st = wait_child(pid);
  return WIFEXITED(st) && WEXITSTATUS(st) == 1;
}

void assert_common_report(const std::string& text, const char* signal_or_exc) {
  CHECK(text.find("kind:") != std::string::npos);
  CHECK(text.find(signal_or_exc) != std::string::npos);
  CHECK(text.find("--- stack ---") != std::string::npos);
  // Symbolization runs after the raw frames, journal and log ring are on disk.
  CHECK(text.find("--- stack (symbolized) ---") != std::string::npos);
  CHECK(text.find("symbolization timed out") == std::string::npos);
  CHECK(text.find("main_slide=") != std::string::npos);
  CHECK(text.find("ctx.project_path: /tmp/x.wdsproject") != std::string::npos);
  CHECK(text.find("--- input journal ---") != std::string::npos);
  CHECK(text.find("--- recent log ---") != std::string::npos);
  CHECK(text.find("wds-crash-test-log-line") != std::string::npos);
}

void run_crash_case(CrashCase c, const char* name, const char* token, bool want_si_addr,
                    bool want_exception) {
  char tmpl[] = "/tmp/wds-crash-XXXXXX";
  char* dir = ::mkdtemp(tmpl);
  CHECK(dir != nullptr);
  if (dir == nullptr) return;

  const pid_t pid = ::fork();
  if (pid == 0) {
    child_run(c, dir);
  }
  const int st = wait_child(pid);
  if (c == CrashCase::Clean) {
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    const auto files = list_crash_txt(dir);
    CHECK(files.empty());
    CHECK(!path_exists((std::string(dir) + "/running.sentinel").c_str()));
    CHECK(!probe_previous(dir));
    (void)::rmdir(dir);
    (void)name;
    (void)token;
    (void)want_si_addr;
    (void)want_exception;
    return;
  }
  if (c == CrashCase::Term) {
    // SIGTERM handler cleaned up, then the default action killed the child.
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM);
    CHECK(list_crash_txt(dir).empty());
    CHECK(!path_exists((std::string(dir) + "/running.sentinel").c_str()));
    CHECK(!probe_previous(dir));
    (void)::rmdir(dir);
    return;
  }
  if (c == CrashCase::Killed) {
    // Stale sentinel + header-only session file: NOT a crash. The probe must
    // say so and clean both files up.
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 3);
    CHECK(path_exists((std::string(dir) + "/running.sentinel").c_str()));
    CHECK(list_crash_txt(dir).size() == 1);
    CHECK(!probe_previous(dir));
    CHECK(!path_exists((std::string(dir) + "/running.sentinel").c_str()));
    CHECK(list_crash_txt(dir).empty());
    (void)::rmdir(dir);
    return;
  }

  CHECK(WIFSIGNALED(st) || WIFEXITED(st));
  const auto files = list_crash_txt(dir);
  CHECK(files.size() == 1);
  if (files.size() == 1) {
    const std::string text = read_file(files[0].c_str());
    if (text.find(token) == std::string::npos) {
      std::fprintf(stderr, "case %s missing token %s\n---- report ----\n%s\n", name, token,
                   text.c_str());
    }
    assert_common_report(text, token);
    if (c == CrashCase::Overflow) {
      CHECK(text.find("SIGSEGV") != std::string::npos ||
            text.find("SIGBUS") != std::string::npos);
    }
    if (want_si_addr) {
      CHECK(text.find("si_addr:") != std::string::npos);
    }
    if (want_exception) {
      CHECK(text.find("runtime_error") != std::string::npos);
      CHECK(text.find("boom") != std::string::npos);
    }
  }
  CHECK(path_exists((std::string(dir) + "/running.sentinel").c_str()));
  CHECK(probe_previous(dir));

  for (const auto& f : files) {
    (void)::unlink(f.c_str());
  }
  (void)::unlink((std::string(dir) + "/running.sentinel").c_str());
  (void)::rmdir(dir);
}

std::atomic<bool> g_in_hook{false};
std::atomic<bool> g_release_hook{false};

void hold_lock_hook() {
  g_in_hook.store(true);
  while (!g_release_hook.load()) {
    ::usleep(1000);
  }
}

// Child: thread A runs report_fatal() and is held inside it (crash lock owned).
// Thread B then segfaults. B must wait, and once A releases it must report its
// own crash and terminate the process (previously B parked forever).
void child_crash_during_report_fatal(const char* dir) {
  child_prepare(dir);
  ::alarm(20);  // a regression hangs; SIGALRM turns that into a test failure
  wds::common::testing::set_report_fatal_hook(hold_lock_hook);
  std::thread a([] { wds::common::report_fatal("test-fatal", "first-report"); });
  while (!g_in_hook.load()) ::usleep(1000);
  std::thread b([] {
    volatile int* p = nullptr;
    *p = 1;
  });
  ::usleep(200 * 1000);  // B is now faulting and parked on the lock
  g_release_hook.store(true);
  a.join();  // report_fatal returned: app "keeps running"
  b.join();  // never reached if B's crash is reported correctly
  ::_exit(125);
}

void run_crash_during_report_fatal() {
  char tmpl[] = "/tmp/wds-crash-XXXXXX";
  char* dir = ::mkdtemp(tmpl);
  CHECK(dir != nullptr);
  if (dir == nullptr) return;
  const pid_t pid = ::fork();
  if (pid == 0) {
    child_crash_during_report_fatal(dir);
  }
  const int st = wait_child(pid);
  CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV);
  const auto files = list_crash_txt(dir);
  CHECK(files.size() == 1);
  if (files.size() == 1) {
    const std::string text = read_file(files[0].c_str());
    CHECK(text.find("first-report") != std::string::npos);
    CHECK(text.find("SIGSEGV") != std::string::npos);
    CHECK(text.find("--- stack ---") != std::string::npos);
    CHECK(text.find("(re-raised from terminate)") == std::string::npos);
  }
  for (const auto& f : files) (void)::unlink(f.c_str());
  (void)::unlink((std::string(dir) + "/running.sentinel").c_str());
  (void)::rmdir(dir);
}

// Waits up to timeout_ms; on timeout SIGKILLs the child and sets *timed_out.
int wait_child_timeout(pid_t pid, int timeout_ms, bool* timed_out) {
  int st = 0;
  *timed_out = false;
  for (int waited = 0; waited < timeout_ms; waited += 10) {
    const pid_t r = ::waitpid(pid, &st, WNOHANG);
    if (r == pid) return st;
    if (r < 0) return -1;
    ::usleep(10 * 1000);
  }
  ::kill(pid, SIGKILL);
  (void)::waitpid(pid, &st, 0);
  *timed_out = true;
  return st;
}

void remove_dir_contents(const char* dir) {
  DIR* d = ::opendir(dir);
  if (d != nullptr) {
    while (const dirent* ent = ::readdir(d)) {
      if (std::strcmp(ent->d_name, ".") == 0 || std::strcmp(ent->d_name, "..") == 0) continue;
      (void)::unlink((std::string(dir) + "/" + ent->d_name).c_str());
    }
    ::closedir(d);
  }
  (void)::rmdir(dir);
}

[[noreturn]] void hang_forever_hook_impl() {
  for (;;) ::pause();
}
void hang_forever_hook() { hang_forever_hook_impl(); }

// Simulates a loader-lock deadlock inside the (non async-signal-safe)
// symbolization step. The watchdog must end the process, with the raw report
// (frames, journal, log ring) already on disk and the original signal re-raised.
void child_symbolize_hang(const char* dir) {
  child_prepare(dir);
  wds::common::testing::set_symbolize_timeout_seconds(1);
  wds::common::testing::set_symbolize_hook(hang_forever_hook);
  volatile int* p = nullptr;
  *p = 1;
  ::_exit(125);
}

void run_symbolize_watchdog() {
  char tmpl[] = "/tmp/wds-crash-XXXXXX";
  char* dir = ::mkdtemp(tmpl);
  CHECK(dir != nullptr);
  if (dir == nullptr) return;
  const pid_t pid = ::fork();
  if (pid == 0) {
    child_symbolize_hang(dir);
  }
  bool timed_out = false;
  const int st = wait_child_timeout(pid, 20 * 1000, &timed_out);
  CHECK(!timed_out);
  CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV);
  const auto files = list_crash_txt(dir);
  CHECK(files.size() == 1);
  if (files.size() == 1) {
    const std::string text = read_file(files[0].c_str());
    CHECK(text.find("--- stack ---") != std::string::npos);
    CHECK(text.find("  #0 0x") != std::string::npos);  // raw PCs
    CHECK(text.find("--- input journal ---") != std::string::npos);
    CHECK(text.find("--- recent log ---") != std::string::npos);
    CHECK(text.find("symbolization timed out") != std::string::npos);
    CHECK(text.find("--- stack (symbolized) ---") == std::string::npos);
    CHECK(text.find("--- images ---") == std::string::npos);
  }
  remove_dir_contents(dir);
}

void custom_alrm_handler(int) {}

// report_fatal() returns to a live app: the symbolization watchdog must be
// fully disarmed afterwards (no stray SIGALRM), the previous SIGALRM
// disposition restored and a pre-existing alarm() handed back.
void child_watchdog_restore(const char* dir) {
  child_prepare(dir);
  struct sigaction sa {};
  sa.sa_handler = custom_alrm_handler;
  sigemptyset(&sa.sa_mask);
  (void)::sigaction(SIGALRM, &sa, nullptr);
  (void)::alarm(50);
  wds::common::report_fatal("test-fatal", "restore");
  struct sigaction cur {};
  (void)::sigaction(SIGALRM, nullptr, &cur);
  const unsigned remaining = ::alarm(0);
  int code = 0;
  if (cur.sa_handler != custom_alrm_handler) code |= 1;
  if (remaining < 40 || remaining > 50) code |= 2;
  ::_exit(code);
}

void run_watchdog_restore() {
  char tmpl[] = "/tmp/wds-crash-XXXXXX";
  char* dir = ::mkdtemp(tmpl);
  CHECK(dir != nullptr);
  if (dir == nullptr) return;
  const pid_t pid = ::fork();
  if (pid == 0) {
    child_watchdog_restore(dir);
  }
  bool timed_out = false;
  const int st = wait_child_timeout(pid, 20 * 1000, &timed_out);
  CHECK(!timed_out);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
  remove_dir_contents(dir);
}

// After mark_clean_exit() the shared crash fd must be detached before it is
// closed. Reopen something (which reuses the lowest free descriptor, i.e. the
// number just released) and crash: the handler must not write into it.
void child_fd_reuse(const char* dir, const char* victim) {
  child_prepare(dir);
  wds::common::mark_clean_exit();
  const int vfd = ::open(victim, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  (void)vfd;
  volatile int* p = nullptr;
  *p = 1;
  ::_exit(125);
}

void run_fd_reuse() {
  char tmpl[] = "/tmp/wds-crash-XXXXXX";
  char* dir = ::mkdtemp(tmpl);
  CHECK(dir != nullptr);
  if (dir == nullptr) return;
  const std::string victim = std::string(dir) + "/victim.bin";
  const pid_t pid = ::fork();
  if (pid == 0) {
    child_fd_reuse(dir, victim.c_str());
  }
  bool timed_out = false;
  const int st = wait_child_timeout(pid, 20 * 1000, &timed_out);
  CHECK(!timed_out);
  CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV);
  struct stat vst {};
  CHECK(::stat(victim.c_str(), &vst) == 0);
  CHECK(vst.st_size == 0);  // the crash report went elsewhere
  const auto files = list_crash_txt(dir);
  CHECK(files.size() == 1);  // re-created by path, not through the stale fd
  if (files.size() == 1) {
    CHECK(read_file(files[0].c_str()).find("SIGSEGV") != std::string::npos);
  }
  remove_dir_contents(dir);
}

// >64 crash files, oldest one is the previous session's report. Rotation must
// keep the 20 newest by name (timestamp) and the stale session's report (plus
// its .dmp companion), and delete everything else, not whatever readdir
// happens to list last.
void run_rotation() {
  char tmpl[] = "/tmp/wds-crash-XXXXXX";
  char* dir = ::mkdtemp(tmpl);
  CHECK(dir != nullptr);
  if (dir == nullptr) return;
  constexpr int kFiles = 70;
  auto txt_path = [&](int i) {
    char name[64];
    std::snprintf(name, sizeof(name), "/crash-20260101-%06d-4242.txt", i);
    return std::string(dir) + name;
  };
  const std::string stale_dmp = std::string(dir) + "/crash-20260101-000000-4242.dmp";
  for (int i = 0; i < kFiles; ++i) {
    FILE* fp = std::fopen(txt_path(i).c_str(), "wb");
    CHECK(fp != nullptr);
    if (fp != nullptr) {
      const std::string body(200, 'x');  // larger than the header-only marker
      std::fwrite(body.data(), 1, body.size(), fp);
      std::fclose(fp);
    }
  }
  {
    FILE* fp = std::fopen(stale_dmp.c_str(), "wb");
    if (fp != nullptr) {
      std::fputs("dmp", fp);
      std::fclose(fp);
    }
  }
  // A pid that is certainly dead: a reaped child.
  const pid_t dead = ::fork();
  if (dead == 0) ::_exit(0);
  (void)::waitpid(dead, nullptr, 0);
  {
    const std::string sentinel = std::string(dir) + "/running.sentinel";
    FILE* fp = std::fopen(sentinel.c_str(), "wb");
    CHECK(fp != nullptr);
    if (fp != nullptr) {
      std::fprintf(fp, "pid=%ld\ncrash=%s\n", static_cast<long>(dead), txt_path(0).c_str());
      std::fclose(fp);
    }
  }
  const pid_t pid = ::fork();
  if (pid == 0) {
    (void)::setenv("WDS_CRASH_NO_DIALOG", "1", 1);
    (void)::setenv("WDS_CRASH_LOG_DIR", dir, 1);
    wds::common::install_crash_handlers();
    char prev[1024] = {};
    const bool crashed = wds::common::previous_session_crashed(prev, sizeof(prev));
    ::_exit(crashed && std::strcmp(prev, txt_path(0).c_str()) == 0 ? 0 : 7);
  }
  bool timed_out = false;
  const int st = wait_child_timeout(pid, 20 * 1000, &timed_out);
  CHECK(!timed_out);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
  CHECK(path_exists(txt_path(0).c_str()));  // stale session report survives
  CHECK(path_exists(stale_dmp.c_str()));    // ... with its companion dump
  for (int i = 1; i < kFiles - 20; ++i) {
    CHECK(!path_exists(txt_path(i).c_str()));  // old ones are gone
  }
  for (int i = kFiles - 20; i < kFiles; ++i) {
    CHECK(path_exists(txt_path(i).c_str()));  // 20 newest are kept
  }
  CHECK(list_crash_txt(dir).size() == 20 + 1 + 1);  // + stale + this session's file
  remove_dir_contents(dir);
}

void emit_to_string(void* ctx, const char* data, std::size_t n) {
  static_cast<std::string*>(ctx)->append(data, n);
}

// NaN / inf / huge floats and INT64_MIN must format without UB (float->uint64
// casts, signed negation) and without garbage.
void run_journal_extremes() {
  using namespace wds::common;
  journal_reset_for_test();
  const float nan_v = std::numeric_limits<float>::quiet_NaN();
  const float inf_v = std::numeric_limits<float>::infinity();
  journal_begin_event(CrashInputKind::KeyDown, 0.0f, 0.0f, 0.0f, 0.0f,
                      std::numeric_limits<std::int32_t>::min(), 0, 0, 0);
  journal_end_event();
  journal_begin_event(CrashInputKind::Scroll, nan_v, inf_v, -inf_v, 1.0e30f, 0, 0, 0, 0);
  journal_add_effect(CrashEffectId::TimelineMs, std::numeric_limits<std::int64_t>::min(),
                     std::numeric_limits<std::int64_t>::max(), -1.0e30f, 3.5f);
  journal_end_event();
  std::string out;
  journal_write_text(emit_to_string, &out);
  CHECK(out.find("pos=nan,inf") != std::string::npos);
  CHECK(out.find("d=-inf,1e18+") != std::string::npos);
  CHECK(out.find("key=-2147483648") != std::string::npos);
  CHECK(out.find("-9223372036854775808->9223372036854775807") != std::string::npos);
  CHECK(out.find("-1e18+->3.50") != std::string::npos);
  journal_reset_for_test();
}

}  // namespace

int main() {
  run_journal_extremes();
  run_crash_case(CrashCase::Segv, "segv", "SIGSEGV", true, false);
  run_crash_case(CrashCase::Abort, "abort", "SIGABRT", false, false);
  run_crash_case(CrashCase::Terminate, "terminate", "terminate", false, true);
  run_crash_case(CrashCase::Overflow, "overflow", "kind: signal", true, false);
  run_crash_case(CrashCase::Clean, "clean", "", false, false);
  run_crash_case(CrashCase::Term, "term", "", false, false);
  run_crash_case(CrashCase::Killed, "killed", "", false, false);
  run_crash_during_report_fatal();
  run_symbolize_watchdog();
  run_watchdog_restore();
  run_fd_reuse();
  run_rotation();

  if (failures != 0) {
    std::fprintf(stderr, "%d crash handler test failure(s)\n", failures);
    return 1;
  }
  std::printf("wds_crash_handler_tests OK\n");
  return 0;
}

#endif

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
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdexcept>

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

int count_stack_frames(const std::string& text) {
  const char* key = "--- stack ---";
  const auto pos = text.find(key);
  if (pos == std::string::npos) return 0;
  std::size_t i = pos + std::strlen(key);
  if (i < text.size() && text[i] == '\n') ++i;
  int frames = 0;
  while (i < text.size()) {
    const auto eol = text.find('\n', i);
    const std::string line =
        text.substr(i, eol == std::string::npos ? std::string::npos : eol - i);
    if (line.rfind("--- ", 0) == 0) break;
    if (!line.empty()) ++frames;
    if (eol == std::string::npos) break;
    i = eol + 1;
  }
  return frames;
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
  // Release/CI often inlines the crash path down to 1–2 frames.
  CHECK(count_stack_frames(text) >= 1);
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

}  // namespace

int main() {
  run_crash_case(CrashCase::Segv, "segv", "SIGSEGV", true, false);
  run_crash_case(CrashCase::Abort, "abort", "SIGABRT", false, false);
  run_crash_case(CrashCase::Terminate, "terminate", "terminate", false, true);
  run_crash_case(CrashCase::Overflow, "overflow", "kind: signal", true, false);
  run_crash_case(CrashCase::Clean, "clean", "", false, false);
  run_crash_case(CrashCase::Term, "term", "", false, false);
  run_crash_case(CrashCase::Killed, "killed", "", false, false);

  if (failures != 0) {
    std::fprintf(stderr, "%d crash handler test failure(s)\n", failures);
    return 1;
  }
  std::printf("wds_crash_handler_tests OK\n");
  return 0;
}

#endif

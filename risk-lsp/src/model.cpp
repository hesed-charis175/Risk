#include "model.h"
#include "analyzer.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

using json = nlohmann::json;

void Analysis::index() {
  by_name.clear();
  for (size_t i = 0; i < syms.size(); ++i)
    by_name[syms[i].n].push_back(i);
}

static std::string jstr(const json &j, const char *k) {
  auto it = j.find(k);
  return (it != j.end() && it->is_string()) ? it->get<std::string>() : "";
}
static int jint(const json &j, const char *k) {
  auto it = j.find(k);
  return (it != j.end() && it->is_number()) ? it->get<int>() : 0;
}

static std::shared_ptr<Analysis> parse_result(const std::string &text) {
  json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
  if (j.is_discarded() || !j.is_object())
    return nullptr;
  auto a = std::make_shared<Analysis>();

  for (auto &d : j["diagnostics"]) {
    DiagInfo di;
    di.sev = jint(d, "s");
    di.msg = jstr(d, "m");
    di.l = jint(d, "l");
    di.c = jint(d, "c");
    di.e = jint(d, "e");
    bool dup = false;
    for (auto &o : a->diags)
      if (o.l == di.l && o.c == di.c && o.msg == di.msg)
        dup = true;
    if (!dup)
      a->diags.push_back(std::move(di));
  }
  for (auto &i : j["imports"]) {
    ImportInfo ii;
    ii.path = jstr(i, "path");
    ii.alias = jstr(i, "alias");
    ii.l = jint(i, "l");
    ii.c = jint(i, "c");
    a->imports.push_back(std::move(ii));
  }
  for (auto &s : j["symbols"]) {
    Sym y;
    y.k = jstr(s, "k");
    y.n = jstr(s, "n");
    y.d = jstr(s, "d");
    y.t = jstr(s, "t");
    y.p = jstr(s, "p");
    y.doc = jstr(s, "doc");
    y.ext = jstr(s, "ext");
    y.l = jint(s, "l");
    y.c = jint(s, "c");
    y.scoped = s.contains("sl");
    y.sl = jint(s, "sl");
    y.sc = jint(s, "sc");
    y.el = jint(s, "el");
    y.ec = jint(s, "ec");
    if (s.contains("ty"))
      y.ty = s["ty"];
    if (s.contains("ps"))
      for (auto &p : s["ps"]) {
        y.ps.push_back(jstr(p, "label"));
        y.psNames.push_back(jstr(p, "n"));
      }
    a->syms.push_back(std::move(y));
  }
  a->index();
  return a;
}

std::shared_ptr<Analysis> run_analysis(const std::string &path,
                                       const std::string &src, bool typecheck,
                                       std::string *err) {
  int fds[2];
  if (pipe(fds) != 0) {
    if (err)
      *err = std::string("pipe failed: ") + strerror(errno);
    return nullptr;
  }
  pid_t pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    if (err)
      *err = std::string("fork failed: ") + strerror(errno);
    return nullptr;
  }
  if (pid == 0) {
    close(fds[0]);
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
      dup2(devnull, 0);
      dup2(devnull, 1);
      dup2(devnull, 2);
    }
    struct rlimit rl;
    rl.rlim_cur = rl.rlim_max = (rlim_t)2 * 1024 * 1024 * 1024; // 2 GiB
    setrlimit(RLIMIT_AS, &rl);
    alarm(5);
    std::string out;
    try {
      out = analyze_source(path, src, typecheck);
    } catch (...) {
      _exit(2);
    }
    const char *p = out.data();
    size_t left = out.size();
    while (left > 0) {
      ssize_t n = write(fds[1], p, left);
      if (n < 0) {
        if (errno == EINTR)
          continue;
        _exit(3);
      }
      p += n;
      left -= (size_t)n;
    }
    close(fds[1]);
    _exit(0);
  }

  close(fds[1]);
  std::string buf;
  char tmp[65536];
  for (;;) {
    ssize_t n = read(fds[0], tmp, sizeof tmp);
    if (n > 0)
      buf.append(tmp, (size_t)n);
    else if (n == 0)
      break;
    else if (errno != EINTR)
      break;
  }
  close(fds[0]);
  int status = 0;
  waitpid(pid, &status, 0);

  if (WIFSIGNALED(status)) {
    if (err)
      *err = std::string("analysis process died with signal ") +
             std::to_string(WTERMSIG(status)) +
             (WTERMSIG(status) == SIGALRM ? " (timed out)" : " (crash)");
    return nullptr;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    if (err)
      *err = "analysis process failed";
    return nullptr;
  }
  auto a = parse_result(buf);
  if (!a && err)
    *err = "analysis returned malformed output";
  return a;
}

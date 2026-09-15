#include "carolina.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <netinet/in.h>
#include <string>
#include <utility>
#include <vector>

static int g_failed;

static void expect(bool cond, const char *msg) {
  if (!cond) {
    std::cerr << "FAIL: " << msg << "\n";
    g_failed = 1;
  } else {
    std::cerr << "ok: " << msg << "\n";
  }
}

static PGconn *fake_connect(const char *dsn) {
  (void)dsn;
  return reinterpret_cast<PGconn *>(static_cast<intptr_t>(0x51c0ffee));
}

static int count_talks_keys(const char *json) {
  int n = 0;
  const char *p = json;
  while (p && (p = std::strstr(p, "\"talks\":"))) {
    n++;
    p += 8;
  }
  return n;
}

static std::string slurp_file(const char *path) {
  std::ifstream in(path);
  std::string msg = std::string("can read ") + path;
  expect(in.good(), msg.c_str());
  if (!in) return {};
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static std::vector<std::string> split_lines(const std::string &s) {
  std::vector<std::string> lines;
  std::string cur;
  for (char c : s) {
    if (c == '\n') {
      lines.push_back(cur);
      cur.clear();
    } else if (c != '\r') {
      cur += c;
    }
  }
  if (!cur.empty()) lines.push_back(cur);
  return lines;
}

static bool is_indent2_key(const std::string &line, std::string *key) {
  if (line.size() < 4) return false;
  if (line[0] != ' ' || line[1] != ' ' || line[2] == ' ') return false;
  std::string body = line;
  auto hash = body.find('#');
  if (hash != std::string::npos) body.resize(hash);
  while (!body.empty() && (body.back() == ' ' || body.back() == '\t')) body.pop_back();
  if (body.size() < 4 || body.back() != ':') return false;
  *key = body.substr(2, body.size() - 3);
  if (key->empty()) return false;
  for (char c : *key) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) return false;
  }
  return true;
}

static std::map<std::string, std::string> parse_gitea_jobs(const std::string &text) {
  std::map<std::string, std::string> jobs;
  auto lines = split_lines(text);
  bool in_jobs = false;
  std::string current;
  std::string body;
  auto flush = [&]() {
    if (!current.empty()) jobs[current] = body;
    current.clear();
    body.clear();
  };
  for (const auto &line : lines) {
    if (!in_jobs) {
      if (line == "jobs:" || line.rfind("jobs:", 0) == 0) in_jobs = true;
      continue;
    }
    if (!line.empty() && line[0] != ' ' && line[0] != '\t' && line[0] != '#') {
      flush();
      in_jobs = false;
      continue;
    }
    std::string key;
    if (is_indent2_key(line, &key)) {
      flush();
      current = key;
      continue;
    }
    if (!current.empty()) {
      body += line;
      body += '\n';
    }
  }
  flush();
  return jobs;
}

static std::map<std::string, std::string> parse_precommit_hooks(const std::string &text) {
  std::map<std::string, std::string> hooks;
  auto lines = split_lines(text);
  std::string id;
  for (const auto &line : lines) {
    auto trimmed = line;
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front()))) trimmed.erase(trimmed.begin());
    if (trimmed.rfind("- ", 0) == 0) {
      trimmed.erase(0, 2);
      while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front()))) trimmed.erase(trimmed.begin());
    }
    if (trimmed.rfind("id:", 0) == 0) {
      id = trimmed.substr(3);
      while (!id.empty() && std::isspace(static_cast<unsigned char>(id.front()))) id.erase(id.begin());
    } else if (trimmed.rfind("entry:", 0) == 0 && !id.empty()) {
      std::string entry = trimmed.substr(6);
      while (!entry.empty() && std::isspace(static_cast<unsigned char>(entry.front()))) entry.erase(entry.begin());
      hooks[id] = entry;
      id.clear();
    }
  }
  return hooks;
}

static bool job_has_needs(const std::string &body) {
  for (const auto &line : split_lines(body)) {
    auto trimmed = line;
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front()))) trimmed.erase(trimmed.begin());
    if (trimmed.rfind("needs:", 0) == 0) return true;
  }
  return false;
}

static bool job_needs_prepare(const std::string &body) {
  for (const auto &line : split_lines(body)) {
    auto trimmed = line;
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front()))) trimmed.erase(trimmed.begin());
    if (trimmed.rfind("needs:", 0) == 0 && trimmed.find("prepare") != std::string::npos) return true;
  }
  return false;
}

static bool contains_shared_tool_install(const std::string &text) {
  if (text.find("apt-get install") != std::string::npos) return true;
  if (text.find("libpq-dev") != std::string::npos) return true;
  if (text.find("cppcheck") != std::string::npos) return true;
  if (text.find("osv-scanner_linux_amd64") != std::string::npos) return true;
  if (text.find("gitleaks_8.30.1") != std::string::npos) return true;
  if (text.find("pip3 install") != std::string::npos) return true;
  if (text.find("clang-format==") != std::string::npos) return true;
  return false;
}

static void expect_contains(const std::string &text, const char *needle, const char *msg) {
  expect(text.find(needle) != std::string::npos, msg);
}

static void test_quality_gate_wiring() {
  const std::string makefile = slurp_file("Makefile");
  const std::string precommit = slurp_file(".pre-commit-config.yaml");
  const std::string hook = slurp_file(".githooks/pre-commit");
  const std::string workflow = slurp_file(".gitea/workflows/ci.yml");

  expect_contains(makefile, "cppcheck", "Makefile SAST invokes cppcheck");
  expect_contains(makefile, "osv-scanner", "Makefile dependency scan invokes osv-scanner");
  expect_contains(makefile, "gitleaks", "Makefile secrets invokes gitleaks");
  expect_contains(makefile, "clang-format", "Makefile style invokes clang-format");
  expect_contains(makefile, "scan source", "osv-scanner uses source scan");

  auto hooks = parse_precommit_hooks(precommit);
  expect(hooks.count("test") && hooks["test"] == "make test", "pre-commit test hook is make test");
  expect(hooks.count("sast") && hooks["sast"] == "make sast", "pre-commit sast hook is make sast");
  expect(hooks.count("vuln") && hooks["vuln"] == "make vuln", "pre-commit vuln hook is make vuln");
  expect(hooks.count("secrets") && hooks["secrets"] == "make secrets", "pre-commit secrets hook is make secrets");
  expect(hooks.count("fmt") && hooks["fmt"] == "make fmt-check", "pre-commit fmt hook is make fmt-check");
  expect(hooks.size() >= 5, "pre-commit has five separate check hooks");

  expect_contains(hook, "make test", "githooks fallback runs make test");
  expect_contains(hook, "make sast", "githooks fallback runs make sast");
  expect_contains(hook, "make vuln", "githooks fallback runs make vuln");
  expect_contains(hook, "make secrets", "githooks fallback runs make secrets");
  expect_contains(hook, "make fmt-check", "githooks fallback runs make fmt-check");
  expect_contains(hook, "pre-commit run", "githooks prefers pre-commit runner");

  expect(workflow.find("actions/checkout") == std::string::npos, "workflow does not use actions/checkout");
  expect(workflow.find("git init") == std::string::npos, "workflow does not git init");

  auto jobs = parse_gitea_jobs(workflow);
  const char *kinds[] = {"test", "sast", "vuln", "secrets", "fmt"};
  const char *makes[] = {"make test", "make sast", "make vuln", "make secrets", "make fmt-check"};
  expect(jobs.count("prepare") == 1, "Gitea workflow has an initial prepare job");
  expect(jobs.size() >= 6, "Gitea workflow has prepare plus five separate check jobs");
  if (jobs.count("prepare")) {
    const std::string &prep = jobs["prepare"];
    expect(!job_has_needs(prep), "prepare job is the initial stage");
    expect_contains(prep, "apt-get install", "prepare job apt-installs the shared toolset");
    expect_contains(prep, "libpq-dev", "prepare job installs libpq");
    expect_contains(prep, "cppcheck", "prepare job installs cppcheck");
    expect_contains(prep, "osv-scanner_linux_amd64", "prepare job downloads osv-scanner");
    expect_contains(prep, "gitleaks_8.30.1", "prepare job downloads gitleaks");
    expect_contains(prep, "pip3 install", "prepare job pip-installs clang-format");
    expect_contains(prep, "clang-format==", "prepare job pins clang-format");
    expect_contains(prep, "ci-pack.sh", "prepare job packs the CI environment");
    expect_contains(prep, "ci-artifact.sh", "prepare job publishes the CI environment");
    expect_contains(prep, "x-access-token", "prepare job clones with job token over HTTPS");
    expect_contains(prep, "GITHUB_SHA", "prepare job checks out GITHUB_SHA");
    expect(prep.find("git clone") != std::string::npos, "prepare job clones over HTTPS");
    expect(prep.find("make test") == std::string::npos, "prepare job is not a combined check");
  }

  const std::string restore = slurp_file("scripts/ci-restore.sh");
  const std::string pack = slurp_file("scripts/ci-pack.sh");
  const std::string artifact = slurp_file("scripts/ci-artifact.sh");
  expect(restore.find("dpkg") != std::string::npos, "restore extracts packed debs with dpkg");
  expect(restore.find("usr-local") != std::string::npos, "restore copies packed /usr/local tools");
  expect(restore.find("apt-get install") == std::string::npos, "restore does not apt-get install the toolset");
  expect(restore.find("osv-scanner_linux_amd64") == std::string::npos, "restore does not download osv-scanner");
  expect(restore.find("gitleaks_8.30.1") == std::string::npos, "restore does not download gitleaks");
  expect(restore.find("pip3 install") == std::string::npos, "restore does not pip-install clang-format");
  expect_contains(pack, "/var/cache/apt/archives", "pack captures apt debs");
  expect_contains(pack, "/usr/local", "pack captures /usr/local tools");
  expect_contains(artifact, "actions_pipeline", "artifact helper uses Gitea pipeline API");
  expect(artifact.find("actions/upload-artifact") == std::string::npos, "artifact helper is not a Node upload action");
  expect(artifact.find("actions/download-artifact") == std::string::npos, "artifact helper is not a Node download action");

  for (int i = 0; i < 5; ++i) {
    std::string present = std::string("Gitea job exists: ") + kinds[i];
    expect(jobs.count(kinds[i]) == 1, present.c_str());
    if (!jobs.count(kinds[i])) continue;
    const std::string &body = jobs[kinds[i]];
    std::string needs_prep = std::string("Gitea job needs prepare: ") + kinds[i];
    expect(job_needs_prepare(body), needs_prep.c_str());
    std::string has_make = std::string("Gitea job runs its check: ") + kinds[i];
    expect(body.find(makes[i]) != std::string::npos, has_make.c_str());
    for (int j = 0; j < 5; ++j) {
      if (i == j) continue;
      expect(body.find(makes[j]) == std::string::npos, "check jobs are not combined");
    }
    expect_contains(body, "x-access-token", "check job clones with job token over HTTPS");
    expect_contains(body, "GITHUB_SHA", "check job checks out GITHUB_SHA");
    expect(body.find("git clone") != std::string::npos, "check job clones over HTTPS");
    expect_contains(body, "ci-restore.sh", "check job restores the prepared environment");
    std::string no_install = std::string("check job does not reinstall shared tools: ") + kinds[i];
    expect(!contains_shared_tool_install(body), no_install.c_str());
  }
}

int main() {
  carolina_init();

  expect(carolina_listen_family() == AF_INET6, "listen family is AF_INET6");
  expect(carolina_bind_ipv6() >= 0, "httplib binds to ::");

  carolina_set_connect_fn(fake_connect);
  carolina_reset_counts();
  PGconn *a = carolina_db_acquire();
  carolina_db_release(a);
  PGconn *b = carolina_db_acquire();
  carolina_db_release(b);
  expect(a != nullptr && b != nullptr, "acquire returns a connection");
  expect(a == b, "second acquire reuses the same pooled conn");
  expect(carolina_connect_count() == 1, "libpq connect count is not one-per-request");
  carolina_set_connect_fn(nullptr);

  carolina_reset_counts();
  char health[256];
  int hstatus = carolina_handle_health_copy(health, sizeof(health));
  expect(hstatus == 200, "/health returns 200");
  expect(std::strstr(health, "ok") != nullptr, "/health body is ok JSON");
  expect(carolina_sql_count() == 0, "/health does not run SQL");
  expect(carolina_connect_count() == 0, "/health does not open Postgres");

  carolina_reset_counts();
  char body[1 << 20];
  int status = carolina_handle_speakers_year(2026, body, sizeof(body));
  int sql = carolina_sql_count();
  int speakers = count_talks_keys(body);
  std::cerr << "year list status=" << status << " sql=" << sql << " speakers=" << speakers
            << " connects=" << carolina_connect_count() << "\n";
  if (status == 200) {
    expect(speakers >= 3, "year listing returns N>=3 speakers");
    expect(sql > 0, "listing runs SQL through shipped exec wrapper");
    expect(sql < 2 * speakers, "SQL count does not grow as ~2N");
    expect(sql <= 4, "year listing SQL is bounded (speakers + talks + years)");
    carolina_reset_counts();
    int status2 = carolina_handle_speakers_year(2026, body, sizeof(body));
    expect(status2 == 200, "second catalog request succeeds");
    expect(carolina_connect_count() == 0, "second catalog request reuses pool (no extra connect)");
  } else {
    expect(sql < 2 * 3, "failed listing did not run per-row SQL for N=3");
  }

  std::ifstream src("src/main.cpp");
  expect(src.good(), "can read src/main.cpp");
  if (src) {
    std::string text((std::istreambuf_iterator<char>(src)), std::istreambuf_iterator<char>());
    expect(text.find("listen(\"::\"") != std::string::npos, "source binds httplib to ::");
    expect(text.find("0.0.0.0") == std::string::npos, "source does not bind IPv4-only 0.0.0.0");
    auto reg = text.find("void register_with_elixir");
    expect(reg != std::string::npos, "register_with_elixir exists");
    if (reg != std::string::npos) {
      auto end = text.find("}  // namespace", reg);
      std::string fn = text.substr(reg, end == std::string::npos ? 800 : end - reg);
      expect(fn.find("db_acquire") == std::string::npos, "register-once does not open Postgres");
      expect(fn.find("exec_params") == std::string::npos, "register-once does not run catalog SQL");
    }
  }

  test_quality_gate_wiring();

  if (g_failed) {
    std::cerr << "perf_test failed\n";
    return 1;
  }
  std::cerr << "perf_test passed\n";
  return 0;
}

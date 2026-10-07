#include "carolina.h"
#include "httplib.h"

#include <arpa/inet.h>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <map>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
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

static void expect_s(bool cond, const std::string &msg) { expect(cond, msg.c_str()); }

static PGconn *fake_connect(const char *dsn) {
  (void)dsn;
  return reinterpret_cast<PGconn *>(static_cast<intptr_t>(0x51c0ffee));
}

static PGconn *fail_connect(const char *dsn) {
  (void)dsn;
  return nullptr;
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
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front())))
      trimmed.erase(trimmed.begin());
    if (trimmed.rfind("- ", 0) == 0) {
      trimmed.erase(0, 2);
      while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front())))
        trimmed.erase(trimmed.begin());
    }
    if (trimmed.rfind("id:", 0) == 0) {
      id = trimmed.substr(3);
      while (!id.empty() && std::isspace(static_cast<unsigned char>(id.front()))) id.erase(id.begin());
    } else if (trimmed.rfind("entry:", 0) == 0 && !id.empty()) {
      std::string entry = trimmed.substr(6);
      while (!entry.empty() && std::isspace(static_cast<unsigned char>(entry.front())))
        entry.erase(entry.begin());
      hooks[id] = entry;
      id.clear();
    }
  }
  return hooks;
}

static bool job_has_needs(const std::string &body) {
  for (const auto &line : split_lines(body)) {
    auto trimmed = line;
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front())))
      trimmed.erase(trimmed.begin());
    if (trimmed.rfind("needs:", 0) == 0) return true;
  }
  return false;
}

static bool job_needs_prepare(const std::string &body) {
  for (const auto &line : split_lines(body)) {
    auto trimmed = line;
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front())))
      trimmed.erase(trimmed.begin());
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

static std::string last_docker_stage(const std::string &text) {
  std::string cur;
  bool in_stage = false;
  for (const auto &line : split_lines(text)) {
    if (line.rfind("FROM ", 0) == 0) {
      cur.clear();
      in_stage = true;
      continue;
    }
    if (in_stage) {
      cur += line;
      cur += '\n';
    }
  }
  return cur;
}

static bool stage_installs_compiler(const std::string &stage) {
  bool in_install = false;
  for (const auto &raw : split_lines(stage)) {
    std::string line = raw;
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
    bool cont = !line.empty() && line.back() == '\\';
    if (cont) line.pop_back();
    if (line.find("apt-get") != std::string::npos && line.find("install") != std::string::npos) in_install = true;
    if (in_install && (line.find("g++") != std::string::npos || line.find("gcc") != std::string::npos ||
                       line.find("build-essential") != std::string::npos))
      return true;
    if (!cont) in_install = false;
  }
  return false;
}

static void test_quality_gate_wiring() {
  const std::string makefile = slurp_file("Makefile");
  const std::string precommit = slurp_file(".pre-commit-config.yaml");
  const std::string hook = slurp_file(".githooks/pre-commit");
  const std::string workflow = slurp_file(".gitea/workflows/ci.yml");
  const std::string fly = slurp_file("fly.toml");
  const std::string docker = slurp_file("Dockerfile");

  expect_contains(makefile, "cppcheck", "Makefile SAST invokes cppcheck");
  expect_contains(makefile, "osv-scanner", "Makefile dependency scan invokes osv-scanner");
  expect_contains(makefile, "gitleaks", "Makefile secrets invokes gitleaks");
  expect_contains(makefile, "clang-format", "Makefile style invokes clang-format");
  expect_contains(makefile, "scan source", "osv-scanner uses source scan");
  expect_contains(makefile, "-Werror", "first-party builds use -Werror");
  expect_contains(makefile, "-fsanitize=address,undefined", "test build uses ASan and UBSan");
  expect_contains(makefile, "-isystem vendor", "vendor headers are system includes");
  expect_contains(makefile, "test: api perf_test", "make test builds the production api");

  auto prod_at = makefile.find("PROD_CXXFLAGS");
  expect(prod_at != std::string::npos, "production flags are separate from the test flags");
  if (prod_at != std::string::npos) {
    auto end = makefile.find('\n', prod_at);
    std::string prod = makefile.substr(prod_at, end == std::string::npos ? std::string::npos : end - prod_at);
    expect(prod.find("-O2") != std::string::npos, "production flags are -O2");
    expect(prod.find("-s") != std::string::npos, "production flags strip debug info");
    expect(prod.find("fsanitize") == std::string::npos, "production flags do not enable sanitizers");
  }
  auto api_at = makefile.find("\napi:");
  auto perf_at = makefile.find("\nperf_test:", api_at == std::string::npos ? 0 : api_at);
  if (api_at != std::string::npos && perf_at != std::string::npos) {
    std::string api_rule = makefile.substr(api_at, perf_at - api_at);
    expect(api_rule.find("CAROLINA_TEST") == std::string::npos, "production api compile includes main");
    expect(api_rule.find("PROD_CXXFLAGS") != std::string::npos, "production api uses the optimized flags");
  }
  expect(makefile.find("-DCAROLINA_TEST") != std::string::npos, "perf_test compiles without production main");
  expect_contains(makefile, "check: test sast vuln secrets fmt-check", "make check runs every gate");

  expect_contains(fly, "auto_stop_machines = \"suspend\"", "fly idle stop is suspend");
  expect(fly.find("auto_stop_machines = \"stop\"") == std::string::npos, "fly does not full-stop idle machines");
  expect(fly.find("auto_stop_machines = \"off\"") == std::string::npos, "fly autostop stays enabled");
  expect_contains(fly, "auto_start_machines = true", "fly autostart stays on");
  expect_contains(fly, "memory = \"256mb\"", "fly memory stays 256mb");

  std::string runtime = last_docker_stage(docker);
  expect(runtime.find("libpq5") != std::string::npos, "runtime image installs libpq");
  expect(!stage_installs_compiler(runtime), "runtime image apt line has no compiler");

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
    expect_contains(prep, "libasan8", "prepare job installs the ASan runtime");
    expect_contains(prep, "libubsan1", "prepare job installs the UBSan runtime");
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
    expect(prep.find("postgresql") == std::string::npos, "prepare job does not install the postgres server");
  }
  if (jobs.count("test")) {
    expect(jobs["test"].find("postgresql") != std::string::npos,
           "test job installs PostgreSQL for the v1 fixture");
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
  expect(artifact.find("actions/download-artifact") == std::string::npos,
         "artifact helper is not a Node download action");

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

static std::string defined_c_string(const std::string &text, const char *macro) {
  std::string needle = std::string("#define ") + macro + " \"";
  auto at = text.find(needle);
  if (at == std::string::npos) return {};
  at += needle.size();
  auto end = text.find('"', at);
  if (end == std::string::npos) return {};
  return text.substr(at, end - at);
}

static void test_shipped_docs() {
  const std::string readme = slurp_file("README.md");
  const std::string agents = slurp_file("AGENTS.md");
  const std::string memory = slurp_file("MEMORY.md");
  const std::string decisions = slurp_file("DECISIONS.md");
  const std::string httplib = slurp_file("vendor/httplib.h");
  std::string ver = defined_c_string(httplib, "CPPHTTPLIB_VERSION");
  expect(!ver.empty(), "vendor/httplib.h defines CPPHTTPLIB_VERSION");
  expect_contains(readme, "C++17", "README states C++17");
  expect(!ver.empty() && readme.find(ver) != std::string::npos,
         "README states the vendored cpp-httplib version");
  expect_contains(readme, "libpq", "README names libpq");
  expect(readme.find("CRaC") == std::string::npos && readme.find("crac") == std::string::npos,
         "README does not claim CRaC");
  expect_contains(agents, "v1_*", "AGENTS.md requires v1_* views");
  expect_contains(agents, "Never query Ash", "AGENTS.md forbids Ash queries");
  expect_contains(agents, "Register once on boot", "AGENTS.md requires register-once");
  expect_contains(agents, "If `CAROLINA_URL` is empty or the POST fails, log and keep serving.",
                  "AGENTS.md keeps serving when CAROLINA_URL is empty or the POST fails");
  expect_contains(agents, "MEMORY.md", "AGENTS.md points at MEMORY.md");
  expect_contains(agents, "DECISIONS.md", "AGENTS.md points at DECISIONS.md");
  expect(!memory.empty(), "MEMORY.md is non-empty");
  expect(!decisions.empty(), "DECISIONS.md is non-empty");
}

static bool pg_ok(PGconn *c) { return c && PQstatus(c) == CONNECTION_OK; }

static bool pg_exec(PGconn *c, const char *sql) {
  PGresult *r = PQexec(c, sql);
  if (!r) {
    std::cerr << "PQexec failed: " << PQerrorMessage(c) << "\n";
    return false;
  }
  ExecStatusType st = PQresultStatus(r);
  bool ok = st == PGRES_COMMAND_OK || st == PGRES_TUPLES_OK;
  if (!ok) std::cerr << "SQL error: " << PQresultErrorMessage(r) << "\n";
  PQclear(r);
  return ok;
}

static std::string capture_line(const char *cmd) {
  FILE *pipe = popen(cmd, "r");
  if (!pipe) return {};
  std::string out;
  char buf[1024];
  while (std::fgets(buf, sizeof(buf), pipe)) out += buf;
  int status = pclose(pipe);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return {};
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
  auto nl = out.rfind('\n');
  if (nl != std::string::npos) out = out.substr(nl + 1);
  return out;
}

static std::string fixture_dsn_from_admin(const std::string &admin) {
  auto slash = admin.rfind('/');
  if (slash == std::string::npos) return {};
  auto q = admin.find('?', slash);
  if (q == std::string::npos) return admin.substr(0, slash) + "/carolina_cpp_api_test";
  return admin.substr(0, slash) + "/carolina_cpp_api_test" + admin.substr(q);
}

static bool apply_fixture(const std::string &admin, std::string *fixture) {
  PGconn *c = PQconnectdb(admin.c_str());
  if (!pg_ok(c)) {
    if (c) PQfinish(c);
    return false;
  }
  pg_exec(c, "SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
             "WHERE datname = 'carolina_cpp_api_test' AND pid <> pg_backend_pid()");
  if (!pg_exec(c, "DROP DATABASE IF EXISTS carolina_cpp_api_test") ||
      !pg_exec(c, "CREATE DATABASE carolina_cpp_api_test")) {
    PQfinish(c);
    return false;
  }
  PQfinish(c);

  *fixture = fixture_dsn_from_admin(admin);
  PGconn *db = PQconnectdb(fixture->c_str());
  if (!pg_ok(db)) {
    if (db) PQfinish(db);
    return false;
  }
  std::string sql = slurp_file("scripts/v1_fixture.sql");
  pg_exec(db, "SET client_min_messages TO warning");
  if (sql.empty() || !pg_exec(db, sql.c_str())) {
    PQfinish(db);
    return false;
  }
  PGresult *r = PQexec(db, "SELECT count(DISTINCT s.slug) FROM v1_speakers s "
                           "JOIN v1_talks t ON t.speaker_slug = s.slug WHERE t.year = 2026");
  bool enough = r && PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) == 1 && std::atoi(PQgetvalue(r, 0, 0)) >= 3;
  if (r) PQclear(r);
  PGresult *sponsors =
      PQexec(db, "SELECT count(*) FROM v1_year_sponsors WHERE year = 2026 AND slug IS NOT NULL");
  bool have_sponsor = sponsors && PQresultStatus(sponsors) == PGRES_TUPLES_OK && PQntuples(sponsors) == 1 &&
                      std::atoi(PQgetvalue(sponsors, 0, 0)) >= 1;
  if (sponsors) PQclear(sponsors);
  PQfinish(db);
  return enough && have_sponsor;
}

static std::string ensure_fixture() {
  const char *admin_candidates[] = {
      "postgres://postgres:postgres@127.0.0.1:5432/postgres",
      nullptr,
  };
  for (int i = 0; admin_candidates[i]; ++i) {
    std::string fixture;
    if (apply_fixture(admin_candidates[i], &fixture)) return fixture;
  }
  std::string started = capture_line("sh scripts/ensure-test-postgres.sh");
  if (started.empty()) return {};
  std::string fixture;
  if (!apply_fixture(started, &fixture)) return {};
  return fixture;
}

struct Reply {
  int status = 0;
  std::string body;
  std::string language;
};

struct Snap {
  int status = 0;
  bool shape = false;
};

static Reply http_get(int port, const std::string &path, int sec, int usec) {
  Reply r;
  httplib::Client cli("127.0.0.1", port);
  cli.set_connection_timeout(sec, usec);
  cli.set_read_timeout(sec, usec);
  auto res = cli.Get(path);
  if (!res) {
    r.status = -1;
    r.body = httplib::to_string(res.error());
    return r;
  }
  r.status = res->status;
  r.body = res->body;
  r.language = res->get_header_value("X-Polyglot-Language");
  return r;
}

static std::string json_string_field(const std::string &body, const char *key) {
  std::string pat = std::string("\"") + key + "\":\"";
  auto p = body.find(pat);
  if (p == std::string::npos) return {};
  p += pat.size();
  std::string out;
  while (p < body.size() && body[p] != '"') {
    if (body[p] == '\\' && p + 1 < body.size()) {
      out += body[p + 1];
      p += 2;
    } else {
      out += body[p++];
    }
  }
  return out;
}

static void record(std::map<std::string, Snap> *snaps, const std::string &path, const Reply &r, bool shape) {
  if (!snaps) return;
  Snap s;
  s.status = r.status;
  s.shape = shape;
  (*snaps)[path] = s;
}

static void exercise_routes(int port, std::map<std::string, Snap> *snaps) {
  const char *endpoints[] = {"/",
                             "/health",
                             "/v1/years",
                             "/v1/speakers",
                             "/v1/speakers/:slug",
                             "/v1/speakers/:year/:slug",
                             "/v1/sponsors",
                             "/v1/sponsors/:slug",
                             "/v1/sponsors/:year/:slug"};

  Reply root = http_get(port, "/", 2, 0);
  bool root_shape = root.status == 200 && root.language == "C++" && root.body.find("\"language\":\"C++\"") != std::string::npos;
  for (const char *ep : endpoints) {
    if (root.body.find(std::string("\"path\":\"") + ep + "\"") == std::string::npos) root_shape = false;
  }
  expect(root.status == 200, "GET / returns 200");
  expect(root.language == "C++", "GET / sends X-Polyglot-Language C++");
  expect(root.body.find("\"language\":\"C++\"") != std::string::npos, "GET / language is C++");
  expect(root_shape, "GET / lists every advertised route");
  record(snaps, "/", root, root_shape);

  Reply health = http_get(port, "/health", 2, 0);
  bool health_shape = health.status == 200 && health.language == "C++" && health.body.find("\"ok\":true") != std::string::npos;
  expect(health_shape, "GET /health ok true");
  record(snaps, "/health", health, health_shape);

  auto data_route = [&](const std::string &path) {
    Reply r = http_get(port, path, 2, 0);
    bool shape = r.status == 200 && r.language == "C++" && r.body.find("\"data\"") != std::string::npos;
    expect_s(shape, path + " returns 200 data");
    record(snaps, path, r, shape);
    return r;
  };

  data_route("/v1/years");
  data_route("/v1/speakers");
  Reply speakers = data_route("/v1/speakers?year=2026");
  std::string speaker = json_string_field(speakers.body, "slug");
  expect(!speaker.empty(), "year speaker list includes a slug");
  if (!speaker.empty()) {
    data_route("/v1/speakers/" + speaker);
    data_route("/v1/speakers/2026/" + speaker);
  }

  data_route("/v1/sponsors");
  Reply sponsors = data_route("/v1/sponsors?year=2026");
  std::string sponsor = json_string_field(sponsors.body, "slug");
  expect(!sponsor.empty(), "year sponsor list includes a slug");
  if (!sponsor.empty()) {
    data_route("/v1/sponsors/" + sponsor);
    data_route("/v1/sponsors/2026/" + sponsor);
  }

  Reply missing = http_get(port, "/no-such", 2, 0);
  bool missing_shape =
      missing.status == 404 && missing.language == "C++" && missing.body == "{\"error\":\"not_found\"}";
  expect(missing_shape, "unknown path returns 404 not_found");
  record(snaps, "/no-such", missing, missing_shape);
}

struct Sink {
  int fd = -1;
  int port = -1;
  Sink() = default;
  Sink(const Sink &) = delete;
  Sink &operator=(const Sink &) = delete;
  Sink(Sink &&o) noexcept : fd(o.fd), port(o.port) {
    o.fd = -1;
    o.port = -1;
  }
  Sink &operator=(Sink &&) = delete;
  ~Sink() {
    if (fd >= 0) close(fd);
  }
};

static Sink start_sink() {
  Sink s;
  s.fd = socket(AF_INET, SOCK_STREAM, 0);
  if (s.fd < 0) return s;
  int yes = 1;
  setsockopt(s.fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (bind(s.fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    close(s.fd);
    s.fd = -1;
    return s;
  }
  if (listen(s.fd, 64) != 0) {
    close(s.fd);
    s.fd = -1;
    return s;
  }
  fcntl(s.fd, F_SETFD, FD_CLOEXEC);
  socklen_t len = sizeof(addr);
  if (getsockname(s.fd, reinterpret_cast<sockaddr *>(&addr), &len) != 0) {
    close(s.fd);
    s.fd = -1;
    return s;
  }
  s.port = ntohs(addr.sin_port);
  return s;
}

static int free_port() {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    close(fd);
    return -1;
  }
  socklen_t len = sizeof(addr);
  if (getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len) != 0) {
    close(fd);
    return -1;
  }
  int port = ntohs(addr.sin_port);
  close(fd);
  return port;
}

struct ApiProc {
  pid_t pid = -1;
  ~ApiProc() { stop(); }
  ApiProc() = default;
  ApiProc(const ApiProc &) = delete;
  ApiProc &operator=(const ApiProc &) = delete;
  void stop() {
    if (pid <= 0) return;
    kill(pid, SIGKILL);
    int status = 0;
    waitpid(pid, &status, 0);
    pid = -1;
  }
};

static void dump_log(const std::string &path) {
  std::ifstream in(path);
  std::cerr << "---- api log ----\n"
            << in.rdbuf() << "---- end api log ----\n";
}

static bool wait_health(ApiProc *proc, int port, int budget_ms, int *elapsed_ms, Reply *health) {
  auto t0 = std::chrono::steady_clock::now();
  while (true) {
    int ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
    Reply r = http_get(port, "/health", 0, 300000);
    if (r.status == 200 && r.body.find("\"ok\":true") != std::string::npos) {
      *elapsed_ms = ms;
      *health = r;
      return ms < budget_ms;
    }
    if (ms >= budget_ms) {
      *elapsed_ms = ms;
      *health = r;
      return false;
    }
    int status = 0;
    pid_t got = waitpid(proc->pid, &status, WNOHANG);
    if (got == proc->pid) {
      proc->pid = -1;
      *elapsed_ms = ms;
      *health = r;
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}

static void launch_api(int port, const std::string &dsn, const std::string &carolina_url, const std::string &log_path,
                       std::map<std::string, Snap> *snaps) {
  ApiProc proc;
  pid_t pid = fork();
  if (pid < 0) {
    expect(false, "fork production api");
    return;
  }
  if (pid == 0) {
    int fd = open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
      if (dup2(fd, STDOUT_FILENO) < 0) _exit(127);
      if (dup2(fd, STDERR_FILENO) < 0) _exit(127);
    }
    if (setenv("PORT", std::to_string(port).c_str(), 1) != 0) _exit(127);
    if (setenv("DATABASE_URL", dsn.c_str(), 1) != 0) _exit(127);
    if (setenv("CAROLINA_URL", carolina_url.c_str(), 1) != 0) _exit(127);
    if (setenv("POLYGLOT_REGISTER_TOKEN", "dev", 1) != 0) _exit(127);
    std::string base = std::string("http://127.0.0.1:") + std::to_string(port);
    if (setenv("PUBLIC_BASE_URL", base.c_str(), 1) != 0) _exit(127);
    execl("./api", "./api", static_cast<char *>(nullptr));
    _exit(127);
  }
  proc.pid = pid;
  Reply health;
  int elapsed = -1;
  bool up = wait_health(&proc, port, 2000, &elapsed, &health);
  std::cerr << "production /health elapsed_ms=" << elapsed << "\n";
  expect(up, "black-hole CAROLINA_URL does not stall /health");
  expect(health.body.find("\"ok\":true") != std::string::npos, "production /health body has ok true");
  if (!up) dump_log(log_path);
  if (up) exercise_routes(port, snaps);
}

int main() {
  std::signal(SIGPIPE, SIG_IGN);
  test_quality_gate_wiring();
  test_shipped_docs();
  expect(access("api", X_OK) == 0, "production api binary is executable");

  expect(carolina_listen_family() == AF_INET6, "listen family is AF_INET6");
  expect(carolina_bind_ipv6() >= 0, "httplib binds to ::");

  carolina_set_connect_fn(fail_connect);
  carolina_reset_counts();
  PGconn *dead = carolina_db_acquire();
  carolina_db_release(dead);
  expect(dead == nullptr, "failed acquire returns null");
  expect(carolina_connect_count() == 1, "failed connect increments the connect count by 1");

  carolina_set_connect_fn(fake_connect);
  carolina_reset_counts();
  PGconn *a = carolina_db_acquire();
  carolina_db_release(a);
  PGconn *b = carolina_db_acquire();
  carolina_db_release(b);
  expect(a != nullptr && b != nullptr, "acquire returns a connection");
  expect(a == b, "later acquire reuses the live connection");
  expect(carolina_connect_count() == 1, "libpq connect count is not one-per-request");
  carolina_set_connect_fn(nullptr);

  carolina_reset_counts();
  char health[256];
  int hstatus = carolina_handle_health_copy(health, sizeof(health));
  expect(hstatus == 200, "/health returns 200");
  expect(std::strstr(health, "\"ok\":true") != nullptr, "/health body is ok JSON");
  expect(carolina_sql_count() == 0, "/health does not run SQL");
  expect(carolina_connect_count() == 0, "/health does not open Postgres");

  std::string fixture = ensure_fixture();
  expect(!fixture.empty(), "fixture database with v1 views is ready");
  if (!fixture.empty()) {
    expect(setenv("DATABASE_URL", fixture.c_str(), 1) == 0, "DATABASE_URL points at the fixture");
    carolina_init();
    std::cerr << "fixture dsn ready\n";

    expect(setenv("DATABASE_URL", "postgres://postgres:postgres@127.0.0.1:1/none?connect_timeout=2", 1) == 0,
           "bad DATABASE_URL is set");
    carolina_init();
    carolina_set_connect_fn(nullptr);
    carolina_reset_counts();
    PGconn *refused = carolina_db_acquire();
    carolina_db_release(refused);
    int refused_connects = carolina_connect_count();
    std::cerr << "refused-connect count=" << refused_connects << "\n";
    expect(refused == nullptr, "refused libpq acquire returns null");
    expect(refused_connects == 1, "real libpq failure increments the connect count by 1");

    expect(setenv("DATABASE_URL", fixture.c_str(), 1) == 0, "DATABASE_URL restored to the fixture");
    carolina_init();
    int before = carolina_connect_count();
    PGconn *live = carolina_db_acquire();
    expect(live != nullptr, "successful acquire returns the libpq connection");
    expect(carolina_connect_count() == before + 1, "successful acquire opens one connection");
    carolina_db_release(live);
    PGconn *again = carolina_db_acquire();
    carolina_db_release(again);
    expect(live == again, "next successful acquire reuses the live connection");
    expect(carolina_connect_count() == before + 1, "reused acquire does not connect again");

    carolina_set_connect_fn(nullptr);
    carolina_reset_counts();
    char body[1 << 20];
    int status = carolina_handle_speakers_year(2026, body, sizeof(body));
    int sql = carolina_sql_count();
    int speakers = count_talks_keys(body);
    std::cerr << "year list status=" << status << " sql=" << sql << " speakers=" << speakers
              << " connects=" << carolina_connect_count() << "\n";
    expect(status == 200, "year listing returns 200");
    expect(speakers >= 3, "year listing returns N>=3 speakers");
    expect(sql > 0, "listing runs SQL through shipped exec wrapper");
    expect(sql < 2 * speakers, "SQL count does not grow as ~2N");
    expect(sql <= 4, "year listing SQL is bounded (speakers + talks + years)");
    expect(carolina_connect_count() == 1, "year listing opens one postgres connection");
    carolina_reset_counts();
    int status2 = carolina_handle_speakers_year(2026, body, sizeof(body));
    expect(status2 == 200, "second catalog request succeeds");
    expect(carolina_connect_count() == 0, "second catalog request reuses pool (no extra connect)");
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

  if (!fixture.empty()) {
    carolina_set_connect_fn(nullptr);
    Sink sink = start_sink();
    expect(sink.port > 0, "registration sink is listening");
    std::string carolina_url = std::string("http://127.0.0.1:") + std::to_string(sink.port);
    char tmpl[] = "/tmp/carolina-cpp-api.XXXXXX";
    int logfd = mkstemp(tmpl);
    expect(logfd >= 0, "api log file created");
    if (logfd >= 0) close(logfd);
    std::string log_path = tmpl;

    std::map<std::string, Snap> first;
    std::map<std::string, Snap> second;
    int port1 = free_port();
    expect(port1 > 0, "first launch port");
    launch_api(port1, fixture, carolina_url, log_path, &first);
    int port2 = free_port();
    expect(port2 > 0, "second launch port");
    launch_api(port2, fixture, carolina_url, log_path, &second);

    expect(!first.empty() && first.size() == second.size(), "both launches recorded the same routes");
    for (const auto &kv : first) {
      auto it = second.find(kv.first);
      expect_s(it != second.end() && it->second.status == kv.second.status && it->second.shape && kv.second.shape,
               std::string("launch statuses match: ") + kv.first);
    }

    carolina_set_connect_fn(nullptr);
    carolina_reset_counts();
    int in_port = carolina_serve_start();
    expect(in_port > 0, "in-process server bound");
    if (in_port > 0) {
      Reply in_health = http_get(in_port, "/health", 2, 0);
      expect(in_health.status == 200 && in_health.body.find("\"ok\":true") != std::string::npos,
             "in-process /health ok true");
      expect(carolina_sql_count() == 0, "in-process /health does not run SQL");
      expect(carolina_connect_count() == 0, "in-process /health does not open Postgres");
      std::map<std::string, Snap> inproc;
      exercise_routes(in_port, &inproc);
      expect(inproc.size() == first.size(), "in-process server covers the same routes");
      carolina_serve_stop();
    }
    std::remove(log_path.c_str());
  }

  carolina_serve_stop();
  carolina_set_connect_fn(nullptr);

  if (g_failed) {
    std::cerr << "perf_test failed\n";
    return 1;
  }
  std::cerr << "perf_test passed\n";
  return 0;
}

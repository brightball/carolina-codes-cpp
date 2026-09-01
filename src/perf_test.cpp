#include "carolina.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <netinet/in.h>
#include <string>

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

  if (g_failed) {
    std::cerr << "perf_test failed\n";
    return 1;
  }
  std::cerr << "perf_test passed\n";
  return 0;
}

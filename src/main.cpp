#include "httplib.h"

#if defined(__has_include)
#if __has_include(<postgresql/libpq-fe.h>)
#include <postgresql/libpq-fe.h>
#else
#include <libpq-fe.h>
#endif
#else
#include <libpq-fe.h>
#endif

#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

const char *kLanguage = "C++";
const char *kApiVersion = "0.2.0";
const char *kFramework = "cpp-httplib";
const int kCreatedYear = 2026;
const int kSchemaVersion = 1;
const char *kLanguageVersion = __VERSION__;

const char *kSpeakerCols =
    "slug, first_name, last_name, name, tagline, bio, company, location, "
    "photo_path, twitter_url, linkedin_url, website_url, github_url, featured";
const char *kYearSponsorCols =
    "slug, name, website, logo_path, description, blurb, tier, featured, year, "
    "twitter_url, linkedin_url, youtube_url, instagram_url, facebook_url";
const char *kSponsorCols =
    "slug, name, website, logo_path, description, twitter_url, linkedin_url, "
    "youtube_url, instagram_url, facebook_url";
const char *kTalkSelect =
    "slug, title, description, format, youtube_id, year, speaker_slug, "
    "COALESCE(array_to_json(languages), '[]'::json)::text AS languages, "
    "COALESCE(array_to_json(topics), '[]'::json)::text AS topics";

std::string g_dsn;

constexpr int kPoolSize = 8;
std::mutex g_db_mu;
std::condition_variable g_db_cv;
PGconn *g_pool[kPoolSize]{};
bool g_busy[kPoolSize]{};
int g_sql_count = 0;
int g_connect_count = 0;
PGconn *(*g_connect_fn)(const char *) = nullptr;

std::string env_or(const char *key, const char *fallback) {
  const char *v = std::getenv(key);
  return (v && *v) ? std::string(v) : std::string(fallback);
}

struct J {
  std::string raw;
  static J n() { return J{"null"}; }
  static J b(bool v) { return J{v ? "true" : "false"}; }
  static J i(long long v) { return J{std::to_string(v)}; }
  static J raw_json(std::string s) { return J{std::move(s)}; }
  static J s(const char *v) {
    if (!v) return n();
    std::string o = "\"";
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(v); *p; ++p) {
      switch (*p) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
          if (*p < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", *p);
            o += buf;
          } else {
            o += static_cast<char>(*p);
          }
      }
    }
    o += '"';
    return J{std::move(o)};
  }
  static J s(const std::string &v) { return s(v.c_str()); }
  static J obj(std::vector<std::pair<std::string, J>> xs) {
    std::string o = "{";
    bool first = true;
    for (auto &kv : xs) {
      if (!first) o += ',';
      first = false;
      o += J::s(kv.first).raw;
      o += ':';
      o += kv.second.raw;
    }
    o += '}';
    return J{std::move(o)};
  }
  static J arr(const std::vector<J> &xs) {
    std::string o = "[";
    for (size_t i = 0; i < xs.size(); ++i) {
      if (i) o += ',';
      o += xs[i].raw;
    }
    o += ']';
    return J{std::move(o)};
  }
  static J ints(const std::vector<int> &xs) {
    std::vector<J> out;
    out.reserve(xs.size());
    for (int v : xs) out.push_back(i(v));
    return arr(out);
  }
  static J strs(const std::vector<std::string> &xs) {
    std::vector<J> out;
    out.reserve(xs.size());
    for (const auto &v : xs) out.push_back(s(v));
    return arr(out);
  }
};

const char *kEndpointsJson =
    "["
    "{\"method\":\"GET\",\"path\":\"/\",\"query\":[]},"
    "{\"method\":\"GET\",\"path\":\"/health\",\"query\":[]},"
    "{\"method\":\"GET\",\"path\":\"/v1/years\",\"query\":[]},"
    "{\"method\":\"GET\",\"path\":\"/v1/speakers\",\"query\":[\"year\"]},"
    "{\"method\":\"GET\",\"path\":\"/v1/speakers/:slug\",\"query\":[]},"
    "{\"method\":\"GET\",\"path\":\"/v1/speakers/:year/:slug\",\"query\":[]},"
    "{\"method\":\"GET\",\"path\":\"/v1/sponsors\",\"query\":[\"year\"]},"
    "{\"method\":\"GET\",\"path\":\"/v1/sponsors/:slug\",\"query\":[]},"
    "{\"method\":\"GET\",\"path\":\"/v1/sponsors/:year/:slug\",\"query\":[]}"
    "]";

bool conn_ok(PGconn *c) {
  if (!c) return false;
  if (g_connect_fn) return true;
  return PQstatus(c) == CONNECTION_OK;
}

PGconn *do_connect() {
  g_connect_count++;
  if (g_connect_fn) return g_connect_fn(g_dsn.c_str());
  return PQconnectdb(g_dsn.c_str());
}

PGconn *db_acquire() {
  std::unique_lock<std::mutex> lock(g_db_mu);
  for (;;) {
    int busy = 0;
    for (int i = 0; i < kPoolSize; ++i) {
      if (g_busy[i]) {
        busy++;
        continue;
      }
      if (!conn_ok(g_pool[i])) {
        if (g_pool[i] && !g_connect_fn) PQfinish(g_pool[i]);
        g_pool[i] = do_connect();
      }
      if (conn_ok(g_pool[i])) {
        g_busy[i] = true;
        return g_pool[i];
      }
      g_pool[i] = nullptr;
    }
    if (busy == 0) return nullptr;
    g_db_cv.wait(lock);
  }
}

void db_release(PGconn *c) {
  if (!c) return;
  std::lock_guard<std::mutex> lock(g_db_mu);
  for (int i = 0; i < kPoolSize; ++i) {
    if (g_pool[i] == c) {
      g_busy[i] = false;
      break;
    }
  }
  g_db_cv.notify_one();
}

void db_reset_pool() {
  std::lock_guard<std::mutex> lock(g_db_mu);
  for (int i = 0; i < kPoolSize; ++i) {
    if (g_pool[i] && !g_connect_fn) PQfinish(g_pool[i]);
    g_pool[i] = nullptr;
    g_busy[i] = false;
  }
}

struct Conn {
  PGconn *c;
  Conn() : c(db_acquire()) {}
  ~Conn() { db_release(c); }
  Conn(const Conn &) = delete;
  Conn &operator=(const Conn &) = delete;
  bool ok() const { return conn_ok(c); }
  const char *err() const {
    if (g_connect_fn) return "connect failed";
    return c ? PQerrorMessage(c) : "connect failed";
  }
};

struct Res {
  PGresult *r = nullptr;
  Res() = default;
  explicit Res(PGresult *rr) : r(rr) {}
  Res(const Res &) = delete;
  Res &operator=(const Res &) = delete;
  Res(Res &&o) noexcept : r(o.r) { o.r = nullptr; }
  ~Res() {
    if (r) PQclear(r);
  }
  bool ok() const {
    return r && (PQresultStatus(r) == PGRES_TUPLES_OK || PQresultStatus(r) == PGRES_COMMAND_OK);
  }
  const char *err() const { return r ? PQresultErrorMessage(r) : "no result"; }
  int n() const { return r ? PQntuples(r) : 0; }
};

Res exec_params(PGconn *c, const char *sql, const std::vector<std::string> &params) {
  g_sql_count++;
  std::vector<const char *> vals;
  vals.reserve(params.size());
  for (const auto &p : params) vals.push_back(p.c_str());
  return Res(PQexecParams(c, sql, static_cast<int>(vals.size()), nullptr,
                          vals.empty() ? nullptr : vals.data(), nullptr, nullptr, 0));
}

const char *col(PGresult *r, int row, const char *name) {
  int i = PQfnumber(r, name);
  if (i < 0 || PQgetisnull(r, row, i)) return nullptr;
  return PQgetvalue(r, row, i);
}

bool truthy(const char *v) { return v && (v[0] == 't' || v[0] == 'T' || v[0] == '1'); }

int as_int(const char *v, int fallback = 0) { return v ? std::atoi(v) : fallback; }

std::vector<std::string> parse_json_str_array(const char *raw) {
  std::vector<std::string> out;
  if (!raw || raw[0] != '[') return out;
  const char *p = raw + 1;
  while (*p) {
    while (*p && (*p == ' ' || *p == ',' || *p == '\n')) ++p;
    if (*p == ']') break;
    if (*p != '"') break;
    ++p;
    std::string cur;
    while (*p && *p != '"') {
      if (*p == '\\' && p[1]) {
        ++p;
        if (*p == 'n') cur += '\n';
        else if (*p == 't') cur += '\t';
        else cur += *p;
        ++p;
      } else {
        cur += *p++;
      }
    }
    if (*p == '"') ++p;
    if (!cur.empty()) out.push_back(cur);
  }
  return out;
}

std::vector<std::string> uniq(const std::vector<std::string> &xs) {
  std::vector<std::string> out;
  std::unordered_set<std::string> seen;
  for (const auto &x : xs) {
    if (x.empty() || seen.count(x)) continue;
    seen.insert(x);
    out.push_back(x);
  }
  return out;
}

J speaker_obj(PGresult *r, int row) {
  return J::obj({
      {"slug", J::s(col(r, row, "slug"))},
      {"first_name", J::s(col(r, row, "first_name"))},
      {"last_name", J::s(col(r, row, "last_name"))},
      {"name", J::s(col(r, row, "name"))},
      {"tagline", J::s(col(r, row, "tagline"))},
      {"bio", J::s(col(r, row, "bio"))},
      {"company", J::s(col(r, row, "company"))},
      {"location", J::s(col(r, row, "location"))},
      {"photo_path", J::s(col(r, row, "photo_path"))},
      {"twitter_url", J::s(col(r, row, "twitter_url"))},
      {"linkedin_url", J::s(col(r, row, "linkedin_url"))},
      {"website_url", J::s(col(r, row, "website_url"))},
      {"github_url", J::s(col(r, row, "github_url"))},
      {"featured", J::b(truthy(col(r, row, "featured")))},
  });
}

struct Talks {
  std::vector<J> items;
  std::vector<std::string> languages;
  std::vector<std::string> topics;
};

Talks load_talks(PGconn *c, const std::string &slug, const int *year) {
  Talks t;
  std::string sql = std::string("SELECT ") + kTalkSelect + " FROM v1_talks WHERE speaker_slug = $1";
  std::vector<std::string> params{slug};
  if (year) {
    sql += " AND year = $2";
    params.push_back(std::to_string(*year));
  }
  sql += " ORDER BY year DESC";
  Res res = exec_params(c, sql.c_str(), params);
  if (!res.ok()) return t;
  std::vector<std::string> langs, topics;
  for (int i = 0; i < res.n(); ++i) {
    auto lang = parse_json_str_array(col(res.r, i, "languages"));
    auto top = parse_json_str_array(col(res.r, i, "topics"));
    langs.insert(langs.end(), lang.begin(), lang.end());
    topics.insert(topics.end(), top.begin(), top.end());
    t.items.push_back(J::obj({
        {"slug", J::s(col(res.r, i, "slug"))},
        {"title", J::s(col(res.r, i, "title"))},
        {"description", J::s(col(res.r, i, "description"))},
        {"format", J::s(col(res.r, i, "format"))},
        {"youtube_id", J::s(col(res.r, i, "youtube_id"))},
        {"year", J::i(as_int(col(res.r, i, "year")))},
        {"speaker_slug", J::s(col(res.r, i, "speaker_slug"))},
        {"languages", J::strs(lang)},
        {"topics", J::strs(top)},
    }));
  }
  t.languages = uniq(langs);
  t.topics = uniq(topics);
  return t;
}

std::vector<int> talk_years(PGconn *c, const std::string &slug) {
  Res res = exec_params(c,
                        "SELECT DISTINCT year FROM v1_talks WHERE speaker_slug = $1 ORDER BY year DESC",
                        {slug});
  std::vector<int> years;
  if (!res.ok()) return years;
  for (int i = 0; i < res.n(); ++i) years.push_back(as_int(col(res.r, i, "year")));
  return years;
}

Talks talk_row(PGresult *r, int i) {
  Talks t;
  auto lang = parse_json_str_array(col(r, i, "languages"));
  auto top = parse_json_str_array(col(r, i, "topics"));
  t.languages = lang;
  t.topics = top;
  t.items.push_back(J::obj({
      {"slug", J::s(col(r, i, "slug"))},
      {"title", J::s(col(r, i, "title"))},
      {"description", J::s(col(r, i, "description"))},
      {"format", J::s(col(r, i, "format"))},
      {"youtube_id", J::s(col(r, i, "youtube_id"))},
      {"year", J::i(as_int(col(r, i, "year")))},
      {"speaker_slug", J::s(col(r, i, "speaker_slug"))},
      {"languages", J::strs(lang)},
      {"topics", J::strs(top)},
  }));
  return t;
}

void merge_talk(Talks &dst, Talks &&src) {
  dst.items.insert(dst.items.end(), src.items.begin(), src.items.end());
  dst.languages.insert(dst.languages.end(), src.languages.begin(), src.languages.end());
  dst.topics.insert(dst.topics.end(), src.topics.begin(), src.topics.end());
}

std::unordered_map<std::string, Talks> load_talks_for_year(PGconn *c, int year) {
  std::string sql = std::string("SELECT ") + kTalkSelect +
                    " FROM v1_talks WHERE year = $1 ORDER BY speaker_slug, year DESC";
  Res res = exec_params(c, sql.c_str(), {std::to_string(year)});
  std::unordered_map<std::string, Talks> by_slug;
  if (!res.ok()) return by_slug;
  for (int i = 0; i < res.n(); ++i) {
    const char *slug = col(res.r, i, "speaker_slug");
    if (!slug) continue;
    merge_talk(by_slug[slug], talk_row(res.r, i));
  }
  for (auto &kv : by_slug) {
    kv.second.languages = uniq(kv.second.languages);
    kv.second.topics = uniq(kv.second.topics);
  }
  return by_slug;
}

std::string pg_text_array(const std::vector<std::string> &slugs) {
  std::string arr = "{";
  for (size_t i = 0; i < slugs.size(); ++i) {
    if (i) arr += ',';
    arr += '"';
    arr += slugs[i];
    arr += '"';
  }
  arr += '}';
  return arr;
}

std::unordered_map<std::string, std::vector<int>> load_years_for_slugs(PGconn *c,
                                                                      const std::vector<std::string> &slugs) {
  std::unordered_map<std::string, std::vector<int>> by_slug;
  if (slugs.empty()) return by_slug;
  Res res = exec_params(c,
                        "SELECT DISTINCT speaker_slug, year FROM v1_talks WHERE speaker_slug = ANY($1::text[]) "
                        "ORDER BY speaker_slug, year DESC",
                        {pg_text_array(slugs)});
  if (!res.ok()) return by_slug;
  for (int i = 0; i < res.n(); ++i) {
    const char *slug = col(res.r, i, "speaker_slug");
    if (!slug) continue;
    by_slug[slug].push_back(as_int(col(res.r, i, "year")));
  }
  return by_slug;
}

J speaker_with_year(PGresult *r, int row, int y, const Talks &talks, const std::vector<int> &years) {
  std::string s = speaker_obj(r, row).raw;
  if (!s.empty() && s.back() == '}') s.pop_back();
  s += ",\"year\":" + std::to_string(y);
  s += ",\"talks\":" + J::arr(talks.items).raw;
  s += ",\"languages\":" + J::strs(talks.languages).raw;
  s += ",\"topics\":" + J::strs(talks.topics).raw;
  s += ",\"years\":" + J::ints(years).raw;
  s += "}";
  return J::raw_json(std::move(s));
}

bool list_speakers_year(PGconn *c, int y, std::vector<J> &rows) {
  std::string sql = std::string("SELECT ") + kSpeakerCols +
                    " FROM v1_speakers WHERE slug IN (SELECT speaker_slug FROM v1_talks WHERE year = $1) "
                    "ORDER BY last_name, first_name";
  Res q = exec_params(c, sql.c_str(), {std::to_string(y)});
  if (!q.ok()) return false;
  std::vector<std::string> slugs;
  slugs.reserve(static_cast<size_t>(q.n()));
  for (int i = 0; i < q.n(); ++i) {
    const char *slug = col(q.r, i, "slug");
    slugs.push_back(slug ? slug : "");
  }
  auto talks_by = load_talks_for_year(c, y);
  auto years_by = load_years_for_slugs(c, slugs);
  rows.clear();
  rows.reserve(static_cast<size_t>(q.n()));
  Talks empty_talks;
  std::vector<int> empty_years;
  for (int i = 0; i < q.n(); ++i) {
    const std::string &slug = slugs[static_cast<size_t>(i)];
    const Talks &talks = talks_by.count(slug) ? talks_by[slug] : empty_talks;
    const std::vector<int> &years = years_by.count(slug) ? years_by[slug] : empty_years;
    rows.push_back(speaker_with_year(q.r, i, y, talks, years));
  }
  return true;
}

std::vector<int> sponsor_years(PGconn *c, const std::string &slug) {
  Res res = exec_params(c,
                        "SELECT DISTINCT year FROM v1_sponsorships WHERE sponsor_slug = $1 ORDER BY year DESC",
                        {slug});
  std::vector<int> years;
  if (!res.ok()) return years;
  for (int i = 0; i < res.n(); ++i) years.push_back(as_int(col(res.r, i, "year")));
  return years;
}

std::vector<int> except_year(const std::vector<int> &years, int year) {
  std::vector<int> out;
  for (int y : years)
    if (y != year) out.push_back(y);
  return out;
}

J year_sponsor_obj(PGresult *r, int row) {
  return J::obj({
      {"slug", J::s(col(r, row, "slug"))},
      {"name", J::s(col(r, row, "name"))},
      {"website", J::s(col(r, row, "website"))},
      {"logo_path", J::s(col(r, row, "logo_path"))},
      {"description", J::s(col(r, row, "description"))},
      {"blurb", J::s(col(r, row, "blurb"))},
      {"tier", J::s(col(r, row, "tier"))},
      {"featured", J::b(truthy(col(r, row, "featured")))},
      {"year", J::i(as_int(col(r, row, "year")))},
      {"twitter_url", J::s(col(r, row, "twitter_url"))},
      {"linkedin_url", J::s(col(r, row, "linkedin_url"))},
      {"youtube_url", J::s(col(r, row, "youtube_url"))},
      {"instagram_url", J::s(col(r, row, "instagram_url"))},
      {"facebook_url", J::s(col(r, row, "facebook_url"))},
  });
}

J sponsor_obj(PGresult *r, int row) {
  return J::obj({
      {"slug", J::s(col(r, row, "slug"))},
      {"name", J::s(col(r, row, "name"))},
      {"website", J::s(col(r, row, "website"))},
      {"logo_path", J::s(col(r, row, "logo_path"))},
      {"description", J::s(col(r, row, "description"))},
      {"twitter_url", J::s(col(r, row, "twitter_url"))},
      {"linkedin_url", J::s(col(r, row, "linkedin_url"))},
      {"youtube_url", J::s(col(r, row, "youtube_url"))},
      {"instagram_url", J::s(col(r, row, "instagram_url"))},
      {"facebook_url", J::s(col(r, row, "facebook_url"))},
  });
}

J sponsorships_for(PGconn *c, const std::string &slug) {
  Res res = exec_params(c,
                        "SELECT sponsor_slug, year, tier, blurb, featured FROM v1_sponsorships "
                        "WHERE sponsor_slug = $1 ORDER BY year DESC",
                        {slug});
  std::vector<J> items;
  if (!res.ok()) return J::arr(items);
  for (int i = 0; i < res.n(); ++i) {
    items.push_back(J::obj({
        {"sponsor_slug", J::s(col(res.r, i, "sponsor_slug"))},
        {"year", J::i(as_int(col(res.r, i, "year")))},
        {"tier", J::s(col(res.r, i, "tier"))},
        {"blurb", J::s(col(res.r, i, "blurb"))},
        {"featured", J::b(truthy(col(res.r, i, "featured")))},
    }));
  }
  return J::arr(items);
}

void send_json(httplib::Response &res, const J &body, int status = 200) {
  res.status = status;
  res.set_content(body.raw, "application/json");
}

void send_error(httplib::Response &res, int status, const char *code) {
  send_json(res, J::obj({{"error", J::s(code)}}), status);
}

J wrap_data(const std::vector<J> &rows) { return J::obj({{"data", J::arr(rows)}}); }

J wrap_data(J row) { return J::obj({{"data", std::move(row)}}); }

J health_json() { return J::obj({{"ok", J::b(true)}}); }

J identity() {
  return J::obj({
      {"language", J::s(kLanguage)},
      {"language_version", J::s(kLanguageVersion)},
      {"api_version", J::s(kApiVersion)},
      {"framework", J::s(kFramework)},
      {"created_year", J::i(kCreatedYear)},
      {"schema_version", J::i(kSchemaVersion)},
      {"endpoints", J::raw_json(kEndpointsJson)},
  });
}

void register_with_elixir(const std::string &port) {
  const char *url = std::getenv("CAROLINA_URL");
  const char *token = std::getenv("POLYGLOT_REGISTER_TOKEN");
  if (!url || !*url || !token || !*token) return;
  try {
    std::string origin = url;
    // cpp-httplib is built without OpenSSL; Fly 6PN HTTP is enough to register.
    if (origin.rfind("https://", 0) == 0) {
      origin = "http://carolina-codes.internal:8080";
    }
    std::string base = env_or("PUBLIC_BASE_URL", ("http://127.0.0.1:" + port).c_str());
    J body = J::obj({
        {"language", J::s(kLanguage)},
        {"language_version", J::s(kLanguageVersion)},
        {"api_version", J::s(kApiVersion)},
        {"framework", J::s(kFramework)},
        {"created_year", J::i(kCreatedYear)},
        {"schema_version", J::i(kSchemaVersion)},
        {"base_url", J::s(base)},
        {"endpoints", J::raw_json(kEndpointsJson)},
    });
    httplib::Headers headers{{"Authorization", std::string("Bearer ") + token}};
    httplib::Client cli(origin);
    cli.set_connection_timeout(5, 0);
    cli.set_read_timeout(5, 0);
    auto resp = cli.Post("/internal/api-endpoints/register", headers, body.raw, "application/json");
    if (resp) {
      std::cerr << "registered with elixir: " << resp->status << "\n";
    } else {
      std::cerr << "register: " << httplib::to_string(resp.error()) << "\n";
    }
  } catch (const std::exception &e) {
    std::cerr << "register: " << e.what() << "\n";
  }
}

}  // namespace

void carolina_init() {
  g_dsn = env_or("DATABASE_URL", "postgres://postgres:postgres@127.0.0.1:5432/carolina_dev");
}

void carolina_reset_counts() {
  g_sql_count = 0;
  g_connect_count = 0;
}

int carolina_sql_count() { return g_sql_count; }

int carolina_connect_count() { return g_connect_count; }

void carolina_set_connect_fn(PGconn *(*fn)(const char *)) {
  db_reset_pool();
  g_connect_fn = fn;
}

PGconn *carolina_db_acquire() { return db_acquire(); }

void carolina_db_release(PGconn *c) { db_release(c); }

int carolina_listen_family() { return AF_INET6; }

int carolina_handle_health_copy(char *buf, size_t buflen) {
  J body = health_json();
  if (buf && buflen) std::snprintf(buf, buflen, "%s", body.raw.c_str());
  return 200;
}

int carolina_handle_speakers_year(int year, char *buf, size_t buflen) {
  Conn db;
  if (!db.ok()) {
    if (buf && buflen) std::snprintf(buf, buflen, "%s", "{\"error\":\"connect failed\"}");
    return 500;
  }
  std::vector<J> rows;
  if (!list_speakers_year(db.c, year, rows)) {
    if (buf && buflen) std::snprintf(buf, buflen, "%s", "{\"error\":\"query failed\"}");
    return 500;
  }
  J payload = wrap_data(rows);
  if (buf && buflen) std::snprintf(buf, buflen, "%s", payload.raw.c_str());
  return 200;
}

int carolina_bind_ipv6() {
  httplib::Server svr;
  int port = svr.bind_to_port("::", 0);
  return port;
}

#ifndef CAROLINA_TEST
int main() {
  g_dsn = env_or("DATABASE_URL", "postgres://postgres:postgres@127.0.0.1:5432/carolina_dev");
  std::string port_s = env_or("PORT", "4009");
  int port = std::atoi(port_s.c_str());
  if (port <= 0) port = 4009;

  httplib::Server svr;
  svr.set_pre_routing_handler([](const httplib::Request &, httplib::Response &res) {
    res.set_header("X-Polyglot-Language", kLanguage);
    res.set_header("X-Polyglot-Framework", kFramework);
    return httplib::Server::HandlerResponse::Unhandled;
  });
  svr.set_error_handler([](const httplib::Request &, httplib::Response &res) {
    if (res.status == 404 && res.body.empty()) {
      res.set_content("{\"error\":\"not_found\"}", "application/json");
    }
  });

  svr.Get("/", [](const httplib::Request &, httplib::Response &res) { send_json(res, identity()); });
  svr.Get("/health", [](const httplib::Request &, httplib::Response &res) { send_json(res, health_json()); });

  svr.Get("/v1/years", [](const httplib::Request &, httplib::Response &res) {
    Conn db;
    if (!db.ok()) return send_error(res, 500, db.err());
    Res q = exec_params(db.c, "SELECT year, slug, name, status FROM v1_years ORDER BY year DESC", {});
    if (!q.ok()) return send_error(res, 500, q.err());
    std::vector<J> rows;
    for (int i = 0; i < q.n(); ++i) {
      rows.push_back(J::obj({
          {"year", J::i(as_int(col(q.r, i, "year")))},
          {"slug", J::s(col(q.r, i, "slug"))},
          {"name", J::s(col(q.r, i, "name"))},
          {"status", J::s(col(q.r, i, "status"))},
      }));
    }
    send_json(res, wrap_data(rows));
  });

  svr.Get("/v1/speakers", [](const httplib::Request &req, httplib::Response &res) {
    Conn db;
    if (!db.ok()) return send_error(res, 500, db.err());
    std::string year = req.get_param_value("year");
    if (!year.empty()) {
      int y = std::atoi(year.c_str());
      std::vector<J> rows;
      if (!list_speakers_year(db.c, y, rows)) return send_error(res, 500, "query failed");
      return send_json(res, wrap_data(rows));
    }
    std::string sql = std::string("SELECT ") + kSpeakerCols + " FROM v1_speakers ORDER BY last_name, first_name";
    Res q = exec_params(db.c, sql.c_str(), {});
    if (!q.ok()) return send_error(res, 500, q.err());
    std::vector<J> rows;
    for (int i = 0; i < q.n(); ++i) rows.push_back(speaker_obj(q.r, i));
    send_json(res, wrap_data(rows));
  });

  svr.Get(R"(/v1/speakers/(\d+)/([^/]+))", [](const httplib::Request &req, httplib::Response &res) {
    Conn db;
    if (!db.ok()) return send_error(res, 500, db.err());
    int y = std::atoi(req.matches[1].str().c_str());
    std::string slug = req.matches[2].str();
    std::string sql = std::string("SELECT ") + kSpeakerCols + " FROM v1_speakers WHERE slug = $1";
    Res q = exec_params(db.c, sql.c_str(), {slug});
    if (!q.ok()) return send_error(res, 500, q.err());
    if (q.n() == 0) return send_error(res, 404, "not_found");
    Talks talks = load_talks(db.c, slug, &y);
    if (talks.items.empty()) return send_error(res, 404, "not_found");
    auto years = talk_years(db.c, slug);
    std::string s = speaker_obj(q.r, 0).raw;
    if (!s.empty() && s.back() == '}') s.pop_back();
    s += ",\"year\":" + std::to_string(y);
    s += ",\"years\":" + J::ints(years).raw;
    s += ",\"other_years\":" + J::ints(except_year(years, y)).raw;
    s += ",\"talks\":" + J::arr(talks.items).raw;
    s += ",\"languages\":" + J::strs(talks.languages).raw;
    s += ",\"topics\":" + J::strs(talks.topics).raw;
    s += "}";
    send_json(res, wrap_data(J::raw_json(s)));
  });

  svr.Get(R"(/v1/speakers/([^/]+))", [](const httplib::Request &req, httplib::Response &res) {
    Conn db;
    if (!db.ok()) return send_error(res, 500, db.err());
    std::string slug = req.matches[1].str();
    std::string sql = std::string("SELECT ") + kSpeakerCols + " FROM v1_speakers WHERE slug = $1";
    Res q = exec_params(db.c, sql.c_str(), {slug});
    if (!q.ok()) return send_error(res, 500, q.err());
    if (q.n() == 0) return send_error(res, 404, "not_found");
    Talks talks = load_talks(db.c, slug, nullptr);
    auto years = talk_years(db.c, slug);
    std::string s = speaker_obj(q.r, 0).raw;
    if (!s.empty() && s.back() == '}') s.pop_back();
    s += ",\"talks\":" + J::arr(talks.items).raw;
    s += ",\"years\":" + J::ints(years).raw;
    s += "}";
    send_json(res, wrap_data(J::raw_json(s)));
  });

  svr.Get("/v1/sponsors", [](const httplib::Request &req, httplib::Response &res) {
    Conn db;
    if (!db.ok()) return send_error(res, 500, db.err());
    std::string year = req.get_param_value("year");
    if (!year.empty()) {
      std::string sql = std::string("SELECT ") + kYearSponsorCols +
                        " FROM v1_year_sponsors WHERE year = $1 ORDER BY name";
      Res q = exec_params(db.c, sql.c_str(), {std::to_string(std::atoi(year.c_str()))});
      if (!q.ok()) return send_error(res, 500, q.err());
      std::vector<J> rows;
      for (int i = 0; i < q.n(); ++i) rows.push_back(year_sponsor_obj(q.r, i));
      return send_json(res, wrap_data(rows));
    }
    std::string sql = std::string("SELECT ") + kSponsorCols + " FROM v1_sponsors ORDER BY name";
    Res q = exec_params(db.c, sql.c_str(), {});
    if (!q.ok()) return send_error(res, 500, q.err());
    std::vector<J> rows;
    for (int i = 0; i < q.n(); ++i) rows.push_back(sponsor_obj(q.r, i));
    send_json(res, wrap_data(rows));
  });

  svr.Get(R"(/v1/sponsors/(\d+)/([^/]+))", [](const httplib::Request &req, httplib::Response &res) {
    Conn db;
    if (!db.ok()) return send_error(res, 500, db.err());
    int y = std::atoi(req.matches[1].str().c_str());
    std::string slug = req.matches[2].str();
    std::string sql = std::string("SELECT ") + kYearSponsorCols +
                      " FROM v1_year_sponsors WHERE year = $1 AND slug = $2";
    Res q = exec_params(db.c, sql.c_str(), {std::to_string(y), slug});
    if (!q.ok()) return send_error(res, 500, q.err());
    if (q.n() == 0) return send_error(res, 404, "not_found");
    auto years = sponsor_years(db.c, slug);
    std::string s = year_sponsor_obj(q.r, 0).raw;
    if (!s.empty() && s.back() == '}') s.pop_back();
    s += ",\"years\":" + J::ints(years).raw;
    s += ",\"other_years\":" + J::ints(except_year(years, y)).raw;
    s += "}";
    send_json(res, wrap_data(J::raw_json(s)));
  });

  svr.Get(R"(/v1/sponsors/([^/]+))", [](const httplib::Request &req, httplib::Response &res) {
    Conn db;
    if (!db.ok()) return send_error(res, 500, db.err());
    std::string slug = req.matches[1].str();
    std::string sql = std::string("SELECT ") + kSponsorCols + " FROM v1_sponsors WHERE slug = $1";
    Res q = exec_params(db.c, sql.c_str(), {slug});
    if (!q.ok()) return send_error(res, 500, q.err());
    if (q.n() == 0) return send_error(res, 404, "not_found");
    std::string s = sponsor_obj(q.r, 0).raw;
    if (!s.empty() && s.back() == '}') s.pop_back();
    s += ",\"sponsorships\":" + sponsorships_for(db.c, slug).raw;
    s += "}";
    send_json(res, wrap_data(J::raw_json(s)));
  });

  std::thread([port_s] {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    register_with_elixir(port_s);
  }).detach();

  std::cerr << "carolina-codes-cpp listening on :" << port << "\n";
  if (!svr.listen("::", port)) {
    std::cerr << "listen failed\n";
    return 1;
  }
  return 0;
}
#endif

# Decisions

Durable choices for this C++ API. Each record is status, context, decision, and consequences (Nygard). Git history is the changelog: add a new record that supersedes an old one, and update [MEMORY.md](MEMORY.md) in the same change. C++ and vendored cpp-httplib have no framework memory tool, so this file plus the short index is the record.

<a id="d1"></a>
## D1. C++17, cpp-httplib, and libpq

- Status: accepted
- Date: 2026-08-28

Context: The language starter ships a contract and a replaceable runtime. This repo is the C++ sibling that serves the read-only v1 API.

Decision: Write the server to C++17 (`-std=c++17` in the Makefile). HTTP is the vendored header `vendor/httplib.h` (cpp-httplib). The version is `CPPHTTPLIB_VERSION` in that header, 0.18.3 as of this record, and the README must name the same string. The header is compiled without OpenSSL. SQL is libpq via `pkg-config`. Identity `api_version` is `0.2.0`. `language_version` is the compiler `__VERSION__` at build. There is no pinned g++ release.

Consequences: osv-scanner has no library lockfile to read; it scans source, including the vendored header. A cpp-httplib bump replaces `vendor/httplib.h` and updates the README (and MEMORY.md) to the new `CPPHTTPLIB_VERSION`. The Fly runtime image needs `libpq5` and the binary.

<a id="d2"></a>
## D2. Query v1 views, never Ash tables

- Status: accepted
- Date: 2026-08-28

Context: The Phoenix app falls back to Ash when no polyglot API is registered. The public contract is ordinary JSON over SQL views defined in the CMS database.

Decision: `SELECT` only from `v1_*` views. This server reads `v1_years`, `v1_speakers`, `v1_talks`, `v1_sponsors`, `v1_year_sponsors`, and `v1_sponsorships`. Never query Ash tables (`speakers`, `organizations`, `talks`, and the other base tables). Responses use `application/json`. The CMS owns the live view definitions (`priv/api/openapi.yaml`). Tests apply `scripts/v1_fixture.sql` instead of shipping `db/*.sql`.

Consequences: Column changes land in the CMS views first. This API follows the columns it already selects. `photo_path` and `logo_path` are returned as stored; this repo does not serve image files.

<a id="d3"></a>
## D3. Register once and keep serving

- Status: accepted
- Date: 2026-08-28

Context: Elixir keeps at most one language API warm and keep-alives that process. Registration is how this API becomes that target. The CMS is often down during local work.

Decision: `POST {CAROLINA_URL}/internal/api-endpoints/register` once, on a thread that is not the listen thread, after the socket binds. The body carries language, language_version, api_version, framework, created_year, base_url (`PUBLIC_BASE_URL`), schema_version 1, and endpoints. The bearer token is `POLYGLOT_REGISTER_TOKEN`. If `CAROLINA_URL` is empty or the POST fails, log and keep serving. There is no heartbeat. cpp-httplib here has no OpenSSL, so an `https://` `CAROLINA_URL` is sent to `http://carolina-codes.internal:8080` (the Fly 6PN HTTP origin already set in `fly.toml`).

Consequences: A missing CMS does not stop HTTP. `GET /health` stays available while registration blocks or fails. Adding OpenSSL solely to register is a new decision.

<a id="d4"></a>
## D4. Quality gates, pre-commit, and one Gitea prepare

- Status: accepted
- Date: 2026-09-15

Context: The same checks have to run before a commit and in CI. Installing g++, sanitizer runtimes, cppcheck, osv-scanner, gitleaks, and clang-format on every job repeated the same apt and download steps.

Decision: `make test`, `make sast` (cppcheck on `src/` only), `make vuln` (osv-scanner source scan), `make secrets` (gitleaks), and `make fmt-check` (clang-format). `make check` runs all five. `.githooks/pre-commit` runs them through the pre-commit runner when it is installed, and falls back to the same Makefile targets. Gitea (`.gitea/workflows/ci.yml`) has a `prepare` job that installs that toolset once, packs apt debs and `/usr/local`, and publishes the tarball. Jobs `test`, `sast`, `vuln`, `secrets`, and `fmt` restore the archive and run one target. The test job also installs PostgreSQL so it can apply `scripts/v1_fixture.sql`. Check jobs clone `GITHUB_SHA` over HTTPS with the job token.

Consequences: A new gate is added to the Makefile, the pre-commit config, the hook fallback, and a Gitea job that `needs: prepare`. Shared tools go into the prepare archive. PostgreSQL stays on the test job only.

<a id="d5"></a>
## D5. Fly IPv6 listen, batched SQL, and a stripped runtime

- Status: accepted
- Date: 2026-09-01

Context: Fly 6PN reaches the process over IPv6. Cold start and a 256 MB machine cannot afford sanitizers, a compiler in the runtime image, or one catalog query per speaker. Idle machines should suspend.

Decision: Listen on `::` (`AF_INET6`). `GET /health` returns `{"ok":true}` and does not open Postgres. Catalog SQL uses one libpq pool of 8 connections. A year speaker listing batches speakers, talks, and years. Production `./api` is `-O2` and stripped (`-s`) with no sanitizers. AddressSanitizer and UndefinedBehaviorSanitizer are linked only into `./perf_test`, which `make test` builds alongside `./api`. The runtime image is Debian bookworm with `libpq5` and the binary. `fly.toml` sets `auto_stop_machines = "suspend"`, `memory = "256mb"`, and checks `GET /health`.

Consequences: Binding only `0.0.0.0` breaks 6PN. Sanitizers on the Fly link slow cold start. Per-speaker queries blow the SQL bound the suite enforces. Switching suspend to a full stop is a new decision. CI's database is PostgreSQL 15 from bookworm's `postgresql` package.

<a id="d6"></a>
## D6. MIT license

- Status: accepted
- Date: 2026-10-07

Context: The tree needed a license that matches the public polyglot siblings.

Decision: MIT License, copyright 2026 Brightball, in `LICENSE`.

Consequences: Keep that copyright notice and permission text with substantial copies of the software.

<a id="d7"></a>
## D7. Decision records in the repo

- Status: accepted
- Date: 2026-10-07

Context: Later sessions need the stack, view, register, gate, and Fly choices without replaying chat. C++ and cpp-httplib provide no project memory command, ADR generator, or `.agents` store.

Decision: Keep accepted choices in this file, one Nygard record each. Keep `MEMORY.md` as a short index of facts that are still true, with links to the records. When a durable decision changes, add a superseding record and update the index in the same change. Do not rewrite history in place, and do not store session transcripts or secrets here.

Consequences: Agents read `MEMORY.md` first, then the linked record, then the code. `AGENTS.md` points at both files.

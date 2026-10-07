# carolina-codes-cpp

Read-only v1 HTTP API in C++ that a Carolina Code Conference Elixir site can rotate onto. This repository is a finished sibling of the language starter. Install, run, and version details are in `README.md`.

The Phoenix app (`Carolina.Polyglot`) keeps at most one language API warm and reads speakers and sponsors from it. With no APIs registered, it falls back to Ash. This process:

1. Queries PostgreSQL `v1_*` views only. Never query Ash tables.
2. Exposes the v1 REST routes below as ordinary JSON. The route contract is the CMS files `priv/api/openapi.yaml` and `priv/api/AGENTS.md` on `github.com/brightball/carolina-codes`.
3. Register once on boot with the Elixir site (no heartbeat). If `CAROLINA_URL` is empty or the POST fails, log and keep serving.

Siblings speak ordinary JSON. The media type is `application/json`.

A checkout of the Elixir CMS is optional for build and test. Handler tests that use a fake catalog do not need Postgres. This tree stays its own git remote. Treat this repo as the workspace root.

## Decision memory

C++ with vendored cpp-httplib has no framework agent-memory tool. Durable choices live in `DECISIONS.md` as Nygard records: status, context, decision, consequences, one record per choice. `MEMORY.md` is a short index of current facts and points at those records. Git history is the changelog. When a durable decision changes, add a superseding record and update `MEMORY.md` in the same change. Leave secrets and session transcripts out of both files.

## Environment

| Variable | Example | Role |
|---|---|---|
| `DATABASE_URL` | `postgres://postgres:postgres@127.0.0.1:5432/carolina_dev` | SQL views |
| `CAROLINA_URL` | `http://127.0.0.1:4000` | Elixir site (optional; register no-ops if down) |
| `POLYGLOT_REGISTER_TOKEN` | `dev` | Bearer token for register |
| `PUBLIC_BASE_URL` | `http://127.0.0.1:4009` | URL Elixir will call |
| `PORT` | `4009` | Listen port (Fly sets `8080`) |

`./api` defaults `PORT` to 4009 and `DATABASE_URL` to the example above.

## SQL views (query these)

Query only PostgreSQL `v1_*` views. This server reads `v1_years`, `v1_speakers`, `v1_talks`, `v1_sponsors`, `v1_year_sponsors`, and `v1_sponsorships`.

The views live in the CMS database. `scripts/v1_fixture.sql` is a minimal copy the tests apply. Year-scoped speaker rows include `languages` and `topics` from `v1_talks`. Year-scoped sponsor rows include `tier` and `blurb`.

Never query Ash tables (`speakers`, `organizations`, `talks`, and the other base tables). The views are the API.

CI (`.gitea/workflows/ci.yml` on `debian:bookworm-slim`) installs Debian bookworm's `postgresql` package, which is PostgreSQL 15, and applies the fixture. That is the server version this repo runs.

## Required HTTP routes

List payloads are `{ "data": [ ... ] }`. A single resource is `{ "data": { ... } }`. Unknown slugs and unknown paths return 404 `{"error":"not_found"}`.

- `GET /health` — liveness `{"ok":true}`. It does not open Postgres.
- `GET /` — identity (`language`, `language_version`, `api_version`, `framework`, `created_year`, `schema_version`, `endpoints`)
- `GET /v1/years`
- `GET /v1/speakers` and `GET /v1/speakers?year=`
- `GET /v1/speakers/{slug}` and `GET /v1/speakers/{year}/{slug}`
- `GET /v1/sponsors` and `GET /v1/sponsors?year=`
- `GET /v1/sponsors/{slug}` and `GET /v1/sponsors/{year}/{slug}`

`language` is `C++`. `framework` is `cpp-httplib`. `api_version` is `0.2.0`. `language_version` is the compiler `__VERSION__` captured at build. `schema_version` is 1.

`photo_path` and `logo_path` are web paths from the view. Return the path. The CMS hosts the bytes.

Responses set `X-Polyglot-Language` and `X-Polyglot-Framework`.

## Register on boot (once)

`POST {CAROLINA_URL}/internal/api-endpoints/register`

```
Authorization: Bearer {POLYGLOT_REGISTER_TOKEN}
Content-Type: application/json
```

Body fields: `language`, `language_version`, `api_version`, `framework`, `created_year`, `base_url` (`PUBLIC_BASE_URL`), `schema_version` (1), `endpoints`.

The POST runs once, on a thread that is not the listen thread, after the socket binds. A black-hole `CAROLINA_URL` must not stall `GET /health`.

If `CAROLINA_URL` is empty or the POST fails, log and keep serving. Elixir keep-alives the currently warm API.

This cpp-httplib build has no OpenSSL. Fly sets `CAROLINA_URL` to `http://carolina-codes.internal:8080`. An `https://` value is sent to that private HTTP origin.

## Build and runtime

- The tree is written to C++17. Production `./api` uses `-O2` and `-s`. AddressSanitizer and UndefinedBehaviorSanitizer are linked only into `./perf_test`.
- HTTP is vendored cpp-httplib. The version is `CPPHTTPLIB_VERSION` in `vendor/httplib.h` (0.18.3). The header is a system include (`-isystem vendor`). cppcheck covers `src/`.
- SQL goes through libpq and one pool of 8 connections. A year speaker listing batches speakers, talks, and years. Keep that query bound.
- The listen address is `::` (`AF_INET6`) so Fly 6PN can connect.
- `Dockerfile` is Debian bookworm. The build stage has `g++` and `libpq-dev`. The runtime stage has `libpq5` and the stripped binary.
- `fly.toml` suspends idle machines (`auto_stop_machines = "suspend"`) at 256 MB. The HTTP check is `GET /health`.

## Quality gates

The same commands run locally, in `.githooks/pre-commit`, and as parallel Gitea jobs (`.gitea/workflows/ci.yml`):

- `make test` — `./perf_test` under ASan+UBSan; also builds stripped `-O2` `./api`
- `make sast` — cppcheck on `src/`
- `make vuln` — osv-scanner source scan
- `make secrets` — gitleaks
- `make fmt-check` — clang-format on `src/main.cpp`, `src/perf_test.cpp`, `src/carolina.h`
- `make check` — all of the above

Gitea `prepare` installs the toolset once and publishes a tarball. `test`, `sast`, `vuln`, `secrets`, and `fmt` restore it and run one target. The test job also installs PostgreSQL for `scripts/v1_fixture.sql`. `make hooks` points `core.hooksPath` at `.githooks`.

## Layout

| Path | Role |
|---|---|
| `src/main.cpp`, `src/carolina.h` | Server, SQL, register-once |
| `src/perf_test.cpp` | Functional suite, including these file checks |
| `vendor/httplib.h` | Vendored cpp-httplib |
| `Makefile` | C++17 build and quality gates |
| `scripts/v1_fixture.sql` | Test views |
| `scripts/ensure-test-postgres.sh` | Starts trust-auth Postgres when tests need a server |
| `scripts/ci-pack.sh`, `scripts/ci-restore.sh`, `scripts/ci-artifact.sh` | Shared Gitea CI environment |
| `Dockerfile` | Bookworm build and stripped runtime |
| `fly.toml` | Fly service (suspend, 256 MB, port 8080) |
| `.gitea/workflows/ci.yml` | prepare plus five check jobs |
| `AGENTS.md` | This file |
| `MEMORY.md` | Index of current facts |
| `DECISIONS.md` | Durable decisions |
| `README.md` | Versions and run commands |

Starter paths that this finished tree does not contain: `openapi.yaml`, `db/*.sql`, `docker-compose.yml`, `tests/test_catalog.py`, and `images/`.

## Requirements

- Routes return 200 with `data` JSON, and 404 `not_found` on an unknown slug
- `?year=` speaker rows include `languages` and `topics`; year sponsor rows include `tier`
- Register once on boot; if `CAROLINA_URL` is empty or the POST fails, log and keep serving
- Writes stay out of this API; query only `v1_*` views; never query Ash tables
- `GET /health` stays off the database
- The production binary stays stripped `-O2`, sanitizers stay on the test link, and the listen address stays `::`
- A durable decision change updates `DECISIONS.md` and `MEMORY.md` together

## Cursor Cloud

This repository is one sibling git remote in the carolina.codes polyglot fleet. Cloud agents treat this repo as the workspace root. The Phoenix CMS is a different remote (`github.com/brightball/carolina-codes`). Sibling directories such as `../elixir` exist only when those remotes are attached to the same environment.

For live HTTP against the views, use PostgreSQL 15 (Debian bookworm's `postgresql` package, the version CI installs) and set:

- `DATABASE_URL=postgres://postgres:postgres@127.0.0.1:5432/carolina_dev`
- `CAROLINA_URL=http://127.0.0.1:4000` (optional; registration no-ops if the CMS is down)
- `POLYGLOT_REGISTER_TOKEN=dev`
- `PUBLIC_BASE_URL` / `PORT` as in the README

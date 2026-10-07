# Memory

Current facts for carolina-codes-cpp. Consequences live in [DECISIONS.md](DECISIONS.md). When a fact changes because a decision changed, update both files in the same commit. This file is an index, not a session log.

- Read-only v1 JSON API. Language C++17, HTTP framework vendored cpp-httplib 0.18.3 (`CPPHTTPLIB_VERSION` in `vendor/httplib.h`), SQL client libpq. `api_version` is `0.2.0`. `language_version` is the compiler `__VERSION__` at build. See [D1](DECISIONS.md#d1).
- Queries `v1_years`, `v1_speakers`, `v1_talks`, `v1_sponsors`, `v1_year_sponsors`, and `v1_sponsorships` only. Never query Ash tables. The CMS owns the live views. See [D2](DECISIONS.md#d2).
- Registers once on boot. If `CAROLINA_URL` is empty or the POST fails, log and keep serving. No heartbeat. See [D3](DECISIONS.md#d3).
- Gates: `make test`, `make sast`, `make vuln`, `make secrets`, `make fmt-check` (`make check`). Pre-commit and Gitea run them. Gitea `prepare` builds one tool archive for the five check jobs. CI Postgres is Debian bookworm PostgreSQL 15. See [D4](DECISIONS.md#d4).
- Fly listens on `::`, ships a stripped `-O2` binary with libpq, and suspends at 256 MB. Sanitizers are only on the `perf_test` link. `GET /health` is `{"ok":true}` and does not open Postgres. See [D5](DECISIONS.md#d5).
- License is MIT, copyright 2026 Brightball. See [D6](DECISIONS.md#d6).
- Decision memory is this index plus `DECISIONS.md`. See [D7](DECISIONS.md#d7).
- Route contract is the CMS `priv/api/openapi.yaml`, not a file in this repo. Local default port is 4009. Fly port is 8080.

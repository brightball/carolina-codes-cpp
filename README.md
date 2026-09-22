# carolina-codes-cpp

Read-only v1 polyglot API for Carolina Code Conference. C++17 + [cpp-httplib](https://github.com/yhirose/cpp-httplib) + libpq.

Queries PostgreSQL `v1_*` views. Registers with Elixir once on boot.

```bash
make
DATABASE_URL=postgres://postgres:postgres@127.0.0.1:5432/carolina_dev \
CAROLINA_URL=http://127.0.0.1:4000 \
POLYGLOT_REGISTER_TOKEN=dev \
PUBLIC_BASE_URL=http://127.0.0.1:4009 \
PORT=4009 \
./api
```

## Quality gates

Same commands locally, in the git pre-commit hook, and as parallel Gitea Actions jobs:

```bash
make test        # functionality (./perf_test) under ASan+UBSan; also builds stripped -O2 ./api
make sast        # cppcheck on first-party src/
make vuln        # osv-scanner source scan (vendored C/C++)
make secrets     # gitleaks
make fmt-check   # clang-format --dry-run --Werror on src/
make check       # all of the above (pre-commit aggregate)
make hooks       # install .githooks/pre-commit (pre-commit runner + Makefile fallback)
```

Tools: `g++`, `cppcheck`, `osv-scanner`, `gitleaks`, `clang-format`. `make fmt` rewrites first-party sources.

Production `./api` is `-O2` and stripped. `make test` links AddressSanitizer and UndefinedBehaviorSanitizer and treats first-party warnings as errors. `vendor/httplib.h` is a system include, same scope as cppcheck. Fly idles machines with `auto_stop_machines = "suspend"` at 256 MB.

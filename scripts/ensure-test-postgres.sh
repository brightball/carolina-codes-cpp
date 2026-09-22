#!/bin/sh
# Start a trust-auth Postgres if the test has no server yet. Prints a libpq admin URL.
set -eu

port="${CAROLINA_PG_PORT:-54329}"
dir="${CAROLINA_PG_DIR:-/tmp/carolina-cpp-pg}"

bindir=""
for d in /usr/lib/postgresql/*/bin; do
  if [ -x "$d/initdb" ]; then
    bindir=$d
    break
  fi
done
if [ -z "$bindir" ] && command -v initdb >/dev/null 2>&1; then
  bindir=$(dirname "$(command -v initdb)")
fi
if [ -z "$bindir" ] || [ ! -x "$bindir/initdb" ]; then
  echo "initdb not found; install postgresql" >&2
  exit 1
fi

as_owner() {
  if [ "$(id -u)" -eq 0 ]; then
    if ! id postgres >/dev/null 2>&1; then
      echo "postgres OS user missing" >&2
      exit 1
    fi
    runuser -u postgres -- "$@"
  else
    "$@"
  fi
}

if [ ! -f "$dir/PG_VERSION" ]; then
  rm -rf "$dir"
  mkdir -p "$dir"
  if [ "$(id -u)" -eq 0 ]; then
    chown postgres "$dir"
  fi
  as_owner "$bindir/initdb" -D "$dir" --auth=trust --username=postgres --no-sync >/dev/null
fi

if ! as_owner "$bindir/pg_ctl" -D "$dir" status >/dev/null 2>&1; then
  as_owner "$bindir/pg_ctl" -D "$dir" -l "$dir/server.log" \
    -o "-c listen_addresses=127.0.0.1 -p $port" start >/dev/null
fi

i=0
while [ "$i" -lt 50 ]; do
  if "$bindir/pg_isready" -h 127.0.0.1 -p "$port" >/dev/null 2>&1; then
    echo "postgres://postgres@127.0.0.1:${port}/postgres"
    exit 0
  fi
  i=$((i + 1))
  sleep 0.1
done

echo "postgres did not become ready on port $port" >&2
if [ -f "$dir/server.log" ]; then
  cat "$dir/server.log" >&2
fi
exit 1

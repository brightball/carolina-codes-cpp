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

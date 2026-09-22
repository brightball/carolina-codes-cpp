FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    g++ make pkg-config libpq-dev ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY Makefile ./
COPY src ./src
COPY vendor ./vendor
RUN make

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    libpq5 ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY --from=build /src/api /app/api
RUN test ! -x /usr/bin/g++ && test ! -x /usr/bin/gcc && test ! -x /usr/bin/cc
ENV PORT=8080
EXPOSE 8080
CMD ["/app/api"]

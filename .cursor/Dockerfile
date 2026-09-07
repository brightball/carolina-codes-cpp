FROM debian:bookworm-slim
USER root
RUN apt-get update \
 && apt-get install -y --no-install-recommends git ca-certificates g++ make pkg-config libpq-dev \
 && rm -rf /var/lib/apt/lists/*

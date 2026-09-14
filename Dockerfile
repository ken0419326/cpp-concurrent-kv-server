FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    g++ make python3 ca-certificates && rm -rf /var/lib/apt/lists/*
WORKDIR /work
COPY . .
CMD ["make", "test"]

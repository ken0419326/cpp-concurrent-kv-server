#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
if ! command -v docker >/dev/null 2>&1 || ! docker info >/dev/null 2>&1; then
    echo 'Docker daemon unavailable; Linux epoll build and tests were not run.' >&2
    exit 1
fi
docker build --quiet -t cpp-kv-epoll-verify .
docker run --rm cpp-kv-epoll-verify make test
docker run --rm cpp-kv-epoll-verify make sanitize
echo 'Container correctness and ASan/UBSan verification completed.'

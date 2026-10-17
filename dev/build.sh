#!/bin/sh
# 在 Docker 交叉编译容器里执行 make。用法：./build.sh [make 参数...]
set -e

DIR="$(cd "$(dirname "$0")" && pwd)"

exec docker run --rm \
    -v "$DIR:/work" \
    -w /work \
    s1e-cross:latest \
    sh -c "make $*"
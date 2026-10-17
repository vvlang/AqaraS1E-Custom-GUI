# 交叉编译环境：原生 arm64 宿主 -> armv7 hard-float 目标
#
# 为什么用原生 arm64 容器而不是 arm32v7：
# 在 Apple Silicon 上跑 arm32v7 镜像需要 QEMU 模拟，慢 10 倍以上。
# 这里用 arm64 的 debian 装 gcc-arm-linux-gnueabihf，编译器本身原生执行，
# 只有产出的二进制是 ARM。

FROM debian:bookworm

RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc-arm-linux-gnueabihf \
        binutils-arm-linux-gnueabihf \
        make \
        file \
        zstd \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work

ENV CROSS_PREFIX=arm-linux-gnueabihf-
ENV CROSS_CC=${CROSS_PREFIX}gcc
ENV CROSS_STRIP=${CROSS_PREFIX}strip

CMD ["/bin/bash"]
FROM debian:trixie-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
    gcc \
    make \
    libc6-dev \
    liburing-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY Makefile main.c simd.c simd.h ./
RUN make

FROM gcr.io/distroless/cc-debian13

COPY --from=build /src/passa /proxy
COPY --from=build /usr/lib/x86_64-linux-gnu/liburing.so.2 /usr/lib/x86_64-linux-gnu/liburing.so.2

ENV PORT=8080
ENV BUF_SIZE=65536

ENTRYPOINT ["/proxy"]

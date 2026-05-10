#!/usr/bin/env python3
"""Benchmark passa TCP→UDS proxy."""

import socket, threading, time, statistics, sys

HOST = ("passa", 8080)
PAYLOAD = b"X" * 1024
WARMUP  = 10
REQUESTS = 500
CONCURRENCY = 20

def bench_one(name, host, port, requests, payload):
    latencies = []
    errors = 0

    def worker(n):
        nonlocal errors
        for _ in range(n):
            t0 = time.perf_counter()
            try:
                s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                s.settimeout(5.0)
                s.connect((host, port))
                s.sendall(payload)
                total = 0
                while total < len(payload):
                    chunk = s.recv(4096)
                    if not chunk:
                        break
                    total += len(chunk)
                s.close()
                if total == len(payload):
                    latencies.append((time.perf_counter() - t0) * 1000)
                else:
                    errors += 1
            except Exception:
                errors += 1

    # Warmup
    threads = [threading.Thread(target=worker, args=(WARMUP // CONCURRENCY,)) for _ in range(CONCURRENCY)]
    for t in threads: t.start()
    for t in threads: t.join()
    latencies.clear()

    # Benchmark
    t_start = time.perf_counter()
    threads = [threading.Thread(target=worker, args=(requests // CONCURRENCY,)) for _ in range(CONCURRENCY)]
    for t in threads: t.start()
    for t in threads: t.join()
    duration = time.perf_counter() - t_start

    ok = len(latencies)
    throughput = ok / duration if duration > 0 else 0
    lat_sorted = sorted(latencies)
    if not lat_sorted:
        print(f"\n{'='*50}")
        print(f"  {name.upper()}  ({host}:{port})")
        print(f"{'='*50}")
        print(f"  ALL REQUESTS FAILED ({errors} errors)")
        return 0, 0
    p50 = lat_sorted[min(len(lat_sorted)-1, int(len(lat_sorted) * 0.50))]
    p99 = lat_sorted[min(len(lat_sorted)-1, int(len(lat_sorted) * 0.99))]
    p999 = lat_sorted[min(len(lat_sorted)-1, int(len(lat_sorted) * 0.999))]

    print(f"\n{'='*50}")
    print(f"  {name.upper()}  ({host}:{port})")
    print(f"{'='*50}")
    print(f"  Requests : {ok}/{requests}  ({errors} errors)")
    print(f"  Duration : {duration:.3f}s")
    print(f"  RPS      : {throughput:.0f}")
    print(f"  Latency  : p50={p50:.3f}ms  p99={p99:.3f}ms  p99.9={p999:.3f}ms")
    return throughput, p99

def main():
    print("=" * 50)
    print("  TCP → UDS Proxy Benchmark — passa")
    print(f"  Payload: {len(PAYLOAD)} bytes  |  Conns: {REQUESTS}  |  Parallel: {CONCURRENCY}")
    print("=" * 50)

    name, host, port = "passa", HOST[0], HOST[1]
    reachable = False
    for _ in range(10):
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(2.0)
            s.connect((host, port))
            s.close()
            reachable = True
            break
        except Exception:
            time.sleep(0.5)
    if not reachable:
        print(f"\n[!] {name} not reachable at {host}:{port}")
        return

    bench_one(name, host, port, REQUESTS, PAYLOAD)

if __name__ == "__main__":
    main()

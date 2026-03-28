#!/usr/bin/env python3
"""
bench_user.py — copy_page_to_iter 用户态驱动脚本

用法：
    sudo python3 bench_user.py [选项]

选项：
    -s, --size   MB    每次 read() 请求的字节数（默认 1 MiB，不超过 buf_pages*4096）
    -n, --iters  N     有效测量轮次（不含内核预热，默认 1000）
    -d, --dev    PATH  设备路径（默认 /dev/cpit_bench）
    -p, --proc   PATH  proc 统计路径（默认 /proc/cpit_bench）
    -r, --reset        测试前重置内核统计
"""

import argparse
import os
import sys
import time
import struct

DEV  = "/dev/cpit_bench"
PROC = "/proc/cpit_bench"


def reset_stats(proc):
    with open(proc, "w") as f:
        f.write("reset\n")
    print(f"[reset] 内核统计已清零 ({proc})")


def read_stats(proc):
    with open(proc, "r") as f:
        return f.read()


def run_bench(dev, size_bytes, iters, proc, do_reset):
    if do_reset:
        reset_stats(proc)

    buf = bytearray(size_bytes)
    total_bytes = 0
    latencies   = []

    print(f"\n[bench] 设备={dev}  每次读取={size_bytes//1024} KiB  轮次={iters}")
    print("=" * 60)

    fd = os.open(dev, os.O_RDONLY)
    try:
        # ---------- 主测量循环 ----------
        t_wall_start = time.perf_counter_ns()
        for i in range(iters):
            t0 = time.perf_counter_ns()
            n  = os.readv(fd, [buf])   # 单次 read()
            t1 = time.perf_counter_ns()
            if n <= 0:
                print(f"  [警告] 第 {i} 轮 read 返回 {n}，跳过")
                continue
            total_bytes += n
            latencies.append(t1 - t0)
        t_wall_end = time.perf_counter_ns()
    finally:
        os.close(fd)

    # ---------- 用户态统计 ----------
    wall_ns      = t_wall_end - t_wall_start
    wall_s       = wall_ns / 1e9
    throughput   = total_bytes / wall_s / (1024**2)   # MiB/s

    lat_avg      = sum(latencies) / len(latencies) if latencies else 0
    lat_sorted   = sorted(latencies)
    lat_p50      = lat_sorted[len(lat_sorted)//2]
    lat_p99      = lat_sorted[int(len(lat_sorted)*0.99)]
    lat_min      = lat_sorted[0]
    lat_max      = lat_sorted[-1]

    print(f"  [用户态结果]")
    print(f"    总传输量    : {total_bytes/(1024**2):.1f} MiB")
    print(f"    wall 耗时   : {wall_s*1000:.1f} ms")
    print(f"    吞吐量      : {throughput:.1f} MiB/s  ({throughput/1024:.2f} GiB/s)")
    print(f"    read() 延迟 : avg={lat_avg/1e3:.1f}µs  p50={lat_p50/1e3:.1f}µs  "
          f"p99={lat_p99/1e3:.1f}µs  min={lat_min/1e3:.1f}µs  max={lat_max/1e3:.1f}µs")
    print()

    # ---------- 内核侧统计 ----------
    print("  [内核侧统计 /proc/cpit_bench]")
    print(read_stats(proc))


def main():
    ap = argparse.ArgumentParser(description="copy_page_to_iter 带宽基准测试")
    ap.add_argument("-s", "--size",  type=int, default=64,  metavar="MB",
                    help="每次 read() 大小（MiB，默认 64；内核环形遍历页数组直到填满）")
    ap.add_argument("-n", "--iters", type=int, default=100, metavar="N",
                    help="有效测量轮次（默认 100；每轮 64 MiB，共 ~6.4 GiB）")
    ap.add_argument("-d", "--dev",   default=DEV,  help=f"设备路径（默认 {DEV}）")
    ap.add_argument("-p", "--proc",  default=PROC, help=f"proc 路径（默认 {PROC}）")
    ap.add_argument("-r", "--reset", action="store_true",
                    help="测试前重置内核统计")
    args = ap.parse_args()

    if os.geteuid() != 0:
        print("警告：建议以 root 运行（需要访问 /dev/cpit_bench）")

    if not os.path.exists(args.dev):
        sys.exit(f"错误：找不到设备 {args.dev}，请先加载内核模块")

    run_bench(
        dev        = args.dev,
        size_bytes = args.size * 1024 * 1024,
        iters      = args.iters,
        proc       = args.proc,
        do_reset   = args.reset,
    )


if __name__ == "__main__":
    main()


# copy_page_to_iter 带宽性能测试

## 文件说明

| 文件 | 说明 |
|------|------|
| `bench.c` | 内核模块，创建 `/dev/cpit_bench` 字符设备 |
| `Makefile` | 内核模块构建脚本 |
| `bench_user.py` | 用户态测试驱动脚本 |

---

## 快速开始

### 1. 编译内核模块

```bash
# 确保安装了内核头文件
sudo apt install linux-headers-$(uname -r)   # Debian/Ubuntu
# 或
sudo dnf install kernel-devel                 # Fedora/RHEL

make
```

### 2. 加载模块

```bash
# 默认：256 页（1 MiB 缓冲区），预热 4 次
sudo insmod bench.ko

# 自定义：2048 页（8 MiB 缓冲区），预热 8 次
sudo insmod bench.ko buf_pages=2048 warmup_iters=8
```

加载后：
- `/dev/cpit_bench` — 字符设备，对其 `read()` 会触发 `copy_page_to_iter`
- `/proc/cpit_bench` — 统计输出接口（读取查看，写入重置）

### 3. 运行测试

**方式 A：Python 驱动脚本（推荐，有延迟分布统计）**

```bash
# 每次读 1 MiB，测 1000 轮
sudo python3 bench_user.py

# 每次读 4 MiB，测 500 轮，测前重置统计
sudo python3 bench_user.py -s 4 -n 500 -r
```

**方式 B：dd（快速粗估）**

```bash
# 传输 4 GiB，bs=1M
sudo dd if=/dev/cpit_bench of=/dev/null bs=1M count=4096 iflag=fullblock
```

**方式 C：直接查看内核统计**

```bash
cat /proc/cpit_bench
```

重置统计：

```bash
echo reset | sudo tee /proc/cpit_bench
```

### 4. 卸载模块

```bash
sudo rmmod bench
```

---

## 理解输出

```
========== copy_page_to_iter Bandwidth Bench ==========
  buf_pages        : 256  (1024 KiB per read)
  warmup_iters     : 4

  total read()     : 1004  (warmup: 4)
  effective read() : 1000
  copy calls       : 256000          ← 总 copy_page_to_iter 调用次数
  bytes copied     : 268435456  (256 MiB)

  -- copy_page_to_iter internal time --
  total_ns         : 45123456 ns  (45 ms)
  avg per call     : 176 ns        ← 每页（4 KiB）拷贝耗时
  bandwidth        : 5678 MiB/s   ← 核心指标：copy_page_to_iter 净带宽

  -- wall clock (first..last effective read) --
  wall_ns          : 98765432 ns  (98 ms)
=======================================================
```

**两个带宽数字的含义：**

- **内核侧带宽**（`/proc` 中）：只计 `copy_page_to_iter` 本身执行时间，反映函数的纯粹吞吐能力。
- **用户态带宽**（脚本输出）：端到端 wall time，包含系统调用开销、调度等，更接近实际应用场景。

---

## 测试策略建议

| 场景 | 参数建议 |
|------|----------|
| 热缓存（L3 命中）测基线 | `buf_pages=64`（256 KiB < L3） |
| DRAM 带宽测试 | `buf_pages=4096`（16 MiB > L3） |
| 对比不同 read() 大小影响 | `-s 1`, `-s 4`, `-s 16`, `-s 64` |
| 多核并发 | 多终端同时运行 `bench_user.py` |

---

## 实现说明

```
用户态 read(fd, buf, N)
        │
        ▼
bench_read()  [内核 file_operations.read]
        │
        ├─ iov_iter_init(&iter, READ, &iov, 1, count)
        │
        └─ 循环（每页）：
             ktime_get()
             copy_page_to_iter(page, offset=0, len, &iter)
             ktime_get()
             累计 elapsed_ns, bytes
```

`copy_page_to_iter` 是 VFS 层将内核页传递给用户 `iov_iter` 的标准路径，
也是 `generic_file_read_iter` → `copy_page_to_iter` 的核心热路径。
通过此测试可以确认它是否成为自定义 `read` 接口的瓶颈。

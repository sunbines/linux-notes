// SPDX-License-Identifier: GPL-2.0
/*
 * copy_page_to_iter 带宽性能测试内核模块
 *
 * 创建 /dev/cpit_bench 字符设备，用户态通过 read() 驱动数据传输，
 * 内核侧调用 copy_page_to_iter() 完成拷贝，并在 /proc/cpit_bench
 * 中汇报吞吐量统计。
 *
 * 编译：make
 * 加载：sudo insmod bench.ko [buf_pages=N] [warmup_iters=N]
 * 测试：sudo ./bench_user          # 或直接 dd / cat
 * 卸载：sudo rmmod bench
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uio.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/highmem.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/atomic.h>
#include <linux/ktime.h>
#include <linux/device.h>
#include <linux/vmalloc.h>

#define DEVICE_NAME   "cpit_bench"
#define CLASS_NAME    "cpit"
#define PROC_NAME     "cpit_bench"

/* -------- 模块参数 -------- */
static unsigned int buf_pages   = 256;   /* 内核缓冲区页数，默认 1 MiB */
static unsigned int warmup_iters = 4;    /* 预热轮次，不计入统计 */
module_param(buf_pages,    uint, 0444);
module_param(warmup_iters, uint, 0444);
MODULE_PARM_DESC(buf_pages,    "Number of pages in the kernel source buffer (default 256 = 1MiB)");
MODULE_PARM_DESC(warmup_iters, "Warm-up read() calls before stats are recorded (default 4)");

/* -------- 统计 -------- */
struct bench_stats {
    atomic64_t  total_bytes;    /* 累计传输字节 */
    atomic64_t  total_calls;    /* copy_page_to_iter 调用次数 */
    atomic64_t  total_ns;       /* 累计耗时（仅计 copy_page_to_iter） */
    atomic64_t  read_calls;     /* read() 调用总次数（含预热） */
    ktime_t     first_ts;       /* 第一次有效 read() 的时间戳 */
    ktime_t     last_ts;        /* 最近一次有效 read() 结束时间戳 */
    bool        warmed_up;
} stats;

/* -------- 内核源缓冲区 -------- */
static struct page **src_pages;   /* 页指针数组 */
static unsigned int  n_pages;

/* -------- 字符设备 -------- */
static dev_t        dev_num;
static struct cdev  bench_cdev;
static struct class *bench_class;

/* ======================================================
 * read 实现：环形遍历 src_pages，直到填满用户请求的 count 字节
 *
 * 计时策略：对整个拷贝循环只打一对 ktime_get()，彻底消除
 * 逐页计时引入的 ktime_get() 自身开销（每次约 20~100 ns，
 * 与 4 KiB 拷贝耗时同量级，会严重低估实际带宽）。
 * ====================================================== */
static ssize_t bench_read(struct file *filp, char __user *ubuf,
                          size_t count, loff_t *ppos)
{
    struct iov_iter iter;
    struct iovec    iov        = { .iov_base = ubuf, .iov_len = count };
    size_t          done       = 0;
    u64             copy_calls = 0;
    unsigned int    page_idx   = 0;   /* 环形索引，对 n_pages 取模 */
    ktime_t         t0, t1;
    s64             elapsed_ns;
    bool            is_warmup;

    if (count == 0)
        return 0;

    iov_iter_init(&iter, READ, &iov, 1, count);

    /*
     * 只在整个循环外侧打一对时间戳。
     * ktime_get() 本身约 20~100 ns，逐页调用会把 ~30 ns 的真实
     * 拷贝耗时完全淹没；批量计时后误差降至 ppm 级别。
     */
    t0 = ktime_get();
    while (iov_iter_count(&iter) > 0) {
        size_t copy_len = min_t(size_t, PAGE_SIZE, iov_iter_count(&iter));
        copy_page_to_iter(src_pages[page_idx % n_pages], 0, copy_len, &iter);
        done += copy_len;
        copy_calls++;
        page_idx++;
    }
    t1 = ktime_get();

    elapsed_ns = ktime_to_ns(ktime_sub(t1, t0));

    /* 统计（预热期不计入） */
    atomic64_inc(&stats.read_calls);
    is_warmup = (atomic64_read(&stats.read_calls) <= warmup_iters);

    if (!is_warmup) {
        if (!stats.warmed_up) {
            stats.warmed_up = true;
            stats.first_ts  = t0;   /* 用已有的 t0，不额外调用 ktime_get */
        }
        atomic64_add((s64)done,       &stats.total_bytes);
        atomic64_add((s64)copy_calls, &stats.total_calls);
        atomic64_add(elapsed_ns,      &stats.total_ns);
        stats.last_ts = t1;
    }

    return (ssize_t)done;
}

static int bench_open(struct inode *inode, struct file *filp)
{
    return 0;
}

static const struct file_operations bench_fops = {
    .owner  = THIS_MODULE,
    .open   = bench_open,
    .read   = bench_read,
};

/* ======================================================
 * /proc/cpit_bench — 输出统计摘要
 * ====================================================== */
static int proc_show(struct seq_file *m, void *v)
{
    s64 total_bytes = atomic64_read(&stats.total_bytes);
    s64 total_calls = atomic64_read(&stats.total_calls);
    s64 total_ns    = atomic64_read(&stats.total_ns);
    s64 read_calls  = atomic64_read(&stats.read_calls);
    s64 wall_ns     = 0;
    u64 bw_copy_MBps = 0;
    u64 bw_wall_MBps = 0;
    u64 avg_ns       = 0;
    /* 整数模拟两位小数的 GiB/s */
    u64 bw_copy_GiB_int = 0, bw_copy_GiB_frac = 0;
    u64 bw_wall_GiB_int = 0, bw_wall_GiB_frac = 0;
    /* 整数模拟一位小数的 ns/byte */
    u64 ns_per_byte_int = 0, ns_per_byte_frac = 0;

    if (stats.warmed_up)
        wall_ns = ktime_to_ns(ktime_sub(stats.last_ts, stats.first_ts));

    /*
     * 带宽公式（MiB/s），防 u64 溢出：
     *   bytes >> 20 → MiB（≤ ~3e5），× 1_000_000 / µs，中间值 ≤ 3e11，安全。
     *
     * 内核 seq_printf 不支持 %f，改用整数模拟两位小数输出 GiB/s：
     *   int_part  = MiB/s / 1024
     *   frac_part = (MiB/s % 1024) * 100 / 1024
     */
    if (total_ns > 0) {
        u64 mib = (u64)total_bytes >> 20;
        u64 us  = (u64)total_ns / 1000;
        if (us > 0) {
            bw_copy_MBps     = div64_u64(mib * 1000000ULL, us);
            bw_copy_GiB_int  = bw_copy_MBps / 1024;
            bw_copy_GiB_frac = (bw_copy_MBps % 1024) * 100 / 1024;
        }
    }
    if (wall_ns > 0) {
        u64 mib = (u64)total_bytes >> 20;
        u64 us  = (u64)wall_ns / 1000;
        if (us > 0) {
            bw_wall_MBps     = div64_u64(mib * 1000000ULL, us);
            bw_wall_GiB_int  = bw_wall_MBps / 1024;
            bw_wall_GiB_frac = (bw_wall_MBps % 1024) * 100 / 1024;
        }
    }
    if (total_calls > 0) {
        avg_ns = div64_u64((u64)total_ns, (u64)total_calls);
        /* ns/byte = avg_ns / PAGE_SIZE，模拟一位小数 */
        ns_per_byte_int  = avg_ns / PAGE_SIZE;
        ns_per_byte_frac = (avg_ns % PAGE_SIZE) * 10 / PAGE_SIZE;
    }

    seq_printf(m,
        "========== copy_page_to_iter Bandwidth Bench ==========\n"
        "  buf_pages        : %u  (%lu KiB kernel pool)\n"
        "  warmup_iters     : %u\n"
        "\n"
        "  total read()     : %lld  (warmup: %u)\n"
        "  effective read() : %lld\n"
        "  copy calls       : %lld\n"
        "  bytes copied     : %lld  (%lld MiB)\n"
        "\n"
        "  -- copy_page_to_iter internal time --\n"
        "  total_ns         : %lld ns  (%lld ms)\n"
        "  avg per call     : %llu ns  (%llu.%llu ns/byte)\n"
        "  bandwidth        : %llu MiB/s  (%llu.%02llu GiB/s)\n"
        "\n"
        "  -- wall clock (first..last effective read) --\n"
        "  wall_ns          : %lld ns  (%lld ms)\n"
        "  wall bandwidth   : %llu MiB/s  (%llu.%02llu GiB/s)\n"
        "=======================================================\n",
        n_pages, (unsigned long)n_pages * PAGE_SIZE / 1024,
        warmup_iters,
        read_calls, warmup_iters,
        max(0LL, read_calls - (s64)warmup_iters),
        total_calls,
        total_bytes, total_bytes >> 20,
        total_ns, total_ns / 1000000,
        avg_ns, ns_per_byte_int, ns_per_byte_frac,
        bw_copy_MBps, bw_copy_GiB_int, bw_copy_GiB_frac,
        wall_ns, wall_ns / 1000000,
        bw_wall_MBps, bw_wall_GiB_int, bw_wall_GiB_frac
    );
    return 0;
}

static int proc_open(struct inode *inode, struct file *file)
{
    return single_open(file, proc_show, NULL);
}

static ssize_t proc_write(struct file *file, const char __user *buf,
                          size_t count, loff_t *ppos)
{
    /* 写入任意内容 → 重置统计 */
    atomic64_set(&stats.total_bytes, 0);
    atomic64_set(&stats.total_calls, 0);
    atomic64_set(&stats.total_ns,    0);
    atomic64_set(&stats.read_calls,  0);
    stats.warmed_up = false;
    pr_info("cpit_bench: stats reset\n");
    return count;
}

static const struct proc_ops proc_fops = {
    .proc_open    = proc_open,
    .proc_read    = seq_read,
    .proc_write   = proc_write,
    .proc_lseek   = seq_lseek,
    .proc_release = single_release,
};

/* ======================================================
 * 模块初始化 / 清理
 * ====================================================== */
static int __init bench_init(void)
{
    int    ret;
    size_t i;

    n_pages = buf_pages;

    /* 分配页数组 */
    src_pages = kvmalloc_array(n_pages, sizeof(*src_pages), GFP_KERNEL);
    if (!src_pages)
        return -ENOMEM;

    /* 分配并填充源页（全部写入 0xAB 防止编译器/缓存优化） */
    for (i = 0; i < n_pages; i++) {
        src_pages[i] = alloc_page(GFP_KERNEL);
        if (!src_pages[i]) {
            pr_err("cpit_bench: failed to alloc page %zu\n", i);
            n_pages = i;   /* 只释放已分配的 */
            ret = -ENOMEM;
            goto err_pages;
        }
        memset(page_address(src_pages[i]), 0xAB, PAGE_SIZE);
    }

    /* 注册字符设备 */
    ret = alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);
    if (ret < 0) {
        pr_err("cpit_bench: alloc_chrdev_region failed: %d\n", ret);
        goto err_pages;
    }

    cdev_init(&bench_cdev, &bench_fops);
    bench_cdev.owner = THIS_MODULE;
    ret = cdev_add(&bench_cdev, dev_num, 1);
    if (ret) {
        pr_err("cpit_bench: cdev_add failed: %d\n", ret);
        goto err_chrdev;
    }

    bench_class = class_create(THIS_MODULE, CLASS_NAME);
    if (IS_ERR(bench_class)) {
        ret = PTR_ERR(bench_class);
        goto err_cdev;
    }

    if (IS_ERR(device_create(bench_class, NULL, dev_num, NULL, DEVICE_NAME))) {
        pr_warn("cpit_bench: device_create failed, use mknod manually\n");
    }

    /* /proc 统计接口 */
    if (!proc_create(PROC_NAME, 0666, NULL, &proc_fops)) {
        pr_warn("cpit_bench: proc_create failed\n");
    }

    /* 初始化统计 */
    atomic64_set(&stats.total_bytes, 0);
    atomic64_set(&stats.total_calls, 0);
    atomic64_set(&stats.total_ns,    0);
    atomic64_set(&stats.read_calls,  0);
    stats.warmed_up = false;

    pr_info("cpit_bench: loaded  pages=%u (%lu KiB)  warmup=%u\n",
            n_pages, (unsigned long)n_pages * PAGE_SIZE / 1024, warmup_iters);
    pr_info("cpit_bench: device  /dev/%s  major=%d\n",
            DEVICE_NAME, MAJOR(dev_num));
    pr_info("cpit_bench: stats   cat /proc/%s\n", PROC_NAME);
    return 0;

err_cdev:
    cdev_del(&bench_cdev);
err_chrdev:
    unregister_chrdev_region(dev_num, 1);
err_pages:
    for (i = 0; i < n_pages; i++)
        if (src_pages[i])
            __free_page(src_pages[i]);
    kvfree(src_pages);
    return ret;
}

static void __exit bench_exit(void)
{
    size_t i;

    remove_proc_entry(PROC_NAME, NULL);
    device_destroy(bench_class, dev_num);
    class_destroy(bench_class);
    cdev_del(&bench_cdev);
    unregister_chrdev_region(dev_num, 1);

    for (i = 0; i < n_pages; i++)
        __free_page(src_pages[i]);
    kvfree(src_pages);

    pr_info("cpit_bench: unloaded\n");
}

module_init(bench_init);
module_exit(bench_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("bench");
MODULE_DESCRIPTION("copy_page_to_iter bandwidth benchmark");
MODULE_VERSION("1.0");
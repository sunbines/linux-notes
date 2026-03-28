#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/debugfs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("nzfs");
MODULE_DESCRIPTION("debugfs backend_bandwidth example");

/* 存储带宽值的变量（单位：MB/s） */
static u64 backend_bandwidth = 0;

/* debugfs 目录和文件节点 */
static struct dentry *nzfs_dir;
static struct dentry *nzfs_bw_file;

/* ---- read 回调：将 backend_bandwidth 输出到用户空间 ---- */
static ssize_t bw_read(struct file *filp, char __user *buf,
                        size_t count, loff_t *ppos)
{
    char tmp[32];
    int len;

    len = scnprintf(tmp, sizeof(tmp), "%llu\n", backend_bandwidth);

    /* simple_read_from_buffer 处理 ppos 偏移，避免死循环 */
    return simple_read_from_buffer(buf, count, ppos, tmp, len);
}

/* ---- write 回调：从用户空间接收新的带宽值 ---- */
static ssize_t bw_write(struct file *filp, const char __user *buf,
                         size_t count, loff_t *ppos)
{
    char tmp[32];
    ssize_t ret;
    u64 val;

    if (count >= sizeof(tmp))
        return -EINVAL;

    /* 将用户数据拷贝到内核缓冲区 */
    ret = simple_write_to_buffer(tmp, sizeof(tmp) - 1, ppos, buf, count);
    if (ret < 0)
        return ret;

    tmp[ret] = '\0';

    /* 解析字符串为 u64 */
    if (kstrtoull(strim(tmp), 10, &val))
        return -EINVAL;

    backend_bandwidth = val;
    pr_info("nzfs_readahead: backend_bandwidth set to %llu MB/s\n", val);

    return ret;
}

static const struct file_operations bw_fops = {
    .owner = THIS_MODULE,
    .read  = bw_read,
    .write = bw_write,
    .llseek = default_llseek,
};

/* ---- 模块初始化 ---- */
static int __init nzfs_init(void)
{
    /* 创建 /sys/kernel/debug/nzfs_readahead/ 目录 */
    nzfs_dir = debugfs_create_dir("nzfs_readahead", NULL);
    if (IS_ERR(nzfs_dir)) {
        pr_err("nzfs_readahead: failed to create debugfs dir\n");
        return PTR_ERR(nzfs_dir);
    }

    /*
     * 创建文件：/sys/kernel/debug/nzfs_readahead/backend_bandwidth
     * 权限 0644：root 可读写，其他用户可读
     */
    nzfs_bw_file = debugfs_create_file("backend_bandwidth", 0644,
                                        nzfs_dir, NULL, &bw_fops);
    if (IS_ERR(nzfs_bw_file)) {
        pr_err("nzfs_readahead: failed to create debugfs file\n");
        debugfs_remove_recursive(nzfs_dir);
        return PTR_ERR(nzfs_bw_file);
    }

    pr_info("nzfs_readahead: debugfs initialized\n");
    return 0;
}

/* ---- 模块卸载 ---- */
static void __exit nzfs_exit(void)
{
  /* 递归删除整个目录及其下所有文件 */
  debugfs_remove_recursive(nzfs_dir);
  pr_info("nzfs_readahead: debugfs removed\n");
}

module_init(nzfs_init);
module_exit(nzfs_exit);
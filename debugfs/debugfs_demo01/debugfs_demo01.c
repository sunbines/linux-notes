#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("nzfs");
MODULE_DESCRIPTION("debugfs backend_bandwidth with single_open/seq_file");

/* 存储带宽值（单位：MB/s） */
static u64 backend_bandwidth = 0;

static struct dentry *nzfs_dir;
static struct dentry *nzfs_bw_file;

/* ------------------------------------------------------------------ */
/*  seq_file show：格式化输出到 /sys/kernel/debug/nzfs_readahead/      */
/*                 backend_bandwidth                                   */
/* ------------------------------------------------------------------ */
static int nz_ra_backend_bw_show(struct seq_file *m, void *v)
{
  u64 *bw = m->private;   /* inode->i_private 透传过来 */

  seq_printf(m, "backend_bandwidth: %llu MB/s\n", *bw);
  return 0;
}

/* ------------------------------------------------------------------ */
/*  open：用 single_open 绑定 show 回调                                */
/* ------------------------------------------------------------------ */
static int nz_ra_backend_bw_open(struct inode *inode, struct file *file)
{
  /*
    * single_open 第三个参数即 inode->i_private，
    * 会被存入 seq_file->private，在 show 里通过 m->private 取出。
    */
  return single_open(file, nz_ra_backend_bw_show, inode->i_private);
}

/* ------------------------------------------------------------------ */
/*  write：从用户空间更新 backend_bandwidth                            */
/* ------------------------------------------------------------------ */
static ssize_t nz_ra_backend_bw_write(struct file *file,
                                       const char __user *buf,
                                       size_t count, loff_t *ppos)
{
  char tmp[32];
  ssize_t ret;
  u64 val;
  /* 通过 seq_file 拿回 private 指针（即 &backend_bandwidth） */
  struct seq_file *m = file->private_data;
  u64 *bw = m->private;

  if (count >= sizeof(tmp))
      return -EINVAL;

  ret = simple_write_to_buffer(tmp, sizeof(tmp) - 1, ppos, buf, count);
  if (ret < 0)
      return ret;

  tmp[ret] = '\0';

  if (kstrtoull(strim(tmp), 10, &val))
      return -EINVAL;

  *bw = val;
  pr_info("nzfs_readahead: backend_bandwidth updated to %llu MB/s\n", val);
  return ret;
}

/* ------------------------------------------------------------------ */
/*  file_operations：open 用 single_open，其余配套固定写法             */
/* ------------------------------------------------------------------ */
static const struct file_operations nz_ra_backend_bw_fops = {
  .owner   = THIS_MODULE,
  .open    = nz_ra_backend_bw_open,  /* single_open 模式 */
  .read    = seq_read,               /* seq_file 提供     */
  .write   = nz_ra_backend_bw_write,
  .llseek  = seq_lseek,              /* seq_file 提供     */
  .release = single_release,         /* 与 single_open 配对 */
};

/* ------------------------------------------------------------------ */
/*  模块初始化                                                         */
/* ------------------------------------------------------------------ */
static int __init nzfs_init(void)
{
  nzfs_dir = debugfs_create_dir("nzfs_readahead", NULL);
  if (IS_ERR(nzfs_dir)) {
      pr_err("nzfs_readahead: failed to create debugfs dir\n");
      return PTR_ERR(nzfs_dir);
  }

  /*
    * 将 &backend_bandwidth 作为 i_private 传入，
    * 这样 open → single_open → show 全链路都能访问到它，
    * 不需要依赖全局变量。
    */
  nzfs_bw_file = debugfs_create_file("backend_bandwidth", 0644,
                                      nzfs_dir,
                                      &backend_bandwidth,   /* i_private */
                                      &nz_ra_backend_bw_fops);
  if (IS_ERR(nzfs_bw_file)) {
      pr_err("nzfs_readahead: failed to create debugfs file\n");
      debugfs_remove_recursive(nzfs_dir);
      return PTR_ERR(nzfs_bw_file);
  }

  pr_info("nzfs_readahead: loaded, /sys/kernel/debug/nzfs_readahead/backend_bandwidth ready\n");
  return 0;
}

/* ------------------------------------------------------------------ */
/*  模块卸载                                                           */
/* ------------------------------------------------------------------ */
static void __exit nzfs_exit(void)
{
  debugfs_remove_recursive(nzfs_dir);
  pr_info("nzfs_readahead: unloaded\n");
}

module_init(nzfs_init);
module_exit(nzfs_exit);

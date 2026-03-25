// complete_demo.c
// 演示 complete() vs complete_all() 的区别
//
// complete()     —— 每次只唤醒一个等待者（计数+1）
// complete_all() —— 一次性唤醒所有等待者（计数设为 UINT_MAX/2）
//
// 本模块分两个阶段：
//   阶段一：3 个线程等待同一个 completion，用 complete() 逐个唤醒
//   阶段二：3 个线程等待同一个 completion，用 complete_all() 一次唤醒全部

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/completion.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Demo");
MODULE_DESCRIPTION("complete() vs complete_all() demo");

#define NR_WAITERS 3

static struct completion phase_comp;

/* ---------- 等待线程 ---------- */
static int waiter_fn(void *data)
{
    int id = (int)(long)data;
    pr_info("[waiter-%d] 开始等待...\n", id);
    wait_for_completion(&phase_comp);
    pr_info("[waiter-%d] 被唤醒！\n", id);
    return 0;
}

/* ---------- 阶段一：complete() 逐个唤醒 ---------- */
static void phase_one(void)
{
    struct task_struct *threads[NR_WAITERS];
    int i;

    pr_info("\n===== 阶段一：complete()，每次只唤醒一个 =====\n");
    init_completion(&phase_comp);

    /* 启动 3 个等待线程 */
    for (i = 0; i < NR_WAITERS; i++) {
        threads[i] = kthread_run(waiter_fn, (void *)(long)i, "waiter-%d", i);
        if (IS_ERR(threads[i])) {
            pr_err("创建线程 %d 失败\n", i);
            return;
        }
    }

    msleep(500); /* 让所有线程都进入等待状态 */

    /* 逐个唤醒 */
    for (i = 0; i < NR_WAITERS; i++) {
        pr_info("[主线程] 调用 complete()，第 %d 次\n", i + 1);
        complete(&phase_comp);
        msleep(300); /* 观察每次只有一个线程被唤醒 */
    }

    msleep(500);
    pr_info("===== 阶段一结束 =====\n");
}

/* ---------- 阶段二：complete_all() 一次唤醒全部 ---------- */
static void phase_two(void)
{
    struct task_struct *threads[NR_WAITERS];
    int i;

    pr_info("\n===== 阶段二：complete_all()，一次唤醒所有 =====\n");
    reinit_completion(&phase_comp); /* 重置 completion */

    /* 启动 3 个等待线程 */
    for (i = 0; i < NR_WAITERS; i++) {
        threads[i] = kthread_run(waiter_fn, (void *)(long)i, "waiter-%d", i);
        if (IS_ERR(threads[i])) {
            pr_err("创建线程 %d 失败\n", i);
            return;
        }
    }

    msleep(500); /* 让所有线程都进入等待状态 */

    pr_info("[主线程] 调用 complete_all()，一次唤醒全部\n");
    complete_all(&phase_comp); /* 一次唤醒所有等待者 */

    msleep(500);
    pr_info("===== 阶段二结束 =====\n");
}

/* ---------- 模块入口 ---------- */
static int __init comp_demo_init(void)
{
    pr_info("[demo] 模块加载\n");

    init_completion(&phase_comp);

    phase_one();
    phase_two();

    pr_info("[demo] 演示完毕\n");
    return 0;
}

static void __exit comp_demo_exit(void)
{
    pr_info("[demo] 模块卸载\n");
}

module_init(comp_demo_init);
module_exit(comp_demo_exit);

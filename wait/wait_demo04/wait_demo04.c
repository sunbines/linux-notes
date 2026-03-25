// serial_prepare_wq.c
// 用 prepare_to_wait / finish_wait 架构实现多线程按到达顺序串行执行
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/atomic.h>
#include <linux/sched.h>
#include <linux/signal.h>
#include <linux/delay.h>

#define NUM_THREADS 5

/* 全局等待队列头 */
static DECLARE_WAIT_QUEUE_HEAD(queue);

/* 取号机：领号 / 叫号 */
static atomic_t ticket_counter = ATOMIC_INIT(0);
static atomic_t now_serving    = ATOMIC_INIT(0);

static struct task_struct *threads[NUM_THREADS];

/* ------------------------------------------------------------------ */
/*  等待函数：prepare_to_wait / finish_wait 架构                        */
/* ------------------------------------------------------------------ */
static int wait_for_my_turn(int my_ticket)
{
    /*
     * DEFINE_WAIT 在栈上声明等待项，并把唤醒函数设为
     * autoremove_wake_function —— 被唤醒时自动把自己从队列摘除，
     * 这正是 finish_wait 能安全重置的基础。
     */
    DEFINE_WAIT(wait);

    /*
     * while (!condition) 先检查条件：
     *   - 进循环前检查一次，避免条件已满足却还去睡眠
     *   - 每次被唤醒后再检查一次，防止虚假唤醒
     */
    while (atomic_read(&now_serving) != my_ticket) {

        /*
         * prepare_to_wait 做两件事（原子完成，不可分割）：
         *   1. add_wait_queue_exclusive / add_wait_queue —— 把 wait 挂入 queue
         *   2. set_current_state(TASK_INTERRUPTIBLE)    —— 声明本线程想睡眠
         *
         * 必须在"再次检查条件"之前调用，原因同上一版：
         * 保证在入队到 schedule() 之间发来的唤醒不会丢失。
         */
        prepare_to_wait(&queue, &wait, TASK_INTERRUPTIBLE);

        /*
         * 入队并设好睡眠状态之后，再检查一次条件。
         * 如果这时条件已经满足（唤醒者在我们 prepare 之后才 inc），
         * 直接跳过 schedule()，走 finish_wait 出循环。
         */
        if (atomic_read(&now_serving) == my_ticket)
            break;

        /* 处理信号中断 */
        if (signal_pending(current)) {
            finish_wait(&queue, &wait);
            return -ERESTARTSYS;
        }

        /*
         * 真正让出 CPU。
         * schedule() 返回说明被某次 wake_up_all 唤醒，
         * 但不代表条件一定为真（可能是其他线程的号被叫到），
         * 所以回到 while 头部重新检查。
         */
        schedule();

        /*
         * finish_wait 做两件事：
         *   1. set_current_state(TASK_RUNNING) —— 恢复运行状态
         *   2. 若 wait 还在队列里则 remove_wait_queue —— 摘除自己
         *      （被 autoremove_wake_function 唤醒时已自动摘除，
         *       此处调用是幂等的，不会二次移除）
         *
         * 放在 schedule() 之后、while 条件重判之前，
         * 确保下一轮循环开始时线程处于 TASK_RUNNING 状态，
         * 可以安全地再次调用 prepare_to_wait。
         */
        finish_wait(&queue, &wait);
    }

    /*
     * 从 break 或 while 条件为假退出：
     * 统一在这里调用一次 finish_wait，
     * 处理"prepare 之后条件立刻满足 break 出来"的情况，
     * 确保状态和队列都被正确清理。
     */
    finish_wait(&queue, &wait);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  工作线程                                                             */
/* ------------------------------------------------------------------ */
static int worker_thread(void *data)
{
    int ret;

    /* 原子领号：先到先得，号码唯一递增 */
    int my_ticket = atomic_fetch_add(1, &ticket_counter);

    pr_info("[pwq] %-20s 领号 %d\n", current->comm, my_ticket);

    /* 等待叫到自己的号 */
    ret = wait_for_my_turn(my_ticket);
    if (ret) {
        pr_warn("[pwq] 号码 %d 被信号中断，退出\n", my_ticket);
        return ret;
    }

    /* ---- 临界区：此刻只有一个线程在这里 ---- */
    pr_info("[pwq] >>> 号码 %d [%s] 开始执行\n",
            my_ticket, current->comm);
    msleep(300);
    pr_info("[pwq] <<< 号码 %d [%s] 执行完毕\n",
            my_ticket, current->comm);
    /* ---- 临界区结束 ---- */

    /* 叫下一号，唤醒所有睡眠线程 */
    atomic_inc(&now_serving);
    wake_up_all(&queue);

    return 0;
}

/* ------------------------------------------------------------------ */
static int __init pwq_init(void)
{
    int i;
    pr_info("[pwq] 模块加载，启动 %d 个线程\n", NUM_THREADS);

    for (i = 0; i < NUM_THREADS; i++) {
        threads[i] = kthread_run(worker_thread, NULL,
                                 "pwq_worker/%d", i);
        if (IS_ERR(threads[i])) {
            pr_err("[pwq] 创建线程 %d 失败\n", i);
            return PTR_ERR(threads[i]);
        }
        msleep(30);
    }
    return 0;
}

static void __exit pwq_exit(void)
{
    int i;
    /* 推进 now_serving 到不可能的值，唤醒所有等待者让它们退出 */
    atomic_set(&now_serving, NUM_THREADS + 999);
    wake_up_all(&queue);

    for (i = 0; i < NUM_THREADS; i++)
        if (threads[i] && !IS_ERR(threads[i]))
            kthread_stop(threads[i]);

    pr_info("[pwq] 模块卸载完成\n");
}

module_init(pwq_init);
module_exit(pwq_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("prepare_to_wait/finish_wait 架构 — 取号串行执行");

// completion_demo.c
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/completion.h>
#include <linux/kthread.h>
#include <linux/delay.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Demo");
MODULE_DESCRIPTION("reinit_completion demo");

static struct completion my_comp;
static struct task_struct *worker_thread;

static int worker_fn(void *data)
{
    int i;
    for (i = 1; i <= 3; i++) {
        pr_info("[worker] 第 %d 次：开始工作...\n", i);
        msleep(1000);  // 模拟耗时工作

        pr_info("[worker] 第 %d 次：工作完成，通知主线程\n", i);
        complete(&my_comp);  // 通知等待方

        msleep(500);   // 给主线程时间处理
    }
    return 0;
}

static int __init comp_demo_init(void)
{
    int i;

    pr_info("[main] 模块加载，初始化 completion\n");
    init_completion(&my_comp);  // 第一次初始化

    // 启动工作线程
    worker_thread = kthread_run(worker_fn, NULL, "comp_worker");
    if (IS_ERR(worker_thread)) {
        pr_err("[main] 创建线程失败\n");
        return PTR_ERR(worker_thread);
    }

    // 主线程循环等待，演示 reinit_completion
    for (i = 1; i <= 3; i++) {
        pr_info("[main] 第 %d 次：等待 completion...\n", i);
        wait_for_completion(&my_comp);  // 阻塞等待
        pr_info("[main] 第 %d 次：收到通知！\n", i);

        if (i < 3) {
            pr_info("[main] 第 %d 次：reinit_completion，准备下一轮\n", i);
            reinit_completion(&my_comp);  // 关键：重置为未完成状态
        }
    }

    pr_info("[main] 所有轮次完成！\n");
    return 0;
}

static void __exit comp_demo_exit(void)
{
    pr_info("[main] 模块卸载\n");
}

module_init(comp_demo_init);
module_exit(comp_demo_exit);

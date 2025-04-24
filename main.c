/* kxo: A Tic-Tac-Toe Game Engine implemented as Linux kernel module */

#include <linux/cdev.h>
#include <linux/circ_buf.h>
#include <linux/interrupt.h>
#include <linux/kfifo.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/version.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>

#include "game.h"
#include "mcts.h"
#include "negamax.h"

MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("National Cheng Kung University, Taiwan");
MODULE_DESCRIPTION("In-kernel Tic-Tac-Toe game engine");

/* Macro DECLARE_TASKLET_OLD exists for compatibiity.
 * See https://lwn.net/Articles/830964/
 */
#ifndef DECLARE_TASKLET_OLD
#define DECLARE_TASKLET_OLD(arg1, arg2) DECLARE_TASKLET(arg1, arg2, 0L)
#endif

#define DEV_NAME "kxo"

#define NR_KMLDRV 1

static int delay = 1000; /* time (in ms) to generate an event */

/* Declare kernel module attribute for sysfs */

struct kxo_attr {
    char display;
    char resume;
    char end;
    rwlock_t lock;
};

static struct kxo_attr attr_obj;

static ssize_t kxo_state_show(struct device *dev,
                              struct device_attribute *attr,
                              char *buf)
{
    read_lock(&attr_obj.lock);
    int ret = snprintf(buf, 6, "%c %c %c\n", attr_obj.display, attr_obj.resume,
                       attr_obj.end);
    read_unlock(&attr_obj.lock);
    return ret;
}

static ssize_t kxo_state_store(struct device *dev,
                               struct device_attribute *attr,
                               const char *buf,
                               size_t count)
{
    write_lock(&attr_obj.lock);
    sscanf(buf, "%c %c %c", &(attr_obj.display), &(attr_obj.resume),
           &(attr_obj.end));
    write_unlock(&attr_obj.lock);
    return count;
}

static DEVICE_ATTR_RW(kxo_state);

/* Data produced by the simulated device */

/* Timer to simulate a periodic IRQ */
static struct timer_list timer;

/* Character device stuff */
static int major;
static struct class *kxo_class;
static struct cdev kxo_cdev, kxo_cdev2;

static char move_step[4];
static char move_step2[4];

/* Data are stored into a kfifo buffer before passing them to the userspace */
static DECLARE_KFIFO_PTR(rx_fifo, unsigned char);
static DECLARE_KFIFO_PTR(rx_fifo2, unsigned char);

/* NOTE: the usage of kfifo is safe (no need for extra locking), until there is
 * only one concurrent reader and one concurrent writer. Writes are serialized
 * from the interrupt context, readers are serialized using this mutex.
 */
static DEFINE_MUTEX(read_lock);
static DEFINE_MUTEX(read_lock2);

/* Wait queue to implement blocking I/O from userspace */
static DECLARE_WAIT_QUEUE_HEAD(rx_wait);
static DECLARE_WAIT_QUEUE_HEAD(rx_wait2);

/* Insert the whole chess board into the kfifo buffer */
static void produce_board(void)
{
    unsigned int len = kfifo_in(&rx_fifo, move_step, sizeof(move_step));
    if (unlikely(len < sizeof(move_step)) && printk_ratelimit())
        pr_warn("%s: %zu bytes dropped\n", __func__, sizeof(move_step) - len);

    pr_debug("kxo: %s: in %u/%u bytes\n", __func__, len, kfifo_len(&rx_fifo));
}

// /* Mutex to serialize kfifo writers within the workqueue handler */
// static DEFINE_MUTEX(producer_lock);  // will deleted

/* Mutex to serialize fast_buf consumers: we can use a mutex because consumers
 * run in workqueue handler (kernel thread context).
 */
// static DEFINE_MUTEX(consumer_lock);  // will deleted

/* We use an additional "faster" circular buffer to quickly store data from
 * interrupt context, before adding them to the kfifo.
 */
static struct circ_buf fast_buf;

static char table[N_GRIDS];
static char table2[N_GRIDS];

/* Clear all data from the circular buffer fast_buf */
static void fast_buf_clear(void)
{
    fast_buf.head = fast_buf.tail = 0;
}

/* Workqueue handler: executed by a kernel thread */
static void drawboard_work_func(void)
{
    read_lock(&attr_obj.lock);
    if (attr_obj.display == '0') {
        read_unlock(&attr_obj.lock);
        return;
    }
    read_unlock(&attr_obj.lock);

    produce_board();

    wake_up_interruptible(&rx_wait);
}
static void drawboard_work_func2(void)
{
    read_lock(&attr_obj.lock);
    if (attr_obj.display == '0') {
        read_unlock(&attr_obj.lock);
        return;
    }
    read_unlock(&attr_obj.lock);
    unsigned int len = kfifo_in(&rx_fifo2, move_step2, sizeof(move_step2));
    pr_debug("kxo: %s: in %u/%u bytes\n", __func__, len, kfifo_len(&rx_fifo2));
    wake_up_interruptible(&rx_wait2);
}

static char turn;
static int finish;

static void ai_one_work_func(void)
{
    int move;
    WRITE_ONCE(move, mcts(table, 'O'));

    smp_mb();

    if (move != -1)
        WRITE_ONCE(table[move], 'O');

    char tmp[4] = {'O', move + '0', 'N', '\n'};
    memcpy(move_step, tmp, sizeof(tmp));
    smp_wmb();

    WRITE_ONCE(turn, 'X');
    WRITE_ONCE(finish, 1);
    smp_wmb();
}
static void ai_one_work_func2(void)
{
    int move;
    WRITE_ONCE(move, mcts(table2, 'O'));
    smp_mb();

    if (move != -1)
        WRITE_ONCE(table2[move], 'O');

    char tmp[4] = {'O', move + '0', 'N', '\n'};
    memcpy(move_step2, tmp, sizeof(tmp));
    smp_wmb();
}

static void ai_two_work_func(void)
{
    int move;
    WRITE_ONCE(move, negamax_predict(table, 'X').move);

    smp_mb();

    if (move != -1)
        WRITE_ONCE(table[move], 'X');

    char tmp[4] = {'X', move + '0', 'N', '\n'};
    memcpy(move_step, tmp, sizeof(tmp));
    smp_wmb();

    WRITE_ONCE(turn, 'O');
    WRITE_ONCE(finish, 1);
    smp_wmb();
}
static void ai_two_work_func2(void)
{
    int move;
    WRITE_ONCE(move, negamax_predict(table2, 'X').move);
    smp_mb();

    if (move != -1)
        WRITE_ONCE(table2[move], 'X');

    char tmp[4] = {'X', move + '0', 'N', '\n'};
    memcpy(move_step2, tmp, sizeof(tmp));
    smp_wmb();
}
/*use coroutine to package ai_1、ai_2 and drawboard in one work item
 *and tasklet can just put one item into work queue
 */

static void entire_game_func(struct work_struct *w)
{
    int cpu;
    WARN_ON_ONCE(in_softirq());
    WARN_ON_ONCE(in_interrupt());

    cpu = get_cpu();
    pr_info("kxo: [CPU#%d] %s\n", cpu, __func__);

    READ_ONCE(finish);
    READ_ONCE(turn);
    smp_rmb();
    if (finish && turn == 'O') {
        WRITE_ONCE(finish, 0);
        smp_wmb();
        ai_one_work_func();
    } else if (finish && turn == 'X') {
        WRITE_ONCE(finish, 0);
        smp_wmb();
        ai_two_work_func();
    }
    drawboard_work_func();
    put_cpu();
}
/*test if game2 worj or not*/
static void test_game2_func(struct work_struct *w)
{
    int cpu;
    WARN_ON_ONCE(in_softirq());
    WARN_ON_ONCE(in_interrupt());
    cpu = get_cpu();
    pr_info("kxo: [CPU#%d] %s\n", cpu, __func__);
    ai_one_work_func2();
    drawboard_work_func2();
    ai_two_work_func2();
    drawboard_work_func2();
    put_cpu();
}
/* Workqueue for asynchronous bottom-half processing */
static struct workqueue_struct *kxo_workqueue;

/* Work item: holds a pointer to the function that is going to be executed
 * asynchronously.
 */
// static DECLARE_WORK(drawboard_work, drawboard_work_func);
// static DECLARE_WORK(ai_one_work, ai_one_work_func);
// static DECLARE_WORK(ai_two_work, ai_two_work_func);
static DECLARE_WORK(entire_game, entire_game_func);
static DECLARE_WORK(test_game2, test_game2_func);

/* Tasklet handler.
 *
 * NOTE: different tasklets can run concurrently on different processors, but
 * two of the same type of tasklet cannot run simultaneously. Moreover, a
 * tasklet always runs on the same CPU that schedules it.
 */
static void game_tasklet_func(unsigned long __data)
{
    ktime_t tv_start, tv_end;
    s64 nsecs;

    WARN_ON_ONCE(!in_interrupt());
    WARN_ON_ONCE(!in_softirq());

    tv_start = ktime_get();

    queue_work(kxo_workqueue, &entire_game);
    queue_work(kxo_workqueue, &test_game2);

    tv_end = ktime_get();

    nsecs = (s64) ktime_to_ns(ktime_sub(tv_end, tv_start));

    pr_info("kxo: [CPU#%d] %s in_softirq: %llu usec\n", smp_processor_id(),
            __func__, (unsigned long long) nsecs >> 10);
}

/* Tasklet for asynchronous bottom-half processing in softirq context */
static DECLARE_TASKLET_OLD(game_tasklet, game_tasklet_func);

static void ai_game(void)
{
    WARN_ON_ONCE(!irqs_disabled());

    pr_info("kxo: [CPU#%d] doing AI game\n", smp_processor_id());
    pr_info("kxo: [CPU#%d] scheduling tasklet\n", smp_processor_id());
    tasklet_schedule(&game_tasklet);
}

static void timer_handler(struct timer_list *__timer)
{
    ktime_t tv_start, tv_end;
    s64 nsecs;

    pr_info("kxo: [CPU#%d] enter %s\n", smp_processor_id(), __func__);
    /* We are using a kernel timer to simulate a hard-irq, so we must expect
     * to be in softirq context here.
     */
    WARN_ON_ONCE(!in_softirq());

    /* Disable interrupts for this CPU to simulate real interrupt context */
    local_irq_disable();

    tv_start = ktime_get();

    char win = check_win(table);
    char win2 = check_win(table2);

    if (win == ' ' && win2 == ' ') {
        ai_game();
        mod_timer(&timer, jiffies + msecs_to_jiffies(delay));
    } else if (win != ' ') {
        read_lock(&attr_obj.lock);
        if (attr_obj.display == '1') {
            int cpu = get_cpu();
            pr_info("kxo: [CPU#%d] Drawing final board\n", cpu);
            put_cpu();

            move_step[2] = 'W';
            smp_wmb();
            move_step[3] = '\n';
            smp_wmb();

            /* Store data to the kfifo buffer */
            produce_board();
            wake_up_interruptible(&rx_wait);
        }

        if (attr_obj.end == '0') {
            memset(table, ' ',
                   N_GRIDS); /* Reset the table so the game restart */
            mod_timer(&timer, jiffies + msecs_to_jiffies(delay));
        }
        read_unlock(&attr_obj.lock);
        pr_info("kxo: %c win!!!\n", win);
    }
    if (win2 != ' ') {
        read_lock(&attr_obj.lock);
        if (attr_obj.display == '1') {
            int cpu = get_cpu();
            pr_info("kxo2: [CPU#%d] Drawing final board\n", cpu);
            put_cpu();

            move_step2[0] = win2;
            smp_wmb();
            move_step2[2] = 'W';
            smp_wmb();
            move_step2[3] = '\n';
            smp_wmb();
            /* Store data to the kfifo buffer */
            pr_info("Test!!!!!%cbut%c\n", win2, move_step2[0]);
            kfifo_in(&rx_fifo2, move_step2, sizeof(move_step2));

            wake_up_interruptible(&rx_wait2);
        }

        if (attr_obj.end == '0') {
            memset(table2, ' ',
                   N_GRIDS); /* Reset the table so the game restart */
            mod_timer(&timer, jiffies + msecs_to_jiffies(delay));
        }

        read_unlock(&attr_obj.lock);

        pr_info("kxo2: %c win!!!\n", win2);
    }
    tv_end = ktime_get();

    nsecs = (s64) ktime_to_ns(ktime_sub(tv_end, tv_start));

    pr_info("kxo: [CPU#%d] %s in_irq: %llu usec\n", smp_processor_id(),
            __func__, (unsigned long long) nsecs >> 10);

    local_irq_enable();
}

static ssize_t kxo_read(struct file *file,
                        char __user *buf,
                        size_t count,
                        loff_t *ppos)
{
    unsigned int read;
    int ret;

    pr_debug("kxo: %s(%p, %zd, %lld)\n", __func__, buf, count, *ppos);

    if (unlikely(!access_ok(buf, count)))
        return -EFAULT;

    if (mutex_lock_interruptible(&read_lock))
        return -ERESTARTSYS;

    do {
        ret = kfifo_to_user(&rx_fifo, buf, count, &read);
        if (unlikely(ret < 0))
            break;
        if (read)
            break;
        if (file->f_flags & O_NONBLOCK) {
            ret = -EAGAIN;
            break;
        }
        ret = wait_event_interruptible(rx_wait, kfifo_len(&rx_fifo));
    } while (ret == 0);
    pr_debug("kxo: %s: out %u/%u bytes\n", __func__, read, kfifo_len(&rx_fifo));

    mutex_unlock(&read_lock);

    return ret ? ret : read;
}

static atomic_t open_cnt;

static int kxo_open(struct inode *inode, struct file *filp)
{
    pr_debug("kxo: %s\n", __func__);
    if (atomic_inc_return(&open_cnt) == 1)
        mod_timer(&timer, jiffies + msecs_to_jiffies(delay));
    pr_info("openm current cnt: %d\n", atomic_read(&open_cnt));

    return 0;
}

static int kxo_release(struct inode *inode, struct file *filp)
{
    pr_debug("kxo: %s\n", __func__);
    if (atomic_dec_and_test(&open_cnt) == 0) {
        del_timer_sync(&timer);
        flush_workqueue(kxo_workqueue);
        fast_buf_clear();
    }
    pr_info("release, current cnt: %d\n", atomic_read(&open_cnt));

    return 0;
}

static const struct file_operations kxo_fops = {
    .read = kxo_read,
    .llseek = no_llseek,
    .open = kxo_open,
    .release = kxo_release,
    .owner = THIS_MODULE,
};

static ssize_t kxo_read2(struct file *file,
                         char __user *buf,
                         size_t count,
                         loff_t *ppos)
{
    unsigned int read;
    int ret;

    if (unlikely(!access_ok(buf, count)))
        return -EFAULT;

    if (mutex_lock_interruptible(&read_lock2))
        return -ERESTARTSYS;

    do {
        ret = kfifo_to_user(&rx_fifo2, buf, count, &read);
        if (unlikely(ret < 0))
            break;
        if (read)
            break;
        if (file->f_flags & O_NONBLOCK) {
            ret = -EAGAIN;
            break;
        }
        ret = wait_event_interruptible(rx_wait2, kfifo_len(&rx_fifo2));
    } while (ret == 0);
    mutex_unlock(&read_lock2);

    return ret ? ret : read;
}
static const struct file_operations kxo_fops2 = {
    .read = kxo_read2,
    .llseek = no_llseek,
    .open = no_llseek,
    .release = no_llseek,
    .owner = THIS_MODULE,
};

static int __init kxo_init(void)
{
    dev_t dev_id;
    int ret;

    if (kfifo_alloc(&rx_fifo, PAGE_SIZE, GFP_KERNEL) < 0)
        return -ENOMEM;

    if (kfifo_alloc(&rx_fifo2, PAGE_SIZE, GFP_KERNEL) < 0)
        return -ENOMEM;

    /* Register major/minor numbers */
    ret = alloc_chrdev_region(&dev_id, 0, 2, DEV_NAME);
    if (ret)
        goto error_alloc;
    major = MAJOR(dev_id);

    /* Add the character device to the system */
    cdev_init(&kxo_cdev, &kxo_fops);
    ret = cdev_add(&kxo_cdev, MKDEV(major, 0), NR_KMLDRV);
    if (ret) {
        kobject_put(&kxo_cdev.kobj);
        goto error_region;
    }
    cdev_init(&kxo_cdev2, &kxo_fops2);
    ret = cdev_add(&kxo_cdev2, MKDEV(major, 1), NR_KMLDRV);
    if (ret) {
        kobject_put(&kxo_cdev2.kobj);
        goto error_region;
    }

    /* Create a class structure */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 4, 0)
    kxo_class = class_create(THIS_MODULE, DEV_NAME);
#else
    kxo_class = class_create(DEV_NAME);
#endif
    if (IS_ERR(kxo_class)) {
        printk(KERN_ERR "error creating kxo class\n");
        ret = PTR_ERR(kxo_class);
        goto error_cdev;
    }

    /* Register the device with sysfs */
    struct device *kxo_dev =
        device_create(kxo_class, NULL, MKDEV(major, 0), NULL, DEV_NAME);
    device_create(kxo_class, NULL, MKDEV(major, 1), NULL, "kxo2");

    ret = device_create_file(kxo_dev, &dev_attr_kxo_state);
    if (ret < 0) {
        printk(KERN_ERR "failed to create sysfs file kxo_state\n");
        goto error_cdev;
    }

    /* Allocate fast circular buffer */
    fast_buf.buf = vmalloc(PAGE_SIZE);
    if (!fast_buf.buf) {
        device_destroy(kxo_class, dev_id);
        class_destroy(kxo_class);
        ret = -ENOMEM;
        goto error_cdev;
    }

    /* Create the workqueue */
    kxo_workqueue = alloc_workqueue("kxod", WQ_UNBOUND, WQ_MAX_ACTIVE);
    if (!kxo_workqueue) {
        vfree(fast_buf.buf);
        device_destroy(kxo_class, dev_id);
        class_destroy(kxo_class);
        ret = -ENOMEM;
        goto error_cdev;
    }

    negamax_init();
    mcts_init();
    memset(table, ' ', N_GRIDS);
    memset(table2, ' ', N_GRIDS);
    turn = 'O';
    finish = 1;

    attr_obj.display = '1';
    attr_obj.resume = '1';
    attr_obj.end = '0';
    rwlock_init(&attr_obj.lock);
    /* Setup the timer */
    timer_setup(&timer, timer_handler, 0);
    atomic_set(&open_cnt, 0);

    pr_info("kxo: registered new kxo device: %d,%d\n", major, 0);
out:
    return ret;
error_cdev:
    cdev_del(&kxo_cdev);
error_region:
    unregister_chrdev_region(dev_id, 2);
error_alloc:
    kfifo_free(&rx_fifo);
    kfifo_free(&rx_fifo2);
    goto out;
}

static void __exit kxo_exit(void)
{
    dev_t dev_id = MKDEV(major, 0);
    dev_t dev_id2 = MKDEV(major, 1);

    del_timer_sync(&timer);
    tasklet_kill(&game_tasklet);
    flush_workqueue(kxo_workqueue);
    destroy_workqueue(kxo_workqueue);
    vfree(fast_buf.buf);
    device_destroy(kxo_class, dev_id);
    device_destroy(kxo_class, dev_id2);
    class_destroy(kxo_class);
    cdev_del(&kxo_cdev);
    unregister_chrdev_region(dev_id, 1);
    unregister_chrdev_region(dev_id2, 1);

    kfifo_free(&rx_fifo);
    kfifo_free(&rx_fifo2);
    pr_info("kxo: unloaded\n");
}

module_init(kxo_init);
module_exit(kxo_exit);

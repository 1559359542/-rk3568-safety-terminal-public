#include <linux/capability.h>
#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/pwm.h>
#include <linux/slab.h>
#include <linux/timekeeping.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include "safety_event_uapi.h"

#define SAFETY_EVENT_NAME "safety_event"
#define SAFETY_EVENT_DOOR_DEBOUNCE_MS 50

#define BH1750_CMD_POWER_DOWN 0x00
#define BH1750_CMD_POWER_ON 0x01
#define BH1750_CMD_RESET 0x07
#define BH1750_CMD_CONT_H_RES 0x10
#define BH1750_FIRST_SAMPLE_MS 180

#define SAFETY_ALARM_PWM_PERIOD_NS 500000U
#define SAFETY_ALARM_PWM_DUTY_NS 250000U

struct safety_event_dev {
    struct device *dev;
    struct cdev cdev;
    dev_t devt;
    struct class *class;
    struct device *char_dev;

    struct mutex lock; /* 保护事件缓存与序号 */
    wait_queue_head_t waitq;
    struct safety_event_record event;
    u32 published_sequence;
    u32 consumed_sequence;

    struct gpio_desc *door_gpiod;
    int door_irq;
    bool door_irq_seen;
    unsigned long last_door_irq_jiffies;

    struct gpio_desc *alarm_gpiod; /* DTS alarm-gpios，模块独占 LED */
    struct pwm_device *alarm_pwm;  /* DTS pwms/alarm，模块独占 PWM15 */
    struct mutex alarm_lock;
    bool door_alarm;
    bool low_lux_alarm;
    bool user_alarm;    /* 用户态守护进程请求的告警状态 */

    struct i2c_client *bh1750;
    struct delayed_work bh1750_work;
    u32 lux_low_threshold;
    u32 lux_recover_threshold;
    u32 lux_sample_period_ms;
};

static bool allow_test_inject;
module_param(allow_test_inject, bool, 0600);
MODULE_PARM_DESC(allow_test_inject, "允许管理员在开发期注入测试事件");

static void safety_event_publish(struct safety_event_dev *sdev,
                                 struct safety_event_record *event)
{
    mutex_lock(&sdev->lock);

    event->sequence = ++sdev->published_sequence;
    event->timestamp_ns = ktime_get_real_ns();
    sdev->event = *event;

    mutex_unlock(&sdev->lock);
    wake_up_interruptible(&sdev->waitq);
}

static int safety_event_apply_alarm_locked(struct safety_event_dev *sdev)
{
    bool active = sdev->door_alarm || sdev->low_lux_alarm ||
                  sdev->user_alarm;
    int ret;
    ret = pwm_set_polarity(sdev->alarm_pwm, PWM_POLARITY_NORMAL);
    if (ret)
        return ret;
    gpiod_set_value_cansleep(sdev->alarm_gpiod, active);

    if (!active) {
        ret = pwm_config(sdev->alarm_pwm, 0,
                        SAFETY_ALARM_PWM_PERIOD_NS);
        if (ret)
            return ret;

        return pwm_enable(sdev->alarm_pwm);
    }

    ret = pwm_config(sdev->alarm_pwm, SAFETY_ALARM_PWM_DUTY_NS,
                     SAFETY_ALARM_PWM_PERIOD_NS);
    if (ret)
        return ret;

    return pwm_enable(sdev->alarm_pwm);
}

static void safety_event_update_alarm(struct safety_event_dev *sdev,
                                      bool door_alarm, bool low_lux_alarm)
{
    int ret;

    mutex_lock(&sdev->alarm_lock);
    sdev->door_alarm = door_alarm;
    sdev->low_lux_alarm = low_lux_alarm;
    ret = safety_event_apply_alarm_locked(sdev);
    mutex_unlock(&sdev->alarm_lock);

    if (ret)
        dev_err(sdev->dev, "failed to update alarm outputs: %d\n", ret);
}

static int safety_event_set_user_alarm(struct safety_event_dev *sdev,
                                       bool user_alarm)
{
    int ret;

    mutex_lock(&sdev->alarm_lock);
    sdev->user_alarm = user_alarm;
    ret = safety_event_apply_alarm_locked(sdev);
    mutex_unlock(&sdev->alarm_lock);

    if (ret)
        dev_err(sdev->dev, "failed to update user alarm: %d\n", ret);

    return ret;
}

static void safety_event_clear_all_alarms(struct safety_event_dev *sdev)
{
    int ret;

    mutex_lock(&sdev->alarm_lock);
    sdev->door_alarm = false;
    sdev->low_lux_alarm = false;
    sdev->user_alarm = false;
    ret = safety_event_apply_alarm_locked(sdev);
    mutex_unlock(&sdev->alarm_lock);

    if (ret)
        dev_err(sdev->dev, "failed to clear alarm outputs: %d\n", ret);
}

static int safety_event_bh1750_write(struct safety_event_dev *sdev, u8 command)
{
    int ret;

    ret = i2c_master_send(sdev->bh1750, &command, sizeof(command));
    if (ret == sizeof(command))
        return 0;

    return ret < 0 ? ret : -EIO;
}

static int safety_event_bh1750_start(struct safety_event_dev *sdev)
{
    int ret;

    ret = safety_event_bh1750_write(sdev, BH1750_CMD_POWER_ON);
    if (ret)
        return ret;

    ret = safety_event_bh1750_write(sdev, BH1750_CMD_RESET);
    if (ret)
        return ret;

    return safety_event_bh1750_write(sdev, BH1750_CMD_CONT_H_RES);
}

static int safety_event_bh1750_read_lux(struct safety_event_dev *sdev, u32 *lux)
{
    u8 data[2];
    u16 raw;
    int ret;

    ret = i2c_master_recv(sdev->bh1750, data, sizeof(data));
    if (ret != sizeof(data))
        return ret < 0 ? ret : -EIO;

    raw = ((u16)data[0] << 8) | data[1];
    /* 默认 MTreg=69 的 H-resolution 模式：lux = raw / 1.2 */
    *lux = ((u32)raw * 10 + 6) / 12;
    return 0;
}

static void safety_event_bh1750_work(struct work_struct *work)
{
    struct safety_event_dev *sdev =
        container_of(to_delayed_work(work), struct safety_event_dev,
                     bh1750_work);
    struct safety_event_record event = {
        .source = SAFETY_EVENT_SOURCE_BH1750,
    };
    u32 lux;
    int ret;

    ret = safety_event_bh1750_read_lux(sdev, &lux);
    if (ret) {
        dev_warn_ratelimited(sdev->dev, "BH1750 read failed: %d\n", ret);
        ret = safety_event_bh1750_start(sdev);
        if (ret)
            dev_warn_ratelimited(sdev->dev,
                                 "BH1750 reinitialize failed: %d\n", ret);
        goto out_reschedule;
    }

    /* 每个成功样本都向用户态发布；阈值穿越时复用同一条记录。 */
    event.type = SAFETY_EVENT_TYPE_BH1750_SAMPLE;
    event.lux = lux;
    mutex_lock(&sdev->alarm_lock);
    if (!sdev->low_lux_alarm && lux <= sdev->lux_low_threshold) {
        sdev->low_lux_alarm = true;
        event.type = SAFETY_EVENT_TYPE_LOW_LUX;
    } else if (sdev->low_lux_alarm && lux >= sdev->lux_recover_threshold) {
        sdev->low_lux_alarm = false;
        event.type = SAFETY_EVENT_TYPE_LUX_RECOVER;
    }
    event.state = sdev->low_lux_alarm ? SAFETY_EVENT_STATE_LUX_LOW :
                      SAFETY_EVENT_STATE_LUX_NORMAL;

    ret = safety_event_apply_alarm_locked(sdev);
    mutex_unlock(&sdev->alarm_lock);

    if (ret)
        dev_err(sdev->dev, "failed to update alarm outputs: %d\n", ret);

    if (event.type != SAFETY_EVENT_TYPE_BH1750_SAMPLE) {
        dev_info(sdev->dev, "BH1750 lux=%u event=%u\n",
                 lux, event.type);
    }
    safety_event_publish(sdev, &event);

out_reschedule:
    schedule_delayed_work(&sdev->bh1750_work,
                          msecs_to_jiffies(sdev->lux_sample_period_ms));
}

static irqreturn_t safety_event_door_irq_thread(int irq, void *data)
{
    struct safety_event_dev *sdev = data;
    struct safety_event_record event = {
        .type = SAFETY_EVENT_TYPE_DOOR_STATE_CHANGED,
        .source = SAFETY_EVENT_SOURCE_DOOR_GPIO,
    };
    unsigned long now = jiffies;
    int value;

    value = gpiod_get_value_cansleep(sdev->door_gpiod);
    if (value < 0) {
        dev_err(sdev->dev, "failed to read door GPIO: %d\n", value);
        return IRQ_HANDLED;
    }

    if (sdev->door_irq_seen &&
        time_before(now, sdev->last_door_irq_jiffies +
                    msecs_to_jiffies(SAFETY_EVENT_DOOR_DEBOUNCE_MS)))
        return IRQ_HANDLED;

    sdev->door_irq_seen = true;
    sdev->last_door_irq_jiffies = now;
    event.state = value ? SAFETY_EVENT_STATE_LOGICAL_ACTIVE :
                          SAFETY_EVENT_STATE_LOGICAL_INACTIVE;

    safety_event_update_alarm(sdev, value, sdev->low_lux_alarm);
    safety_event_publish(sdev, &event);
    return IRQ_HANDLED;
}

static int safety_event_open(struct inode *inode, struct file *file)
{
    file->private_data = container_of(inode->i_cdev,
                                      struct safety_event_dev, cdev);
    return 0;
}

static ssize_t safety_event_read(struct file *file, char __user *buffer,
                                 size_t count, loff_t *offset)
{
    struct safety_event_dev *sdev = file->private_data;
    struct safety_event_record event;
    int ret;

    if (count < sizeof(event))
        return -EINVAL;

    for (;;) {
        ret = mutex_lock_interruptible(&sdev->lock);
        if (ret)
            return ret;

        if (sdev->published_sequence != sdev->consumed_sequence) {
            event = sdev->event;
            sdev->consumed_sequence = sdev->published_sequence;
            mutex_unlock(&sdev->lock);

            if (copy_to_user(buffer, &event, sizeof(event)))
                return -EFAULT;
            return sizeof(event);
        }

        mutex_unlock(&sdev->lock);

        if (file->f_flags & O_NONBLOCK)
            return -EAGAIN;

        ret = wait_event_interruptible(
            sdev->waitq,
            READ_ONCE(sdev->published_sequence) !=
                READ_ONCE(sdev->consumed_sequence));
        if (ret)
            return ret;
    }
}

static unsigned int safety_event_poll(struct file *file, poll_table *wait)
{
    struct safety_event_dev *sdev = file->private_data;
    unsigned int mask = 0;

    poll_wait(file, &sdev->waitq, wait);

    mutex_lock(&sdev->lock);
    if (sdev->published_sequence != sdev->consumed_sequence)
        mask |= POLLIN | POLLRDNORM;
    mutex_unlock(&sdev->lock);

    return mask;
}

static long safety_event_ioctl(struct file *file, unsigned int cmd,
                               unsigned long arg)
{
    struct safety_event_dev *sdev = file->private_data;
    struct safety_event_record event;
    struct safety_event_user_alarm_request request;
    __u32 version = SAFETY_EVENT_API_VERSION;

    switch (cmd) {
    case SAFETY_EVENT_IOC_GET_API_VERSION:
        if (copy_to_user((void __user *)arg, &version, sizeof(version)))
            return -EFAULT;
        return 0;

    case SAFETY_EVENT_IOC_INJECT:
        if (!allow_test_inject || !capable(CAP_SYS_ADMIN))
            return -EPERM;
        if (copy_from_user(&event, (void __user *)arg, sizeof(event)))
            return -EFAULT;
        if (event.type == SAFETY_EVENT_TYPE_NONE)
            return -EINVAL;

        safety_event_publish(sdev, &event);
        return 0;
    case SAFETY_EVENT_IOC_SET_USER_ALARM:
        if (!capable(CAP_SYS_ADMIN))
            return -EPERM;
        if (copy_from_user(&request, (void __user *)arg, sizeof(request)))
            return -EFAULT;
        if (request.active > 1 || request.reserved != 0)
            return -EINVAL;

        return safety_event_set_user_alarm(sdev, request.active != 0);
    default:
        return -ENOTTY;
    }
}

static const struct file_operations safety_event_fops = {
    .owner = THIS_MODULE,
    .open = safety_event_open,
    .read = safety_event_read,
    .poll = safety_event_poll,
    .unlocked_ioctl = safety_event_ioctl,
    .llseek = no_llseek,
};

static int safety_event_parse_bh1750(struct platform_device *pdev,
                                     struct safety_event_dev *sdev)
{
    struct i2c_adapter *adapter;
    struct i2c_board_info info = {
        I2C_BOARD_INFO("safety-bh1750", 0),
    };
    u32 bus_num;
    u32 address;
    int ret;

    ret = of_property_read_u32(pdev->dev.of_node, "bh1750-bus-num",
                               &bus_num);
    if (ret)
        return ret;

    ret = of_property_read_u32(pdev->dev.of_node, "bh1750-addr", &address);
    if (ret || address > 0x7f)
        return ret ? ret : -EINVAL;

    ret = of_property_read_u32(pdev->dev.of_node, "lux-low-threshold",
                               &sdev->lux_low_threshold);
    if (ret)
        return ret;

    ret = of_property_read_u32(pdev->dev.of_node, "lux-recover-threshold",
                               &sdev->lux_recover_threshold);
    if (ret)
        return ret;

    ret = of_property_read_u32(pdev->dev.of_node, "lux-sample-period-ms",
                               &sdev->lux_sample_period_ms);
    if (ret)
        return ret;

    if (!sdev->lux_sample_period_ms ||
        sdev->lux_low_threshold >= sdev->lux_recover_threshold)
        return -EINVAL;

    adapter = i2c_get_adapter(bus_num);
    if (!adapter)
        return -EPROBE_DEFER;

    info.addr = address;
    sdev->bh1750 = i2c_new_device(adapter, &info);
    i2c_put_adapter(adapter);
    if (!sdev->bh1750)
        return -ENODEV;

    return 0;
}

static int safety_event_probe(struct platform_device *pdev)
{
    struct safety_event_dev *sdev;
    int ret;

    sdev = devm_kzalloc(&pdev->dev, sizeof(*sdev), GFP_KERNEL);
    if (!sdev)
        return -ENOMEM;

    sdev->dev = &pdev->dev;
    mutex_init(&sdev->lock);
    mutex_init(&sdev->alarm_lock);
    init_waitqueue_head(&sdev->waitq);
    INIT_DELAYED_WORK(&sdev->bh1750_work, safety_event_bh1750_work);

    sdev->door_gpiod = devm_gpiod_get_optional(&pdev->dev, "door", GPIOD_IN);
    if (IS_ERR(sdev->door_gpiod))
        return PTR_ERR(sdev->door_gpiod);

    sdev->alarm_gpiod = devm_gpiod_get(&pdev->dev, "alarm", GPIOD_OUT_LOW);
    if (IS_ERR(sdev->alarm_gpiod))
        return PTR_ERR(sdev->alarm_gpiod);

    sdev->alarm_pwm = devm_pwm_get(&pdev->dev, "alarm");
    if (IS_ERR(sdev->alarm_pwm))
        return PTR_ERR(sdev->alarm_pwm);

    ret = pwm_config(sdev->alarm_pwm, 0, SAFETY_ALARM_PWM_PERIOD_NS);
    if (ret)
        return ret;
    pwm_disable(sdev->alarm_pwm);

    ret = alloc_chrdev_region(&sdev->devt, 0, 1, SAFETY_EVENT_NAME);
    if (ret)
        return ret;

    cdev_init(&sdev->cdev, &safety_event_fops);
    sdev->cdev.owner = THIS_MODULE;
    ret = cdev_add(&sdev->cdev, sdev->devt, 1);
    if (ret)
        goto err_unregister;

    sdev->class = class_create(THIS_MODULE, SAFETY_EVENT_NAME);
    if (IS_ERR(sdev->class)) {
        ret = PTR_ERR(sdev->class);
        goto err_cdev;
    }

    sdev->char_dev = device_create(sdev->class, &pdev->dev, sdev->devt,
                                   sdev, SAFETY_EVENT_NAME);
    if (IS_ERR(sdev->char_dev)) {
        ret = PTR_ERR(sdev->char_dev);
        goto err_class;
    }

    ret = safety_event_parse_bh1750(pdev, sdev);
    if (ret)
        goto err_device;

    ret = safety_event_bh1750_start(sdev);
    if (ret)
        goto err_i2c;

    if (sdev->door_gpiod) {
        sdev->door_irq = gpiod_to_irq(sdev->door_gpiod);
        if (sdev->door_irq < 0) {
            ret = sdev->door_irq;
            goto err_i2c;
        }

        ret = devm_request_threaded_irq(
            &pdev->dev, sdev->door_irq, NULL,
            safety_event_door_irq_thread,
            IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
            SAFETY_EVENT_NAME "-door", sdev);
        if (ret)
            goto err_i2c;
    }

    platform_set_drvdata(pdev, sdev);
    schedule_delayed_work(&sdev->bh1750_work,
                          msecs_to_jiffies(BH1750_FIRST_SAMPLE_MS));
    dev_info(&pdev->dev, "BH1750 I2C3 address=0x%02x, lux %u/%u, sample=%ums\n",
             sdev->bh1750->addr, sdev->lux_low_threshold,
             sdev->lux_recover_threshold, sdev->lux_sample_period_ms);
    return 0;

err_i2c:
    i2c_unregister_device(sdev->bh1750);
err_device:
    device_destroy(sdev->class, sdev->devt);
err_class:
    class_destroy(sdev->class);
err_cdev:
    cdev_del(&sdev->cdev);
err_unregister:
    unregister_chrdev_region(sdev->devt, 1);
    return ret;
}

static int safety_event_remove(struct platform_device *pdev)
{
    struct safety_event_dev *sdev = platform_get_drvdata(pdev);

    cancel_delayed_work_sync(&sdev->bh1750_work);
    safety_event_bh1750_write(sdev, BH1750_CMD_POWER_DOWN);
    i2c_unregister_device(sdev->bh1750);
    safety_event_clear_all_alarms(sdev);

    device_destroy(sdev->class, sdev->devt);
    class_destroy(sdev->class);
    cdev_del(&sdev->cdev);
    unregister_chrdev_region(sdev->devt, 1);
    return 0;
}

static const struct of_device_id safety_event_of_match[] = {
    { .compatible = "lyy,rk3568-safety-event" },
    { }
};
MODULE_DEVICE_TABLE(of, safety_event_of_match);

static struct platform_driver safety_event_driver = {
    .probe = safety_event_probe,
    .remove = safety_event_remove,
    .driver = {
        .name = SAFETY_EVENT_NAME,
        .of_match_table = safety_event_of_match,
    },
};

module_platform_driver(safety_event_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("罗越洋");
MODULE_DESCRIPTION("RK3568 safety event driver with BH1750 low-lux alarm");

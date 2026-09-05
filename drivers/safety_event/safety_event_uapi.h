#ifndef _SAFETY_EVENT_UAPI_H_
#define _SAFETY_EVENT_UAPI_H_

#include <linux/ioctl.h>
#include <linux/types.h>

/*
 * v5 在 v4 基础上新增 BH1750 正常采样事件。
 * 内核模块、守护进程和测试程序必须使用同一版本重新编译。
 */
#define SAFETY_EVENT_API_VERSION 5

enum safety_event_type {
    SAFETY_EVENT_TYPE_NONE = 0,
    SAFETY_EVENT_TYPE_TEST = 1,
    SAFETY_EVENT_TYPE_DOOR_OPEN = 2, /* 保留 v1 值 */
    SAFETY_EVENT_TYPE_EMERGENCY_STOP = 3,
    SAFETY_EVENT_TYPE_DOOR_STATE_CHANGED = 4,
    SAFETY_EVENT_TYPE_LOW_LUX = 5,
    SAFETY_EVENT_TYPE_LUX_RECOVER = 6,
    SAFETY_EVENT_TYPE_BH1750_SAMPLE = 7,
};

enum safety_event_source {
    SAFETY_EVENT_SOURCE_NONE = 0,
    SAFETY_EVENT_SOURCE_TEST = 1,
    SAFETY_EVENT_SOURCE_DOOR_GPIO = 2,
    SAFETY_EVENT_SOURCE_BH1750 = 3,
};

enum safety_event_state {
    SAFETY_EVENT_STATE_UNKNOWN = 0,
    SAFETY_EVENT_STATE_LOGICAL_ACTIVE = 1,
    SAFETY_EVENT_STATE_LOGICAL_INACTIVE = 2,
    SAFETY_EVENT_STATE_LUX_LOW = 3,
    SAFETY_EVENT_STATE_LUX_NORMAL = 4,
};

struct safety_event_record {
    __u32 sequence;
    __u32 type;
    __u32 source;
    __u32 state;
    __u64 timestamp_ns;
    __u32 lux;      /* BH1750 换算后的整数照度；非照度事件为 0 */
    __u32 reserved; /* 保持 8 字节对齐，供后续 ABI 扩展 */
};

/* 用户态只能请求置位或清除自身告警，不能直接操作 GPIO/PWM。 */
struct safety_event_user_alarm_request {
    __u32 active;
    __u32 reserved;
};

#define SAFETY_EVENT_IOC_MAGIC 'S'

#define SAFETY_EVENT_IOC_GET_API_VERSION \
    _IOR(SAFETY_EVENT_IOC_MAGIC, 0x00, __u32)

#define SAFETY_EVENT_IOC_INJECT \
    _IOW(SAFETY_EVENT_IOC_MAGIC, 0x01, struct safety_event_record)

#define SAFETY_EVENT_IOC_SET_USER_ALARM \
    _IOW(SAFETY_EVENT_IOC_MAGIC, 0x02, \
         struct safety_event_user_alarm_request)

#endif /* _SAFETY_EVENT_UAPI_H_ */

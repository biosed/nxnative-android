// QoS classes on Linux (see ../README.md): a class sets the thread's nice value (interactive -4, user initiated -2, default 0, utility 5, background 10)
#pragma once
#include <pthread.h>
typedef enum { QOS_CLASS_USER_INTERACTIVE = 0x21, QOS_CLASS_USER_INITIATED = 0x19, QOS_CLASS_DEFAULT = 0x15, QOS_CLASS_UTILITY = 0x11, QOS_CLASS_BACKGROUND = 0x09, QOS_CLASS_UNSPECIFIED = 0 } qos_class_t;
typedef struct dm_qos_override *pthread_override_t;
int pthread_set_qos_class_self_np(qos_class_t q, int rel);
int pthread_get_qos_class_np(pthread_t t, qos_class_t *q, int *rel);
int pthread_attr_set_qos_class_np(pthread_attr_t *a, qos_class_t q, int rel);
pthread_override_t pthread_override_qos_class_start_np(pthread_t t, qos_class_t q, int rel);
int pthread_override_qos_class_end_np(pthread_override_t o);

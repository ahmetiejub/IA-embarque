#ifndef __SYS_ARCH_H__
#define __SYS_ARCH_H__

#include <stdint.h>
#include "cmsis_os2.h"

typedef osSemaphoreId_t    sys_sem_t;
typedef osMutexId_t        sys_mutex_t;
typedef osMessageQueueId_t sys_mbox_t;
typedef osThreadId_t       sys_thread_t;
typedef uint32_t           sys_prot_t;      /* requis par SYS_LIGHTWEIGHT_PROT */

#define SYS_MBOX_NULL      NULL
#define SYS_SEM_NULL       NULL

#endif /* __SYS_ARCH_H__ */

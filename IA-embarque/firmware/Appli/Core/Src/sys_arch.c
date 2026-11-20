/*
 * Copyright (c) 2017 Simon Goldschmidt
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
 * SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 */

#include "lwip/opt.h"
#include "lwip/sys.h"
#include "cmsis_os2.h"
#include <string.h>

#if !NO_SYS

/* Initialisation du sous-systeme systeme de LwIP */
void sys_init(void)
{
}

u32_t sys_now(void)
{
  return osKernelGetTickCount() * (1000 / osKernelGetTickFreq());
}

u32_t sys_jiffies(void)
{
  return osKernelGetTickCount();
}

/* ----------------- Semaphores ----------------- */

err_t sys_sem_new(sys_sem_t *sem, u8_t count)
{
  *sem = osSemaphoreNew(1, count, NULL);
  if (*sem == NULL) {
    return ERR_MEM;
  }
  return ERR_OK;
}

void sys_sem_free(sys_sem_t *sem)
{
  osSemaphoreDelete(*sem);
  *sem = NULL;
}

u32_t sys_arch_sem_wait(sys_sem_t *sem, u32_t timeout)
{
  uint32_t start_time = osKernelGetTickCount();
  osStatus_t status;

  if (timeout == 0) {
    status = osSemaphoreAcquire(*sem, osWaitForever);
  } else {
    uint32_t ticks = (timeout * osKernelGetTickFreq()) / 1000;
    if (ticks == 0) ticks = 1;
    status = osSemaphoreAcquire(*sem, ticks);
  }

  if (status == osOK) {
    return (osKernelGetTickCount() - start_time) * (1000 / osKernelGetTickFreq());
  }

  return SYS_ARCH_TIMEOUT;
}

void sys_sem_signal(sys_sem_t *sem)
{
  osSemaphoreRelease(*sem);
}

int sys_sem_valid(sys_sem_t *sem)
{
  return (*sem != NULL);
}

void sys_sem_set_invalid(sys_sem_t *sem)
{
  *sem = NULL;
}

/* ----------------- Mutexes ----------------- */

err_t sys_mutex_new(sys_mutex_t *mutex)
{
  *mutex = osMutexNew(NULL);
  if (*mutex == NULL) {
    return ERR_MEM;
  }
  return ERR_OK;
}

void sys_mutex_free(sys_mutex_t *mutex)
{
  osMutexDelete(*mutex);
  *mutex = NULL;
}

void sys_mutex_lock(sys_mutex_t *mutex)
{
  osMutexAcquire(*mutex, osWaitForever);
}

void sys_mutex_unlock(sys_mutex_t *mutex)
{
  osMutexRelease(*mutex);
}

int sys_mutex_valid(sys_mutex_t *mutex)
{
  return (*mutex != NULL);
}

void sys_mutex_set_invalid(sys_mutex_t *mutex)
{
  *mutex = NULL;
}

/* ----------------- Mailboxes / Queues ----------------- */

err_t sys_mbox_new(sys_mbox_t *mbox, int size)
{
  *mbox = osMessageQueueNew((uint32_t)size, sizeof(void *), NULL);
  if (*mbox == NULL) {
    return ERR_MEM;
  }
  return ERR_OK;
}

void sys_mbox_free(sys_mbox_t *mbox)
{
  osMessageQueueDelete(*mbox);
  *mbox = NULL;
}

void sys_mbox_post(sys_mbox_t *mbox, void *msg)
{
  osMessageQueuePut(*mbox, &msg, 0, osWaitForever);
}

err_t sys_mbox_trypost(sys_mbox_t *mbox, void *msg)
{
  osStatus_t status = osMessageQueuePut(*mbox, &msg, 0, 0);
  if (status == osOK) {
    return ERR_OK;
  }
  return ERR_MEM;
}

err_t sys_mbox_trypost_fromisr(sys_mbox_t *mbox, void *msg)
{
  return sys_mbox_trypost(mbox, msg);
}

u32_t sys_arch_mbox_fetch(sys_mbox_t *mbox, void **msg, u32_t timeout)
{
  uint32_t start_time = osKernelGetTickCount();
  osStatus_t status;
  void *dummy_msg;
  void **target_msg = (msg != NULL) ? msg : &dummy_msg;

  if (timeout == 0) {
    status = osMessageQueueGet(*mbox, target_msg, NULL, osWaitForever);
  } else {
    uint32_t ticks = (timeout * osKernelGetTickFreq()) / 1000;
    if (ticks == 0) ticks = 1;
    status = osMessageQueueGet(*mbox, target_msg, NULL, ticks);
  }

  if (status == osOK) {
    return (osKernelGetTickCount() - start_time) * (1000 / osKernelGetTickFreq());
  }

  return SYS_ARCH_TIMEOUT;
}

u32_t sys_arch_mbox_tryfetch(sys_mbox_t *mbox, void **msg)
{
  void *dummy_msg;
  void **target_msg = (msg != NULL) ? msg : &dummy_msg;

  osStatus_t status = osMessageQueueGet(*mbox, target_msg, NULL, 0);
  if (status == osOK) {
    return 0;
  }
  return SYS_MBOX_EMPTY;
}

int sys_mbox_valid(sys_mbox_t *mbox)
{
  return (*mbox != NULL);
}

void sys_mbox_set_invalid(sys_mbox_t *mbox)
{
  *mbox = NULL;
}

/* ----------------- Threads / Tasks ----------------- */

sys_thread_t sys_thread_new(const char *name, lwip_thread_fn thread, void *arg, int stacksize, int prio)
{
  osThreadAttr_t task_attr;
  memset(&task_attr, 0, sizeof(task_attr));
  task_attr.name = name;
  task_attr.stack_size = (uint32_t)stacksize;          /* en octets avec CMSIS-RTOS2 */
  task_attr.priority = (osPriority_t)prio;
  return osThreadNew((osThreadFunc_t)thread, arg, &task_attr);
}

/* ----------------- Protection (SYS_LIGHTWEIGHT_PROT = 1) ----------------- */
/* Ne pas appeler les fonctions lwIP depuis une interruption (ISR). */

sys_prot_t sys_arch_protect(void)
{
  return (sys_prot_t)osKernelLock();
}

void sys_arch_unprotect(sys_prot_t pval)
{
  osKernelRestoreLock((int32_t)pval);
}

#endif /* !NO_SYS */

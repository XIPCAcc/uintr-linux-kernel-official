/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_UINTR_H
#define _LINUX_UINTR_H

#include <linux/sched.h>

#ifdef CONFIG_X86_USER_INTERRUPTS

/*
 * Kernel-side UINTR notification helper.
 *
 * Sends a user interrupt to the given task by posting to the lowest
 * registered vector in the receiver's UPID. Used by the tty signal
 * path (e.g. Ctrl-C) to additionally notify UINTR-aware tasks.
 *
 * Returns 0 on success, -EINVAL if the task is not a UINTR receiver.
 * Safe to call from atomic context (no sleeping, spinlock-free).
 */
int uintr_kern_notify(struct task_struct *task);

#else /* !CONFIG_X86_USER_INTERRUPTS */

static inline int uintr_kern_notify(struct task_struct *task)
{
	return -EINVAL;
}

#endif /* CONFIG_X86_USER_INTERRUPTS */

#endif /* _LINUX_UINTR_H */

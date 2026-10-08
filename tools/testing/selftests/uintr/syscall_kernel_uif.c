// SPDX-License-Identifier: GPL-2.0-only
/*
 * Verify user-interrupt notification behavior while the receiver is in
 * the kernel (CPL=0) with UIF=0 (CLUI executed before the syscall).
 *
 * Question under test:
 *   After entering the kernel with UIF=0, does a SENDUIPI notification
 *   still cause the CPU to take the kernel notification handlers
 *   (0xec spurious / 0xeb kernel-notification IDT entries)?
 *
 * Expected result:
 *   - UIF gates only *delivery to the user handler*. The notification IPI
 *     itself is gated by the UPID SN bit, not by UIF.  At CPL=0 the IPI is
 *     dispatched through the IDT, so either UIS (0xec) or UKN (0xeb) must
 *     increment.
 *   - The user handler must NOT run while UIF=0.  After the receiver
 *     returns to user space and executes STUI, the pending vector held in
 *     UIRR is delivered and the handler runs.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <x86gprintrin.h>

#include "uintr_common.h"

static atomic_uint uintr_received;
static atomic_uint receiver_ready;
static int uipi_index;

static void __attribute__((interrupt))
uintr_handler(struct __uintr_frame *ui_frame, unsigned long long vector)
{
	atomic_store(&uintr_received, 1);
}

static void cpu_delay_n(unsigned long iters)
{
	volatile unsigned long c = iters;

	while (c--)
		asm volatile("" ::: "memory");
}

static unsigned long read_counter(const char *tag)
{
	FILE *f = fopen("/proc/interrupts", "r");
	char line[1024];
	unsigned long total = 0;

	if (!f) {
		perror("fopen");
		return 0;
	}

	while (fgets(line, sizeof(line), f)) {
		char *p = line;

		while (*p == ' ')
			p++;
		if (strncmp(p, tag, strlen(tag)))
			continue;
		p += strlen(tag);
		while (*p) {
			char *end;
			unsigned long v;

			while (*p == ' ' || *p == ':')
				p++;
			v = strtoul(p, &end, 10);
			if (end == p)
				break;
			total += v;
			p = end;
		}
		break;
	}

	fclose(f);
	return total;
}

static void *sender_thread(void *arg)
{
	unsigned long delay = (unsigned long)arg;

	while (!atomic_load(&receiver_ready))
		asm volatile("pause" ::: "memory");

	/* Receiver has just invoked the syscall; vary the delay so the IPI
	 * lands both before switch-out (0xec spurious) and after (0xeb). */
	cpu_delay_n(delay);
	_senduipi(uipi_index);
	return NULL;
}

static int one_round(unsigned long delay, int uif_zero)
{
	pthread_t pt;
	long ret;
	int handler_ran_before_stui;

	atomic_store(&uintr_received, 0);
	atomic_store(&receiver_ready, 0);

	if (pthread_create(&pt, NULL, sender_thread, (void *)delay)) {
		printf("[SKIP]\tpthread_create\n");
		return -1;
	}

	/*
	 * UIF setup right before the syscall:
	 *  uif_zero == 1 : CLUI -> UIF=0 while in kernel
	 *  uif_zero == 0 : STUI -> UIF=1 control case
	 */
	if (uif_zero)
		_clui();
	else
		_stui();

	atomic_store(&receiver_ready, 1);

	/* 2 second finite wait: long enough that we expect the IPI to wake
	 * us; if no kernel wakeup happens the timeout expires at 2s. */
	ret = uintr_wait(2000000, 0);

	/* Back in user space.  UIF is still whatever we set below. */
	handler_ran_before_stui = atomic_load(&uintr_received);

	if (uif_zero) {
		/* Pending request must be sitting in UIRR; STUI delivers. */
		_stui();
	}

	for (int i = 0; i < 10000000 && !atomic_load(&uintr_received); i++)
		asm volatile("pause" ::: "memory");

	pthread_join(pt, NULL);

	if (ret == -1 && errno == EINVAL)
		return -2;

	printf("\t  syscall returned %ld (errno=%d), handler before STUI: %s, "
	       "handler after STUI: %s\n",
	       ret, errno,
	       handler_ran_before_stui ? "YES" : "no",
	       atomic_load(&uintr_received) ? "YES" : "NO");

	return 0;
}

int main(void)
{
	unsigned long uis_b, ukn_b, uis_a, ukn_a;
	int uvec_fd;

	if (!uintr_supported())
		return EXIT_SUCCESS;

	if (uintr_register_handler(uintr_handler,
				   UINTR_HANDLER_FLAG_WAITING_RECEIVER)) {
		printf("[SKIP]\tregister_handler\n");
		return EXIT_SUCCESS;
	}

	uvec_fd = uintr_vector_fd(0, 0);
	if (uvec_fd < 0) {
		printf("[SKIP]\tvector_fd\n");
		return EXIT_SUCCESS;
	}

	uipi_index = uintr_register_sender(uvec_fd, 0);
	if (uipi_index < 0) {
		printf("[SKIP]\tregister_sender\n");
		return EXIT_SUCCESS;
	}

	/* ---- Case A: enter kernel with UIF=0 (CLUI) ---- */
	printf("[RUN]\tCPL=0 with UIF=0: sender fires while in kernel\n");
	uis_b = read_counter("UIS");
	ukn_b = read_counter("UKN");

	/* Two timings: very early (likely 0xec) and later (likely 0xeb). */
	one_round(10, 1);
	one_round(20000, 1);

	uis_a = read_counter("UIS");
	ukn_a = read_counter("UKN");
	printf("\t  UIS delta=%lu  UKN delta=%lu\n",
	       uis_a - uis_b, ukn_a - ukn_b);

	if ((uis_a - uis_b) + (ukn_a - ukn_b) >= 1)
		printf("[OK]\tNotification DID enter a kernel IDT handler at "
		       "CPL=0 despite UIF=0\n");
	else
		printf("[FAIL]\tNo kernel handler ran (UIS/UKN unchanged)\n");

	/* ---- Control case B: UIF=1, should behave identically in kernel ---- */
	printf("[RUN]\tCPL=0 with UIF=1 (control): sender fires while in kernel\n");
	uis_b = read_counter("UIS");
	ukn_b = read_counter("UKN");
	one_round(10, 0);
	one_round(20000, 0);
	uis_a = read_counter("UIS");
	ukn_a = read_counter("UKN");
	printf("\t  UIS delta=%lu  UKN delta=%lu\n",
	       uis_a - uis_b, ukn_a - ukn_b);

	uintr_unregister_sender(uipi_index, 0);
	close(uvec_fd);
	uintr_unregister_handler(0);

	return EXIT_SUCCESS;
}

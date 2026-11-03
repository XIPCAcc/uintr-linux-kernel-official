// SPDX-License-Identifier: GPL-2.0-only
/*
 * tty_isig.c - Test that tty-generated signals (Ctrl-C) also deliver a UINTR
 *
 * When the kernel config option UINTR_NOTIFY_ISIG is enabled and the runtime
 * sysctl kernel.uintr_notify_isig is set to 1, pressing Ctrl-C in a terminal
 * should:
 *   1) deliver SIGINT to the foreground process group, AND
 *   2) post a user interrupt to each UINTR-receiver thread in that group.
 *
 * This test registers a UINTR handler and vector, enables UIF, then spins
 * in user space waiting for Ctrl-C. It verifies that both the SIGINT handler
 * and the UINTR handler are invoked.
 *
 * Prerequisites:
 *   - UINTR-capable CPU and kernel (CONFIG_X86_USER_INTERRUPTS=y)
 *   - sysctl kernel.uintr_notify_isig = 1
 *
 * Run in the foreground of a terminal and press Ctrl-C.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "uintr_common.h"

static volatile sig_atomic_t sigint_received;
static volatile int uintr_received;
static int nerrs;

static void sigint_handler(int sig)
{
	sigint_received = 1;
}

static void __attribute__((interrupt)) uintr_handler(struct __uintr_frame *ui_frame,
						     unsigned long long vector)
{
	uintr_received = 1;
}

static int setup_uintr(void)
{
	int uvec_fd;

	if (uintr_register_handler(uintr_handler, 0)) {
		printf("[SKIP]\t%s:%d\n", __func__, __LINE__);
		return -1;
	}

	uvec_fd = uintr_vector_fd(0, 0);
	if (uvec_fd < 0) {
		uintr_unregister_handler(0);
		printf("[SKIP]\t%s:%d\n", __func__, __LINE__);
		return -1;
	}

	return uvec_fd;
}

static int setup_sigint(void)
{
	struct sigaction sa;

	sigemptyset(&sa.sa_mask);
	sa.sa_handler = sigint_handler;
	sa.sa_flags = 0;

	return sigaction(SIGINT, &sa, NULL);
}

int main(void)
{
	int uvec_fd;
	int wait_for = 1000000;

	if (!uintr_supported())
		return EXIT_SUCCESS;

	printf("[INFO]\tRun in foreground and press Ctrl-C\n");
	printf("[INFO]\tEnsure /proc/sys/kernel/uintr_notify_isig == 1\n");

	uvec_fd = setup_uintr();
	if (uvec_fd < 0)
		return EXIT_SUCCESS;

	if (setup_sigint()) {
		perror("sigaction");
		close(uvec_fd);
		uintr_unregister_handler(0);
		return EXIT_FAILURE;
	}

	_stui();
	printf("[INFO]\tUIF enabled. Waiting for Ctrl-C...\n");

	/*
	 * Spin in user space with UIF=1 so the kernel-posted UINTR is
	 * delivered immediately. The SIGINT handler will also fire.
	 */
	while (wait_for-- && !sigint_received)
		cpu_delay();

	/* Re-sync UIF in case the signal path cleared it */
	_stui();

	printf("[RESULT]\tSIGINT=%d UINTR=%d\n", sigint_received, uintr_received);

	if (sigint_received && uintr_received) {
		printf("[OK]\tCtrl-C: both SIGINT and UINTR delivered\n");
	} else if (sigint_received && !uintr_received) {
		printf("[FAIL]\tCtrl-C: SIGINT delivered but no UINTR "
		       "(check sysctl kernel.uintr_notify_isig)\n");
		nerrs++;
	} else {
		printf("[FAIL]\tCtrl-C: SIGINT not delivered\n");
		nerrs++;
	}

	close(uvec_fd);
	uintr_unregister_handler(0);

	return nerrs ? EXIT_FAILURE : EXIT_SUCCESS;
}

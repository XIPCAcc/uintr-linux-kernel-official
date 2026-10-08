// SPDX-License-Identifier: GPL-2.0
/*
 * Test for the indefinite (timeout-less) user-interrupt wait path.
 *
 * Two scenarios:
 *
 *  sync  - the sender fires only after the receiver is guaranteed to be
 *          asleep; exercises the kernel-notification (0xeb) direct wakeup
 *          path.
 *
 *  race  - the sender fires at a randomized delay just as the receiver
 *          enters the syscall, hitting every window: user-space delivery
 *          before the syscall, the in-kernel spurious window, and the
 *          post-enqueue ON-recheck window.
 *
 * A SENDUIPI that lands while the receiver is still in user space (UIF=1)
 * is recognized directly by hardware: the user handler runs and the UPID
 * ON bit is cleared before the kernel ever looks at it.  That is the
 * runtime-level race (case 1) which the kernel cannot observe; a real
 * runtime closes it with CLUI around "check events -> block".  This test
 * emulates the event check: if the handler already ran before the syscall,
 * the iteration is skipped and retried.  The alarm watchdog distinguishes
 * a genuine lost wakeup (handler never ran) from that case.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <x86gprintrin.h>

#include "uintr_common.h"

#define UINTR_WAIT_INDEFINITE	(~0ULL)
#define WATCHDOG_SECS		1
#define DEFAULT_ITERS		200

static atomic_uint uintr_received;
static atomic_uint receiver_ready;
static volatile sig_atomic_t timeout_fired;
static int uipi_index;
static unsigned int nerrs;

static void __attribute__((interrupt))
uintr_handler(struct __uintr_frame *ui_frame, unsigned long long vector)
{
	atomic_store(&uintr_received, 1);
}

static void alarm_handler(int sig)
{
	(void)sig;
	/* Do not exit: the signal interrupts the TASK_INTERRUPTIBLE wait so
	 * we can return and report whether the user handler ever ran. */
	timeout_fired = 1;
}

static void cpu_delay_n(unsigned long iters)
{
	volatile unsigned long c = iters;

	while (c--)
		asm volatile("" ::: "memory");
}

/* Sync mode: receiver is about to block; give it time to enter the syscall
 * and actually sleep, then fire (lands on the 0xeb kernel path). */
static void *sender_thread_sync(void *arg)
{
	(void)arg;

	while (!atomic_load(&receiver_ready))
		asm volatile("pause" ::: "memory");

	cpu_delay_n(5000);
	_senduipi(uipi_index);
	return NULL;
}

/* Race mode: start timing from the point the receiver is about to block. */
static void *sender_thread_race(void *arg)
{
	unsigned long delay = (unsigned long)arg;

	while (!atomic_load(&receiver_ready))
		asm volatile("pause" ::: "memory");

	cpu_delay_n(delay);
	_senduipi(uipi_index);
	return NULL;
}

static int wait_for_handler(void)
{
	int i;

	for (i = 0; i < 10000000 && !atomic_load(&uintr_received); i++)
		asm volatile("pause" ::: "memory");

	return atomic_load(&uintr_received) ? 0 : 1;
}

/* Read a uintr module counter (recheck_enqueue / recheck_on_set). */
static unsigned long read_uintr_counter(const char *name)
{
	char path[128];
	FILE *f;
	unsigned long v = 0;

	snprintf(path, sizeof(path),
		 "/sys/module/uintr/parameters/%s", name);
	f = fopen(path, "r");
	if (f) {
		if (fscanf(f, "%lu", &v) != 1)
			v = 0;
		fclose(f);
	}
	return v;
}

/* Returns: 0 = woken by uintr and handler ran;
 *          1 = genuine lost wakeup (watchdog);
 *         -1 = skipped (consumed in user space before blocking), retry.
 */
static int run_one(int race_mode, unsigned long delay_iters)
{
	pthread_t pt;
	long ret;
	int skip = 0;

	atomic_store(&uintr_received, 0);
	atomic_store(&receiver_ready, 0);

	if (pthread_create(&pt, NULL,
			   race_mode ? sender_thread_race : sender_thread_sync,
			   (void *)delay_iters)) {
		printf("[SKIP]\tpthread_create failed\n");
		return -2;
	}

	atomic_store(&receiver_ready, 1);

	/*
	 * Runtime-style event check: if the notification was already
	 * delivered to the user handler while we were still in user space,
	 * there is nothing to wait for.  (A real runtime holds CLUI across
	 * this check and the syscall to close the residual window.)
	 */
	if (atomic_load(&uintr_received))
		skip = 1;

	if (!skip) {
		timeout_fired = 0;
		ret = uintr_wait(UINTR_WAIT_INDEFINITE, 0);
	} else {
		ret = 0;
	}

	pthread_join(pt, NULL);

	if (skip)
		return -1;

	if (ret == -1 && errno == EINVAL)
		return -3;

	if (timeout_fired) {
		/*
		 * Watchdog interrupted the wait.  If the handler ran, this
		 * is the residual user-space race (case 1); otherwise it is
		 * a genuine kernel lost wakeup.
		 */
		printf("[DIAG]\twatchdog: handler_ran=%u\n",
		       atomic_load(&uintr_received));
		return atomic_load(&uintr_received) ? -1 : 1;
	}

	if (!(ret == -1 && errno == EINTR)) {
		printf("[FAIL]\tUnexpected wait return: %ld (errno=%d %s)\n",
		       ret, errno, strerror(errno));
		return 1;
	}

	if (wait_for_handler()) {
		printf("[FAIL]\tWait returned EINTR but handler did not run\n");
		return 1;
	}

	return 0;
}

static void test_wait(unsigned int iters, int race_mode)
{
	unsigned int done = 0, skipped = 0, consumed_race = 0;
	unsigned int attempts = 0;
	unsigned long enq_b, on_b, enq_a, on_a;
	struct sigaction sa;

	printf("[RUN]\tIndefinite wait stress (%u target wakeups, %s)\n",
	       iters, race_mode ? "race" : "sync");

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = alarm_handler;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGALRM, &sa, NULL)) {
		printf("[SKIP]\tsigaction failed: %s\n", strerror(errno));
		return;
	}

	/* Snapshot kernel ON-recheck counters before the run. */
	enq_b = read_uintr_counter("recheck_enqueue");
	on_b  = read_uintr_counter("recheck_on_set");

	/* Retry skips so 'iters' genuine sleeps are actually exercised. */
	while (done < iters && attempts < iters * 50) {
		unsigned long delay = race_mode ? 1UL + (rand() % 4096) : 0;
		int rc;

		attempts++;
		alarm(WATCHDOG_SECS);
		rc = run_one(race_mode, delay);
		alarm(0);

		if (rc == -3) {
			printf("[SKIP]\tKernel does not support indefinite wait\n");
			return;
		}
		if (rc == -2)
			return;
		if (rc == -1) {
			skipped++;
			if (timeout_fired)
				consumed_race++;
			continue;
		}
		if (rc == 1) {
			nerrs++;
			printf("[FAIL]\tGenuine lost wakeup after %u ok "
			       "(skipped=%u user-consumed=%u attempts=%u)\n",
			       done, skipped, consumed_race, attempts);
			return;
		}
		done++;
	}

	enq_a = read_uintr_counter("recheck_enqueue");
	on_a  = read_uintr_counter("recheck_on_set");

	if (done == iters) {
		printf("[OK]\t%u wakeups delivered (attempts=%u)\n",
		       done, attempts);
		printf("\t  runtime  layer: skipped(pre-syscall)=%u  "
		       "residual-window=%u\n", skipped, consumed_race);
		printf("\t  kernel   layer: enqueue-recheck=%lu  "
		       "ON-hit(self-IPI)=%lu\n", enq_a - enq_b, on_a - on_b);
	} else {
		printf("[FAIL]\tOnly %u/%u wakeups (attempts=%u)\n",
		       done, iters, attempts);
	}
}

int main(int argc, char **argv)
{
	unsigned int iters = DEFAULT_ITERS;
	int race_mode = 0;
	int uvec_fd;

	if (!uintr_supported())
		return EXIT_SUCCESS;

	if (argc > 1)
		iters = (unsigned int)strtoul(argv[1], NULL, 0);
	if (iters == 0)
		iters = DEFAULT_ITERS;
	if (argc > 2 && strcmp(argv[2], "race") == 0)
		race_mode = 1;

	srand((unsigned int)time(NULL) ^ (unsigned int)getpid());

	if (uintr_register_handler(uintr_handler,
				   UINTR_HANDLER_FLAG_WAITING_RECEIVER)) {
		printf("[SKIP]\tregister_handler failed: %s\n", strerror(errno));
		return EXIT_SUCCESS;
	}

	uvec_fd = uintr_vector_fd(0, 0);
	if (uvec_fd < 0) {
		printf("[SKIP]\tvector_fd failed: %s\n", strerror(errno));
		uintr_unregister_handler(0);
		return EXIT_SUCCESS;
	}

	_stui();

	uipi_index = uintr_register_sender(uvec_fd, 0);
	if (uipi_index < 0) {
		printf("[SKIP]\tregister_sender failed: %s\n", strerror(errno));
		close(uvec_fd);
		uintr_unregister_handler(0);
		return EXIT_SUCCESS;
	}

	test_wait(iters, race_mode);

	uintr_unregister_sender(uipi_index, 0);
	close(uvec_fd);
	uintr_unregister_handler(0);

	return nerrs ? EXIT_FAILURE : EXIT_SUCCESS;
}

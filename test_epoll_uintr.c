#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <x86gprintrin.h>

#define __USE_GNU
#include <pthread.h>
#include <sys/epoll.h>
#include <errno.h>
#include <string.h>

/* Define correct UINTR handler flags */
#define UINTR_HANDLER_FLAG_WAITING_NONE		0x0
#define UINTR_HANDLER_FLAG_WAITING_RECEIVER	0x1000
#define UINTR_HANDLER_FLAG_WAITING_SENDER	0x2000
#define UINTR_HANDLER_FLAG_WAITING_ANY		(UINTR_HANDLER_FLAG_WAITING_SENDER | \
							 UINTR_HANDLER_FLAG_WAITING_RECEIVER)

#ifndef __NR_uintr_register_handler
#define __NR_uintr_register_handler	471
#define __NR_uintr_unregister_handler	472
#define __NR_uintr_vector_fd		473
#define __NR_uintr_register_sender	474
#define __NR_uintr_unregister_sender	475
#define __NR_uintr_wait			476
#endif

#define sys_uintr_register_handler(handler, flags)	syscall(__NR_uintr_register_handler, handler, flags)
#define sys_uintr_unregister_handler(flags)	syscall(__NR_uintr_unregister_handler, flags)
#define sys_uintr_vector_fd(vector, flags)	syscall(__NR_uintr_vector_fd, vector, flags)
#define sys_uintr_register_sender(fd, flags)	syscall(__NR_uintr_register_sender, fd, flags)
#define sys_uintr_unregister_sender(ipi_idx, flags)	syscall(__NR_uintr_unregister_sender, ipi_idx, flags)
#define sys_uintr_wait(usec, flags)	syscall(__NR_uintr_wait, usec, flags)

#define UINTR_WAIT_MAX_USEC			1000000

volatile unsigned long uintr_received;
volatile unsigned int uintr_count = 0;
int descriptor;
int epfd;

void __attribute__ ((interrupt))
     __attribute__((target("general-regs-only", "inline-all-stringops")))
     ui_handler(struct __uintr_frame *ui_frame,
		unsigned long long vector) {

	uintr_count++;
	uintr_received = 1;
	printf("User interrupt handler called! vector=%llu\n", vector);
}

void *client_communicate(void *arg) {
	int loop;
	int count = *(int*)arg;

	int uipi_index = sys_uintr_register_sender(descriptor, 0);
	if (uipi_index < 0) {
		printf("Sender register error: %s (%d)\n", strerror(errno), errno);
		return NULL;
	}
	printf("Registered sender successfully, uipi_index=%d\n", uipi_index);

	for (loop = count; loop > 0; --loop) {
		uintr_received = 0;
		
		// Wait a bit before sending
		sleep(1);
		
		printf("Sending user interrupt %d...\n", count - loop + 1);
		
		// Send User IPI
		_senduipi(uipi_index);
		
		while (!uintr_received) {
			// Keep spinning until this user interrupt is received.
		}
		uintr_received = 0;
	}

	sys_uintr_unregister_sender(uipi_index, 0);
	return NULL;
}

void test_epoll_wait() {
	struct epoll_event event, events[1];
	int ret;
	
	printf("\n=== Testing epoll_wait ===\n");
	
	// Test 1: Timeout test
	printf("\nTest 1: Timeout test\n");
	printf("Calling epoll_wait with timeout 10ms...\n");
	ret = epoll_wait(epfd, events, 1, 10);
	
	if (ret == -1) {
		printf("epoll_wait returned error: %s (%d)\n", strerror(errno), errno);
		if (errno == EINTR) {
			printf("epoll_wait was interrupted by user interrupt!\n");
		}
	} else {
		printf("epoll_wait returned: %d (expected: 0 for timeout)\n", ret);
	}
	
	// Test 2: Wait for interrupt
	printf("\n Wait for interrupt\n Wait for interrupt\n");
	printf("Calling epoll_wait with timeout 100000ms...\n");
	ret = epoll_wait(epfd, events, 1, 100000);
	
	if (ret == -1) {
		printf("epoll_wait returned error: %s (%d)\n", strerror(errno), errno);
		if (errno == EINTR) {
			printf("epoll_wait was interrupted by user interrupt!\n");
		}
	} else {
		printf("epoll_wait returned: %d\n", ret);
	}
}

int main(int argc, char* argv[]) {
	pthread_t pt;
	int test_count = 3;
	
	printf("Testing epoll_wait with user interrupt support\n");
	
	// Create epoll fd
	epfd = epoll_create1(0);
	printf("epoll fd: %d\n", epfd);
	
	// Register handler
	if (sys_uintr_register_handler((void *)ui_handler, UINTR_HANDLER_FLAG_WAITING_ANY)) {
		printf("Interrupt handler register error: %s (%d)\n", strerror(errno), errno);
		close(epfd);
		return 1;
	}
	printf("Registered user interrupt handler successfully\n");

	// Create a new uintrfd object and get the corresponding
	// file descriptor.
	descriptor = sys_uintr_vector_fd(0, 0);
	if (descriptor < 0) {
		printf("Interrupt vector allocation error: %s (%d)\n", strerror(errno), errno);
		sys_uintr_unregister_handler(0);
		close(epfd);
		return 1;
	}
	printf("Created user interrupt vector fd: %d\n", descriptor);

	// Enable interrupts
	_stui();
	printf("User interrupts enabled\n");

	// Create sender thread
	printf("\nCreating sender thread...\n");
	if (pthread_create(&pt, NULL, &client_communicate, &test_count)) {
		printf("Error creating sender thread\n");
		close(descriptor);
		sys_uintr_unregister_handler(0);
		close(epfd);
		return 1;
	}

	// Test epoll_wait
	test_epoll_wait();
	while (uintr_count < test_count) {
		// Keep spinning until all user interrupts are received.
	}
	// Wait for sender thread to finish
	pthread_join(pt, NULL);

	// Cleanup
	close(descriptor);
	sys_uintr_unregister_handler(0);
	close(epfd);

	printf("\nTest completed!\n");
	printf("Total interrupts received: %u\n", uintr_count);

	return EXIT_SUCCESS;
}

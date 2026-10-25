/*
obs-playlister
Copyright (C) 2026 Jeffrey A. Brick

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

/* Trace tests for the player's command FIFO (PLAN section 3.3): ordering, wake, quit while busy, and freeing
 * commands still queued. */

#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <util/bmem.h>
#include <util/platform.h>
#include <windows.h>

#include "core/command-queue.h"

static void push_load(struct pl_command_queue *queue, const char *path)
{
	struct pl_command command = {.type = PL_COMMAND_LOAD, .path = bstrdup(path), .is_url = false};
	pl_command_queue_push(queue, &command);
	/* push takes the strings and clears the caller's copy */
	assert(command.path == NULL);
}

static void push_type(struct pl_command_queue *queue, enum pl_command_type type)
{
	struct pl_command command = {.type = type};
	pl_command_queue_push(queue, &command);
}

/* A load followed by a pause performs both, in order; nothing is merged or dropped */
static void test_order(void)
{
	struct pl_command_queue queue;
	struct pl_command command;
	assert(pl_command_queue_init(&queue));

	push_load(&queue, "C:/media/a.mp4");
	struct pl_command pause = {.type = PL_COMMAND_PLAY_PAUSE, .pause = true};
	pl_command_queue_push(&queue, &pause);
	struct pl_command settings = {.type = PL_COMMAND_SET_CHILD_SETTINGS,
				      .child = {.hw_decode = true, .speed_percent = 150}};
	pl_command_queue_push(&queue, &settings);
	push_load(&queue, "C:/media/b.mp4");
	push_type(&queue, PL_COMMAND_QUIT);

	assert(pl_command_queue_pop(&queue, &command));
	assert(command.type == PL_COMMAND_LOAD && strcmp(command.path, "C:/media/a.mp4") == 0);
	pl_command_free(&command);
	assert(pl_command_queue_pop(&queue, &command));
	assert(command.type == PL_COMMAND_PLAY_PAUSE && command.pause);
	pl_command_free(&command);
	assert(pl_command_queue_pop(&queue, &command));
	assert(command.type == PL_COMMAND_SET_CHILD_SETTINGS && command.child.hw_decode &&
	       command.child.speed_percent == 150);
	pl_command_free(&command);
	assert(pl_command_queue_pop(&queue, &command));
	assert(command.type == PL_COMMAND_LOAD && strcmp(command.path, "C:/media/b.mp4") == 0);
	pl_command_free(&command);
	assert(pl_command_queue_pop(&queue, &command));
	assert(command.type == PL_COMMAND_QUIT);
	pl_command_free(&command);
	assert(!pl_command_queue_pop(&queue, &command));

	pl_command_queue_free(&queue);
}

struct consumer {
	struct pl_command_queue *queue;
	int executed;
	bool saw_quit;
	bool woke;
	DWORD work_ms;
};

static void *wake_consumer(void *data)
{
	struct consumer *consumer = data;
	struct pl_command command;
	consumer->woke = pl_command_queue_wait(consumer->queue, 5000);
	while (pl_command_queue_pop(consumer->queue, &command)) {
		consumer->executed++;
		pl_command_free(&command);
	}
	return NULL;
}

/* A push from another thread wakes a waiting consumer */
static void test_wake(void)
{
	struct pl_command_queue queue;
	struct consumer consumer = {.queue = &queue};
	pthread_t thread;
	assert(pl_command_queue_init(&queue));
	assert(pthread_create(&thread, NULL, wake_consumer, &consumer) == 0);

	os_sleep_ms(50);
	push_load(&queue, "C:/media/a.mp4");
	pthread_join(thread, NULL);

	assert(consumer.woke);
	assert(consumer.executed == 1);
	pl_command_queue_free(&queue);
}

static void *busy_consumer(void *data)
{
	struct consumer *consumer = data;
	struct pl_command command;
	for (;;) {
		while (pl_command_queue_pop(consumer->queue, &command)) {
			bool quit = command.type == PL_COMMAND_QUIT;
			if (!quit) {
				Sleep(consumer->work_ms);
				consumer->executed++;
			}
			pl_command_free(&command);
			if (quit) {
				consumer->saw_quit = true;
				return NULL;
			}
		}
		pl_command_queue_wait(consumer->queue, 1000);
	}
}

/* QUIT queued behind slow work: every earlier command still runs, then the consumer exits */
static void test_quit_while_busy(void)
{
	struct pl_command_queue queue;
	struct consumer consumer = {.queue = &queue, .work_ms = 5};
	pthread_t thread;
	assert(pl_command_queue_init(&queue));
	assert(pthread_create(&thread, NULL, busy_consumer, &consumer) == 0);

	for (int i = 0; i < 20; i++)
		push_load(&queue, "C:/media/clip.mp4");
	push_type(&queue, PL_COMMAND_QUIT);
	pthread_join(thread, NULL);

	assert(consumer.saw_quit);
	assert(consumer.executed == 20);
	pl_command_queue_free(&queue);
}

struct numbering_producer {
	struct pl_command_queue *queue;
	volatile long *sequence;
};

static void *numbering_producer(void *data)
{
	struct numbering_producer *producer = data;
	for (int i = 0; i < 2000; i++) {
		struct pl_command command = {.type = PL_COMMAND_STOP};
		pl_command_queue_push_request(producer->queue, &command, producer->sequence);
	}
	return NULL;
}

/* Two producers numbering requests at once: the numbers still rise in queue order */
static void test_request_numbers_follow_queue_order(void)
{
	struct pl_command_queue queue;
	volatile long sequence = 0;
	struct numbering_producer producer = {.queue = &queue, .sequence = &sequence};
	pthread_t first, second;
	assert(pl_command_queue_init(&queue));
	assert(pthread_create(&first, NULL, numbering_producer, &producer) == 0);
	assert(pthread_create(&second, NULL, numbering_producer, &producer) == 0);
	pthread_join(first, NULL);
	pthread_join(second, NULL);

	struct pl_command command;
	long previous = 0;
	int count = 0;
	while (pl_command_queue_pop(&queue, &command)) {
		assert(command.request == previous + 1);
		previous = command.request;
		count++;
		pl_command_free(&command);
	}
	assert(count == 4000 && sequence == 4000);
	pl_command_queue_free(&queue);
}

/* Freeing the queue frees the strings of commands nobody popped */
static void test_free_pending(void)
{
	long allocations = bnum_allocs();
	struct pl_command_queue queue;
	assert(pl_command_queue_init(&queue));
	for (int i = 0; i < 3; i++)
		push_load(&queue, "C:/media/pending.mp4");
	pl_command_queue_free(&queue);
	assert(bnum_allocs() == allocations);
}

/* has_pending looks without taking: the player's pause wait (R-UP-79) stops for a newer command */
static void test_has_pending(void)
{
	struct pl_command_queue queue;
	struct pl_command command;
	assert(pl_command_queue_init(&queue));
	assert(!pl_command_queue_has_pending(&queue));
	push_type(&queue, PL_COMMAND_STOP);
	assert(pl_command_queue_has_pending(&queue));
	assert(pl_command_queue_has_pending(&queue));
	assert(pl_command_queue_pop(&queue, &command) && command.type == PL_COMMAND_STOP);
	assert(!pl_command_queue_has_pending(&queue));
	pl_command_queue_free(&queue);
}

int main(void)
{
	/* a failed assert prints to stderr and exits non-zero instead of opening a dialog */
	_set_error_mode(_OUT_TO_STDERR);
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

	test_order();
	test_wake();
	test_quit_while_busy();
	test_request_numbers_follow_queue_order();
	test_free_pending();
	test_has_pending();
	printf("command queue tests: passed\n");
	return 0;
}

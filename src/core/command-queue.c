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

#include "command-queue.h"

#include <errno.h>
#include <string.h>
#include <util/bmem.h>

bool pl_command_queue_init(struct pl_command_queue *queue)
{
	memset(queue, 0, sizeof(*queue));
	if (pthread_mutex_init(&queue->mutex, NULL) != 0)
		return false;
	if (os_event_init(&queue->wake, OS_EVENT_TYPE_AUTO) != 0) {
		pthread_mutex_destroy(&queue->mutex);
		return false;
	}
	deque_init(&queue->commands);
	return true;
}

void pl_command_queue_free(struct pl_command_queue *queue)
{
	struct pl_command command;
	while (pl_command_queue_pop(queue, &command))
		pl_command_free(&command);
	deque_free(&queue->commands);
	os_event_destroy(queue->wake);
	pthread_mutex_destroy(&queue->mutex);
	memset(queue, 0, sizeof(*queue));
}

void pl_command_queue_push(struct pl_command_queue *queue, struct pl_command *command)
{
	pthread_mutex_lock(&queue->mutex);
	deque_push_back(&queue->commands, command, sizeof(*command));
	pthread_mutex_unlock(&queue->mutex);
	/* the queue owns the strings now */
	memset(command, 0, sizeof(*command));
	os_event_signal(queue->wake);
}

long pl_command_queue_push_request(struct pl_command_queue *queue, struct pl_command *command,
				   volatile long *request_sequence)
{
	pthread_mutex_lock(&queue->mutex);
	long request = os_atomic_inc_long(request_sequence);
	command->request = request;
	deque_push_back(&queue->commands, command, sizeof(*command));
	pthread_mutex_unlock(&queue->mutex);
	memset(command, 0, sizeof(*command));
	os_event_signal(queue->wake);
	return request;
}

bool pl_command_queue_pop(struct pl_command_queue *queue, struct pl_command *command)
{
	bool popped = false;
	pthread_mutex_lock(&queue->mutex);
	if (queue->commands.size >= sizeof(*command)) {
		deque_pop_front(&queue->commands, command, sizeof(*command));
		popped = true;
	}
	pthread_mutex_unlock(&queue->mutex);
	return popped;
}

bool pl_command_queue_has_pending(struct pl_command_queue *queue)
{
	pthread_mutex_lock(&queue->mutex);
	bool pending = queue->commands.size > 0;
	pthread_mutex_unlock(&queue->mutex);
	return pending;
}

bool pl_command_queue_wait(struct pl_command_queue *queue, unsigned long timeout_ms)
{
	return os_event_timedwait(queue->wake, timeout_ms) == 0;
}

void pl_command_free(struct pl_command *command)
{
	bfree(command->path);
	memset(command, 0, sizeof(*command));
}

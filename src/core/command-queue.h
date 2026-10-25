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

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <util/deque.h>
#include <util/threading.h>

/* Commands for the player thread, the only code that touches the child media source (PLAN section 3.3,
 * R-THR-1). A restart is a LOAD of the current path: the player restarts the child when the path is unchanged. */
enum pl_command_type {
	PL_COMMAND_LOAD,
	PL_COMMAND_PLAY_PAUSE,
	PL_COMMAND_STOP,
	PL_COMMAND_CLEAR,
	PL_COMMAND_SET_TIME,
	PL_COMMAND_SET_CHILD_SETTINGS,
	PL_COMMAND_QUIT,
};

/* Child media source settings that the player keeps and writes on every load */
struct pl_child_settings {
	bool hw_decode;
	bool close_when_inactive;
	bool restart_on_activate;
	long long speed_percent;
};

struct pl_command {
	enum pl_command_type type;
	char *path;                     /* LOAD: owned, freed by pl_command_free */
	bool is_url;                    /* LOAD */
	bool still;                     /* LOAD: a still image, kept on screen after its end (R-DIR-9) */
	long request;                   /* LOAD, STOP, CLEAR: request number, see load-tracker.h */
	bool pause;                     /* PLAY_PAUSE */
	int64_t time_ms;                /* SET_TIME */
	struct pl_child_settings child; /* SET_CHILD_SETTINGS */
};

/* A FIFO of owned commands with one consumer. push and pop take the queue's own lock, so producers may push
 * while holding the list lock (lock order: list lock, then queue lock). Nothing is dropped or merged: a load
 * followed by a pause performs both. */
struct pl_command_queue {
	pthread_mutex_t mutex;
	os_event_t *wake;
	struct deque commands;
};

bool pl_command_queue_init(struct pl_command_queue *queue);
/* Frees every command still queued */
void pl_command_queue_free(struct pl_command_queue *queue);
/* Takes ownership of the command's strings and wakes the consumer */
void pl_command_queue_push(struct pl_command_queue *queue, struct pl_command *command);
/* Like push, and numbers the command from *request_sequence under the queue's lock, so request numbers follow
 * queue order even with several producers. Returns the number. */
long pl_command_queue_push_request(struct pl_command_queue *queue, struct pl_command *command,
				   volatile long *request_sequence);
/* Moves the oldest command into *command; false when the queue is empty */
bool pl_command_queue_pop(struct pl_command_queue *queue, struct pl_command *command);
/* Whether a command is waiting, without taking it */
bool pl_command_queue_has_pending(struct pl_command_queue *queue);
/* Waits until a push happens (or happened since the last wait); false on timeout */
bool pl_command_queue_wait(struct pl_command_queue *queue, unsigned long timeout_ms);
void pl_command_free(struct pl_command *command);

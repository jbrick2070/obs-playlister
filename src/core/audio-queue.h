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
#include <util/darray.h>
#include <util/deque.h>
#include <util/threading.h>

/* The most planes a chunk can have: libobs's MAX_AV_PLANES, which media-playlist-source.c checks */
#define PL_AUDIO_MAX_PLANES 8

/* Audio the child's capture callback hands over on the audio thread, replayed by the source from video_tick
 * (R-UP-60). A chunk is planar float, as OBS gives audio to capture callbacks: one plane of `frames` samples per
 * channel. push and pop take the queue's own lock. The ring buffers can wrap, so pop copies a chunk out into one
 * contiguous run per plane; it never hands out pointers into a ring buffer. */
struct pl_audio_queue {
	bool initialized; /* free does nothing without it, so an error path may free a queue never set up */
	pthread_mutex_t mutex;
	size_t channels;
	struct deque planes[PL_AUDIO_MAX_PLANES];
	struct deque frames;     /* uint32_t per chunk */
	struct deque timestamps; /* uint64_t per chunk */
	DARRAY(float) chunk;     /* the last chunk popped; the consumer's only */
};

/* channels is fixed for the queue's life; false when it is over PL_AUDIO_MAX_PLANES or the lock fails */
bool pl_audio_queue_init(struct pl_audio_queue *queue, size_t channels);
void pl_audio_queue_free(struct pl_audio_queue *queue);
/* Copies one chunk: planes[0] to planes[channels - 1], frames samples each */
void pl_audio_queue_push(struct pl_audio_queue *queue, uint8_t *const planes[], uint32_t frames, uint64_t timestamp);
/* Takes the oldest chunk: planes[0] to planes[channels - 1] point into the queue's chunk buffer until the next pop
 * or free, and the rest are NULL. false when the queue is empty. */
bool pl_audio_queue_pop(struct pl_audio_queue *queue, const uint8_t *planes[PL_AUDIO_MAX_PLANES], uint32_t *frames,
			uint64_t *timestamp);

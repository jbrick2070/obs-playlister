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

#include "audio-queue.h"

#include <string.h>

bool pl_audio_queue_init(struct pl_audio_queue *queue, size_t channels)
{
	memset(queue, 0, sizeof(*queue));
	if (channels > PL_AUDIO_MAX_PLANES)
		return false;
	if (pthread_mutex_init(&queue->mutex, NULL) != 0)
		return false;
	queue->channels = channels;
	queue->initialized = true;
	return true;
}

void pl_audio_queue_free(struct pl_audio_queue *queue)
{
	/* w32-pthreads dereferences a zeroed mutex when destroying it */
	if (!queue->initialized)
		return;
	for (size_t i = 0; i < PL_AUDIO_MAX_PLANES; i++)
		deque_free(&queue->planes[i]);
	deque_free(&queue->frames);
	deque_free(&queue->timestamps);
	da_free(queue->chunk);
	pthread_mutex_destroy(&queue->mutex);
	memset(queue, 0, sizeof(*queue));
}

void pl_audio_queue_push(struct pl_audio_queue *queue, uint8_t *const planes[], uint32_t frames, uint64_t timestamp)
{
	size_t size = frames * sizeof(float);
	pthread_mutex_lock(&queue->mutex);
	for (size_t i = 0; i < queue->channels; i++)
		deque_push_back(&queue->planes[i], planes[i], size);
	deque_push_back(&queue->frames, &frames, sizeof(frames));
	deque_push_back(&queue->timestamps, &timestamp, sizeof(timestamp));
	pthread_mutex_unlock(&queue->mutex);
}

bool pl_audio_queue_pop(struct pl_audio_queue *queue, const uint8_t *planes[PL_AUDIO_MAX_PLANES], uint32_t *frames,
			uint64_t *timestamp)
{
	pthread_mutex_lock(&queue->mutex);
	if (!queue->frames.size) {
		pthread_mutex_unlock(&queue->mutex);
		return false;
	}
	deque_pop_front(&queue->frames, frames, sizeof(*frames));
	deque_pop_front(&queue->timestamps, timestamp, sizeof(*timestamp));
	da_resize(queue->chunk, (size_t)*frames * queue->channels);
	for (size_t i = 0; i < queue->channels; i++)
		deque_pop_front(&queue->planes[i], queue->chunk.array + i * *frames, *frames * sizeof(float));
	pthread_mutex_unlock(&queue->mutex);

	for (size_t i = 0; i < PL_AUDIO_MAX_PLANES; i++)
		planes[i] = i < queue->channels ? (const uint8_t *)(queue->chunk.array + i * *frames) : NULL;
	return true;
}

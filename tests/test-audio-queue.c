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

/* Tests for the queue between the child's audio capture callback and video_tick (R-UP-60): chunks come back whole
 * and in order, also when they wrap around the end of a ring buffer, and an empty queue gives nothing. */

#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <windows.h>

#include "core/audio-queue.h"

#define MAX_TEST_FRAMES 1024

/* Each sample encodes its chunk, plane and position, so a sample read from the wrong place shows. The values stay
 * below 2^24, so floats hold them exactly. */
static float sample_value(uint32_t chunk, size_t plane, uint32_t sample)
{
	return (float)(chunk * 100000 + plane * 10000 + sample);
}

static uint32_t chunk_frames(uint32_t chunk)
{
	/* sizes that do not divide the ring buffers evenly, so chunks end up across their end */
	return 300 + (chunk * 37) % 500;
}

static void push_chunk(struct pl_audio_queue *queue, uint32_t chunk, uint32_t frames)
{
	static float samples[PL_AUDIO_MAX_PLANES][MAX_TEST_FRAMES];
	uint8_t *planes[PL_AUDIO_MAX_PLANES] = {0};
	assert(frames <= MAX_TEST_FRAMES);
	for (size_t p = 0; p < queue->channels; p++) {
		for (uint32_t s = 0; s < frames; s++)
			samples[p][s] = sample_value(chunk, p, s);
		planes[p] = (uint8_t *)samples[p];
	}
	pl_audio_queue_push(queue, planes, frames, 1000 + chunk);
}

static void pop_and_check(struct pl_audio_queue *queue, uint32_t chunk, uint32_t frames)
{
	const uint8_t *planes[PL_AUDIO_MAX_PLANES];
	uint32_t popped_frames = 0;
	uint64_t timestamp = 0;
	assert(pl_audio_queue_pop(queue, planes, &popped_frames, &timestamp));
	assert(popped_frames == frames);
	assert(timestamp == 1000 + chunk);
	for (size_t p = 0; p < PL_AUDIO_MAX_PLANES; p++) {
		if (p >= queue->channels) {
			assert(planes[p] == NULL);
			continue;
		}
		const float *samples = (const float *)planes[p];
		for (uint32_t s = 0; s < frames; s++)
			assert(samples[s] == sample_value(chunk, p, s));
	}
}

static void assert_empty(struct pl_audio_queue *queue)
{
	const uint8_t *planes[PL_AUDIO_MAX_PLANES];
	uint32_t frames;
	uint64_t timestamp;
	assert(!pl_audio_queue_pop(queue, planes, &frames, &timestamp));
}

/* Chunks of different sizes come back whole and in order */
static void test_order(void)
{
	struct pl_audio_queue queue;
	assert(pl_audio_queue_init(&queue, 2));
	assert_empty(&queue);
	push_chunk(&queue, 1, 480);
	push_chunk(&queue, 2, MAX_TEST_FRAMES);
	push_chunk(&queue, 3, 7);
	pop_and_check(&queue, 1, 480);
	pop_and_check(&queue, 2, MAX_TEST_FRAMES);
	pop_and_check(&queue, 3, 7);
	assert_empty(&queue);
	pl_audio_queue_free(&queue);
}

/* Interleaved pushes and pops make the ring buffers wrap; a chunk across the end still comes back whole (upstream
 * handed out a pointer into the ring buffer and read past its end) */
static void test_wrapped_chunks(void)
{
	struct pl_audio_queue queue;
	uint32_t pushed = 0;
	uint32_t popped = 0;
	int across_end = 0;
	assert(pl_audio_queue_init(&queue, 6));
	for (int round = 0; round < 60; round++) {
		while (pushed - popped < 4) {
			push_chunk(&queue, pushed, chunk_frames(pushed));
			pushed++;
		}
		/* the chunk about to be popped runs past the physical end of the ring buffer */
		if (queue.planes[0].start_pos + chunk_frames(popped) * sizeof(float) > queue.planes[0].capacity)
			across_end++;
		pop_and_check(&queue, popped, chunk_frames(popped));
		popped++;
	}
	while (popped < pushed) {
		pop_and_check(&queue, popped, chunk_frames(popped));
		popped++;
	}
	assert(across_end > 0);
	assert_empty(&queue);
	pl_audio_queue_free(&queue);
}

/* More channels than libobs has planes is refused, and a queue that was never set up can be freed, as the source's
 * error path does */
static void test_refused_and_unset_queues_free(void)
{
	struct pl_audio_queue queue;
	assert(!pl_audio_queue_init(&queue, PL_AUDIO_MAX_PLANES + 1));
	pl_audio_queue_free(&queue);
	struct pl_audio_queue zeroed = {0};
	pl_audio_queue_free(&zeroed);
}

int main(void)
{
	/* a failed assert prints to stderr and exits non-zero instead of opening a dialog */
	_set_error_mode(_OUT_TO_STDERR);
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

	test_order();
	test_wrapped_chunks();
	test_refused_and_unset_queues_free();
	printf("audio queue tests: passed\n");
	return 0;
}

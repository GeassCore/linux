// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved. */
/*
 * Single-frame functional test for the Allwinner sun60i DI301 driver.
 *
 * Queue one interlaced NV12/NV21 frame and verify that the mem2mem device
 * returns two progressive frames in the requested field order.
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define TEST_WIDTH	64U
#define TEST_HEIGHT	32U
#define CAPTURE_BUFFERS	2U
#define POLL_TIMEOUT_MS	2000

struct mapped_buffer {
	void *addr;
	size_t length;
};

struct mapped_queue {
	struct mapped_buffer *buffers;
	unsigned int count;
	enum v4l2_buf_type type;
};

static int xioctl(int fd, unsigned long request, void *arg)
{
	int ret;

	do {
		ret = ioctl(fd, request, arg);
	} while (ret < 0 && errno == EINTR);

	return ret;
}

static int set_format(int fd, enum v4l2_buf_type type, uint32_t pixelformat,
		      enum v4l2_field field, struct v4l2_pix_format *result)
{
	struct v4l2_format format = {
		.type = type,
		.fmt.pix = {
			.width = TEST_WIDTH,
			.height = TEST_HEIGHT,
			.pixelformat = pixelformat,
			.field = field,
		},
	};

	if (xioctl(fd, VIDIOC_S_FMT, &format) < 0) {
		perror("VIDIOC_S_FMT");
		return -1;
	}

	if (format.fmt.pix.width != TEST_WIDTH ||
	    format.fmt.pix.height != TEST_HEIGHT ||
	    format.fmt.pix.pixelformat != pixelformat ||
	    format.fmt.pix.field != field) {
		fprintf(stderr,
			"unexpected format: %ux%u %.4s field=%u\n",
			format.fmt.pix.width, format.fmt.pix.height,
			(char *)&format.fmt.pix.pixelformat,
			format.fmt.pix.field);
		return -1;
	}

	*result = format.fmt.pix;
	return 0;
}

static int map_queue(int fd, enum v4l2_buf_type type, unsigned int count,
		     struct mapped_queue *queue)
{
	struct v4l2_requestbuffers request = {
		.count = count,
		.type = type,
		.memory = V4L2_MEMORY_MMAP,
	};
	unsigned int i;

	if (xioctl(fd, VIDIOC_REQBUFS, &request) < 0) {
		perror("VIDIOC_REQBUFS");
		return -1;
	}
	if (request.count < count) {
		fprintf(stderr, "only %u of %u buffers allocated\n",
			request.count, count);
		return -1;
	}

	queue->buffers = calloc(request.count, sizeof(*queue->buffers));
	if (!queue->buffers) {
		perror("calloc");
		return -1;
	}
	queue->count = request.count;
	queue->type = type;

	for (i = 0; i < request.count; i++) {
		struct v4l2_buffer buffer = {
			.type = type,
			.memory = V4L2_MEMORY_MMAP,
			.index = i,
		};

		if (xioctl(fd, VIDIOC_QUERYBUF, &buffer) < 0) {
			perror("VIDIOC_QUERYBUF");
			return -1;
		}

		queue->buffers[i].length = buffer.length;
		queue->buffers[i].addr = mmap(NULL, buffer.length,
					      PROT_READ | PROT_WRITE,
					      MAP_SHARED, fd, buffer.m.offset);
		if (queue->buffers[i].addr == MAP_FAILED) {
			queue->buffers[i].addr = NULL;
			perror("mmap");
			return -1;
		}
	}

	return 0;
}

static void unmap_queue(struct mapped_queue *queue)
{
	unsigned int i;

	for (i = 0; i < queue->count; i++)
		if (queue->buffers[i].addr)
			munmap(queue->buffers[i].addr,
			       queue->buffers[i].length);
	free(queue->buffers);
	memset(queue, 0, sizeof(*queue));
}

static int queue_buffer(int fd, enum v4l2_buf_type type, unsigned int index,
			unsigned int bytesused)
{
	struct v4l2_buffer buffer = {
		.type = type,
		.memory = V4L2_MEMORY_MMAP,
		.index = index,
		.bytesused = bytesused,
	};

	if (xioctl(fd, VIDIOC_QBUF, &buffer) < 0) {
		perror("VIDIOC_QBUF");
		return -1;
	}

	return 0;
}

static int dequeue_buffer(int fd, enum v4l2_buf_type type,
			  struct v4l2_buffer *buffer)
{
	struct pollfd pollfd = {
		.fd = fd,
		.events = POLLIN | POLLOUT | POLLERR,
	};
	int ret;

	for (;;) {
		ret = poll(&pollfd, 1, POLL_TIMEOUT_MS);
		if (ret < 0 && errno == EINTR)
			continue;
		if (ret == 0) {
			fprintf(stderr, "buffer dequeue timed out\n");
			return -1;
		}
		if (ret < 0) {
			perror("poll");
			return -1;
		}

		memset(buffer, 0, sizeof(*buffer));
		buffer->type = type;
		buffer->memory = V4L2_MEMORY_MMAP;
		if (xioctl(fd, VIDIOC_DQBUF, buffer) == 0)
			return 0;
		if (errno != EAGAIN) {
			perror("VIDIOC_DQBUF");
			return -1;
		}
	}
}

static void fill_input(void *data, const struct v4l2_pix_format *format,
		       uint32_t pixelformat)
{
	uint8_t *luma = data;
	uint8_t *chroma = luma + format->bytesperline * format->height;
	unsigned int x, y;

	memset(data, 0x10, format->sizeimage);
	for (y = 0; y < format->height; y++)
		memset(luma + y * format->bytesperline,
		       y & 1 ? 224 : 32, format->width);

	for (y = 0; y < format->height / 2; y++) {
		uint8_t first = y & 1 ? 192 : 64;
		uint8_t second = y & 1 ? 64 : 192;

		if (pixelformat == V4L2_PIX_FMT_NV21) {
			uint8_t tmp = first;

			first = second;
			second = tmp;
		}
		for (x = 0; x < format->width; x += 2) {
			chroma[y * format->bytesperline + x] = first;
			chroma[y * format->bytesperline + x + 1] = second;
		}
	}
}

static uint64_t fnv1a64(const void *data, size_t size)
{
	const uint8_t *bytes = data;
	uint64_t hash = UINT64_C(1469598103934665603);
	size_t i;

	for (i = 0; i < size; i++) {
		hash ^= bytes[i];
		hash *= UINT64_C(1099511628211);
	}

	return hash;
}

static unsigned int luma_mean(const void *data,
			      const struct v4l2_pix_format *format)
{
	const uint8_t *luma = data;
	uint64_t sum = 0;
	unsigned int x, y;

	for (y = 0; y < format->height; y++)
		for (x = 0; x < format->width; x++)
			sum += luma[y * format->bytesperline + x];

	return sum / (format->width * format->height);
}

static int save_buffer(const char *directory, const char *format_name,
		       const char *field_name, unsigned int number,
		       const void *data, size_t size)
{
	char path[512];
	FILE *file;

	if (snprintf(path, sizeof(path), "%s/%s-%s-%u.nv12", directory,
		     format_name, field_name, number) >= (int)sizeof(path)) {
		fprintf(stderr, "result path is too long\n");
		return -1;
	}

	file = fopen(path, "wb");
	if (!file) {
		perror(path);
		return -1;
	}
	if (fwrite(data, 1, size, file) != size) {
		perror("fwrite");
		fclose(file);
		return -1;
	}
	fclose(file);
	return 0;
}

static int run_case(const char *device, const char *directory,
		    uint32_t pixelformat, enum v4l2_field field)
{
	const char *format_name = pixelformat == V4L2_PIX_FMT_NV12 ?
				  "nv12" : "nv21";
	const char *field_name = field == V4L2_FIELD_INTERLACED_TB ?
				 "tb" : "bt";
	struct mapped_queue output = {}, capture = {};
	struct v4l2_pix_format output_format, capture_format;
	struct v4l2_capability capability = {};
	unsigned int means[CAPTURE_BUFFERS] = {};
	bool capture_streaming = false;
	bool output_streaming = false;
	unsigned int i;
	int fd = -1;
	int ret = -1;

	fd = open(device, O_RDWR | O_NONBLOCK);
	if (fd < 0) {
		perror(device);
		goto out;
	}
	if (xioctl(fd, VIDIOC_QUERYCAP, &capability) < 0) {
		perror("VIDIOC_QUERYCAP");
		goto out;
	}
	if (!(capability.device_caps & V4L2_CAP_VIDEO_M2M) ||
	    !(capability.device_caps & V4L2_CAP_STREAMING)) {
		fprintf(stderr, "%s is not a streaming V4L2 M2M device\n",
			device);
		goto out;
	}

	if (set_format(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT, pixelformat, field,
		       &output_format) ||
	    set_format(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, pixelformat,
		       V4L2_FIELD_NONE, &capture_format))
		goto out;

	if (map_queue(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT, 1, &output) ||
	    map_queue(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, CAPTURE_BUFFERS,
		      &capture))
		goto out;

	fill_input(output.buffers[0].addr, &output_format, pixelformat);
	for (i = 0; i < CAPTURE_BUFFERS; i++) {
		memset(capture.buffers[i].addr, 0x5a,
		       capture.buffers[i].length);
		if (queue_buffer(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, i, 0))
			goto out;
	}
	if (queue_buffer(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT, 0,
			 output_format.sizeimage))
		goto out;

	if (xioctl(fd, VIDIOC_STREAMON,
		   &(enum v4l2_buf_type){ V4L2_BUF_TYPE_VIDEO_CAPTURE }) < 0) {
		perror("VIDIOC_STREAMON capture");
		goto out;
	}
	capture_streaming = true;
	if (xioctl(fd, VIDIOC_STREAMON,
		   &(enum v4l2_buf_type){ V4L2_BUF_TYPE_VIDEO_OUTPUT }) < 0) {
		perror("VIDIOC_STREAMON output");
		goto out;
	}
	output_streaming = true;

	for (i = 0; i < CAPTURE_BUFFERS; i++) {
		struct v4l2_buffer buffer;
		void *data;

		if (dequeue_buffer(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, &buffer))
			goto out;
		if (buffer.index >= capture.count ||
		    buffer.bytesused != capture_format.sizeimage ||
		    buffer.field != V4L2_FIELD_NONE ||
		    buffer.flags & V4L2_BUF_FLAG_ERROR) {
			fprintf(stderr,
				"bad capture buffer: index=%u bytes=%u field=%u flags=%#x\n",
				buffer.index, buffer.bytesused, buffer.field,
				buffer.flags);
			goto out;
		}

		data = capture.buffers[buffer.index].addr;
		means[i] = luma_mean(data, &capture_format);
		printf("%s/%s output%u: index=%u mean=%u fnv64=%016llx\n",
		       format_name, field_name, i, buffer.index,
		       means[i],
		       (unsigned long long)fnv1a64(data, buffer.bytesused));
		if (save_buffer(directory, format_name, field_name, i,
				data, buffer.bytesused))
			goto out;
	}

	{
		struct v4l2_buffer buffer;

		if (dequeue_buffer(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT, &buffer))
			goto out;
		if (buffer.flags & V4L2_BUF_FLAG_ERROR) {
			fprintf(stderr, "output buffer completed with error\n");
			goto out;
		}
	}

	if (means[0] + 64 >= means[1] && means[1] + 64 >= means[0]) {
		fprintf(stderr, "%s/%s fields are not distinct: %u, %u\n",
			format_name, field_name, means[0], means[1]);
		goto out;
	}
	if ((field == V4L2_FIELD_INTERLACED_TB && means[0] >= means[1]) ||
	    (field == V4L2_FIELD_INTERLACED_BT && means[0] <= means[1])) {
		fprintf(stderr, "%s/%s field order is wrong: %u, %u\n",
			format_name, field_name, means[0], means[1]);
		goto out;
	}

	printf("PASS %s/%s\n", format_name, field_name);
	ret = 0;

out:
	if (output_streaming)
		xioctl(fd, VIDIOC_STREAMOFF,
		       &(enum v4l2_buf_type){ V4L2_BUF_TYPE_VIDEO_OUTPUT });
	if (capture_streaming)
		xioctl(fd, VIDIOC_STREAMOFF,
		       &(enum v4l2_buf_type){ V4L2_BUF_TYPE_VIDEO_CAPTURE });
	unmap_queue(&capture);
	unmap_queue(&output);
	if (fd >= 0)
		close(fd);
	return ret;
}

int main(int argc, char **argv)
{
	const char *device = argc > 1 ? argv[1] : "/dev/video0";
	const char *directory = argc > 2 ? argv[2] : "/tmp";
	unsigned int failures = 0;

	failures += run_case(device, directory, V4L2_PIX_FMT_NV12,
			     V4L2_FIELD_INTERLACED_TB) != 0;
	failures += run_case(device, directory, V4L2_PIX_FMT_NV12,
			     V4L2_FIELD_INTERLACED_BT) != 0;
	failures += run_case(device, directory, V4L2_PIX_FMT_NV21,
			     V4L2_FIELD_INTERLACED_TB) != 0;
	failures += run_case(device, directory, V4L2_PIX_FMT_NV21,
			     V4L2_FIELD_INTERLACED_BT) != 0;

	if (failures) {
		fprintf(stderr, "FAIL: %u of 4 cases failed\n", failures);
		return EXIT_FAILURE;
	}

	printf("PASS: all 4 single-frame DI cases passed\n");
	return EXIT_SUCCESS;
}

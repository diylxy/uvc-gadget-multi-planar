/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * V4L2 video source
 *
 * Copyright (C) 2018 Laurent Pinchart
 *
 * Contact: Laurent Pinchart <laurent.pinchart@ideasonboard.com>
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "events.h"
#include "tools.h"
#include "v4l2.h"
#include "v4l2-source.h"
#include "video-buffers.h"


// #ifdef CONFIG_CAN_ENCODE
#include <pthread.h>
#include <jpeglib.h>
#include "mjpeg_encoder_v4l2.h"
// #endif

struct v4l2_source {
	struct video_source src;

	struct v4l2_device *vdev;

	struct video_buffer_set buffers_sink;
	
	struct mjpeg_encoder_v4l2_t encoder;
};

#define to_v4l2_source(s) container_of(s, struct v4l2_source, src)

static void v4l2_source_video_process(void *d)
{
	struct v4l2_source *src = d;
	struct video_buffer buf;
	int ret;

	ret = v4l2_dequeue_buffer(src->vdev, &buf);
	if (ret < 0)
		return;

	// TODO: 判断如果需要编码，在这里编码buf->mem，buf->bytesused，将结果放入buffers_sink（之后使用队列存储传入的空闲缓冲区）对应的mem中，并设置bytesused
	// 否则直接调用src->src.handler
	// src->src.handler中会把sink_buf对应index（过程中还会用到bytesused）传递给sink
	printf("source enqueue: %d\n", buf.index);
	if (src->src.type == VIDEO_SOURCE_ENCODED) {
		mjpeg_source_enqueue(
			&src->encoder,
			buf.index,
			buf.mem,
			src->vdev->format.width,
			src->vdev->format.height,
			src->vdev->format.bytesperline,
			src->src.handler_data,
			&src->src,
			src->vdev
		);
		return;
	}
	src->src.handler(src->src.handler_data, &src->src, &buf);
}

static void v4l2_source_destroy(struct video_source *s)
{
	struct v4l2_source *src = to_v4l2_source(s);

	v4l2_close(src->vdev);
	free(src);
}

static int v4l2_source_set_format(struct video_source *s,
				  struct v4l2_pix_format *fmt)
{
	struct v4l2_source *src = to_v4l2_source(s);
	__u32 chosen_pixelformat = fmt->pixelformat;
	int ret;

	// 在这里判断编码方式，即fmt使用MJPEG编码，则修改src->src.type为VIDEO_SOURCE_ENCODED，初始化V4L2时按照YUV方式编码
	if (chosen_pixelformat == V4L2_PIX_FMT_MJPEG) {
		src->src.type = VIDEO_SOURCE_ENCODED;
		fmt->pixelformat = V4L2_PIX_FMT_YUV420;
		mjpeg_begin(&src->encoder, &src->src.handler);
	} else {
		src->src.type = VIDEO_SOURCE_DMABUF;
	}
	
	ret = v4l2_set_format(src->vdev, fmt);
	fmt->pixelformat = chosen_pixelformat;

	return ret;
}

static int v4l2_source_set_frame_rate(struct video_source *s, unsigned int fps)
{
	struct v4l2_source *src = to_v4l2_source(s);

	return v4l2_set_frame_rate(src->vdev, fps);
}

static int v4l2_source_alloc_buffers(struct video_source *s, unsigned int nbufs)
{
	struct v4l2_source *src = to_v4l2_source(s);

	return v4l2_alloc_buffers(src->vdev, V4L2_MEMORY_MMAP, nbufs);
}

static int v4l2_source_import_buffers(struct video_source *s,
				      struct video_buffer_set *buffers)
{
	struct v4l2_source *src = to_v4l2_source(s);

	if (src->buffers_sink.buffers) {
		free(src->buffers_sink.buffers);
		src->buffers_sink.buffers = NULL;
		src->buffers_sink.nbufs = 0;
	}

	src->buffers_sink.buffers = calloc(buffers->nbufs,
					   sizeof *src->buffers_sink.buffers);
	if (!src->buffers_sink.buffers)
		return -ENOMEM;

	for (unsigned int i = 0; i < buffers->nbufs; i++) {
		src->buffers_sink.buffers[i].mem = buffers->buffers[i].mem;
		src->buffers_sink.buffers[i].index = buffers->buffers[i].index;
		mjpeg_sink_enqueue(&src->encoder, buffers->buffers[i].index, buffers->buffers[i].mem);
	}
	
	src->buffers_sink.nbufs = buffers->nbufs;

	return 0;
}

static int v4l2_source_export_buffers(struct video_source *s,
				      struct video_buffer_set **bufs)
{
	struct v4l2_source *src = to_v4l2_source(s);
	struct video_buffer_set *buffers;
	unsigned int i;
	int ret;

	ret = v4l2_export_buffers(src->vdev);
	if (ret < 0)
		return ret;

	buffers = video_buffer_set_new(src->vdev->buffers.nbufs);
	if (!buffers)
		return -ENOMEM;

	for (i = 0; i < src->vdev->buffers.nbufs; ++i) {
		struct video_buffer *buffer = &src->vdev->buffers.buffers[i];

		buffers->buffers[i].size = buffer->size;
		buffers->buffers[i].dmabuf = buffer->dmabuf;
	}

	*bufs = buffers;
	return 0;
}

static int v4l2_source_free_buffers(struct video_source *s)
{
	struct v4l2_source *src = to_v4l2_source(s);

	if (src->buffers_sink.buffers) {
		free(src->buffers_sink.buffers);
		src->buffers_sink.buffers = NULL;
		src->buffers_sink.nbufs = 0;
	}

	return v4l2_free_buffers(src->vdev);
}

static int v4l2_source_mmap_buffers(struct video_source *s)
{
	struct v4l2_source *src = to_v4l2_source(s);

	return v4l2_mmap_buffers(src->vdev);
}

static int v4l2_source_stream_on(struct video_source *s)
{
	struct v4l2_source *src = to_v4l2_source(s);
	unsigned int i;
	int ret;

	/* Queue all buffers. */
	for (i = 0; i < src->vdev->buffers.nbufs; ++i) {
		struct video_buffer buf = {
			.index = i,
			.size = src->vdev->buffers.buffers[i].size,
			.dmabuf = src->vdev->buffers.buffers[i].dmabuf,
		};

		ret = v4l2_queue_buffer(src->vdev, &buf);
		if (ret < 0)
			return ret;
	}

	ret = v4l2_stream_on(src->vdev);
	if (ret < 0)
		return ret;

	events_watch_fd(src->src.events, src->vdev->fd, EVENT_READ,
			v4l2_source_video_process, src);

	return 0;
}

static int v4l2_source_stream_off(struct video_source *s)
{
	struct v4l2_source *src = to_v4l2_source(s);

	events_unwatch_fd(src->src.events, src->vdev->fd, EVENT_READ);

	if (src->src.type == VIDEO_SOURCE_ENCODED) {
		mjpeg_abort(&src->encoder);
	}

	/*
	 * We need to reinitialise this here, as if the user selected an
	 * unsupported MJPEG format the encoding routine will have overriden
	 * this setting.
	 */
	src->src.type = VIDEO_SOURCE_DMABUF;

	return v4l2_stream_off(src->vdev);
}

static int v4l2_source_queue_buffer(struct video_source *s,
				    struct video_buffer *buf)
{
	struct v4l2_source *src = to_v4l2_source(s);
	// 问题：sink在首次调用时不会有出队事件，需要特殊处理或修改算法
	printf("sink enqueue: %d\n", buf->index);
	// sink出队事件
	if (src->src.type == VIDEO_SOURCE_ENCODED) {
		// TODO: 若使用JPEG编码，入编码器sink队列待编码，并返回
		mjpeg_sink_enqueue(&src->encoder, buf->index, buf->mem);
		return 0;
	}

	// 否则说明初始化为DMABUF方式，入v4l2队列
	return v4l2_queue_buffer(src->vdev, buf);
}

static const struct video_source_ops v4l2_source_ops = {
	.destroy = v4l2_source_destroy,
	.set_format = v4l2_source_set_format,
	.set_frame_rate = v4l2_source_set_frame_rate,
	.alloc_buffers = v4l2_source_alloc_buffers,
	.import_buffers = v4l2_source_import_buffers,
	.export_buffers = v4l2_source_export_buffers,
	.free_buffers = v4l2_source_free_buffers,
	.mmap_buffers = v4l2_source_mmap_buffers,
	.stream_on = v4l2_source_stream_on,
	.stream_off = v4l2_source_stream_off,
	.queue_buffer = v4l2_source_queue_buffer,
};

struct video_source *v4l2_video_source_create(const char *devname)
{
	struct v4l2_source *src;

	src = malloc(sizeof *src);
	if (!src)
		return NULL;

	memset(src, 0, sizeof *src);
	src->src.ops = &v4l2_source_ops;
	src->src.type = VIDEO_SOURCE_DMABUF;

	src->vdev = v4l2_open(devname);
	if (!src->vdev) {
		free(src);
		return NULL;
	}

	return &src->src;
}

void v4l2_video_source_init(struct video_source *s, struct events *events)
{
	struct v4l2_source *src = to_v4l2_source(s);

	src->src.events = events;
}

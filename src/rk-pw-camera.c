// SPDX-License-Identifier: MIT
/*
 * rk-pw-camera: expose a Rockchip ISP output as a PipeWire camera.
 *
 * The vendor rkisp driver only offers the V4L2 multi-planar capture API,
 * which PipeWire's V4L2 monitor does not support, so desktop camera apps
 * (GNOME Snapshot, browsers via the camera portal) cannot see it.
 *
 * This daemon publishes a Video/Source node with media.role=Camera and
 * drives it itself. The ISP is only streamed while a client is linked:
 * on PAUSED the V4L2 stream is stopped, so the sensor is off when no app
 * uses the camera.
 *
 * Every buffer carries a header meta with the V4L2 capture time. Without
 * it all frames reach the client with the same PTS, and a clock-synced sink
 * (GTK's paintable sink in GNOME Snapshot) never shows any of them.
 *
 * NV12 is offered first (one memcpy, the GPU converts). BGRx is a fallback
 * for clients that cannot take YUV; the ISP emits full-range BT.601, which
 * libyuv converts on the CPU.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <linux/videodev2.h>

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/buffers.h>
#include <spa/node/io.h>
#include <spa/buffer/meta.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

#include <libyuv.h>

#define NBUF 4

struct vbuf {
	void *ptr;
	size_t len;
};

struct cam {
	struct pw_main_loop *loop;
	struct pw_stream *stream;
	struct spa_source *io;
	const char *dev;

	struct spa_video_info_raw fmt;
	int has_fmt;

	int fd;
	struct vbuf buf[NBUF];
	unsigned int nbuf;
	unsigned int stride;	/* V4L2 bytes per line */
	int pending;		/* dequeued V4L2 buffer waiting for process */
	uint64_t pending_ns;	/* its capture time, CLOCK_MONOTONIC */
	uint32_t pending_seq;

	struct spa_io_position *position;
};

static int xioctl(int fd, unsigned long req, void *arg)
{
	int r;

	do
		r = ioctl(fd, req, arg);
	while (r < 0 && errno == EINTR);
	return r;
}

static void v4l2_stop(struct cam *c)
{
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	struct v4l2_requestbuffers rb = { 0 };
	unsigned int i;

	if (c->fd < 0)
		return;
	if (c->io) {
		pw_loop_destroy_source(pw_main_loop_get_loop(c->loop), c->io);
		c->io = NULL;
	}
	xioctl(c->fd, VIDIOC_STREAMOFF, &type);
	for (i = 0; i < c->nbuf; i++)
		munmap(c->buf[i].ptr, c->buf[i].len);
	rb.type = type;
	rb.memory = V4L2_MEMORY_MMAP;
	xioctl(c->fd, VIDIOC_REQBUFS, &rb);
	close(c->fd);
	c->fd = -1;
	c->nbuf = 0;
	c->pending = -1;
	pw_log_info("camera stopped");
}

static void on_v4l2_ready(void *data, int fd, uint32_t mask)
{
	struct cam *c = data;
	struct v4l2_plane pl[VIDEO_MAX_PLANES];
	struct v4l2_buffer b = { 0 };

	b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	b.memory = V4L2_MEMORY_MMAP;
	b.m.planes = pl;
	b.length = VIDEO_MAX_PLANES;
	if (xioctl(fd, VIDIOC_DQBUF, &b) < 0)
		return;

	/* Drop a stale frame the graph has not consumed yet. */
	if (c->pending >= 0) {
		struct v4l2_buffer q = { 0 };

		q.type = b.type;
		q.memory = b.memory;
		q.index = c->pending;
		q.m.planes = pl;
		q.length = 1;
		xioctl(fd, VIDIOC_QBUF, &q);
	}
	c->pending = b.index;
	if ((b.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) == V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC) {
		c->pending_ns = (uint64_t)b.timestamp.tv_sec * SPA_NSEC_PER_SEC +
				(uint64_t)b.timestamp.tv_usec * SPA_NSEC_PER_USEC;
	} else {
		struct timespec ts;

		clock_gettime(CLOCK_MONOTONIC, &ts);
		c->pending_ns = SPA_TIMESPEC_TO_NSEC(&ts);
	}
	c->pending_seq = b.sequence;

	pw_stream_trigger_process(c->stream);
}

static int v4l2_start(struct cam *c)
{
	struct v4l2_format f = { 0 };
	struct v4l2_requestbuffers rb = { 0 };
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	unsigned int i;

	if (c->fd >= 0 || !c->has_fmt)
		return 0;

	c->fd = open(c->dev, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (c->fd < 0) {
		pw_log_error("open %s: %m", c->dev);
		return -errno;
	}

	f.type = type;
	f.fmt.pix_mp.width = c->fmt.size.width;
	f.fmt.pix_mp.height = c->fmt.size.height;
	f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
	f.fmt.pix_mp.num_planes = 1;
	if (xioctl(c->fd, VIDIOC_S_FMT, &f) < 0)
		goto fail;
	if (f.fmt.pix_mp.width != c->fmt.size.width ||
	    f.fmt.pix_mp.height != c->fmt.size.height ||
	    f.fmt.pix_mp.num_planes != 1) {
		pw_log_error("ISP gave %ux%u/%u planes for %ux%u",
			     f.fmt.pix_mp.width, f.fmt.pix_mp.height,
			     f.fmt.pix_mp.num_planes,
			     c->fmt.size.width, c->fmt.size.height);
		errno = EINVAL;
		goto fail;
	}
	c->stride = f.fmt.pix_mp.plane_fmt[0].bytesperline;

	rb.type = type;
	rb.memory = V4L2_MEMORY_MMAP;
	rb.count = NBUF;
	if (xioctl(c->fd, VIDIOC_REQBUFS, &rb) < 0 || rb.count < 2)
		goto fail;

	for (i = 0; i < rb.count && i < NBUF; i++) {
		struct v4l2_plane pl = { 0 };
		struct v4l2_buffer b = { 0 };

		b.type = type;
		b.memory = V4L2_MEMORY_MMAP;
		b.index = i;
		b.m.planes = &pl;
		b.length = 1;
		if (xioctl(c->fd, VIDIOC_QUERYBUF, &b) < 0)
			goto fail;
		c->buf[i].len = pl.length;
		c->buf[i].ptr = mmap(NULL, pl.length, PROT_READ, MAP_SHARED,
				     c->fd, pl.m.mem_offset);
		if (c->buf[i].ptr == MAP_FAILED)
			goto fail;
		c->nbuf = i + 1;
		if (xioctl(c->fd, VIDIOC_QBUF, &b) < 0)
			goto fail;
	}

	if (xioctl(c->fd, VIDIOC_STREAMON, &type) < 0)
		goto fail;

	c->io = pw_loop_add_io(pw_main_loop_get_loop(c->loop), c->fd,
			       SPA_IO_IN, false, on_v4l2_ready, c);
	pw_log_info("camera streaming %ux%u NV12 from %s",
		    c->fmt.size.width, c->fmt.size.height, c->dev);
	return 0;

fail:
	pw_log_error("V4L2 setup on %s failed: %m", c->dev);
	v4l2_stop(c);
	return -EIO;
}

static void on_process(void *data)
{
	struct cam *c = data;
	struct pw_buffer *pb;
	struct spa_buffer *sb;
	struct v4l2_plane pl = { 0 };
	struct v4l2_buffer q = { 0 };
	unsigned int w, h, y;
	uint8_t *dst, *src;

	if (c->pending < 0 || c->fd < 0)
		return;

	pb = pw_stream_dequeue_buffer(c->stream);
	if (pb) {
		struct spa_meta_header *hdr;

		sb = pb->buffer;
		hdr = spa_buffer_find_meta_data(sb, SPA_META_Header, sizeof(*hdr));
		if (hdr) {
			hdr->flags = 0;
			hdr->offset = 0;
			hdr->pts = c->pending_ns;
			hdr->dts_offset = 0;
			hdr->seq = c->pending_seq;
		}
		dst = sb->datas[0].data;
		w = c->fmt.size.width;
		h = c->fmt.size.height;
		src = c->buf[c->pending].ptr;
		if (dst && c->fmt.format == SPA_VIDEO_FORMAT_NV12 &&
		    sb->datas[0].maxsize >= w * h * 3 / 2) {
			if (c->fmt.format == SPA_VIDEO_FORMAT_NV12) {
				if (c->stride == w) {
					memcpy(dst, src, w * h * 3 / 2);
				} else {
					for (y = 0; y < h * 3 / 2; y++)
						memcpy(dst + y * w, src + y * c->stride, w);
				}
				sb->datas[0].chunk->stride = w;
				sb->datas[0].chunk->size = w * h * 3 / 2;
			}
		}
		if (dst && c->fmt.format == SPA_VIDEO_FORMAT_BGRx &&
		    sb->datas[0].maxsize >= w * h * 4) {
			/* libyuv "ARGB" is B,G,R,A in memory = BGRx */
			NV12ToARGBMatrix(src, c->stride,
					 src + c->stride * h, c->stride,
					 dst, w * 4, &kYuvJPEGConstants, w, h);
			sb->datas[0].chunk->stride = w * 4;
			sb->datas[0].chunk->size = w * h * 4;
		}
		sb->datas[0].chunk->offset = 0;
		pw_stream_queue_buffer(c->stream, pb);
	}

	q.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	q.memory = V4L2_MEMORY_MMAP;
	q.index = c->pending;
	q.m.planes = &pl;
	q.length = 1;
	xioctl(c->fd, VIDIOC_QBUF, &q);
	c->pending = -1;
}

static void on_state_changed(void *data, enum pw_stream_state old,
			     enum pw_stream_state state, const char *error)
{
	struct cam *c = data;

	pw_log_info("stream state %s -> %s", pw_stream_state_as_string(old),
		    pw_stream_state_as_string(state));
	if (state == PW_STREAM_STATE_STREAMING)
		v4l2_start(c);
	else
		v4l2_stop(c);
}

static void on_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
	struct cam *c = data;
	uint8_t buf[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
	const struct spa_pod *params[2];
	unsigned int size;

	if (id != SPA_PARAM_Format)
		return;
	if (!param) {
		c->has_fmt = 0;
		v4l2_stop(c);
		return;
	}
	if (spa_format_video_raw_parse(param, &c->fmt) < 0)
		return;
	c->has_fmt = 1;
	v4l2_stop(c);	/* a new size needs a new V4L2 format */

	if (c->fmt.format == SPA_VIDEO_FORMAT_BGRx)
		size = c->fmt.size.width * c->fmt.size.height * 4;
	else
		size = c->fmt.size.width * c->fmt.size.height * 3 / 2;
	params[0] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
		SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 8),
		SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
		SPA_PARAM_BUFFERS_size, SPA_POD_Int(size),
		SPA_PARAM_BUFFERS_stride, SPA_POD_Int(c->fmt.format == SPA_VIDEO_FORMAT_BGRx ?
						       c->fmt.size.width * 4 : c->fmt.size.width),
		SPA_PARAM_BUFFERS_dataType,
			SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemPtr) |
						 (1 << SPA_DATA_MemFd)));
	params[1] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
		SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));
	pw_stream_update_params(c->stream, params, 2);
}

static void on_io_changed(void *data, uint32_t id, void *area, uint32_t size)
{
	struct cam *c = data;

	if (id == SPA_IO_Position)
		c->position = area;
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = on_state_changed,
	.param_changed = on_param_changed,
	.io_changed = on_io_changed,
	.process = on_process,
};

static void on_signal(void *data, int sig)
{
	pw_main_loop_quit(((struct cam *)data)->loop);
}

static const char *find_device(int argc, char **argv)
{
	static char line[256];
	FILE *f;

	if (argc > 1)
		return argv[1];
	f = fopen("/run/rk-camera/device", "r");
	if (!f)
		return NULL;
	if (!fgets(line, sizeof(line), f)) {
		fclose(f);
		return NULL;
	}
	fclose(f);
	line[strcspn(line, "\n")] = 0;
	return line[0] ? line : NULL;
}

int main(int argc, char **argv)
{
	struct cam c = { .fd = -1, .pending = -1 };
	uint8_t buf[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
	const struct spa_pod *params[1];
	int ret;

	pw_init(&argc, &argv);

	c.dev = find_device(argc, argv);
	if (!c.dev) {
		fprintf(stderr, "rk-pw-camera: no camera (see rk-camera-select)\n");
		return 0;	/* not an error: board has no sensor attached */
	}

	c.loop = pw_main_loop_new(NULL);
	pw_loop_add_signal(pw_main_loop_get_loop(c.loop), SIGINT, on_signal, &c);
	pw_loop_add_signal(pw_main_loop_get_loop(c.loop), SIGTERM, on_signal, &c);

	c.stream = pw_stream_new_simple(pw_main_loop_get_loop(c.loop), "rkisp-camera",
		pw_properties_new(
			PW_KEY_MEDIA_TYPE, "Video",
			PW_KEY_MEDIA_CATEGORY, "Capture",
			PW_KEY_MEDIA_ROLE, "Camera",
			PW_KEY_MEDIA_CLASS, "Video/Source",
			PW_KEY_NODE_NAME, "rkisp-camera",
			PW_KEY_NODE_DESCRIPTION, "Camera",
			PW_KEY_DEVICE_API, "rkisp",
			"object.path", c.dev,
			NULL),
		&stream_events, &c);

	params[0] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
		SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
		SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
		SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(3,
			SPA_VIDEO_FORMAT_NV12,
			SPA_VIDEO_FORMAT_NV12,
			SPA_VIDEO_FORMAT_BGRx),
		/* 720p first: at 1080p GNOME Snapshot alone needs a whole core. */
		SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_ENUM_Rectangle(3,
			&SPA_RECTANGLE(1280, 720),
			&SPA_RECTANGLE(1280, 720),
			&SPA_RECTANGLE(1920, 1080)),
		SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&SPA_FRACTION(30, 1)));

	ret = pw_stream_connect(c.stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
				PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_MAP_BUFFERS,
				params, 1);
	if (ret < 0) {
		fprintf(stderr, "rk-pw-camera: connect failed: %s\n", spa_strerror(ret));
		return 1;
	}

	pw_main_loop_run(c.loop);

	v4l2_stop(&c);
	pw_stream_destroy(c.stream);
	pw_main_loop_destroy(c.loop);
	pw_deinit();
	return 0;
}

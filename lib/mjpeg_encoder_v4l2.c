#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <jpeglib.h>

#if JPEG_LIB_VERSION_MAJOR > 9 || (JPEG_LIB_VERSION_MAJOR == 9 && JPEG_LIB_VERSION_MINOR >= 4)
typedef size_t jpeg_mem_len_t;
#else
typedef unsigned long jpeg_mem_len_t;
#endif

#include "v4l2.h"
#include "video-source.h"
#include "mjpeg_encoder_v4l2.h"

static int output_enqueue(struct mjpeg_encoder_v4l2_t* encoder, struct output_item_t* output);
static int output_dequeue(struct mjpeg_encoder_v4l2_t* encoder, struct output_item_t* output);
static int source_sink_dequeue(struct mjpeg_encoder_v4l2_t* encoder, struct source_item_t* source, struct sink_item_t* sink);
static void encodeJPEG(struct jpeg_compress_struct cinfo, struct source_item_t* source, struct sink_item_t* sink, struct output_item_t* output);

static uint8_t* minp(uint8_t* p, uint8_t* max)
{
    return p < max ? p : max;
}

static void encodeJPEG(struct jpeg_compress_struct cinfo, struct source_item_t* source, struct sink_item_t* sink, struct output_item_t* output)
{
    cinfo.image_width = source->width;
    cinfo.image_height = source->height;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_YCbCr;
    cinfo.restart_interval = 0;
    jpeg_set_defaults(&cinfo);
    cinfo.raw_data_in = TRUE;
    jpeg_set_quality(&cinfo, MJPEG_ENCODER_QUALITY, TRUE);

    jpeg_mem_len_t jpeg_mem_len = source->width * source->height * 2;
    jpeg_mem_dest(&cinfo, (unsigned char**)(&sink->dest), &jpeg_mem_len);
    jpeg_start_compress(&cinfo, TRUE);
    // 输入格式为YUV420 Planar
    // YUV420 Planar格式：YYYYYYYY UUUU VVVV
    int stride2 = source->byte_per_line / 2;
    uint8_t* Y = (uint8_t*)source->mem;
    uint8_t* U = (uint8_t*)Y + source->byte_per_line * source->height;
    uint8_t* V = (uint8_t*)U + stride2 * (source->height / 2);
    JSAMPROW y_rows[16];
    JSAMPROW u_rows[8];
    JSAMPROW v_rows[8];
    uint8_t* Y_max = U - source->byte_per_line;
    uint8_t* U_max = V - stride2;
    uint8_t* V_max = U_max + stride2 * (source->height / 2);
    JSAMPARRAY rows[] = { y_rows, u_rows, v_rows };
    for (uint8_t* Y_row = Y, *U_row = U, *V_row = V; cinfo.next_scanline < source->height;) {
        for (int i = 0; i < 16; i++, Y_row += source->byte_per_line)
            y_rows[i] = minp(Y_row, Y_max);
        for (int i = 0; i < 8; i++, U_row += stride2, V_row += stride2) {
            u_rows[i] = minp(U_row, U_max);
            v_rows[i] = minp(V_row, V_max);
        }

        jpeg_write_raw_data(&cinfo, rows, 16);
    }

    jpeg_finish_compress(&cinfo);
    output->bytesused = jpeg_mem_len;
    output->source_id = source->source_id;
    output->sink_id = sink->sink_id;
    output->handler_data = source->handler_data;
    output->src = source->src;
    output->vdev = source->vdev;
    printf("encoded: %d -> %d, size=%d\n", output->source_id, output->sink_id, output->bytesused);
}

static void* encode_thread(void* param)
{
    struct mjpeg_encoder_v4l2_t* encoder = (struct mjpeg_encoder_v4l2_t*)param;
    struct source_item_t source;
    struct sink_item_t sink;
    struct output_item_t output;

    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);

    while (1) {
        if (source_sink_dequeue(encoder, &source, &sink) < 0) {
            break;
        }
        encodeJPEG(cinfo, &source, &sink, &output);
        output_enqueue(encoder, &output);
    }
    jpeg_destroy_compress(&cinfo);
    return NULL;
}

static void* output_thread(void* param)
{
    struct mjpeg_encoder_v4l2_t* encoder = (struct mjpeg_encoder_v4l2_t*)param;
    struct output_item_t output;
    struct video_buffer buf;
    while (1) {
        if (output_dequeue(encoder, &output) < 0) {
            break;
        }
        memset(&buf, 0, sizeof buf);
        buf.bytesused = output.bytesused;
        buf.dmabuf = -1;

        buf.index = output.source_id;
        v4l2_queue_buffer(output.vdev, &buf);

        buf.index = output.sink_id;
        (*encoder->handler)(output.handler_data, output.src, &buf);
    }
    return NULL;
}

static int output_enqueue(struct mjpeg_encoder_v4l2_t* encoder, struct output_item_t* output)
{
    pthread_mutex_lock(&encoder->output_mutex);
    encoder->output_queue[encoder->output_queue_tail] = *output;
    encoder->output_queue_tail = (encoder->output_queue_tail + 1) % MJPEG_ENCODER_BUFFER_QUEUE_SIZE;
    pthread_cond_signal(&encoder->output_cond_var);
    pthread_mutex_unlock(&encoder->output_mutex);

    return 0;
}

static void get_200ms_ts(struct timespec* ts)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    ts->tv_sec = tv.tv_sec + 0;
    ts->tv_nsec = tv.tv_usec * 1000 + 200 * 1000 * 1000;
    if (ts->tv_nsec >= 1000 * 1000 * 1000) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000 * 1000 * 1000;
    }
}

static int output_dequeue(struct mjpeg_encoder_v4l2_t* encoder, struct output_item_t* output)
{
    struct timespec ts;
    pthread_mutex_lock(&encoder->output_mutex);
    while (encoder->output_queue_head == encoder->output_queue_tail) {
        get_200ms_ts(&ts);
        pthread_cond_timedwait(&encoder->output_cond_var, &encoder->output_mutex, &ts);
        if (encoder->abort && encoder->output_queue_head == encoder->output_queue_tail) {
            pthread_mutex_unlock(&encoder->output_mutex);
            return -1;
        }
        else if (encoder->abort) {
            printf("WARNING: waiting for encoder\n");
        }
    }
    *output = encoder->output_queue[encoder->output_queue_head];
    encoder->output_queue_head = (encoder->output_queue_head + 1) % MJPEG_ENCODER_BUFFER_QUEUE_SIZE;
    pthread_mutex_unlock(&encoder->output_mutex);
    printf("output dequeue: %d -> %d\n", output->source_id, output->sink_id);
    return 0;
}

static int source_sink_dequeue(struct mjpeg_encoder_v4l2_t* encoder, struct source_item_t* source, struct sink_item_t* sink)
{
    struct timespec ts;
    pthread_mutex_lock(&encoder->encode_mutex);
    while (encoder->source_queue_head == encoder->source_queue_tail || encoder->sink_queue_head == encoder->sink_queue_tail) {
        get_200ms_ts(&ts);
        pthread_cond_timedwait(&encoder->encode_cond_var, &encoder->encode_mutex, &ts);
        if (encoder->abort) {
            pthread_mutex_unlock(&encoder->encode_mutex);
            return -1;
        }
        else if (encoder->abort) {
            printf("WARNING: waiting for source/sink\n");
        }
    }
    *source = encoder->source_queue[encoder->source_queue_head];
    *sink = encoder->sink_queue[encoder->sink_queue_head];
    encoder->source_queue_head = (encoder->source_queue_head + 1) % MJPEG_ENCODER_BUFFER_QUEUE_SIZE;
    encoder->sink_queue_head = (encoder->sink_queue_head + 1) % MJPEG_ENCODER_BUFFER_QUEUE_SIZE;
    pthread_mutex_unlock(&encoder->encode_mutex);

    return 0;
}

int mjpeg_begin(struct mjpeg_encoder_v4l2_t* encoder, video_source_buffer_handler_t *handler)
{
    encoder->handler = handler;

    encoder->source_queue_head = 0;
    encoder->source_queue_tail = 0;
    encoder->sink_queue_head = 0;
    encoder->sink_queue_tail = 0;
    encoder->output_queue_head = 0;
    encoder->output_queue_tail = 0;

    encoder->abort = 0;

    pthread_mutex_init(&encoder->encode_mutex, NULL);
    pthread_cond_init(&encoder->encode_cond_var, NULL);
    pthread_mutex_init(&encoder->output_mutex, NULL);
    pthread_cond_init(&encoder->output_cond_var, NULL);

    for (int i = 0; i < ENCODE_THREAD_NUM; i++) {
        pthread_create(&encoder->encode_thread[i], NULL, encode_thread, encoder);
    }
    pthread_create(&encoder->output_thread, NULL, output_thread, encoder);

    return 0;
}

int mjpeg_abort(struct mjpeg_encoder_v4l2_t* encoder)
{
    encoder->abort = 1;
    return 0;
}

int mjpeg_sink_enqueue(struct mjpeg_encoder_v4l2_t* encoder, int sink_id, void* dest)
{
    pthread_mutex_lock(&encoder->encode_mutex);
    encoder->sink_queue[encoder->sink_queue_tail].sink_id = sink_id;
    encoder->sink_queue[encoder->sink_queue_tail].dest = dest;
    encoder->sink_queue_tail = (encoder->sink_queue_tail + 1) % MJPEG_ENCODER_BUFFER_QUEUE_SIZE;
    if (encoder->source_queue_head != encoder->source_queue_tail) {
        pthread_cond_signal(&encoder->encode_cond_var);
    }
    pthread_mutex_unlock(&encoder->encode_mutex);

    return 0;
}

int mjpeg_source_enqueue(struct mjpeg_encoder_v4l2_t *encoder, int source_id, void *mem, unsigned int width, unsigned int height, unsigned int byte_per_line, void *handler_data, struct video_source *src, struct v4l2_device *vdev)
{
    if (byte_per_line == 0) {
        byte_per_line = width;
    }
    pthread_mutex_lock(&encoder->encode_mutex);
    encoder->source_queue[encoder->source_queue_tail].source_id = source_id;
    encoder->source_queue[encoder->source_queue_tail].mem = mem;
    encoder->source_queue[encoder->source_queue_tail].width = width;
    encoder->source_queue[encoder->source_queue_tail].height = height;
    encoder->source_queue[encoder->source_queue_tail].byte_per_line = byte_per_line;
    encoder->source_queue[encoder->source_queue_tail].handler_data = handler_data;
    encoder->source_queue[encoder->source_queue_tail].src = src;
    encoder->source_queue[encoder->source_queue_tail].vdev = vdev;
    encoder->source_queue_tail = (encoder->source_queue_tail + 1) % MJPEG_ENCODER_BUFFER_QUEUE_SIZE;
    if (encoder->sink_queue_head != encoder->sink_queue_tail) {
        pthread_cond_signal(&encoder->encode_cond_var);
    }
    pthread_mutex_unlock(&encoder->encode_mutex);

    return 0;
}


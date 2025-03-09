#ifndef __MJPEG_ENCODER_V4L2_H__
#define __MJPEG_ENCODER_V4L2_H__

#define MJPEG_ENCODER_BUFFER_QUEUE_SIZE 8
#define MJPEG_ENCODER_QUALITY 50
#define ENCODE_THREAD_NUM 4

struct source_item_t
{
    int source_id;
    void *mem;
    unsigned int width;
    unsigned int height;
    unsigned int byte_per_line;
    // handler相关
    // src->src.handler(src->src.handler_data, &src->src, &buf);
    void *handler_data;
    struct video_source *src;
    struct v4l2_device *vdev;
    // buf由输出线程根据sink_id source_id bytesused生成
};

struct sink_item_t
{
    int sink_id;
    void *dest;
};

struct output_item_t
{
    int source_id;
    int sink_id;
    unsigned int bytesused;
    // handler相关
    void *handler_data;
    struct video_source *src;
    struct v4l2_device *vdev;
};

struct mjpeg_encoder_v4l2_t
{
    pthread_t encode_thread[ENCODE_THREAD_NUM];
    pthread_t output_thread;

    struct source_item_t source_queue[MJPEG_ENCODER_BUFFER_QUEUE_SIZE];
    int source_queue_head;
    int source_queue_tail;
    struct sink_item_t sink_queue[MJPEG_ENCODER_BUFFER_QUEUE_SIZE];
    int sink_queue_head;
    int sink_queue_tail;
    struct output_item_t output_queue[MJPEG_ENCODER_BUFFER_QUEUE_SIZE];
    int output_queue_head;
    int output_queue_tail;

    pthread_mutex_t encode_mutex;
    pthread_cond_t encode_cond_var;
    pthread_mutex_t output_mutex;
    pthread_cond_t output_cond_var;

    video_source_buffer_handler_t *handler;

    int abort;
};

int mjpeg_begin(struct mjpeg_encoder_v4l2_t* encoder, video_source_buffer_handler_t *handler);
int mjpeg_abort(struct mjpeg_encoder_v4l2_t *encoder);
int mjpeg_sink_enqueue(struct mjpeg_encoder_v4l2_t *encoder, int sink_id, void *dest);
int mjpeg_source_enqueue(struct mjpeg_encoder_v4l2_t *encoder, int source_id, void *mem, unsigned int width, unsigned int height, unsigned int byte_per_line, void *handler_data, struct video_source *src, struct v4l2_device *vdev);

#endif

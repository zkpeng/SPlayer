#include <stdio.h>

#include "SDL.h"
#include "libavformat/avformat.h"
#include "libavutil/time.h"
#include "libswresample/swresample.h"
#include "limits.h"

#ifdef _WIN32
#    include <windows.h>
#endif

typedef struct Packet {
    AVPacket av_packet;
    struct Packet* next;
    int serial;
} Packet;

typedef struct PacketQueue {
    Packet* first;
    Packet* last;
    int nb_pkts;
    int serial;
    int abort_queue;
    SDL_mutex* mutex;
    SDL_cond* cond;
} PacketQueue;

typedef struct Frame {
    AVFrame* av_frame;
    int width;
    int height;
    double pts;
    double duration;
    int serial;
} Frame;

#define FRAME_MAX_SIZE 10
#define AV_SYNC_THRESHOLD_MAX 0.1
#define AV_SYNC_THRESHOLD_MIN 0.04
#define AV_SYNC_FRAMEDUP_THRESHOLD 0.1

typedef struct FrameQueue {
    Frame frame_arr[FRAME_MAX_SIZE];
    int nb_frames;
    int r_index;
    int w_index;
    int rindex_shown;
    PacketQueue* pkt_queue;
    SDL_mutex* mutex;
    SDL_cond* cond;
} FrameQueue;

typedef struct AudioParams {
    int channels;
    int64_t channel_layout;
    int sample_rate;
    enum AVSampleFormat fmt;
    int size_per_sample;
    int bytes_per_second;
} AudioParams;

typedef struct Decoder {
    AVCodecContext* codec_cxt;
    int pkt_serial;
    int finished;
    AVPacket* packet;
    int pending;
    PacketQueue* pkt_queue;
    SDL_cond* contine_read;
    SDL_Thread* decoder_tid;
} Decoder;

typedef struct Clock {
    int64_t last_updated;
    int64_t drift;
    int pts;
    int serial;
    int paused;
    int* queue_serial;
} Clock;

typedef struct SPlayerContext {
    char* file_name;
    AVFormatContext* fmt_ctx;
    AVInputFormat* input_fmt;
    SDL_Thread* read_tid;
    int abort_read;
    SDL_cond* contine_read;
    AVStream* audioStream;
    AVStream* videoStream;
    int audio_stream_index;
    int video_stream_index;
    PacketQueue audio_pq;
    PacketQueue video_pq;
    FrameQueue audio_fq;
    FrameQueue video_fq;
    Decoder audio_decoder;
    Decoder video_decoder;
    Clock audio_clock;
    Clock video_clock;
    int eof;
    double frame_start_time;
    SDL_Texture* video_texture;
    int width;
    int audio_hw_buffer_size;
    uint8_t* audio_buffer;
    int audio_buffer_size;
    int audio_buffer_index;
    int audio_remain_buffer_size;
    AudioParams audio_params_src, audio_params_dst;
    double audio_pts;
    SwrContext* swr_ctx;
    int paused;
    int seek_req;
    int64_t seek_pos;
    int64_t seek_incr;
    int seek_flags;
    int step;
} SPlayerContext;

void packet_queue_init(PacketQueue* pkt_queue) {
    memset(pkt_queue, 0, sizeof(PacketQueue));
    pkt_queue->mutex = SDL_CreateMutex();
    pkt_queue->cond = SDL_CreateCond();
    pkt_queue->abort_queue = 1;
}

static AVPacket flush_pkt;

int packet_queue_put_inner(PacketQueue* pkt_queue, AVPacket* av_packet) {
    if (pkt_queue->abort_queue) {
        return -1;
    }
    Packet* pkt = av_mallocz(sizeof(Packet));
    pkt->av_packet = *av_packet;
    if (av_packet->data == flush_pkt.data) {
        pkt_queue->serial++;
    }
    pkt->serial = pkt_queue->serial;
    pkt->next = NULL;
    if (!pkt_queue->last) {
        pkt_queue->first = pkt;
    } else {
        pkt_queue->last->next = pkt;
    }
    pkt_queue->last = pkt;
    pkt_queue->nb_pkts++;
    SDL_CondSignal(pkt_queue->cond);
    return 0;
}

void packet_queue_start(PacketQueue* pkt_queue) {
    SDL_LockMutex(pkt_queue->mutex);
    pkt_queue->abort_queue = 0;
    packet_queue_put_inner(pkt_queue, &flush_pkt);
    SDL_UnlockMutex(pkt_queue->mutex);
}

void packet_queue_flush(PacketQueue* pkt_queue) {
    Packet *pkt, *pkt_tmp;
    pkt = pkt_queue->first;
    while (pkt) {
        av_packet_unref(&pkt->av_packet);
        pkt_tmp = pkt->next;
        av_freep(&pkt);
        pkt = pkt_tmp;
    }
    pkt_queue->nb_pkts = 0;
    pkt_queue->first = NULL;
    pkt_queue->last = NULL;
}

void packet_queue_destroy(PacketQueue* pkt_queue) {
    packet_queue_flush(pkt_queue);
    SDL_DestroyMutex(pkt_queue->mutex);
    SDL_DestroyCond(pkt_queue->cond);
}

int packet_queue_put(PacketQueue* pkt_queue, AVPacket* av_packet) {
    SDL_LockMutex(pkt_queue->mutex);
    int ret = packet_queue_put_inner(pkt_queue, av_packet);
    SDL_UnlockMutex(pkt_queue->mutex);
    if (ret == -1 && av_packet != &flush_pkt) {
        av_packet_unref(av_packet);
    }
    return ret;
}

int packet_queue_put_nullpkt(PacketQueue* pkt_queue) {
    int ret;
    AVPacket* pkt = av_packet_alloc();
    av_init_packet(pkt);
    pkt->data = NULL;
    pkt->size = 0;
    ret = packet_queue_put(pkt_queue, pkt);
    return ret;
}

int packet_queue_get(PacketQueue* pkt_queue, AVPacket* av_packet, int* serial) {
    SDL_LockMutex(pkt_queue->mutex);
    while (pkt_queue->nb_pkts <= 0 && !pkt_queue->abort_queue) {
        SDL_CondWait(pkt_queue->cond, pkt_queue->mutex);
    }
    if (pkt_queue->abort_queue) {
        av_log(NULL, AV_LOG_INFO, "*****************packet_queue_get --> receive abort*****************.\n");
        return -1;
    }
    Packet* pkt = pkt_queue->first;
    //    av_packet_move_ref(av_packet, pkt->av_packet);
    *av_packet = pkt->av_packet;
    pkt_queue->first = pkt->next;
    if (serial) {
        *serial = pkt->serial;
    }
    av_freep(&pkt);
    pkt_queue->nb_pkts--;
    if (pkt_queue->nb_pkts == 0) {
        pkt_queue->first = NULL;
        pkt_queue->last = NULL;
    }
    SDL_UnlockMutex(pkt_queue->mutex);
    return 1;
}

void frame_queue_init(FrameQueue* frame_queue, PacketQueue* pkt_queue) {
    memset(frame_queue, 0, sizeof(FrameQueue));
    for (int i = 0; i < FRAME_MAX_SIZE; i++) {
        frame_queue->frame_arr[i].av_frame = av_frame_alloc();
    }
    frame_queue->pkt_queue = pkt_queue;
    frame_queue->mutex = SDL_CreateMutex();
    frame_queue->cond = SDL_CreateCond();
}

void frame_queue_flush(FrameQueue* frame_queue) {
    for (int i = 0; i < FRAME_MAX_SIZE; i++) {
        av_frame_unref(frame_queue->frame_arr[i].av_frame);
    }
    frame_queue->nb_frames = 0;
    frame_queue->r_index = frame_queue->w_index = 0;
}

void frame_queue_destroy(FrameQueue* frame_queue) {
    frame_queue_flush(frame_queue);
    for (int i = 0; i < FRAME_MAX_SIZE; i++) {
        av_frame_free(&(frame_queue->frame_arr[i].av_frame));
    }
    SDL_DestroyMutex(frame_queue->mutex);
    SDL_DestroyCond(frame_queue->cond);
}

Frame* frame_queue_peek_writable(FrameQueue* frame_queue) {
    Frame* frame;
    SDL_LockMutex(frame_queue->mutex);
    while (frame_queue->nb_frames >= FRAME_MAX_SIZE && !frame_queue->pkt_queue->abort_queue) {
        SDL_CondWait(frame_queue->cond, frame_queue->mutex);
    }
    if (frame_queue->pkt_queue->abort_queue) {
        av_log(NULL, AV_LOG_INFO, "*****************frame_queue_peek_writable --> receive abort*****************.\n");
        return NULL;
    }
    frame = &(frame_queue->frame_arr[frame_queue->w_index]);
    SDL_UnlockMutex(frame_queue->mutex);
    return frame;
}

void frame_queue_push(FrameQueue* frame_queue) {
    SDL_LockMutex(frame_queue->mutex);
    frame_queue->nb_frames++;
    frame_queue->w_index++;
    if (frame_queue->w_index > FRAME_MAX_SIZE - 1) {
        frame_queue->w_index = 0;
    }
    SDL_CondSignal(frame_queue->cond);
    SDL_UnlockMutex(frame_queue->mutex);
}

int queue_picture(FrameQueue* video_fq, AVFrame* frame, double pts, double duration, int serial) {
    int ret;
    Frame* my_frame = frame_queue_peek_writable(video_fq);
    if (my_frame) {
        my_frame->duration = duration;
        my_frame->pts = pts;
        my_frame->width = frame->width;
        my_frame->height = frame->height;
        my_frame->serial = serial;
        av_frame_move_ref(my_frame->av_frame, frame);
        frame_queue_push(video_fq);
    }
    return ret;
}

Frame* frame_queue_peek_readable(FrameQueue* frame_queue) {
    Frame* frame;
    SDL_LockMutex(frame_queue->mutex);
    while (frame_queue->nb_frames <= 0 && !frame_queue->pkt_queue->abort_queue) {
        SDL_CondWait(frame_queue->cond, frame_queue->mutex);
    }
    if (frame_queue->pkt_queue->abort_queue) {
        return NULL;
    }
    frame = &(frame_queue->frame_arr[(frame_queue->r_index + frame_queue->rindex_shown) % FRAME_MAX_SIZE]);
    SDL_UnlockMutex(frame_queue->mutex);
    return frame;
}

void frame_queue_next(FrameQueue* frame_queue) {
    if (!frame_queue->rindex_shown) {
        frame_queue->rindex_shown = 1;
        return;
    }
    SDL_LockMutex(frame_queue->mutex);
    av_frame_unref(frame_queue->frame_arr[frame_queue->r_index].av_frame);
    frame_queue->r_index++;
    if (frame_queue->r_index > FRAME_MAX_SIZE - 1) {
        frame_queue->r_index = 0;
    }
    frame_queue->nb_frames--;
    SDL_CondSignal(frame_queue->cond);
    SDL_UnlockMutex(frame_queue->mutex);
}

Frame* frame_queue_peek_last(FrameQueue* frame_queue) { return &frame_queue->frame_arr[frame_queue->r_index]; }

Frame* frame_queue_peek(FrameQueue* frame_queue) {
    return &frame_queue->frame_arr[frame_queue->r_index + frame_queue->rindex_shown];
}

Frame* frame_queue_peek_next(FrameQueue* frame_queue) {
    return &frame_queue->frame_arr[(frame_queue->r_index + frame_queue->rindex_shown + 1) % FRAME_MAX_SIZE];
}

int frame_queue_nb_remaining(FrameQueue* fq) { return fq->nb_frames - fq->rindex_shown; }

void decoder_init(Decoder* decoder, AVCodecContext* codec_ctx, PacketQueue* pkt_queue, SDL_cond* contine_read) {
    memset(decoder, 0, sizeof(Decoder));
    decoder->codec_cxt = codec_ctx;
    decoder->pkt_queue = pkt_queue;
    decoder->contine_read = contine_read;
    decoder->packet = av_packet_alloc();
    decoder->pending = 0;
    decoder->pkt_serial = -1;
    decoder->finished = 0;
}

int decoder_start(Decoder* decoder, int (*decoder_fun)(void*), const char* thread_name, void* arg) {
    packet_queue_start(decoder->pkt_queue);
    decoder->decoder_tid = SDL_CreateThread(decoder_fun, thread_name, arg);
    return 0;
}

double get_clock(Clock* clock) {
    if (clock->paused) {
        return clock->pts;
    } else {
        double cur_time = av_gettime_relative() / 1000000.0;
        return clock->drift + cur_time;
    }
}

void set_clock_at(Clock* clock, int pts, int serial, double time) {
    clock->last_updated = time;
    clock->pts = pts;
    clock->drift = clock->pts - time;
    clock->serial = serial;
}

void set_clock(Clock* clock, int pts, int serial) {
    double time = av_gettime_relative() / 1000000.0;
    set_clock_at(clock, pts, serial, time);
}

void init_clock(Clock* clock, int* queue_serial) {
    clock->queue_serial = queue_serial;
    clock->paused = 0;
    set_clock(clock, NAN, -1);
}

int get_video_frame(SPlayerContext* spc, AVFrame* frame) {
    int ret = AVERROR(EAGAIN);
    AVPacket pkt;
    av_init_packet(&pkt);
    Decoder* decoder = &spc->video_decoder;
    AVCodecContext* video_codec_ctx = decoder->codec_cxt;

    for (;;) {
        if (decoder->pkt_serial == decoder->pkt_queue->serial) {
            do {
                ret = avcodec_receive_frame(video_codec_ctx, frame);
                if (ret == 0) {
                    return 1;
                }
                if (ret == AVERROR_EOF) {
                    avcodec_flush_buffers(video_codec_ctx);
                    return 0;
                }
            } while (ret != AVERROR(EAGAIN));
        }
        if (decoder->pkt_queue->nb_pkts == 0) {
            SDL_CondSignal(decoder->contine_read);
        }
        if (decoder->pending) {
            av_packet_move_ref(&pkt, decoder->packet);
            decoder->pending = 0;
        } else {
            do {
                ret = packet_queue_get(decoder->pkt_queue, &pkt, &decoder->pkt_serial);
                if (ret < 0) {
                    return -1;
                }
                if (decoder->pkt_serial != decoder->pkt_queue->serial) {
                    av_log(NULL, AV_LOG_INFO, "jump video packet.\n");
                    av_packet_unref(&pkt);
                }
            } while (decoder->pkt_serial != decoder->pkt_queue->serial);
        }
        if (pkt.data == flush_pkt.data) {
            avcodec_flush_buffers(video_codec_ctx);
        } else {
            ret = avcodec_send_packet(video_codec_ctx, &pkt);
            if (ret == AVERROR(EAGAIN)) {
                av_packet_move_ref(decoder->packet, &pkt);
                decoder->pending = 1;
            }
            av_packet_unref(&pkt);
        }
    };
    return 1;
}

int video_decoder_thread(void* arg) {
    int ret;
    double pts;
    double duration;
    SPlayerContext* spc = arg;
    AVRational tb = spc->videoStream->time_base;
    AVRational frame_rate = av_guess_frame_rate(spc->fmt_ctx, spc->videoStream, NULL);
    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        av_log(NULL, AV_LOG_ERROR, "frame alloc failed.\n");
    }
    for (;;) {
        ret = get_video_frame(spc, frame);
        if (ret < 0) {
            goto the_end;
        }
        if (!ret) {
            continue;
        }
        duration = (frame_rate.num && frame_rate.den) ? av_q2d((AVRational){frame_rate.den, frame_rate.num}) : 0;
        pts = frame->pts == AV_NOPTS_VALUE ? NAN : frame->pts * av_q2d(tb);
        queue_picture(&spc->video_fq, frame, pts, duration, spc->video_decoder.pkt_serial);
        av_frame_unref(frame);
    }
the_end:
    if (frame) {
        av_frame_free(&frame);
    }
    return ret;
}

int get_audio_frame(SPlayerContext* spc, AVFrame* frame) {
    int ret = AVERROR(EAGAIN);
    AVPacket pkt;
    av_init_packet(&pkt);
    Decoder* decoder = &spc->audio_decoder;
    AVCodecContext* audio_codec_ctx = decoder->codec_cxt;

    for (;;) {
        if (decoder->pkt_serial == spc->audio_pq.serial) {
            do {
                ret = avcodec_receive_frame(audio_codec_ctx, frame);
                if (ret == 0) {
                    AVRational tb = (AVRational){1, frame->sample_rate};
                    if (frame->pts != AV_NOPTS_VALUE) {
                        frame->pts = av_rescale_q(frame->pts, audio_codec_ctx->pkt_timebase, tb);
                    }
                    return 1;
                }
                if (ret == AVERROR_EOF) {
                    avcodec_flush_buffers(audio_codec_ctx);
                    return 0;
                }
            } while (ret != AVERROR(EAGAIN));
        }
        if (decoder->pkt_queue->nb_pkts == 0) {
            SDL_CondSignal(decoder->contine_read);
        }
        if (decoder->pending) {
            av_packet_move_ref(&pkt, decoder->packet);
            decoder->pending = 0;
        } else {
            do {
                ret = packet_queue_get(decoder->pkt_queue, &pkt, &decoder->pkt_serial);
                if (ret < 0) {
                    return -1;
                }
                if (decoder->pkt_serial != decoder->pkt_queue->serial) {
                    av_packet_unref(&pkt);
                }
            } while (decoder->pkt_serial != decoder->pkt_queue->serial);
        }
        if (pkt.data == flush_pkt.data) {
            avcodec_flush_buffers(audio_codec_ctx);
        } else {
            ret = avcodec_send_packet(audio_codec_ctx, &pkt);
            if (ret == AVERROR_EOF) {
                av_log(NULL, AV_LOG_INFO, "audio avcodec_send_packet AVERROR_EOF.\n");
            }
            if (ret == AVERROR(EAGAIN)) {
                av_log(NULL, AV_LOG_INFO, "audio avcodec_send_packet EAGAIN.\n");
                av_packet_move_ref(decoder->packet, &pkt);
                decoder->pending = 1;
            }
            av_packet_unref(&pkt);
        }
    };
    return 1;
}

int audio_decoder_thread(void* arg) {
    int ret;
    SPlayerContext* spc = arg;
    Frame* frame;
    AVFrame* av_frame = av_frame_alloc();
    AVRational tb;
    for (;;) {
        ret = get_audio_frame(spc, av_frame);
        if (ret < 0) {
            goto end;
        }
        if (!ret) {
            continue;
        }
        tb = (AVRational){1, av_frame->sample_rate};
        frame = frame_queue_peek_writable(&spc->audio_fq);
        if (!frame) {
            goto end;
        }
        frame->pts = av_frame->pts == AV_NOPTS_VALUE ? NAN : av_frame->pts * av_q2d(tb);
        frame->duration = av_frame->nb_samples * av_q2d(tb);
        frame->serial = spc->audio_decoder.pkt_serial;
        av_frame_move_ref(frame->av_frame, av_frame);
        frame_queue_push(&spc->audio_fq);
    }
end:
    av_frame_free(&av_frame);
    return ret;
}

char* file_name;
SDL_Window* window;
SDL_Renderer* renderer;
SDL_RendererInfo renderer_info = {0};
SDL_AudioDeviceID audio_dev_id;
int window_width = 640;
int window_height = 368;
int window_left = SDL_WINDOWPOS_CENTERED;
int window_top = SDL_WINDOWPOS_CENTERED;
float seek_interval = 5;

int audio_decode_frame(SPlayerContext* spc) {
    int ret;
    int data_size;
    Frame* frame;
    if (spc->paused) {
        return -1;
    }

    do {
        if (frame_queue_nb_remaining(&spc->audio_fq) == 0) {
            return -1;
        }
        frame = frame_queue_peek_readable(&spc->audio_fq);
        if (!frame) {
            return -1;
        }
        frame_queue_next(&spc->audio_fq);
    } while (frame->serial != spc->audio_pq.serial);

    if ((frame->av_frame->channel_layout != spc->audio_params_dst.channel_layout ||
         frame->av_frame->sample_rate != spc->audio_params_dst.sample_rate ||
         frame->av_frame->format != spc->audio_params_dst.fmt) &&
        !spc->swr_ctx) {
        spc->swr_ctx = swr_alloc_set_opts(NULL, spc->audio_params_dst.channel_layout, spc->audio_params_dst.fmt,
                                          spc->audio_params_dst.sample_rate, frame->av_frame->channel_layout,
                                          frame->av_frame->format, frame->av_frame->sample_rate, 0, NULL);
        if (!spc->swr_ctx) {
            swr_free(&spc->swr_ctx);
            return -1;
        }
        ret = swr_init(spc->swr_ctx);
        if (ret < 0) {
            swr_free(&spc->swr_ctx);
            return -1;
        }
        spc->audio_params_src.channel_layout = frame->av_frame->channel_layout;
        spc->audio_params_src.channels = frame->av_frame->channels;
        spc->audio_params_src.sample_rate = frame->av_frame->sample_rate;
        spc->audio_params_src.fmt = frame->av_frame->format;
    }
    if (spc->swr_ctx) {
        uint8_t** in_data = frame->av_frame->extended_data;
        int in_samples = frame->av_frame->nb_samples;
        uint8_t** out_data;
        int out_line_size;
        int out_samples =
            (int64_t)frame->av_frame->nb_samples * spc->audio_params_dst.sample_rate / frame->av_frame->sample_rate +
            256;
        ret = av_samples_alloc_array_and_samples(&out_data, &out_line_size, spc->audio_params_dst.channels, out_samples,
                                                 spc->audio_params_dst.fmt, 0);
        int out_samples_real = swr_convert(spc->swr_ctx, out_data, out_samples, in_data, in_samples);
        spc->audio_buffer = *out_data;
        data_size =
            out_samples_real * spc->audio_params_dst.channels * av_get_bytes_per_sample(spc->audio_params_dst.fmt);
    } else {
        spc->audio_buffer = frame->av_frame->data[0];
        data_size = av_samples_get_buffer_size(NULL, frame->av_frame->channels, frame->av_frame->nb_samples,
                                               frame->av_frame->format, 1);
    }
    if (data_size < 0) {
        av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size err:%s.\n", av_err2str(data_size));
        return 0;
    }
    spc->audio_pts = frame->pts + frame->duration;
    return data_size;
}

void sdl_audio_callback(void* _spc, uint8_t* stream, int len) {
    int len1;
    SPlayerContext* spc = _spc;
    int data_size;
    double sdl_callback_time = av_gettime_relative() / 1000000.0;
    while (len > 0) {
        int remain_size = spc->audio_remain_buffer_size;
        if (remain_size == 0) {
            data_size = audio_decode_frame(spc);
            if (data_size < 0) {
                spc->audio_buffer_size = 512;
                spc->audio_buffer = NULL;
            } else {
                spc->audio_buffer_size = data_size;
            }
            spc->audio_buffer_index = 0;
        }
        len1 = spc->audio_buffer_size - spc->audio_buffer_index;
        if (len1 > len) {
            len1 = len;
        }
        if (spc->audio_buffer) {
            memcpy(stream, spc->audio_buffer + spc->audio_buffer_index, len1);
        } else {
            memset(stream, 0, len1);
        }
        spc->audio_buffer_index += len1;

        stream += len1;
        len -= len1;
    }
    spc->audio_remain_buffer_size = spc->audio_buffer_size - spc->audio_buffer_index;
    double sdl_callback_pts = spc->audio_pts - (spc->audio_hw_buffer_size * 2.0 + spc->audio_remain_buffer_size) /
                                                   spc->audio_params_dst.bytes_per_second;
    set_clock_at(&spc->audio_clock, sdl_callback_pts, 1, sdl_callback_time);
}

int audio_open(SPlayerContext* spc, int nb_channels, int sample_rate, int64_t channel_layout,
               AudioParams* audio_params_dst) {
    SDL_AudioSpec wanted_spec, real_spec;
    wanted_spec.channels = nb_channels;
    wanted_spec.freq = sample_rate;
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.silence = 0;
    wanted_spec.samples = 1024;
    wanted_spec.callback = sdl_audio_callback;
    wanted_spec.userdata = spc;
    audio_dev_id = SDL_OpenAudioDevice(NULL, 0, &wanted_spec, &real_spec,
                                       SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHANNELS_CHANGE);
    if (!audio_dev_id) {
        av_log(NULL, AV_LOG_ERROR, "open audio device failed.\n");
        return -1;
    }
    audio_params_dst->fmt = AV_SAMPLE_FMT_S16;
    audio_params_dst->sample_rate = real_spec.freq;
    audio_params_dst->channels = real_spec.channels;
    audio_params_dst->channel_layout = av_get_default_channel_layout(real_spec.channels);
    audio_params_dst->size_per_sample =
        av_samples_get_buffer_size(NULL, audio_params_dst->channels, 1, audio_params_dst->fmt, 1);
    audio_params_dst->bytes_per_second = av_samples_get_buffer_size(
        NULL, audio_params_dst->channels, audio_params_dst->sample_rate, audio_params_dst->fmt, 1);
    return real_spec.size;
}

int stream_component_open(SPlayerContext* spc, int stream_index) {
    int ret;
    AVCodecContext* codec_ctx;
    AVCodec* codec;
    AVFormatContext* fmt_ctx = spc->fmt_ctx;
    int nb_channels, sample_rate;
    int64_t channel_layout;

    codec_ctx = avcodec_alloc_context3(NULL);
    if (!codec_ctx) {
        return -1;
    }
    ret = avcodec_parameters_to_context(codec_ctx, fmt_ctx->streams[stream_index]->codecpar);
    if (ret < 0) {
        goto fail;
    }
    codec_ctx->pkt_timebase = fmt_ctx->streams[stream_index]->time_base;
    codec = avcodec_find_decoder(codec_ctx->codec_id);
    ret = avcodec_open2(codec_ctx, codec, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "open codec failed.\n");
        goto fail;
    }
    switch (codec_ctx->codec_type) {
        case AVMEDIA_TYPE_VIDEO:
            spc->video_stream_index = stream_index;
            spc->videoStream = fmt_ctx->streams[stream_index];
            decoder_init(&spc->video_decoder, codec_ctx, &spc->video_pq, spc->contine_read);
            ret = decoder_start(&spc->video_decoder, video_decoder_thread, "video_decoder_thread", spc);
            break;
        case AVMEDIA_TYPE_AUDIO:
            nb_channels = codec_ctx->channels;
            sample_rate = codec_ctx->sample_rate;
            channel_layout = codec_ctx->channel_layout;
            ret = audio_open(spc, nb_channels, sample_rate, channel_layout, &spc->audio_params_dst);
            if (ret < 0) {
                goto fail;
            }
            spc->audio_hw_buffer_size = ret;
            //            spc->audio_params_src = spc->audio_params_dst;
            spc->audio_buffer_size = spc->audio_remain_buffer_size = spc->audio_buffer_index = 0;

            spc->audio_stream_index = stream_index;
            spc->audioStream = fmt_ctx->streams[stream_index];
            decoder_init(&spc->audio_decoder, codec_ctx, &spc->audio_pq, spc->contine_read);
            ret = decoder_start(&spc->audio_decoder, audio_decoder_thread, "audio_decoder_thread", spc);
            SDL_PauseAudioDevice(audio_dev_id, 0);
            break;
        default: break;
    }
    goto success;
fail:
    if (codec_ctx) {
        avcodec_free_context(&codec_ctx);
    }
success:

    return ret;
}

int stream_has_enough_packets(PacketQueue* pq) { return pq->abort_queue || pq->nb_pkts >= 25; }

void step_to_next_frame(SPlayerContext* spc) {
    if (spc->paused)
        toggle_pause_inner(spc);
    spc->step = 1;
}

int read_thread(void* arg) {
    int ret;
    SPlayerContext* spc = arg;
    spc->eof = 0;
    AVFormatContext* fmt_ctx = avformat_alloc_context();
    AVPacket* pkt = av_packet_alloc();
    av_init_packet(pkt);
    int st_index[AVMEDIA_TYPE_NB];
    memset(st_index, -1, sizeof(st_index));
    spc->audio_stream_index = spc->video_stream_index = -1;
    SDL_mutex* wait_mutex = SDL_CreateMutex();
    ret = avformat_open_input(&fmt_ctx, spc->file_name, NULL, NULL);
    if (ret < 0) {
        goto fail;
    }
    ret = avformat_find_stream_info(fmt_ctx, NULL);
    if (ret < 0) {
        goto fail;
    }
    spc->fmt_ctx = fmt_ctx;
    st_index[AVMEDIA_TYPE_VIDEO] = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    st_index[AVMEDIA_TYPE_AUDIO] = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);

    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
        AVCodecParameters* codec_params = fmt_ctx->streams[st_index[AVMEDIA_TYPE_VIDEO]]->codecpar;
        window_width = codec_params->width;
        window_height = codec_params->height;
    }

    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
        ret = stream_component_open(spc, st_index[AVMEDIA_TYPE_VIDEO]);
    }
    if (st_index[AVMEDIA_TYPE_AUDIO] >= 0) {
        ret = stream_component_open(spc, st_index[AVMEDIA_TYPE_AUDIO]);
    }
    for (;;) {
        if (spc->abort_read) {
            break;
        }
        if (spc->seek_req) {
            int64_t seek_min = INT64_MIN;
            int64_t seek_max = INT64_MAX;
            if (spc->seek_incr > 0) {
                seek_min = spc->seek_pos - spc->seek_incr + 2;
            } else {
                seek_max = spc->seek_pos - spc->seek_incr - 2;
            }
            ret = avformat_seek_file(spc->fmt_ctx, -1, seek_min, spc->seek_pos, seek_max, spc->seek_flags);
            if (ret >= 0) {
                if (st_index[AVMEDIA_TYPE_AUDIO] >= 0) {
                    packet_queue_flush(&spc->audio_pq);
                    packet_queue_put(&spc->audio_pq, &flush_pkt);
                }
                if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
                    packet_queue_flush(&spc->video_pq);
                    packet_queue_put(&spc->video_pq, &flush_pkt);
                }
            }
            spc->seek_req = 0;
            if (spc->paused) {
                step_to_next_frame(spc);
            }
        }
        if (stream_has_enough_packets(&spc->audio_pq) && stream_has_enough_packets(&spc->video_pq)) {
            SDL_LockMutex(wait_mutex);
            SDL_CondWaitTimeout(spc->contine_read, wait_mutex, 10);
            SDL_UnlockMutex(wait_mutex);
            continue;
        }
        ret = av_read_frame(fmt_ctx, pkt);
        if (ret < 0) {
            if (ret == AVERROR_EOF && !spc->eof) {
                if (spc->video_stream_index >= 0) {
                    packet_queue_put_nullpkt(&spc->video_pq);
                }
                if (spc->audio_stream_index >= 0) {
                    packet_queue_put_nullpkt(&spc->audio_pq);
                }
                spc->eof = 1;
            }
            continue;
        } else {
            spc->eof = 0;
        }
        if (pkt->stream_index == spc->video_stream_index) {
            packet_queue_put(&spc->video_pq, pkt);
        } else if (pkt->stream_index == spc->audio_stream_index) {
            packet_queue_put(&spc->audio_pq, pkt);
        } else {
            av_packet_unref(pkt);
        }
    }
fail:
    if (fmt_ctx && !spc->fmt_ctx) {
        avformat_close_input(&fmt_ctx);
    }
    SDL_DestroyMutex(wait_mutex);
    return ret;
}

void frame_queue_signal(FrameQueue* fq) {
    SDL_LockMutex(fq->mutex);
    SDL_CondSignal(fq->cond);
    SDL_UnlockMutex(fq->mutex);
}

void packet_queue_abort(PacketQueue* pq) {
    SDL_LockMutex(pq->mutex);
    pq->abort_queue = 1;
    SDL_CondSignal(pq->cond);
    SDL_UnlockMutex(pq->mutex);
}

void decoder_abort(Decoder* decoder, FrameQueue* fq) {
    packet_queue_abort(decoder->pkt_queue);
    frame_queue_signal(fq);
    SDL_WaitThread(decoder->decoder_tid, NULL);
    decoder->decoder_tid = NULL;
    packet_queue_flush(decoder->pkt_queue);
}

void decoder_destroy(Decoder* decoder) {
    av_packet_unref(decoder->packet);
    avcodec_free_context(&decoder->codec_cxt);
}

void stream_component_close(SPlayerContext* spc, int stream_index) {
    AVCodecParameters* codec_params;
    AVFormatContext* fmt_ctx;
    fmt_ctx = spc->fmt_ctx;
    AVStream* streams = fmt_ctx->streams[stream_index];
    codec_params = streams->codecpar;
    switch (codec_params->codec_type) {
        case AVMEDIA_TYPE_VIDEO:
            decoder_abort(&spc->video_decoder, &spc->video_fq);
            decoder_destroy(&spc->video_decoder);
            break;
        case AVMEDIA_TYPE_AUDIO:
            decoder_abort(&spc->audio_decoder, &spc->audio_fq);
            decoder_destroy(&spc->audio_decoder);
            SDL_CloseAudioDevice(audio_dev_id);
            swr_free(&spc->swr_ctx);
            av_freep(&spc->audio_buffer);
            break;
        default: break;
    }
}

void video_close(SPlayerContext* spc) {
    spc->abort_read = 1;
    SDL_WaitThread(spc->read_tid, NULL);
    if (spc->video_stream_index >= 0) {
        stream_component_close(spc, spc->video_stream_index);
    }
    if (spc->audio_stream_index >= 0) {
        stream_component_close(spc, spc->audio_stream_index);
    }
    avformat_close_input(&spc->fmt_ctx);
    packet_queue_destroy(&spc->video_pq);
    packet_queue_destroy(&spc->audio_pq);
    frame_queue_destroy(&spc->video_fq);
    frame_queue_destroy(&spc->audio_fq);
    SDL_DestroyCond(spc->contine_read);
    av_freep(&spc);
}

void do_exit(SPlayerContext* spc) {
    SDL_CloseAudioDevice(audio_dev_id);
    video_close(spc);
    if (renderer) {
        SDL_DestroyRenderer(renderer);
    }
    if (window) {
        SDL_DestroyWindow(window);
    }
    SDL_Quit();
    exit(0);
}

SPlayerContext* video_open(const char* file_name) {
    SPlayerContext* spc;
    spc = av_mallocz(sizeof(SPlayerContext));
    spc->file_name = file_name;

    frame_queue_init(&spc->audio_fq, &spc->audio_pq);
    frame_queue_init(&spc->video_fq, &spc->video_pq);

    packet_queue_init(&spc->audio_pq);
    packet_queue_init(&spc->video_pq);

    spc->contine_read = SDL_CreateCond();

    init_clock(&spc->audio_clock, &spc->audio_pq.serial);
    init_clock(&spc->video_clock, &spc->video_pq.serial);

    spc->read_tid = SDL_CreateThread(read_thread, "read_thread", spc);

    if (!spc->read_tid) {
        av_log(NULL, AV_LOG_ERROR, "create read thread failed:%s.\n", SDL_GetError());
        video_close(spc);
        return NULL;
    }
    return spc;
}

double frame_duration(SPlayerContext* spc, Frame* last_frame, Frame* frame) {
    double duration = frame->pts - last_frame->pts;
    if (isnan(duration) || duration <= 0) {
        duration = last_frame->duration;
    }
    return duration;
}

double compute_target_delay(double delay, SPlayerContext* spc) {
    double diff = get_clock(&spc->video_clock) - get_clock(&spc->audio_clock);
    double sync_thres = FFMAX(AV_SYNC_THRESHOLD_MIN, FFMIN(AV_SYNC_THRESHOLD_MAX, delay));
    if (diff <= -sync_thres) {
        delay = FFMAX(0, delay + diff);
    } else if (diff >= sync_thres && delay > AV_SYNC_FRAMEDUP_THRESHOLD) {
        delay = delay + diff;
    } else if (diff >= sync_thres) {
        delay = 2 * delay;
    }
    return delay;
}

const struct TextureFormatEntry {
    enum AVPixelFormat format;
    int texture_fmt;
} sdl_texture_format_map[] = {
    {AV_PIX_FMT_RGB8, SDL_PIXELFORMAT_RGB332},
    {AV_PIX_FMT_RGB444, SDL_PIXELFORMAT_RGB444},
    {AV_PIX_FMT_RGB555, SDL_PIXELFORMAT_RGB555},
    {AV_PIX_FMT_BGR555, SDL_PIXELFORMAT_BGR555},
    {AV_PIX_FMT_RGB565, SDL_PIXELFORMAT_RGB565},
    {AV_PIX_FMT_BGR565, SDL_PIXELFORMAT_BGR565},
    {AV_PIX_FMT_RGB24, SDL_PIXELFORMAT_RGB24},
    {AV_PIX_FMT_BGR24, SDL_PIXELFORMAT_BGR24},
    {AV_PIX_FMT_0RGB32, SDL_PIXELFORMAT_RGB888},
    {AV_PIX_FMT_0BGR32, SDL_PIXELFORMAT_BGR888},
    {AV_PIX_FMT_NE(RGB0, 0BGR), SDL_PIXELFORMAT_RGBX8888},
    {AV_PIX_FMT_NE(BGR0, 0RGB), SDL_PIXELFORMAT_BGRX8888},
    {AV_PIX_FMT_RGB32, SDL_PIXELFORMAT_ARGB8888},
    {AV_PIX_FMT_RGB32_1, SDL_PIXELFORMAT_RGBA8888},
    {AV_PIX_FMT_BGR32, SDL_PIXELFORMAT_ABGR8888},
    {AV_PIX_FMT_BGR32_1, SDL_PIXELFORMAT_BGRA8888},
    {AV_PIX_FMT_YUV420P, SDL_PIXELFORMAT_IYUV},
    {AV_PIX_FMT_YUYV422, SDL_PIXELFORMAT_YUY2},
    {AV_PIX_FMT_UYVY422, SDL_PIXELFORMAT_UYVY},
    {AV_PIX_FMT_NONE, SDL_PIXELFORMAT_UNKNOWN},
};

void get_sdl_pixfmt_and_blendmode(int fmt, Uint32* sdl_pix_fmt, SDL_BlendMode* sdl_blendmode) {
    int i;
    *sdl_blendmode = SDL_BLENDMODE_NONE;
    *sdl_pix_fmt = SDL_PIXELFORMAT_UNKNOWN;
    if (fmt == AV_PIX_FMT_RGB32 || fmt == AV_PIX_FMT_RGB32_1 || fmt == AV_PIX_FMT_BGR32 || fmt == AV_PIX_FMT_BGR32_1) {
        *sdl_blendmode = SDL_BLENDMODE_BLEND;
    }
    for (i = 0; i < FF_ARRAY_ELEMS(sdl_texture_format_map) - 1; i++) {
        if (fmt == sdl_texture_format_map[i].format) {
            *sdl_pix_fmt = sdl_texture_format_map[i].texture_fmt;
            return;
        }
    }
}

int realloc_texture(SDL_Texture** texture, Uint32 new_format, int new_width, int new_height, SDL_BlendMode blendmode,
                    int init_texture) {
    Uint32 format;
    int access, w, h;
    if (!*texture || SDL_QueryTexture(*texture, &format, &access, &w, &h) < 0 || new_width != w || new_height != h ||
        new_format != format) {
        void* pixels;
        int pitch;
        if (*texture) {
            SDL_DestroyTexture(*texture);
        }

        if (!(*texture = SDL_CreateTexture(renderer, new_format, SDL_TEXTUREACCESS_STREAMING, new_width, new_height)))
            return -1;
        if (SDL_SetTextureBlendMode(*texture, blendmode) < 0)
            return -1;
        if (init_texture) {
            if (SDL_LockTexture(*texture, NULL, &pixels, &pitch) < 0)
                return -1;
            memset(pixels, 0, pitch * new_height);
            SDL_UnlockTexture(*texture);
        }
        av_log(NULL, AV_LOG_VERBOSE, "Created %dx%d texture with %s.\n", new_width, new_height,
               SDL_GetPixelFormatName(new_format));
    }
    return 0;
}

int upload_texture(SDL_Texture** texture, AVFrame* frame) {
    int ret = -1;
    Uint32 sdl_pix_fmt;
    SDL_BlendMode sdl_blendmode;
    get_sdl_pixfmt_and_blendmode(frame->format, &sdl_pix_fmt, &sdl_blendmode);
    ret = realloc_texture(texture, sdl_pix_fmt, frame->width, frame->height, sdl_blendmode, 0);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "realloc_texture ret < 0 %d.\n", ret);
        return -1;
    }
    switch (sdl_pix_fmt) {
        case SDL_PIXELFORMAT_IYUV:
            if (frame->linesize[0] > 0 && frame->linesize[1] > 0 && frame->linesize[2] > 0) {
                ret = SDL_UpdateYUVTexture(*texture, NULL, frame->data[0], frame->linesize[0], frame->data[1],
                                           frame->linesize[1], frame->data[2], frame->linesize[2]);
            } else if (frame->linesize[0] < 0 && frame->linesize[1] < 0 && frame->linesize[2] < 0) {
                ret = SDL_UpdateYUVTexture(
                    *texture, NULL, frame->data[0] + frame->linesize[0] * (frame->height - 1), -frame->linesize[0],
                    frame->data[1] + frame->linesize[1] * (AV_CEIL_RSHIFT(frame->height, 1) - 1), -frame->linesize[1],
                    frame->data[2] + frame->linesize[2] * (AV_CEIL_RSHIFT(frame->height, 1) - 1), -frame->linesize[2]);
            } else {
                av_log(NULL, AV_LOG_ERROR, "Mixed negative and positive linesizes are not supported.\n");
                return -1;
            }
            break;
        default: break;
    }
    return ret;
}

void set_sdl_yuv_conversion_mode(AVFrame* frame) {
#if SDL_VERSION_ATLEAST(2, 0, 8)
    SDL_YUV_CONVERSION_MODE mode = SDL_YUV_CONVERSION_AUTOMATIC;
    if (frame && (frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUYV422 ||
                  frame->format == AV_PIX_FMT_UYVY422)) {
        if (frame->color_range == AVCOL_RANGE_JPEG)
            mode = SDL_YUV_CONVERSION_JPEG;
        else if (frame->colorspace == AVCOL_SPC_BT709)
            mode = SDL_YUV_CONVERSION_BT709;
        else if (frame->colorspace == AVCOL_SPC_BT470BG || frame->colorspace == AVCOL_SPC_SMPTE170M ||
                 frame->colorspace == AVCOL_SPC_SMPTE240M)
            mode = SDL_YUV_CONVERSION_BT601;
    }
    SDL_SetYUVConversionMode(mode);
#endif
}

int window_open(SPlayerContext* spc) {
    SDL_SetWindowSize(window, window_width, window_height);
    SDL_SetWindowPosition(window, window_left, window_top);
    //    if (is_full_screen)
    //        SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
    SDL_ShowWindow(window);
    spc->width = window_width;
    return 0;
}

void SaveAVFrame(AVFrame* avFrame) {
    FILE* fDump = fopen("frame.yuv", "ab");

    uint32_t pitchY = avFrame->linesize[0];
    uint32_t pitchU = avFrame->linesize[1];
    uint32_t pitchV = avFrame->linesize[2];

    uint8_t* avY = avFrame->data[0];
    uint8_t* avU = avFrame->data[1];
    uint8_t* avV = avFrame->data[2];

    for (uint32_t i = 0; i < avFrame->height; i++) {
        fwrite(avY, avFrame->width, 1, fDump);
        avY += pitchY;
    }

    for (uint32_t i = 0; i < avFrame->height / 2; i++) {
        fwrite(avU, avFrame->width / 2, 1, fDump);
        avU += pitchU;
    }

    for (uint32_t i = 0; i < avFrame->height / 2; i++) {
        fwrite(avV, avFrame->width / 2, 1, fDump);
        avV += pitchV;
    }

    fclose(fDump);
}

int s = 1;

void video_display(SPlayerContext* spc) {
    int ret;
    if (!spc->width) {
        window_open(spc);
    }
    FrameQueue* frame_queue = &spc->video_fq;
    Frame* frame = frame_queue_peek_last(frame_queue);
    SDL_Rect rect;
    rect.x = rect.y = 0;
    rect.w = frame->width;
    rect.h = frame->height;
    if (s) {
        SaveAVFrame(frame->av_frame);
        s = 0;
    }
    ret = upload_texture(&spc->video_texture, frame->av_frame);
    if (ret < 0) {
        return;
    }
    ret = SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255);
    ret = SDL_RenderClear(renderer);
    set_sdl_yuv_conversion_mode(frame->av_frame);
    ret = SDL_RenderCopyEx(renderer, spc->video_texture, NULL, &rect, 0, NULL, 0);
    set_sdl_yuv_conversion_mode(NULL);
    SDL_RenderPresent(renderer);
}

void video_refresh(SPlayerContext* spc) {
    double time;
    Frame *last_frame, *frame;
    double last_duration, duration, delay;

    do {
        if (frame_queue_nb_remaining(&spc->video_fq) == 0) {
            return;
        }
        last_frame = frame_queue_peek_last(&spc->video_fq);
        frame = frame_queue_peek(&spc->video_fq);
        if (frame->serial != spc->video_pq.serial) {
            frame_queue_next(&spc->video_fq);
        }
    } while (frame->serial != spc->video_pq.serial);

    if (spc->paused) {
        goto display;
    }

    last_duration = frame_duration(spc, last_frame, frame);
    delay = compute_target_delay(last_duration, spc);
    time = av_gettime_relative() / 1000000.0;

    if (time < spc->frame_start_time + delay) {
        goto display;
    }
    spc->frame_start_time += delay;
    if (time - spc->frame_start_time >= AV_SYNC_THRESHOLD_MAX) {
        spc->frame_start_time = time;
    }
    if (!isnan(frame->pts)) {
        set_clock(&spc->video_clock, frame->pts, frame->serial);
    }
    frame_queue_next(&spc->video_fq);
    if (spc->step && !spc->paused) {
        toggle_pause_inner(spc);
    }
display:
    video_display(spc);
}

void refresh_loop_wait_event(SPlayerContext* spc, SDL_Event* event) {
    SDL_PumpEvents();
    while (!SDL_PeepEvents(event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT)) {
        video_refresh(spc);
        SDL_PumpEvents();
    }
}

void toggle_pause_inner(SPlayerContext* spc) {
    if (spc->paused) {
        spc->frame_start_time += av_gettime_relative() / 1000000.0 - spc->video_clock.last_updated;
        set_clock(&spc->video_clock, get_clock(&spc->video_clock), spc->video_clock.serial);
    }
    spc->paused = spc->audio_clock.paused = spc->video_clock.paused = !spc->paused;
}

void toggle_pause(SPlayerContext* spc) {
    toggle_pause_inner(spc);
    spc->step = 0;
}

void stream_seek(SPlayerContext* spc, int64_t dst_pos, int64_t incr_pos) {
    if (!spc->seek_req) {
        spc->seek_incr = incr_pos;
        spc->seek_pos = dst_pos;
        spc->seek_req = 1;
        spc->seek_flags &= ~AVSEEK_FLAG_BYTE;
        SDL_CondSignal(spc->contine_read);
    }
}

void event_loop(SPlayerContext* spc) {
    SDL_Event event;
    double incr, cur_pts;
    for (;;) {
        refresh_loop_wait_event(spc, &event);
        switch (event.type) {
            case SDL_KEYDOWN:
                switch (event.key.keysym.sym) {
                    case SDLK_SPACE: toggle_pause(spc); break;
                    case SDLK_LEFT:
                        incr = -seek_interval;
                        goto do_seek;
                        break;
                    case SDLK_RIGHT:
                        incr = seek_interval;
                    do_seek:
                        cur_pts = get_clock(&spc->audio_clock);
                        cur_pts += incr;
                        stream_seek(spc, cur_pts * AV_TIME_BASE, incr * AV_TIME_BASE);
                        break;
                    case SDLK_ESCAPE:
                    case SDLK_q: do_exit(spc); break;
                    default: break;
                }
                break;
            case SDL_QUIT: do_exit(spc); break;
            default: break;
        }
    }
}

#undef main

int main(int argc, char* argv[]) {
    if (argc != 2) {
        av_log(NULL, AV_LOG_ERROR, "arg error!\n");
        exit(1);
    }
#ifdef _WIN32
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
#endif
    file_name = argv[1];
    SPlayerContext* spc;

    int flags_init = SDL_INIT_AUDIO | SDL_INIT_VIDEO | SDL_INIT_TIMER;
    if (SDL_Init(flags_init)) {
        av_log(NULL, AV_LOG_ERROR, "init sdl failed:%s.\n", SDL_GetError());
        exit(1);
    }
    int flags_window = SDL_WINDOW_HIDDEN;
#if SDL_VERSION_ATLEAST(2, 0, 5)
    flags_window |= SDL_WINDOW_ALWAYS_ON_TOP;
#else
    av_log(NULL, AV_LOG_WARNING,
           "Your SDL version doesn't support SDL_WINDOW_ALWAYS_ON_TOP. Feature will be inactive.\n");
#endif
    window = SDL_CreateWindow("SPlayer", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, window_width, window_height,
                              flags_window);
    if (window) {
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!renderer) {
            av_log(NULL, AV_LOG_WARNING, "Failed to initialize a hardware accelerated renderer: %s.\n", SDL_GetError());
            renderer = SDL_CreateRenderer(window, -1, 0);
        }
        if (renderer) {
            if (!SDL_GetRendererInfo(renderer, &renderer_info))
                av_log(NULL, AV_LOG_VERBOSE, "Initialized %s renderer.\n", renderer_info.name);
        }
    }
    if (!window || !renderer) {
        av_log(NULL, AV_LOG_ERROR, "create window or renderer failed:%s.\n", SDL_GetError());
        if (!renderer) {
            SDL_DestroyWindow(window);
        }
        SDL_Quit();
        exit(1);
    }

    av_init_packet(&flush_pkt);
    flush_pkt.data = (uint8_t*)&flush_pkt;

    spc = video_open(file_name);

    event_loop(spc);

    av_log(NULL, AV_LOG_INFO, "----end----.\n");
    return 0;
}

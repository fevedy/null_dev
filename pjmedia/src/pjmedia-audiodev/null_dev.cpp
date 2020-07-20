/* $Id: alsa_dev.c 5965 2019-04-10 06:32:27Z nanang $ */
/*
 * Copyright (C) 2009-2011 Teluu Inc. (http://www.teluu.com)
 * Copyright (C) 2007-2009 Keystream AB and Konftel AB, All rights reserved.
 *                         Author: <dan.aberg@keystream.se>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#include <pjmedia_audiodev.h>
#include <pj/assert.h>
#include <pj/log.h>
#include <pj/os.h>
#include <pj/pool.h>
#include <pjmedia/errno.h>

#if PJMEDIA_AUDIO_DEV_HAS_NULL_AUDIO


#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/select.h>
#include <pthread.h>
#include <errno.h>
#include <alsa/asoundlib.h>


#define THIS_FILE 			"null_dev.cpp"
#define ALSA_DEVICE_NAME 		"plughw:%d,%d"
#define ALSASOUND_PLAYBACK 		1
#define ALSASOUND_CAPTURE  		2
#define MAX_SOUND_CARDS 		5
#define MAX_SOUND_DEVICES_PER_CARD 	5
#define MAX_DEVICES			32
#define MAX_MIX_NAME_LEN                64 

/* Set to 1 to enable tracing */
#define ENABLE_TRACING			0

#if ENABLE_TRACING
#	define TRACE_(expr)		PJ_LOG(5,expr)
#else
#	define TRACE_(expr)
#endif

/*
 * Factory prototypes
 */
static pj_status_t null_factory_init(pjmedia_aud_dev_factory *f);
static pj_status_t null_factory_destroy(pjmedia_aud_dev_factory *f);
static pj_status_t null_factory_refresh(pjmedia_aud_dev_factory *f);
static unsigned    null_factory_get_dev_count(pjmedia_aud_dev_factory *f);
static pj_status_t null_factory_get_dev_info(pjmedia_aud_dev_factory *f,
					     unsigned index,
					     pjmedia_aud_dev_info *info);
static pj_status_t null_factory_default_param(pjmedia_aud_dev_factory *f,
					      unsigned index,
					      pjmedia_aud_param *param);
static pj_status_t null_factory_create_stream(pjmedia_aud_dev_factory *f,
					      const pjmedia_aud_param *param,
					      pjmedia_aud_rec_cb rec_cb,
					      pjmedia_aud_play_cb play_cb,
					      void *user_data,
					      pjmedia_aud_stream **p_strm);

/*
 * Stream prototypes
 */
static pj_status_t null_stream_get_param(pjmedia_aud_stream *strm,
					 pjmedia_aud_param *param);
static pj_status_t null_stream_get_cap(pjmedia_aud_stream *strm,
				       pjmedia_aud_dev_cap cap,
				       void *value);
static pj_status_t null_stream_set_cap(pjmedia_aud_stream *strm,
				       pjmedia_aud_dev_cap cap,
				       const void *value);
static pj_status_t null_stream_start(pjmedia_aud_stream *strm);
static pj_status_t null_stream_stop(pjmedia_aud_stream *strm);
static pj_status_t null_stream_destroy(pjmedia_aud_stream *strm);


struct null_audio_dev_info
{
    pjmedia_aud_dev_info	 info;
    unsigned			 dev_id;
};

struct null_factory
{
    pjmedia_aud_dev_factory	 base;
    pj_pool_factory		*pf;
    pj_pool_t			*pool;
    pj_pool_t			*base_pool;

    unsigned			 dev_cnt;
    pjmedia_aud_dev_info	 devs[MAX_DEVICES];
    char                         pb_mixer_name[MAX_MIX_NAME_LEN];
};

struct null_stream
{
    pjmedia_aud_stream	 base;

    /* Common */
    pj_pool_t		*pool;
    struct null_factory *af;
    void		*user_data;
    pjmedia_aud_param	 param;		/* Running parameter 		*/
    int                  rec_id;      	/* Capture device id		*/
    int                  quit;

    /* Playback */
    snd_pcm_t		*pb_pcm;
    snd_pcm_uframes_t    pb_frames; 	/* samples_per_frame		*/
    pjmedia_aud_play_cb  pb_cb;
    unsigned             pb_buf_size;
    char		*pb_buf;
    pj_thread_t		*pb_thread;

    /* Capture */
    snd_pcm_t		*ca_pcm;
    snd_pcm_uframes_t    ca_frames; 	/* samples_per_frame		*/
    pjmedia_aud_rec_cb   ca_cb;
    unsigned             ca_buf_size;
    char		*ca_buf;
    pj_thread_t		*ca_thread;
};

static pjmedia_aud_dev_factory_op null_factory_op =
{
    &null_factory_init,
    &null_factory_destroy,
    &null_factory_get_dev_count,
    &null_factory_get_dev_info,
    &null_factory_default_param,
    &null_factory_create_stream,
    &null_factory_refresh
};

static pjmedia_aud_stream_op null_stream_op =
{
    &null_stream_get_param,
    &null_stream_get_cap,
    &null_stream_set_cap,
    &null_stream_start,
    &null_stream_stop,
    &null_stream_destroy
};

#if ENABLE_TRACING==0
static void null_null_error_handler (const char *file,
				int line,
				const char *function,
				int err,
				const char *fmt,
				...)
{
    PJ_UNUSED_ARG(file);
    PJ_UNUSED_ARG(line);
    PJ_UNUSED_ARG(function);
    PJ_UNUSED_ARG(err);
    PJ_UNUSED_ARG(fmt);
}
#endif

static void null_error_handler (const char *file,
				int line,
				const char *function,
				int err,
				const char *fmt,
				...)
{
    char err_msg[128];
    int index, len;
    va_list arg;

#ifndef NDEBUG
    index = snprintf (err_msg, sizeof(err_msg), "null lib %s:%i:(%s) ",
		      file, line, function);
#else
    index = snprintf (err_msg, sizeof(err_msg), "null lib: ");
#endif
    if (index < 1 || index >= (int)sizeof(err_msg)) {
	index = sizeof(err_msg)-1;
	err_msg[index] = '\0';
	goto print_msg;
    }

    va_start (arg, fmt);
    if (index < sizeof(err_msg)-1) {
	len = vsnprintf( err_msg+index, sizeof(err_msg)-index, fmt, arg);
	if (len < 1 || len >= (int)sizeof(err_msg)-index)
	    len = sizeof(err_msg)-index-1;
	index += len;
	err_msg[index] = '\0';
    }
    va_end(arg);
    if (err && index < sizeof(err_msg)-1) {
	len = snprintf( err_msg+index, sizeof(err_msg)-index, ": %s",
			snd_strerror(err));
	if (len < 1 || len >= (int)sizeof(err_msg)-index)
	    len = sizeof(err_msg)-index-1;
	index += len;
	err_msg[index] = '\0';
    }
print_msg:
    PJ_LOG (4,(THIS_FILE, "%s", err_msg));
}


static pj_status_t add_dev (struct null_factory *af, const char *dev_name)
{
    pjmedia_aud_dev_info *adi;
    snd_pcm_t* pcm;
    int pb_result, ca_result;

    if (af->dev_cnt >= PJ_ARRAY_SIZE(af->devs))
	return PJ_ETOOMANY;

    adi = &af->devs[af->dev_cnt];

    /* Reset device info */
    pj_bzero(adi, sizeof(*adi));

    /* Set device name */
    strncpy(adi->name, dev_name, sizeof(adi->name));

    /* Check the number of playback channels */
    adi->output_count = 1;

    /* Check the number of capture channels */
    adi->input_count = 1;

    /* Set the default sample rate */
    adi->default_samples_per_sec = 16000;

    /* Driver name */
    strcpy(adi->driver, "null");

    ++af->dev_cnt;

    PJ_LOG (5,(THIS_FILE, "Added sound device %s", adi->name));

    return PJ_SUCCESS;
}

static void get_mixer_name(struct null_factory *af)
{
    snd_mixer_t *handle;
    snd_mixer_elem_t *elem;

    if (snd_mixer_open(&handle, 0) < 0)
	return;

    if (snd_mixer_attach(handle, "default") < 0) {
	snd_mixer_close(handle);
	return;
    }

    if (snd_mixer_selem_register(handle, NULL, NULL) < 0) {
	snd_mixer_close(handle);
	return;
    }

    if (snd_mixer_load(handle) < 0) {
	snd_mixer_close(handle);
	return;
    }

    for (elem = snd_mixer_first_elem(handle); elem;
	 elem = snd_mixer_elem_next(elem))
    {
	if (snd_mixer_selem_is_active(elem) &&
	    snd_mixer_selem_has_playback_volume(elem))
	{
	    pj_ansi_strncpy(af->pb_mixer_name, snd_mixer_selem_get_name(elem),
	    		    sizeof(af->pb_mixer_name));
	    TRACE_((THIS_FILE, "Playback mixer name: %s", af->pb_mixer_name));
	    break;
	}
    }
    snd_mixer_close(handle);
}


/* Create null audio driver. */
pjmedia_aud_dev_factory* pjmedia_null_audio_factory(pj_pool_factory *pf)
{
    struct null_factory *af;
    pj_pool_t *pool;

    pool = pj_pool_create(pf, "null audio", 1000, 1000, NULL);
    af = PJ_POOL_ZALLOC_T(pool, struct null_factory);
    af->pf = pf;
    af->base_pool = pool;
    af->base.op = &null_factory_op;

    return &af->base;
}

/* API: init factory */
static pj_status_t null_factory_init(pjmedia_aud_dev_factory *f)
{
    pj_status_t status = null_factory_refresh(f);
    if (PJ_SUCCESS != status)
	return status;

    PJ_LOG(4,(THIS_FILE, "null initialized"));
    return PJ_SUCCESS;
}


/* API: destroy factory */
static pj_status_t null_factory_destroy(pjmedia_aud_dev_factory *f)
{
    struct null_factory *af = (struct null_factory*)f;

    if (af->pool)
	pj_pool_release(af->pool);

    if (af->base_pool) {
	pj_pool_t *pool = af->base_pool;
	af->base_pool = NULL;
	pj_pool_release(pool);
    }

    /* Restore handler */
    snd_lib_error_set_handler(NULL);

    return PJ_SUCCESS;
}


/* API: refresh the device list */
static pj_status_t null_factory_refresh(pjmedia_aud_dev_factory *f)
{
    struct null_factory *af = (struct null_factory*)f;
    char **hints, **n;
    int err;

    TRACE_((THIS_FILE, "pjmedia_snd_init: Enumerate sound devices"));

    if (af->pool != NULL) {
	pj_pool_release(af->pool);
	af->pool = NULL;
    }

    af->pool = pj_pool_create(af->pf, "null_aud", 256, 256, NULL);
    af->dev_cnt = 0;

    /* Enumerate sound devices */
    add_dev(af, "null device");

    /* Get the mixer name */
    //get_mixer_name(af);

    PJ_LOG(4,(THIS_FILE, "null driver found %d devices", af->dev_cnt));

    return PJ_SUCCESS;
}


/* API: get device count */
static unsigned  null_factory_get_dev_count(pjmedia_aud_dev_factory *f)
{
    struct null_factory *af = (struct null_factory*)f;
    return af->dev_cnt;
}


/* API: get device info */
static pj_status_t null_factory_get_dev_info(pjmedia_aud_dev_factory *f,
					     unsigned index,
					     pjmedia_aud_dev_info *info)
{
    struct null_factory *af = (struct null_factory*)f;

    PJ_ASSERT_RETURN(index>=0 && index<af->dev_cnt, PJ_EINVAL);

    pj_memcpy(info, &af->devs[index], sizeof(*info));
    info->caps = PJMEDIA_AUD_DEV_CAP_INPUT_LATENCY |
		 PJMEDIA_AUD_DEV_CAP_OUTPUT_LATENCY;
    return PJ_SUCCESS;
}

/* API: create default parameter */
static pj_status_t null_factory_default_param(pjmedia_aud_dev_factory *f,
					      unsigned index,
					      pjmedia_aud_param *param)
{
    struct null_factory *af = (struct null_factory*)f;
    pjmedia_aud_dev_info *adi;

    PJ_ASSERT_RETURN(index>=0 && index<af->dev_cnt, PJ_EINVAL);

    adi = &af->devs[index];

    pj_bzero(param, sizeof(*param));
    if (adi->input_count && adi->output_count) {
	param->dir = PJMEDIA_DIR_CAPTURE_PLAYBACK;
	param->rec_id = index;
	param->play_id = index;
    } else if (adi->input_count) {
	param->dir = PJMEDIA_DIR_CAPTURE;
	param->rec_id = index;
	param->play_id = PJMEDIA_AUD_INVALID_DEV;
    } else if (adi->output_count) {
	param->dir = PJMEDIA_DIR_PLAYBACK;
	param->play_id = index;
	param->rec_id = PJMEDIA_AUD_INVALID_DEV;
    } else {
	return PJMEDIA_EAUD_INVDEV;
    }

    param->clock_rate = adi->default_samples_per_sec;
    param->channel_count = 1;
    param->samples_per_frame = adi->default_samples_per_sec * 20 / 1000;
    param->bits_per_sample = 16;
    param->flags = adi->caps;
    param->input_latency_ms = PJMEDIA_SND_DEFAULT_REC_LATENCY;
    param->output_latency_ms = PJMEDIA_SND_DEFAULT_PLAY_LATENCY;

    return PJ_SUCCESS;
}


static int pb_thread_func (void *arg)
{
    struct null_stream* stream = (struct null_stream*) arg;
    snd_pcm_t* pcm             = stream->pb_pcm;
    int size                   = stream->pb_buf_size;
    snd_pcm_uframes_t nframes  = stream->pb_frames;
    void* user_data            = stream->user_data;
    char* buf 		       = stream->pb_buf;
    pj_timestamp tstamp;
    int result;

    pj_bzero (buf, size);
    tstamp.u64 = 0;

    TRACE_((THIS_FILE, "pb_thread_func(%u): Started",
	    (unsigned)syscall(SYS_gettid)));

    while (!stream->quit) {
	pjmedia_frame frame;

	frame.type = PJMEDIA_FRAME_TYPE_AUDIO;
	frame.buf = buf;
	frame.size = size;
	frame.timestamp.u64 = tstamp.u64;
	frame.bit_info = 0;

	result = stream->pb_cb (user_data, &frame);//TODO:获取数据
	if (result != PJ_SUCCESS || stream->quit)
	    break;

	if (frame.type != PJMEDIA_FRAME_TYPE_AUDIO)
    {
        pj_bzero (buf, size);
    }   

	//result = snd_pcm_writei (pcm, buf, nframes);
    //播放数据

	tstamp.u64 += nframes;
    }

    //snd_pcm_drain (pcm);
    //TODO:这里是不是要做些关闭动作
    TRACE_((THIS_FILE, "pb_thread_func: Stopped"));
    return PJ_SUCCESS;
}



static int ca_thread_func (void *arg)
{
    struct null_stream* stream = (struct null_stream*) arg;
    snd_pcm_t* pcm             = stream->ca_pcm;
    int size                   = stream->ca_buf_size;
    snd_pcm_uframes_t nframes  = stream->ca_frames;
    void* user_data            = stream->user_data;
    char* buf 		       = stream->ca_buf;
    pj_timestamp tstamp;
    int result;
    struct sched_param param;
    pthread_t* thid;

    thid = (pthread_t*) pj_thread_get_os_handle (pj_thread_this());
    param.sched_priority = sched_get_priority_max (SCHED_RR);
    PJ_LOG (5,(THIS_FILE, "ca_thread_func(%u): Set thread priority "
		          "for audio capture thread.",
		          (unsigned)syscall(SYS_gettid)));
    result = pthread_setschedparam (*thid, SCHED_RR, &param);
    if (result) {
	if (result == EPERM)
	    PJ_LOG (5,(THIS_FILE, "Unable to increase thread priority, "
				  "root access needed."));
	else
	    PJ_LOG (5,(THIS_FILE, "Unable to increase thread priority, "
				  "error: %d",
				  result));
    }

    pj_bzero (buf, size);
    tstamp.u64 = 0;

    TRACE_((THIS_FILE, "ca_thread_func(%u): Started",
	    (unsigned)syscall(SYS_gettid)));

    while (!stream->quit) {
	pjmedia_frame frame;

	pj_bzero (buf, size);
    //result = snd_pcm_readi (pcm, buf, nframes);
    //TODO:读取一帧
	if (stream->quit)
	    break;

	frame.type = PJMEDIA_FRAME_TYPE_AUDIO;
	frame.buf = (void*) buf;
	frame.size = size;
	frame.timestamp.u64 = tstamp.u64;
	frame.bit_info = 0;

	result = stream->ca_cb (user_data, &frame);
	if (result != PJ_SUCCESS || stream->quit)
	    break;

	tstamp.u64 += nframes;
    }
    //snd_pcm_drain (pcm);
    //TODO:是不是要做一些关闭动作
    TRACE_((THIS_FILE, "ca_thread_func: Stopped"));

    return PJ_SUCCESS;
}


static pj_status_t open_playback (struct null_stream* stream,
			          const pjmedia_aud_param *param)
{
    snd_pcm_hw_params_t* params;
    snd_pcm_format_t format;
    int result;
    unsigned int rate;
    snd_pcm_uframes_t tmp_buf_size;
    snd_pcm_uframes_t tmp_period_size;

    if (param->play_id < 0 || param->play_id >= stream->af->dev_cnt)
	return PJMEDIA_EAUD_INVDEV;

    /* Open PCM for playback */
    PJ_LOG (5,(THIS_FILE, "open_playback: Open playback device '%s'",
	       stream->af->devs[param->play_id].name));

    /* Set format */
    switch (param->bits_per_sample) 
    {}

    /* Set clock rate */
    rate = param->clock_rate;
    TRACE_((THIS_FILE, "open_playback: set clock rate: %d", rate));

    /* Set period size to samples_per_frame frames. */
    stream->pb_frames = (snd_pcm_uframes_t) param->samples_per_frame /
					    param->channel_count;
    TRACE_((THIS_FILE, "open_playback: set period size: %d",
	    stream->pb_frames));
    tmp_period_size = stream->pb_frames;


    /* Set the sound device buffer size and latency */
    if (param->flags & PJMEDIA_AUD_DEV_CAP_OUTPUT_LATENCY)
	tmp_buf_size = (rate / 1000) * param->output_latency_ms;
    else
	tmp_buf_size = (rate / 1000) * PJMEDIA_SND_DEFAULT_PLAY_LATENCY;

    stream->param.output_latency_ms = tmp_buf_size / (rate / 1000);

    /* Set our buffer */
    stream->pb_buf_size = stream->pb_frames * param->channel_count *
			  (param->bits_per_sample/8);
    stream->pb_buf = (char*) pj_pool_alloc(stream->pool, stream->pb_buf_size);

    TRACE_((THIS_FILE, "open_playback: buffer size set to: %d",
	    (int)tmp_buf_size));
    TRACE_((THIS_FILE, "open_playback: playback_latency set to: %d ms",
	    (int)stream->param.output_latency_ms));

    PJ_LOG (5,(THIS_FILE, "Opened device null(%s) for playing, sample rate=%d"
	       ", ch=%d, bits=%d, period size=%d frames, latency=%d ms",
	       stream->af->devs[param->play_id].name,
	       rate, param->channel_count,
	       param->bits_per_sample, stream->pb_frames,
	       (int)stream->param.output_latency_ms));

    return PJ_SUCCESS;
}


static pj_status_t open_capture (struct null_stream* stream,
			         const pjmedia_aud_param *param)
{
    snd_pcm_hw_params_t* params;
    snd_pcm_format_t format;
    int result;
    unsigned int rate;
    snd_pcm_uframes_t tmp_buf_size;
    snd_pcm_uframes_t tmp_period_size;

    if (param->rec_id < 0 || param->rec_id >= stream->af->dev_cnt)
	return PJMEDIA_EAUD_INVDEV;

    /* Open PCM for capture */
    PJ_LOG (5,(THIS_FILE, "open_capture: Open capture device '%s'",
	       stream->af->devs[param->rec_id].name));
    result = snd_pcm_open (&stream->ca_pcm,
		            stream->af->devs[param->rec_id].name,
			   SND_PCM_STREAM_CAPTURE,
			   0);
    if (result < 0)
	return PJMEDIA_EAUD_SYSERR;

    /* Allocate a hardware parameters object. */
    snd_pcm_hw_params_alloca (&params);

    /* Fill it in with default values. */
    snd_pcm_hw_params_any (stream->ca_pcm, params);

    /* Set interleaved mode */
    snd_pcm_hw_params_set_access (stream->ca_pcm, params,
				  SND_PCM_ACCESS_RW_INTERLEAVED);

    /* Set format */
    switch (param->bits_per_sample) {
    case 8:
	TRACE_((THIS_FILE, "open_capture: set format SND_PCM_FORMAT_S8"));
	format = SND_PCM_FORMAT_S8;
	break;
    case 16:
	TRACE_((THIS_FILE, "open_capture: set format SND_PCM_FORMAT_S16_LE"));
	format = SND_PCM_FORMAT_S16_LE;
	break;
    case 24:
	TRACE_((THIS_FILE, "open_capture: set format SND_PCM_FORMAT_S24_LE"));
	format = SND_PCM_FORMAT_S24_LE;
	break;
    case 32:
	TRACE_((THIS_FILE, "open_capture: set format SND_PCM_FORMAT_S32_LE"));
	format = SND_PCM_FORMAT_S32_LE;
	break;
    default:
	TRACE_((THIS_FILE, "open_capture: set format SND_PCM_FORMAT_S16_LE"));
	format = SND_PCM_FORMAT_S16_LE;
	break;
    }
    snd_pcm_hw_params_set_format (stream->ca_pcm, params, format);

    /* Set number of channels */
    TRACE_((THIS_FILE, "open_capture: set channels: %d",
	    param->channel_count));
    snd_pcm_hw_params_set_channels (stream->ca_pcm, params,
				    param->channel_count);

    /* Set clock rate */
    rate = param->clock_rate;
    TRACE_((THIS_FILE, "open_capture: set clock rate: %d", rate));
    snd_pcm_hw_params_set_rate_near (stream->ca_pcm, params, &rate, NULL);
    TRACE_((THIS_FILE, "open_capture: clock rate set to: %d", rate));

    /* Set period size to samples_per_frame frames. */
    stream->ca_frames = (snd_pcm_uframes_t) param->samples_per_frame /
					    param->channel_count;
    TRACE_((THIS_FILE, "open_capture: set period size: %d",
	    stream->ca_frames));
    tmp_period_size = stream->ca_frames;
    snd_pcm_hw_params_set_period_size_near (stream->ca_pcm, params,
					    &tmp_period_size, NULL);
    TRACE_((THIS_FILE, "open_capture: period size set to: %d",
	    tmp_period_size));

    /* Set the sound device buffer size and latency */
    if (param->flags & PJMEDIA_AUD_DEV_CAP_INPUT_LATENCY)
	tmp_buf_size = (rate / 1000) * param->input_latency_ms;
    else
	tmp_buf_size = (rate / 1000) * PJMEDIA_SND_DEFAULT_REC_LATENCY;
    snd_pcm_hw_params_set_buffer_size_near (stream->ca_pcm, params,
					    &tmp_buf_size);
    stream->param.input_latency_ms = tmp_buf_size / (rate / 1000);

    /* Set our buffer */
    stream->ca_buf_size = stream->ca_frames * param->channel_count *
			  (param->bits_per_sample/8);
    stream->ca_buf = (char*) pj_pool_alloc (stream->pool, stream->ca_buf_size);

    TRACE_((THIS_FILE, "open_capture: buffer size set to: %d",
	    (int)tmp_buf_size));
    TRACE_((THIS_FILE, "open_capture: capture_latency set to: %d ms",
	    (int)stream->param.input_latency_ms));

    /* Activate the parameters */
    result = snd_pcm_hw_params (stream->ca_pcm, params);
    if (result < 0) {
	snd_pcm_close (stream->ca_pcm);
	return PJMEDIA_EAUD_SYSERR;
    }

    PJ_LOG (5,(THIS_FILE, "Opened device null(%s) for capture, sample rate=%d"
	       ", ch=%d, bits=%d, period size=%d frames, latency=%d ms",
	       stream->af->devs[param->rec_id].name,
	       rate, param->channel_count,
	       param->bits_per_sample, stream->ca_frames,
	       (int)stream->param.input_latency_ms));

    return PJ_SUCCESS;
}


/* API: create stream */
static pj_status_t null_factory_create_stream(pjmedia_aud_dev_factory *f,
					      const pjmedia_aud_param *param,
					      pjmedia_aud_rec_cb rec_cb,
					      pjmedia_aud_play_cb play_cb,
					      void *user_data,
					      pjmedia_aud_stream **p_strm)
{
    struct null_factory *af = (struct null_factory*)f;
    pj_status_t status;
    pj_pool_t* pool;
    struct null_stream* stream;

    pool = pj_pool_create(af->pf, "null_audio-dev", 1024, 1024, NULL);
    if (!pool)
	return PJ_ENOMEM;

    /* Allocate and initialize comon stream data */
    stream = PJ_POOL_ZALLOC_T (pool, struct null_stream);
    stream->base.op = &null_stream_op;
    stream->pool      = pool;
    stream->af 	      = af;
    stream->user_data = user_data;
    stream->pb_cb     = play_cb;
    stream->ca_cb     = rec_cb;
    stream->quit      = 0;
    pj_memcpy(&stream->param, param, sizeof(*param));

    /* Init playback */
    if (param->dir & PJMEDIA_DIR_PLAYBACK) {
	status = open_playback (stream, param);
	if (status != PJ_SUCCESS) {
	    pj_pool_release (pool);
	    return status;
	}
    }
    
#if 0
    /* Init capture */
    if (param->dir & PJMEDIA_DIR_CAPTURE) {
	status = open_capture (stream, param);
	if (status != PJ_SUCCESS) {
	    if (param->dir & PJMEDIA_DIR_PLAYBACK)
		snd_pcm_close (stream->pb_pcm);
	    pj_pool_release (pool);
	    return status;
	}
    }
#endif

    *p_strm = &stream->base;
    return PJ_SUCCESS;
}


/* API: get running parameter */
static pj_status_t null_stream_get_param(pjmedia_aud_stream *s,
					 pjmedia_aud_param *pi)
{
    struct null_stream *stream = (struct null_stream*)s;

    PJ_ASSERT_RETURN(s && pi, PJ_EINVAL);

    pj_memcpy(pi, &stream->param, sizeof(*pi));

    return PJ_SUCCESS;
}


/* API: get capability */
static pj_status_t null_stream_get_cap(pjmedia_aud_stream *s,
				       pjmedia_aud_dev_cap cap,
				       void *pval)
{
    struct null_stream *stream = (struct null_stream*)s;

    PJ_ASSERT_RETURN(s && pval, PJ_EINVAL);

    if (cap==PJMEDIA_AUD_DEV_CAP_INPUT_LATENCY &&
	(stream->param.dir & PJMEDIA_DIR_CAPTURE))
    {
	/* Recording latency */
	*(unsigned*)pval = stream->param.input_latency_ms;
	return PJ_SUCCESS;
    } else if (cap==PJMEDIA_AUD_DEV_CAP_OUTPUT_LATENCY &&
	       (stream->param.dir & PJMEDIA_DIR_PLAYBACK))
    {
	/* Playback latency */
	*(unsigned*)pval = stream->param.output_latency_ms;
	return PJ_SUCCESS;
    } else {
	return PJMEDIA_EAUD_INVCAP;
    }
}


/* API: set capability */
static pj_status_t null_stream_set_cap(pjmedia_aud_stream *strm,
				       pjmedia_aud_dev_cap cap,
				       const void *value)
{
    struct null_factory *af = ((struct null_stream*)strm)->af;

    if (cap==PJMEDIA_AUD_DEV_CAP_OUTPUT_VOLUME_SETTING && 
	pj_ansi_strlen(af->pb_mixer_name)) 
    {
	pj_ssize_t min, max;
	snd_mixer_t *handle;
	snd_mixer_selem_id_t *sid;
	snd_mixer_elem_t* elem;
	unsigned vol = *(unsigned*)value;

	if (snd_mixer_open(&handle, 0) < 0)
	    return PJMEDIA_EAUD_SYSERR;

	if (snd_mixer_attach(handle, "default") < 0)
	    return PJMEDIA_EAUD_SYSERR;

	if (snd_mixer_selem_register(handle, NULL, NULL) < 0)
	    return PJMEDIA_EAUD_SYSERR;

	if (snd_mixer_load(handle) < 0)
	    return PJMEDIA_EAUD_SYSERR;

	snd_mixer_selem_id_alloca(&sid);
	snd_mixer_selem_id_set_index(sid, 0);
	snd_mixer_selem_id_set_name(sid, af->pb_mixer_name);
	elem = snd_mixer_find_selem(handle, sid);
	if (!elem)
	    return PJMEDIA_EAUD_SYSERR;

	snd_mixer_selem_get_playback_volume_range(elem, &min, &max);
	if (snd_mixer_selem_set_playback_volume_all(elem, vol * max / 100) < 0)
	    return PJMEDIA_EAUD_SYSERR;

	snd_mixer_close(handle);
	return PJ_SUCCESS;
    }

    return PJMEDIA_EAUD_INVCAP;
}


/* API: start stream */
static pj_status_t null_stream_start (pjmedia_aud_stream *s)
{
    struct null_stream *stream = (struct null_stream*)s;
    pj_status_t status = PJ_SUCCESS;

    stream->quit = 0;
    if (stream->param.dir & PJMEDIA_DIR_PLAYBACK) {
	status = pj_thread_create (stream->pool,
				   "nullsound_playback",
				   pb_thread_func,
				   stream,
				   0, //ZERO,
				   0,
				   &stream->pb_thread);
	if (status != PJ_SUCCESS)
	    return status;
    }

#if 0
    if (stream->param.dir & PJMEDIA_DIR_CAPTURE) {
	status = pj_thread_create (stream->pool,
				   "nullsound_playback",
				   ca_thread_func,
				   stream,
				   0, //ZERO,
				   0,
				   &stream->ca_thread);
	if (status != PJ_SUCCESS) {
	    stream->quit = PJ_TRUE;
	    pj_thread_join(stream->pb_thread);
	    pj_thread_destroy(stream->pb_thread);
	    stream->pb_thread = NULL;
	}
    }
#endif
    return status;
}


/* API: stop stream */
static pj_status_t null_stream_stop (pjmedia_aud_stream *s)
{
    struct null_stream *stream = (struct null_stream*)s;

    stream->quit = 1;

    if (stream->pb_thread) {
	TRACE_((THIS_FILE,
		   "null_stream_stop(%u): Waiting for playback to stop.",
		   (unsigned)syscall(SYS_gettid)));
	pj_thread_join (stream->pb_thread);
	TRACE_((THIS_FILE,
		   "null_stream_stop(%u): playback stopped.",
		   (unsigned)syscall(SYS_gettid)));
	pj_thread_destroy(stream->pb_thread);
	stream->pb_thread = NULL;
    }

    if (stream->ca_thread) {
	TRACE_((THIS_FILE,
		   "null_stream_stop(%u): Waiting for capture to stop.",
		   (unsigned)syscall(SYS_gettid)));
	pj_thread_join (stream->ca_thread);
	TRACE_((THIS_FILE,
		   "null_stream_stop(%u): capture stopped.",
		   (unsigned)syscall(SYS_gettid)));
	pj_thread_destroy(stream->ca_thread);
	stream->ca_thread = NULL;
    }

    return PJ_SUCCESS;
}



static pj_status_t null_stream_destroy (pjmedia_aud_stream *s)
{
    struct null_stream *stream = (struct null_stream*)s;

    null_stream_stop (s);

    if (stream->param.dir & PJMEDIA_DIR_PLAYBACK) {
	//snd_pcm_close (stream->pb_pcm);
	stream->pb_pcm = NULL;
    }
    if (stream->param.dir & PJMEDIA_DIR_CAPTURE) {
	//snd_pcm_close (stream->ca_pcm);
	stream->ca_pcm = NULL;
    }

    pj_pool_release (stream->pool);

    return PJ_SUCCESS;
}

#endif	/* PJMEDIA_AUDIO_DEV_HAS_null */


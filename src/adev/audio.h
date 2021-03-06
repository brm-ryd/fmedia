/** Shared code for audio I/O
Copyright (c) 2020 Simon Zolin */

#include <fmedia.h>
#include <ffaudio/audio.h>


#define warnlog1(trk, ...)  fmed_warnlog(a->core, trk, NULL, __VA_ARGS__)
#define errlog1(trk, ...)  fmed_errlog(a->core, trk, NULL, __VA_ARGS__)
#define dbglog1(trk, ...)  fmed_dbglog(a->core, trk, NULL, __VA_ARGS__)


static const ushort ffaudio_formats[] = {
	FFAUDIO_F_INT8, FFAUDIO_F_INT16, FFAUDIO_F_INT24, FFAUDIO_F_INT32, FFAUDIO_F_INT24_4, FFAUDIO_F_FLOAT32,
};
static const char ffaudio_formats_str[][8] = {
	"int8", "int16", "int24", "int32", "int24_4", "float32",
};
static const ushort ffpcm_formats[] = {
	FFPCM_8, FFPCM_16, FFPCM_24, FFPCM_32, FFPCM_24_4, FFPCM_FLOAT,
};

static inline int ffpcm_to_ffaudio(uint f)
{
	int i = ffarrint16_find(ffpcm_formats, FF_COUNT(ffpcm_formats), f);
	if (i < 0)
		return -1;
	return ffaudio_formats[i];
}

static inline int ffaudio_to_ffpcm(uint f)
{
	int i = ffarrint16_find(ffaudio_formats, FF_COUNT(ffaudio_formats), f);
	if (i < 0)
		return -1;
	return ffpcm_formats[i];
}

static inline const char* ffaudio_format_str(uint f)
{
	int i = ffarrint16_find(ffaudio_formats, FF_COUNT(ffaudio_formats), f);
	return ffaudio_formats_str[i];
}


static int audio_dev_list(const fmed_core *core, const ffaudio_interface *audio, fmed_adev_ent **ents, uint flags, const char *mod_name)
{
	ffarr a = {};
	ffaudio_dev *d;
	fmed_adev_ent *e;
	int r, rr = -1;

	uint f;
	if (flags == FMED_ADEV_PLAYBACK)
		f = FFAUDIO_DEV_PLAYBACK;
	else if (flags == FMED_ADEV_CAPTURE)
		f = FFAUDIO_DEV_CAPTURE;
	else
		return -1;
	d = audio->dev_alloc(f);

	for (;;) {
		r = audio->dev_next(d);
		if (r == 1)
			break;
		else if (r < 0) {
			fmed_errlog(core, NULL, mod_name, "dev_next(): %s", audio->dev_error(d));
			goto end;
		}

		if (NULL == (e = ffarr_pushgrowT(&a, 4, fmed_adev_ent)))
			goto end;
		ffmem_tzero(e);

		if (NULL == (e->name = ffsz_dup(audio->dev_info(d, FFAUDIO_DEV_NAME))))
			goto end;

		e->default_device = !!audio->dev_info(d, FFAUDIO_DEV_IS_DEFAULT);

		const ffuint *def_fmt = (void*)audio->dev_info(d, FFAUDIO_DEV_MIX_FORMAT);
		if (def_fmt != NULL) {
			e->default_format.format = ffaudio_to_ffpcm(def_fmt[0]);
			e->default_format.sample_rate = def_fmt[1];
			e->default_format.channels = def_fmt[2];
		}
	}

	if (NULL == (e = ffarr_pushT(&a, fmed_adev_ent)))
		goto end;
	e->name = NULL;
	*ents = (void*)a.ptr;
	rr = a.len - 1;

end:
	audio->dev_free(d);
	if (rr < 0) {
		FFARR_WALKT(&a, e, fmed_adev_ent) {
			ffmem_free(e->name);
		}
		ffarr_free(&a);
	}
	return rr;
}

static void audio_dev_listfree(fmed_adev_ent *ents)
{
	fmed_adev_ent *e;
	for (e = ents;  e->name != NULL;  e++) {
		ffmem_free(e->name);
	}
	ffmem_free(ents);
}

/** Get device by index */
static int audio_devbyidx(const ffaudio_interface *audio, ffaudio_dev **d, uint idev, uint flags)
{
	*d = audio->dev_alloc(flags);

	for (uint i = 0;  ;  i++) {

		int r = audio->dev_next(*d);
		if (r != 0) {
			audio->dev_free(*d);
			return r;
		}

		if (i + 1 == idev)
			break;
	}

	return 0;
}


typedef struct audio_out {
	// input
	const fmed_core *core;
	const ffaudio_interface *audio;
	uint buffer_length_msec; // input, output
	uint try_open;
	uint dev_idx; // 0:default
	const fmed_track *track;
	void *trk;
	uint aflags;

	// runtime
	ffaudio_buf *stream;
	ffaudio_dev *dev;
	uint async;

	// user's
	uint state;
} audio_out;

static inline int audio_out_open(audio_out *a, fmed_filt *d, const ffpcm *fmt)
{
	if (!ffsz_eq(d->datatype, "pcm")) {
		errlog1(d->trk, "unsupported input data type: %s", d->datatype);
		return FMED_RERR;
	}

	int rc, r;
	ffaudio_conf conf = {};

	if (a->dev == NULL
		&& a->dev_idx != 0) {
		if (0 != audio_devbyidx(a->audio, &a->dev, a->dev_idx, FFAUDIO_DEV_PLAYBACK)) {
			errlog1(d->trk, "no audio device by index #%u", a->dev_idx);
			rc = FMED_RERR;
			goto end;
		}
		conf.device_id = a->audio->dev_info(a->dev, FFAUDIO_DEV_ID);
	}

	a->stream = a->audio->alloc();

	int afmt = ffpcm_to_ffaudio(fmt->format);
	if (afmt < 0) {
		errlog1(d->trk, "format not supported", 0);
		rc = FMED_RERR;
		goto end;
	}
	conf.format = afmt;
	conf.sample_rate = fmt->sample_rate;
	conf.channels = fmt->channels;

	conf.buffer_length_msec = a->buffer_length_msec;

	uint aflags = a->aflags;
	ffaudio_conf in_conf = conf;
	dbglog1(d->trk, "opening device #%d, %s/%u/%u"
		, a->dev_idx
		, ffaudio_format_str(conf.format), conf.sample_rate, conf.channels);
	r = a->audio->open(a->stream, &conf, FFAUDIO_PLAYBACK | FFAUDIO_O_NONBLOCK | FFAUDIO_O_UNSYNC_NOTIFY | aflags);

	if (r == FFAUDIO_EFORMAT) {
		if (a->try_open) {
			int new_format = 0;
			if (conf.format != in_conf.format) {
				d->audio.convfmt.format = ffaudio_to_ffpcm(conf.format);
				new_format = 1;
			}

			if (conf.sample_rate != in_conf.sample_rate) {
				d->audio.convfmt.sample_rate = conf.sample_rate;
				new_format = 1;
			}

			if (conf.channels != in_conf.channels) {
				d->audio.convfmt.channels = conf.channels;
				new_format = 1;
			}

			if (new_format) {
				rc = FMED_RMORE;
				goto end;
			}
		}

		errlog1(d->trk, "open(): unsupported format", 0);
		rc = FMED_RERR;
		goto end;

	} else if (r != 0) {
		errlog1(d->trk, "open() device #%u: %s  format:%s/%u/%u"
			, a->dev_idx
			, a->audio->error(a->stream)
			, ffaudio_format_str(conf.format), conf.sample_rate, conf.channels);
		rc = FMED_RERR;
		goto end;
	}

	a->buffer_length_msec = conf.buffer_length_msec;
	rc = FMED_ROK;

end:
	if (rc != FMED_ROK) {
		a->audio->free(a->stream);
		a->stream = NULL;
	}
	return rc;
}

static inline void audio_out_onplay(void *param)
{
	audio_out *a = param;
	if (!a->async)
		return;
	a->async = 0;
	a->track->cmd(a->trk, FMED_TRACK_WAKE);
}

static inline int audio_out_write(audio_out *a, fmed_filt *d)
{
	int r;

	if (d->snd_output_clear) {
		d->snd_output_clear = 0;
		a->audio->stop(a->stream);
		a->audio->clear(a->stream);
		return FMED_RMORE;
	}

	if (d->snd_output_pause) {
		d->snd_output_pause = 0;
		d->track->cmd(d->trk, FMED_TRACK_PAUSE);
		a->audio->stop(a->stream);
		return FMED_RASYNC;
	}

	while (d->datalen != 0) {

		r = a->audio->write(a->stream, d->data, d->datalen);
		if (r == -FFAUDIO_ESYNC) {
			warnlog1(d->trk, "underrun detected", 0);
			continue;

		} else if (r < 0) {
			errlog1(d->trk, "write(): %s", a->audio->error(a->stream));
			return FMED_RERR;

		} else if (r == 0) {
			a->async = 1;
			return FMED_RASYNC;
		}

		d->data += r;
		d->datalen -= r;
		dbglog1(d->trk, "written %u bytes"
			, r);
	}

	if (d->flags & FMED_FLAST) {

		r = a->audio->drain(a->stream);
		if (r == 1)
			return FMED_RDONE;
		else if (r < 0) {
			errlog1(d->trk, "drain(): %s", a->audio->error(a->stream));
			return FMED_RERR;
		}

		a->async = 1;
		return FMED_RASYNC; //wait until all filled bytes are played
	}

	return FMED_RMORE;
}


typedef struct audio_in {
	// input
	const fmed_core *core;
	const ffaudio_interface *audio;
	uint dev_idx; // 0:default
	void *trk;
	const fmed_track *track;
	ffuint buffer_length_msec; // input, output
	uint loopback;
	uint aflags;

	// runtime
	ffaudio_buf *stream;
	uint64 total_samples;
	uint frame_size;
	uint async;
} audio_in;

static int audio_in_open(audio_in *a, fmed_filt *d)
{
	int r;
	ffbool first_try = 1;
	ffaudio_dev *dev = NULL;
	ffaudio_conf conf = {};

	if (a->dev_idx != 0) {
		ffuint mode = (a->loopback) ? FFAUDIO_DEV_PLAYBACK : FFAUDIO_DEV_CAPTURE;
		if (0 != audio_devbyidx(a->audio, &dev, a->dev_idx, mode)) {
			errlog1(d->trk, "no audio device by index #%u", a->dev_idx);
			goto err;
		}
		conf.device_id = a->audio->dev_info(dev, FFAUDIO_DEV_ID);
	}

	int afmt = ffpcm_to_ffaudio(d->audio.fmt.format);
	if (afmt < 0) {
		errlog1(d->trk, "format not supported", 0);
		goto err;
	}
	conf.format = afmt;
	conf.sample_rate = d->audio.fmt.sample_rate;
	conf.channels = d->audio.fmt.channels;

	conf.buffer_length_msec = (d->a_in_buf_time != 0) ? d->a_in_buf_time : a->buffer_length_msec;

	ffaudio_conf in_conf = conf;

	int aflags = (a->loopback) ? FFAUDIO_LOOPBACK : FFAUDIO_CAPTURE;
	aflags |= a->aflags;

	if (NULL == (a->stream = a->audio->alloc()))
		goto err;

	for (;;) {
		dbglog1(d->trk, "opening device #%d, %s/%u/%u"
			, a->dev_idx
			, ffaudio_format_str(conf.format), conf.sample_rate, conf.channels);
		r = a->audio->open(a->stream, &conf, aflags | FFAUDIO_O_NONBLOCK | FFAUDIO_O_UNSYNC_NOTIFY);

		if (r == FFAUDIO_EFORMAT) {
			if (first_try) {
				first_try = 0;
				int new_format = 0;

				if (conf.format != in_conf.format) {
					if (d->audio.convfmt.format == 0)
						d->audio.convfmt.format = d->audio.fmt.format;
					d->audio.fmt.format = ffaudio_to_ffpcm(conf.format);
					new_format = 1;
				}

				if (conf.sample_rate != in_conf.sample_rate) {
					if (d->audio.convfmt.sample_rate == 0)
						d->audio.convfmt.sample_rate = d->audio.fmt.sample_rate;
					d->audio.fmt.sample_rate = conf.sample_rate;
					new_format = 1;
				}

				if (conf.channels != in_conf.channels) {
					if (d->audio.convfmt.channels == 0)
						d->audio.convfmt.channels = d->audio.fmt.channels;
					d->audio.fmt.channels = conf.channels;
					new_format = 1;
				}

				if (new_format)
					continue;
			}

			if (aflags & FFAUDIO_O_HWDEV) {
				aflags &= ~FFAUDIO_O_HWDEV;
				continue;
			}

			errlog1(d->trk, "open device #%u: unsupported format: %s/%u/%u"
				, a->dev_idx
				, ffaudio_format_str(in_conf.format), in_conf.sample_rate, in_conf.channels);
			goto err;

		} else if (r != 0) {
			errlog1(d->trk, "open device #%u: %s  format:%s/%u/%u"
				, a->dev_idx
				, a->audio->error(a->stream)
				, ffaudio_format_str(in_conf.format), in_conf.sample_rate, in_conf.channels);
			goto err;
		}

		break;
	}

	dbglog1(d->trk, "opened audio capture buffer: %ums"
		, conf.buffer_length_msec);

	a->buffer_length_msec = conf.buffer_length_msec;
	a->audio->dev_free(dev);
	d->audio.fmt.ileaved = 1;
	d->datatype = "pcm";
	a->frame_size = ffpcm_size1(&d->audio.fmt);
	return 0;

err:
	a->audio->dev_free(dev);
	a->audio->free(a->stream);
	a->stream = NULL;
	return -1;
}

static void audio_in_close(audio_in *a)
{
	a->audio->free(a->stream);
}

static void audio_oncapt(void *udata)
{
	audio_in *a = udata;
	if (!a->async)
		return;
	a->async = 0;
	a->track->cmd(a->trk, FMED_TRACK_WAKE);
}

static int audio_in_read(audio_in *a, fmed_filt *d)
{
	int r;
	const void *buf;

	if (d->flags & FMED_FSTOP) {
		a->audio->stop(a->stream);
		d->outlen = 0;
		return FMED_RDONE;
	}

	for (;;) {
		r = a->audio->read(a->stream, &buf);
		if (r == -FFAUDIO_ESYNC) {
			warnlog1(d->trk, "overrun detected", 0);
			continue;

		} else if (r < 0) {
			errlog1(d->trk, "read(): %s", a->audio->error(a->stream));
			return FMED_RERR;

		} else if (r == 0) {
			a->async = 1;
			return FMED_RASYNC;
		}
		break;
	}

	dbglog1(d->trk, "read %L bytes", r);

	d->audio.pos = a->total_samples;
	a->total_samples += r / a->frame_size;
	d->out = buf,  d->outlen = r;
	return FMED_RDATA;
}

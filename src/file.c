/** File input/output.
Copyright (c) 2015 Simon Zolin */

#include <fmedia.h>

#include <FF/array.h>
#include <FF/data/parse.h>
#include <FFOS/file.h>
#include <FFOS/asyncio.h>
#include <FFOS/error.h>
#include <FFOS/dir.h>
#include <FF/path.h>


struct file_in_conf_t {
	uint nbufs;
	size_t bsize;
	size_t align;
	byte directio;
};

struct file_out_conf_t {
	size_t bsize;
	size_t prealloc;
	uint file_del :1;
	uint prealloc_grow :1;
};

typedef struct filemod {
	struct file_in_conf_t in_conf;
	struct file_out_conf_t out_conf;
} filemod;

static filemod *mod;
static const fmed_core *core;

typedef struct databuf {
	char *ptr;
	uint off;
	uint len;
} databuf;

typedef struct fmed_file {
	const char *fn;
	fffd fd;
	uint wdata;
	uint rdata;
	databuf *data;
	uint64 fsize;
	uint64 foff
		, aoff;
	ffaio_filetask ftask;
	int64 seek;

	fmed_handler handler;
	void *trk;

	unsigned async :1
		, done :1
		, want_read :1
		, err :1
		, out :1;
} fmed_file;

typedef struct fmed_fileout {
	ffstr fname;
	fffd fd;
	ffarr buf;
	uint64 fsize
		, preallocated;
	uint64 prealloc_by;
	fftime modtime;
	uint ok :1;

	struct {
		uint nmwrite;
		uint nfwrite;
		uint nprealloc;
	} stat;
} fmed_fileout;


//FMEDIA MODULE
static const void* file_iface(const char *name);
static int file_conf(const char *name, ffpars_ctx *ctx);
static int file_sig(uint signo);
static void file_destroy(void);
static const fmed_mod fmed_file_mod = {
	&file_iface, &file_sig, &file_destroy, &file_conf
};

//INPUT
static void* file_open(fmed_filt *d);
static int file_getdata(void *ctx, fmed_filt *d);
static void file_close(void *ctx);
static int file_in_conf(ffpars_ctx *ctx);
static const fmed_filter fmed_file_input = {
	&file_open, &file_getdata, &file_close
};

static void file_read(void *udata);

static const ffpars_arg file_in_conf_args[] = {
	{ "buffer_size",  FFPARS_TSIZE | FFPARS_FNOTZERO,  FFPARS_DSTOFF(struct file_in_conf_t, bsize) }
	, { "buffers",  FFPARS_TINT | FFPARS_F8BIT,  FFPARS_DSTOFF(struct file_in_conf_t, nbufs) }
	, { "align",  FFPARS_TSIZE | FFPARS_FNOTZERO,  FFPARS_DSTOFF(struct file_in_conf_t, align) }
	, { "direct_io",  FFPARS_TBOOL | FFPARS_F8BIT,  FFPARS_DSTOFF(struct file_in_conf_t, directio) }
};


//OUTPUT
static void* fileout_open(fmed_filt *d);
static int fileout_write(void *ctx, fmed_filt *d);
static void fileout_close(void *ctx);
static int fileout_config(ffpars_ctx *ctx);
static const fmed_filter fmed_file_output = {
	&fileout_open, &fileout_write, &fileout_close
};

static int fileout_writedata(fmed_fileout *f, const char *data, size_t len, fmed_filt *d);
static char* fileout_getname(fmed_fileout *f, fmed_filt *d);

static const ffpars_arg file_out_conf_args[] = {
	{ "buffer_size",  FFPARS_TSIZE | FFPARS_FNOTZERO,  FFPARS_DSTOFF(struct file_out_conf_t, bsize) }
	, { "preallocate",  FFPARS_TSIZE | FFPARS_FNOTZERO,  FFPARS_DSTOFF(struct file_out_conf_t, prealloc) }
};


const fmed_mod* fmed_getmod_file(const fmed_core *_core)
{
	if (mod != NULL)
		return &fmed_file_mod;

	if (0 != ffaio_fctxinit())
		return NULL;
	core = _core;
	if (NULL == (mod = ffmem_tcalloc1(filemod)))
		return NULL;
	return &fmed_file_mod;
}


static const void* file_iface(const char *name)
{
	if (!ffsz_cmp(name, "in")) {
		return &fmed_file_input;
	} else if (!ffsz_cmp(name, "out")) {
		return &fmed_file_output;
	}
	return NULL;
}

static int file_conf(const char *name, ffpars_ctx *ctx)
{
	if (!ffsz_cmp(name, "in"))
		return file_in_conf(ctx);
	else if (!ffsz_cmp(name, "out"))
		return fileout_config(ctx);
	return -1;
}

static int file_sig(uint signo)
{
	switch (signo) {
	case FMED_OPEN:
		break;
	}
	return 0;
}

static void file_destroy(void)
{
	ffaio_fctxclose();
	ffmem_free0(mod);
}


static int file_in_conf(ffpars_ctx *ctx)
{
	mod->in_conf.align = 4096;
	mod->in_conf.bsize = 64 * 1024;
	mod->in_conf.nbufs = 2;
	mod->in_conf.directio = 1;
	ffpars_setargs(ctx, &mod->in_conf, file_in_conf_args, FFCNT(file_in_conf_args));
	return 0;
}

static void* file_open(fmed_filt *d)
{
	fmed_file *f;
	uint i;
	fffileinfo fi;

	f = ffmem_tcalloc1(fmed_file);
	if (f == NULL)
		return NULL;
	f->seek = -1;
	f->fd = FF_BADFD;
	f->fn = d->track->getvalstr(d->trk, "input");
	if (NULL == (f->data = ffmem_tcalloc(databuf, mod->in_conf.nbufs)))
		goto done;

	uint flags = O_RDONLY | O_NOATIME | O_NONBLOCK | FFO_NODOSNAME;
	flags |= (mod->in_conf.directio) ? O_DIRECT : 0;
	for (;;) {
		f->fd = fffile_open(f->fn, flags);

#ifdef FF_LINUX
		if (f->fd == FF_BADFD && fferr_last() == EINVAL && (flags & O_DIRECT)) {
			flags &= ~O_DIRECT;
			continue;
		}

		if (f->fd == FF_BADFD && fferr_last() == EPERM && (flags & O_NOATIME)) {
			flags &= ~O_NOATIME;
			continue;
		}
#endif

		break;
	}

	if (f->fd == FF_BADFD) {
		syserrlog(core, d->trk, "file", "%e: %s", FFERR_FOPEN, f->fn);
		goto done;
	}
	if (0 != fffile_info(f->fd, &fi)) {
		syserrlog(core, d->trk, "file", "get file info: %s", f->fn);
		goto done;
	}
	f->fsize = fffile_infosize(&fi);

	dbglog(core, d->trk, "file", "opened %s (%U kbytes)", f->fn, f->fsize / 1024);

	ffaio_finit(&f->ftask, f->fd, f);
	f->ftask.kev.udata = f;
	if (0 != ffaio_fattach(&f->ftask, core->kq, !!(flags & O_DIRECT))) {
		syserrlog(core, d->trk, "file", "%e: %s", FFERR_KQUATT, f->fn);
		goto done;
	}

	for (i = 0;  i < mod->in_conf.nbufs;  i++) {
		f->data[i].ptr = ffmem_align(mod->in_conf.bsize, mod->in_conf.align);
		if (f->data[i].ptr == NULL) {
			syserrlog(core, d->trk, "file", "%e: %s", FFERR_BUFALOC, f->fn);
			goto done;
		}
	}

	d->input.size = f->fsize;

	if (d->out_preserve_date) {
		fftime t = fffile_infomtime(&fi);
		fmed_setval("output_time", fftime_mcs(&t));
	}

	f->handler = d->handler;
	f->trk = d->trk;
	return f;

done:
	file_close(f);
	return NULL;
}

static void file_close(void *ctx)
{
	fmed_file *f = ctx;
	uint i;

	if (f->fd != FF_BADFD) {
		fffile_close(f->fd);
		f->fd = FF_BADFD;
	}
	if (f->async)
		return; //wait until async operation is completed

	if (f->data != NULL) {
		for (i = 0;  i < mod->in_conf.nbufs;  i++) {
			if (f->data[i].ptr != NULL)
				ffmem_alignfree(f->data[i].ptr);
		}
		ffmem_free(f->data);
	}
	ffmem_free(f);
}

static int file_getdata(void *ctx, fmed_filt *d)
{
	fmed_file *f = ctx;
	uint64 seek;
	uint i;

	if (f->err)
		return FMED_RERR;

	if ((int64)d->input.seek != FMED_NULL) {
		seek = d->input.seek;
		f->seek = seek;
		d->input.seek = FMED_NULL;
		if (seek >= f->fsize) {
			errlog(core, d->trk, "file", "too big seek position %U", f->seek);
			return FMED_RERR;
		}

		/* don't do unnecessary file_read() if seek position is within the current buffers,
			instead, just shift offsets in the buffers */
		uint64 cursize = 0;
		for (i = 0;  i != mod->in_conf.nbufs;  i++) {
			cursize += f->data[i].len;
			f->data[i].off = 0;
		}
		if (seek >= f->foff - cursize && seek < f->foff) {
			uint64 pos = seek - (f->foff - cursize);
			dbglog(core, NULL, "file", "shifting %U bytes", pos);

			while (pos != 0) {
				databuf *d = &f->data[f->rdata];
				size_t n = ffmin(d->len - d->off, pos);
				d->off += n;
				if (d->off == d->len) {
					d->len = 0;
					d->off = 0;
					f->rdata = (f->rdata + 1) % mod->in_conf.nbufs;
				}

				pos -= n;
			}

			f->out = 0;
			f->done = 0;
			f->seek = -1;
			f->aoff = seek;
			goto rd;
		}

		f->wdata = 0;
		f->rdata = 0;
		f->out = 0;
		f->done = 0;
		if (!f->async) {
			f->foff = f->seek;
			f->aoff = f->seek;
			f->seek = -1;
		}

		//reset all buffers
		for (i = 0;  i < mod->in_conf.nbufs;  i++) {
			f->data[i].len = 0;
			f->data[i].off = 0;
		}
	}

	if (f->out) {
		f->out = 0;
		f->aoff += f->data[f->rdata].len - f->data[f->rdata].off;
		f->data[f->rdata].len = 0;
		f->data[f->rdata].off = 0;
		f->rdata = (f->rdata + 1) % mod->in_conf.nbufs;

		if (f->done && f->data[f->rdata].len == 0) {
			/* We finished reading in the previous iteration.
			After that, noone's asked to seek back. */
			d->outlen = 0;
			return FMED_RDONE;
		}
	}

rd:
	if (!f->async && !f->done)
		file_read(f);

	if (f->data[f->rdata].len == 0 && !f->done) {
		f->want_read = 1;
		return FMED_RASYNC; //wait until the buffer is full
	}

	fmed_setval("input_off", f->aoff);

	d->out = f->data[f->rdata].ptr;
	d->outlen = f->data[f->rdata].len;
	if (f->data[f->rdata].off != 0) {
		d->out += f->data[f->rdata].off;
		d->outlen -= f->data[f->rdata].off;
	}
	f->out = 1;

	return FMED_ROK;
}

static void file_read(void *udata)
{
	fmed_file *f = udata;
	uint filled = 0;
	int r;

	if (f->async && f->fd == FF_BADFD) {
		f->async = 0;
		file_close(f);
		return;
	}

	for (;;) {
		uint64 off = f->foff;
		if (f->foff % mod->in_conf.align)
			off &= ~(mod->in_conf.align-1);

		if (f->data[f->wdata].len != 0)
			break;

		r = (int)ffaio_fread(&f->ftask, f->data[f->wdata].ptr, mod->in_conf.bsize, off, &file_read);
		f->async = 0;
		if (r < 0) {
			if (fferr_again(fferr_last())) {
				dbglog(core, f->trk, "file", "async read, offset:%xU", off);
				f->async = 1;
				break;
			}

			syserrlog(core, f->trk, "file", "%e: %s", FFERR_READ, f->fn);
			f->err = 1;
			return;
		}

		if (f->seek != -1) {
			f->foff = f->seek;
			f->aoff = f->seek;
			f->seek = -1;
			continue;
		}

		if ((uint)r != mod->in_conf.bsize) {
			dbglog(core, f->trk, "file", "reading's done", 0);
			f->done = 1;
			if (r == 0)
				break;
		}

		dbglog(core, f->trk, "file", "read %U bytes at offset %U", (int64)r - (f->foff % mod->in_conf.align), f->foff);
		if (f->foff % mod->in_conf.align != 0)
			f->data[f->wdata].off = f->foff % mod->in_conf.align;
		f->foff = (f->foff & ~(mod->in_conf.align-1)) + r;
		f->data[f->wdata].len = r;
		filled = 1;

		f->wdata = (f->wdata + 1) % mod->in_conf.nbufs;
		if (f->data[f->wdata].len != 0 || f->done)
			break; //all buffers are filled or end-of-file is reached
	}

	if (filled && f->want_read) {
		f->want_read = 0;
		f->handler(f->trk);
	}
}


static int fileout_config(ffpars_ctx *ctx)
{
	mod->out_conf.bsize = 64 * 1024;
	mod->out_conf.prealloc = 1 * 1024 * 1024;
	mod->out_conf.prealloc_grow = 1;
	mod->out_conf.file_del = 1;
	ffpars_setargs(ctx, &mod->out_conf, file_out_conf_args, FFCNT(file_out_conf_args));
	return 0;
}

enum VARS {
	VAR_DATE,
	VAR_FNAME,
	VAR_FPATH,
	VAR_TIME,
	VAR_TIMEMS,
	VAR_YEAR,
};

static const char* const vars[] = {
	"date",
	"filename",
	"filepath",
	"time",
	"timems",
	"year",
};

static FFINL char* fileout_getname(fmed_fileout *f, fmed_filt *d)
{
	ffsvar p;
	ffstr fn, val, fdir, fname;
	char *tstr;
	const char *in;
	ffarr buf = {0};
	int r, have_dt = 0, ivar;
	ffdtm dt;

	ffmem_tzero(&p);
	ffstr_setz(&fn, d->track->getvalstr(d->trk, "output"));

	while (fn.len != 0) {
		size_t n = fn.len;
		r = ffsvar_parse(&p, fn.ptr, &n);
		ffstr_shift(&fn, n);

		switch (r) {
		case FFSVAR_S:
			if (0 > (ivar = ffszarr_findsorted(vars, FFCNT(vars), p.val.ptr, p.val.len))) {
				if (FMED_PNULL == (tstr = d->track->getvalstr3(d->trk, &p.val, FMED_TRK_META | FMED_TRK_NAMESTR)))
					continue;
				ffstr_setz(&val, tstr);
				goto data;
			}

			switch (ivar) {

			case VAR_FPATH:
			case VAR_FNAME:
				if (NULL == (in = d->track->getvalstr(d->trk, "input")))
					goto done;
				ffpath_split2(in, ffsz_len(in), &fdir, &fname);
				break;

			case VAR_DATE:
			case VAR_TIME:
			case VAR_TIMEMS:
				if (!have_dt) {
					// get time only once
					fftime t;
					fftime_now(&t);
					fftime_split(&dt, &t, FFTIME_TZLOCAL);
					have_dt = 1;
				}
				break;
			}

			switch (ivar) {

			case VAR_FPATH:
				if (fdir.len == 0)
					goto done;
				if (NULL == ffarr_append(&buf, fdir.ptr, fdir.len))
					goto syserr;
				break;

			case VAR_FNAME:
				ffpath_splitname(fname.ptr, fname.len, &val, NULL);
				goto data;

			case VAR_DATE:
				if (0 == ffstr_catfmt(&buf, "%04u%02u%02u", dt.year, dt.month, dt.day))
					goto syserr;
				break;

			case VAR_TIME:
				if (0 == ffstr_catfmt(&buf, "%02u%02u%02u", dt.hour, dt.min, dt.sec))
					goto syserr;
				break;

			case VAR_TIMEMS:
				if (0 == ffstr_catfmt(&buf, "%02u%02u%02u-%03u", dt.hour, dt.min, dt.sec, dt.msec))
					goto syserr;
				break;

			case VAR_YEAR:
				ffstr_setcz(&val, "date");
				if (FMED_PNULL == (tstr = d->track->getvalstr3(d->trk, &val, FMED_TRK_META | FMED_TRK_NAMESTR)))
					continue;
				ffstr_setz(&val, tstr);
				goto data;
			}

			continue;

		case FFSVAR_TEXT:
			val = p.val;
			break;

		default:
			goto done;
		}

data:
		if (NULL == ffarr_grow(&buf, val.len, 0))
			goto syserr;

		switch (r) {
		case FFSVAR_S:
			ffpath_makefn(ffarr_end(&buf), -1, val.ptr, val.len, '_');
			buf.len += val.len;
			break;

		case FFSVAR_TEXT:
			ffarr_append(&buf, val.ptr, val.len);
			break;
		}
	}

	if (NULL == ffarr_append(&buf, "", 1))
		goto syserr;
	ffstr_acqstr3(&f->fname, &buf);
	f->fname.len--;

	if (!ffstr_eq2(&f->fname, &fn))
		d->track->setvalstr(d->trk, "output", f->fname.ptr);

	return f->fname.ptr;

syserr:
	syserrlog(core, d->trk, NULL, "%e", FFERR_BUFALOC);

done:
	ffarr_free(&buf);
	return NULL;
}

static void* fileout_open(fmed_filt *d)
{
	const char *filename;
	fmed_fileout *f = ffmem_tcalloc1(fmed_fileout);
	if (f == NULL)
		return NULL;
	f->fd = FF_BADFD;

	if (NULL == (filename = fileout_getname(f, d)))
		goto done;

	uint flags = (d->out_overwrite) ? O_CREAT : FFO_CREATENEW;
	flags |= O_WRONLY;
	f->fd = fffile_open(filename, flags);
	if (f->fd == FF_BADFD) {

		if (fferr_nofile(fferr_last())) {
			if (0 != ffdir_make_path((void*)filename)) {
				syserrlog(core, d->trk, "file", "%e: for filename %s", FFERR_DIRMAKE, filename);
				goto done;
			}

			f->fd = fffile_open(filename, flags);
		}

		if (f->fd == FF_BADFD) {
			syserrlog(core, d->trk, "file", "%e: %s", FFERR_FOPEN, filename);
			goto done;
		}
	}

	if (NULL == ffarr_alloc(&f->buf, mod->out_conf.bsize)) {
		syserrlog(core, d->trk, "file", "%e", FFERR_BUFALOC);
		goto done;
	}

	if ((int64)d->output.size != FMED_NULL) {
		if (0 == fffile_trunc(f->fd, d->output.size)) {
			f->preallocated = d->output.size;
			f->stat.nprealloc++;
		}
	}

	int64 mtime;
	if (FMED_NULL != (mtime = fmed_getval("output_time")))
		fftime_setmcs(&f->modtime, mtime);

	f->prealloc_by = mod->out_conf.prealloc;
	return f;

done:
	fileout_close(f);
	return NULL;
}

static void fileout_close(void *ctx)
{
	fmed_fileout *f = ctx;

	if (f->fd != FF_BADFD) {

		fffile_trunc(f->fd, f->fsize);

		if (!f->ok && mod->out_conf.file_del) {

			if (0 != fffile_close(f->fd))
				syserrlog(core, NULL, "file", "%e", FFERR_FCLOSE);

			if (0 == fffile_rm(f->fname.ptr))
				dbglog(core, NULL, "file", "removed file %S", &f->fname);

		} else {

			if (f->modtime.s != 0)
				fffile_settime(f->fd, &f->modtime);

			if (0 != fffile_close(f->fd))
				syserrlog(core, NULL, "file", "%e", FFERR_FCLOSE);

			core->log(FMED_LOG_USER, NULL, "file", "saved file %S, %U kbytes"
				, &f->fname, f->fsize / 1024);
		}
	}

	ffstr_free(&f->fname);
	ffarr_free(&f->buf);
	dbglog(core, NULL, "file", "mem write#:%u  file write#:%u  prealloc#:%u"
		, f->stat.nmwrite, f->stat.nfwrite, f->stat.nprealloc);
	ffmem_free(f);
}

static int fileout_writedata(fmed_fileout *f, const char *data, size_t len, fmed_filt *d)
{
	size_t r;
	if (f->fsize + len > f->preallocated) {
		uint64 n = ff_align_ceil(f->fsize + len, f->prealloc_by);
		if (0 == fffile_trunc(f->fd, n)) {

			if (mod->out_conf.prealloc_grow)
				f->prealloc_by += f->prealloc_by;

			f->preallocated = n;
			f->stat.nprealloc++;
		}
	}

	r = fffile_write(f->fd, data, len);
	if (r != len) {
		syserrlog(core, d->trk, "file", "%e: %s", FFERR_WRITE, f->fname.ptr);
		return -1;
	}
	f->stat.nfwrite++;

	dbglog(core, d->trk, "file", "written %L bytes at offset %U (%L pending)", r, f->fsize, d->datalen);
	f->fsize += r;
	return r;
}

static int fileout_write(void *ctx, fmed_filt *d)
{
	fmed_fileout *f = ctx;
	ssize_t r;
	ffstr dst;
	int64 seek;

	if ((int64)d->output.seek != FMED_NULL) {
		seek = d->output.seek;
		d->output.seek = FMED_NULL;

		if (f->buf.len != 0) {
			if (-1 == fileout_writedata(f, f->buf.ptr, f->buf.len, d))
				return FMED_RERR;
			f->buf.len = 0;
		}

		if (0 > fffile_seek(f->fd, seek, SEEK_SET)) {
			syserrlog(core, d->trk, "file", "%e: %s", FFERR_FSEEK, f->fname.ptr);
			return -1;
		}

		if (d->datalen != (size_t)fffile_write(f->fd, d->data, d->datalen)) {
			syserrlog(core, d->trk, "file", "%e: %s", FFERR_WRITE, f->fname.ptr);
			return -1;
		}
		f->stat.nfwrite++;

		dbglog(core, d->trk, "file", "written %L bytes at offset %U", d->datalen, seek);

		if (f->fsize < d->datalen)
			f->fsize = d->datalen;

		if (0 > fffile_seek(f->fd, f->fsize, SEEK_SET)) {
			syserrlog(core, d->trk, "file", "%e: %s", FFERR_FSEEK, f->fname.ptr);
			return -1;
		}

		d->datalen = 0;
	}

	for (;;) {

		r = ffbuf_add(&f->buf, d->data, d->datalen, &dst);
		d->data += r;
		d->datalen -= r;
		if (dst.len == 0) {
			f->stat.nmwrite++;
			if (!(d->flags & FMED_FLAST) || f->buf.len == 0)
				break;
			ffstr_set(&dst, f->buf.ptr, f->buf.len);
		}

		if (-1 == fileout_writedata(f, dst.ptr, dst.len, d))
			return FMED_RERR;
		if (d->datalen == 0)
			break;
	}

	if (d->flags & FMED_FLAST) {
		f->ok = 1;
		return FMED_RDONE;
	}

	return FMED_ROK;
}

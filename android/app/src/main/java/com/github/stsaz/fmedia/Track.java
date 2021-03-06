package com.github.stsaz.fmedia;

import android.media.MediaPlayer;
import android.media.MediaRecorder;
import android.os.Handler;
import android.os.Looper;

import androidx.annotation.NonNull;
import androidx.collection.SimpleArrayMap;

import java.io.File;
import java.util.ArrayList;
import java.util.Timer;
import java.util.TimerTask;

abstract class Filter {
	/**
	 * Initialize filter.  Called once.
	 */
	public void init() {
	}

	/**
	 * Open filter track context.  Called for each new track.
	 * Return -1: close the track.
	 */
	public int open(TrackHandle t) {
		return 0;
	}

	/**
	 * Close filter track context.
	 */
	public void close(TrackHandle t) {
	}

	/**
	 * Update track progress.  Called periodically by timer.
	 */
	public int process(TrackHandle t) {
		return 0;
	}
}

class TrackHandle {
	MediaRecorder mr;
	Track.State state;
	boolean stopped; // stopped by user
	boolean error; // processing error
	String url;
	String name; // track name shown in GUI
	int pos; // current progress (msec)
	int seek_percent; // default:-1
	int time_total; // track duration (msec)
}

class MP {
	private final String TAG = "MP";
	private MediaPlayer mp;
	private Timer tmr;
	private Handler mloop;
	private Core core;
	private Track track;

	void init(Core core) {
		this.core = core;
		mloop = new Handler(Looper.getMainLooper());

		mp = new MediaPlayer();

		track = core.track();
		track.filter_add(new Filter() {
			@Override
			public int open(TrackHandle t) {
				return on_open(t);
			}

			@Override
			public void close(TrackHandle t) {
				on_close(t);
			}

			@Override
			public int process(TrackHandle t) {
				return on_process(t);
			}
		});
	}

	private int on_open(final TrackHandle t) {
		t.seek_percent = -1;
		t.name = new File(t.url).getName();
		t.state = Track.State.OPENING;

		mp.setOnPreparedListener(new MediaPlayer.OnPreparedListener() {
			public void onPrepared(MediaPlayer mp) {
				on_start(t);
			}
		});
		mp.setOnCompletionListener(new MediaPlayer.OnCompletionListener() {
			public void onCompletion(MediaPlayer mp) {
				on_complete(t);
			}
		});
		mp.setOnErrorListener(new MediaPlayer.OnErrorListener() {
			@Override
			public boolean onError(MediaPlayer mp, int what, int extra) {
				on_error(t);
				return false;
			}
		});
		try {
			mp.setDataSource(t.url);
		} catch (Exception e) {
			core.errlog(TAG, "mp.setDataSource: %s", e);
			return -1;
		}
		mp.prepareAsync(); // -> on_start()
		return 0;
	}

	private void on_close(TrackHandle t) {
		if (tmr != null) {
			tmr.cancel();
			tmr = null;
		}

		if (mp != null) {
			try {
				mp.stop();
			} catch (Exception ignored) {
			}
			mp.reset();
		}
	}

	private int on_process(TrackHandle t) {
		if (t.seek_percent != -1) {
			int ms = t.seek_percent * mp.getDuration() / 100;
			t.seek_percent = -1;
			mp.seekTo(ms);
		}

		if (t.state == Track.State.PAUSED)
			mp.pause();
		else if (t.state == Track.State.UNPAUSE) {
			t.state = Track.State.PLAYING;
			mp.start();
		}
		return 0;
	}

	/**
	 * Called by MediaPlayer when it's ready to start
	 */
	private void on_start(final TrackHandle t) {
		core.dbglog(TAG, "prepared");

		tmr = new Timer();
		tmr.schedule(new TimerTask() {
			@Override
			public void run() {
				on_timer(t);
			}
		}, 0, 500);

		t.state = Track.State.PLAYING;
		t.time_total = mp.getDuration();
		mp.start(); // ->on_complete(), ->on_error()
	}

	/**
	 * Called by MediaPlayer when it's finished playing
	 */
	private void on_complete(TrackHandle t) {
		core.dbglog(TAG, "completed");
		if (t.state == Track.State.NONE)
			return;

		t.stopped = false;
		track.close(t);
	}

	private void on_error(TrackHandle t) {
		core.dbglog(TAG, "onerror");
		t.error = true;
		// -> on_complete()
	}

	private void on_timer(final TrackHandle t) {
		mloop.post(new Runnable() {
			public void run() {
				update(t);
			}
		});
	}

	private void update(TrackHandle t) {
		if (t.state != Track.State.PLAYING)
			return;
		t.pos = mp.getCurrentPosition();
		track.update(t);
	}
}

/**
 * Chain: Queue -> MP -> SysJobs -> Svc
 */
class Track {
	private final String TAG = "Track";
	private Core core;
	private ArrayList<Filter> filters;
	private SimpleArrayMap<String, Boolean> supp_exts;

	private TrackHandle tplay;
	TrackHandle trec;

	enum State {
		NONE,
		OPENING, // ->PLAYING
		PLAYING,
		PAUSED, // ->UNPAUSE
		UNPAUSE, // ->PLAYING
	}

	Track(Core core) {
		this.core = core;
		tplay = new TrackHandle();
		filters = new ArrayList<>();
		tplay.state = State.NONE;

		String[] exts = {"mp3", "ogg", "m4a", "wav", "flac", "mp4", "mkv", "avi"};
		supp_exts = new SimpleArrayMap<>(exts.length);
		for (String e : exts) {
			supp_exts.put(e, true);
		}

		core.dbglog(TAG, "init");
	}

	boolean supported_url(@NonNull String name) {
		if (name.startsWith("http://") || name.startsWith("https://"))
			return true;
		return false;
	}

	/**
	 * Return TRUE if file name's extension is supported
	 */
	boolean supported(@NonNull String name) {
		if (supported_url(name))
			return true;

		int dot = name.lastIndexOf('.');
		if (dot <= 0)
			return false;
		dot++;
		String ext = name.substring(dot);
		return supp_exts.containsKey(ext);
	}

	void filter_add(Filter f) {
		filters.add(f);
	}

	void filter_notify(Filter f) {
		if (tplay.state != State.NONE) {
			f.open(tplay);
			f.process(tplay);
		}
	}

	void filter_rm(Filter f) {
		filters.remove(f);
	}

	State state() {
		return tplay.state;
	}

	/**
	 * Start playing
	 */
	void start(String url) {
		core.dbglog(TAG, "play: %s", url);
		if (tplay.state != State.NONE)
			return;

		tplay.url = url;
		for (Filter f : filters) {
			core.dbglog(TAG, "opening filter %s", f);
			int r = f.open(tplay);
			if (r != 0) {
				core.errlog(TAG, "f.open(): %d", r);
				trk_close(tplay);
				return;
			}
		}
	}

	TrackHandle record(String out) {
		trec = new TrackHandle();
		trec.mr = new MediaRecorder();
		try {
			trec.mr.setAudioSource(MediaRecorder.AudioSource.MIC);
			trec.mr.setOutputFormat(MediaRecorder.OutputFormat.MPEG_4);
			trec.mr.setAudioEncoder(MediaRecorder.AudioEncoder.AAC);
			trec.mr.setAudioEncodingBitRate(192000);
			trec.mr.setOutputFile(out);
			trec.mr.prepare();
			trec.mr.start();
		} catch (Exception e) {
			core.errlog(TAG, "mr.prepare(): %s", e);
			trec = null;
			return null;
		}
		return trec;
	}

	void record_stop(TrackHandle t) {
		try {
			t.mr.stop();
			t.mr.reset();
			t.mr.release();
		} catch (Exception e) {
			core.errlog(TAG, "mr.stop(): %s", e);
		}
		t.mr = null;
		trec = null;
	}

	private void trk_close(TrackHandle t) {
		t.state = State.NONE;
		for (int i = filters.size() - 1; i >= 0; i--) {
			Filter f = filters.get(i);
			core.dbglog(TAG, "closing filter %s", f);
			f.close(t);
		}
		t.error = false;
	}

	/**
	 * Stop playing and notifiy filters
	 */
	void stop() {
		core.dbglog(TAG, "stop");
		tplay.stopped = true;
		trk_close(tplay);
	}

	void close(TrackHandle t) {
		core.dbglog(TAG, "close");
		trk_close(t);
	}

	void pause() {
		core.dbglog(TAG, "pause");
		if (tplay.state != State.PLAYING)
			return;
		tplay.state = State.PAUSED;
		update(tplay);
	}

	void unpause() {
		core.dbglog(TAG, "unpause");
		if (tplay.state != State.PAUSED)
			return;
		tplay.state = State.UNPAUSE;
		update(tplay);
	}

	void seek(int percent) {
		core.dbglog(TAG, "seek: %d", percent);
		if (tplay.state != State.PLAYING && tplay.state != State.PAUSED)
			return;
		tplay.seek_percent = percent;
		update(tplay);
	}

	/**
	 * Notify filters on the track's progress
	 */
	void update(TrackHandle t) {
		for (Filter f : filters) {
			f.process(t);
		}
	}
}

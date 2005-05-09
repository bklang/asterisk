/*
 * jitterbuf: an application-independent jitterbuffer
 *
 * Copyrights:
 * Copyright (C) 2004-2005, Horizon Wimba, Inc.
 *
 * Contributors:
 * Steve Kann <stevek@stevek.com>
 *
 * This program is free software, distributed under the terms of
 * the GNU Lesser (Library) General Public License
 *
 * Copyright on this file is disclaimed to Digium for inclusion in Asterisk
 */

#ifndef _JITTERBUF_H_
#define _JITTERBUF_H_

#ifdef __cplusplus
extern "C" {
#endif

/* configuration constants */
	/* Number of historical timestamps to use in calculating jitter and drift */
#define JB_HISTORY_SZ		500	
	/* what percentage of timestamps should we drop from the history when we examine it;
	 * this might eventually be something made configurable */
#define JB_HISTORY_DROPPCT	3
	/* the maximum droppct we can handle (say it was configurable). */
#define JB_HISTORY_DROPPCT_MAX	4
	/* the size of the buffer we use to keep the top and botton timestamps for dropping */
#define JB_HISTORY_MAXBUF_SZ	JB_HISTORY_SZ * JB_HISTORY_DROPPCT_MAX / 100 
	/* amount of additional jitterbuffer adjustment  */
#define JB_TARGET_EXTRA 40
	/* ms between growing and shrinking; may not be honored if jitterbuffer runs out of space */
#define JB_ADJUST_DELAY 40


/* return codes */
#define JB_OK		0
#define JB_EMPTY	1
#define JB_NOFRAME	2
#define JB_INTERP	3
#define JB_DROP		4

/* frame types */
#define JB_TYPE_CONTROL	0
#define JB_TYPE_VOICE	1
#define JB_TYPE_VIDEO	2  /* reserved */
#define JB_TYPE_SILENCE	3

typedef struct jb_info {
	/* statistics */
	long frames_in;  	/* number of frames input to the jitterbuffer.*/
	long frames_out;  	/* number of frames output from the jitterbuffer.*/
	long frames_late; 	/* number of frames which were too late, and dropped.*/
	long frames_lost; 	/* number of missing frames.*/
	long frames_dropped; 	/* number of frames dropped (shrinkage) */
	long frames_ooo; 	/* number of frames received out-of-order */
	long frames_cur; 	/* number of frames presently in jb, awaiting delivery.*/
	long jitter; 		/* jitter measured within current history interval*/
	long min;		/* minimum lateness within current history interval */
	long current; 		/* the present jitterbuffer adjustment */
	long target; 		/* the target jitterbuffer adjustment */
	long losspct; 		/* recent lost frame percentage (* 1000) */
	long last_voice_ts;	/* the last ts that was read from the jb - in receiver's time */
	long last_voice_ms;	/* the duration of the last voice frame */
	long silence;		/* we are presently playing out silence */
	long last_adjustment;   /* the time of the last adjustment */

	/* settings */
	long max_jitterbuf;	/* defines a hard clamp to use in setting the jitter buffer delay */
} jb_info;

typedef struct jb_frame {
	void *data;		/* the frame data */
	long ts;	/* the relative delivery time expected */
	long ms;	/* the time covered by this frame, in sec/8000 */
	int  type;	/* the type of frame */
	struct jb_frame *next, *prev;
} jb_frame;

typedef struct jitterbuf {
	jb_info info;

	/* history */
	long history[JB_HISTORY_SZ];   		/* history */
	int  hist_ptr;				/* points to index in history for next entry */
	long hist_maxbuf[JB_HISTORY_MAXBUF_SZ];	/* a sorted buffer of the max delays (highest first) */
	long hist_minbuf[JB_HISTORY_MAXBUF_SZ];	/* a sorted buffer of the min delays (lowest first) */
	int  hist_maxbuf_valid;			/* are the "maxbuf"/minbuf valid? */


	jb_frame *frames; 		/* queued frames */
	jb_frame *free; 		/* free frames (avoid malloc?) */
} jitterbuf;


/* new jitterbuf */
jitterbuf *		jb_new(void);

/* destroy jitterbuf */
void			jb_destroy(jitterbuf *jb);

/* reset jitterbuf */
/* NOTE:  The jitterbuffer should be empty before you call this, otherwise
 * you will leak queued frames, and some internal structures */
void			jb_reset(jitterbuf *jb);

/* queue a frame data=frame data, timings (in ms): ms=length of frame (for voice), ts=ts (sender's time) 
 * now=now (in receiver's time)*/
int 			jb_put(jitterbuf *jb, void *data, int type, long ms, long ts, long now);

/* get a frame for time now (receiver's time)  return value is one of
 * JB_OK:  You've got frame!
 * JB_DROP: Here's an audio frame you should just drop.  Ask me again for this time..
 * JB_NOFRAME: There's no frame scheduled for this time.
 * JB_INTERP: Please interpolate an audio frame for this time (either we need to grow, or there was a lost frame 
 * JB_EMPTY: The jb is empty.
 */
int			jb_get(jitterbuf *jb, jb_frame *frame, long now);

/* unconditionally get frames from jitterbuf until empty */
int jb_getall(jitterbuf *jb, jb_frame *frameout);

/* when is the next frame due out, in receiver's time (0=EMPTY) 
 * This value may change as frames are added (esp non-audio frames) */
long			jb_next(jitterbuf *jb);

/* get jitterbuf info: only "statistics" may be valid */
int			jb_getinfo(jitterbuf *jb, jb_info *stats);

/* set jitterbuf info: only "settings" may be honored */
int			jb_setinfo(jitterbuf *jb, jb_info *settings);

typedef 		void (*jb_output_function_t)(const char *fmt, ...);
void 			jb_setoutput(jb_output_function_t warn, jb_output_function_t err, jb_output_function_t dbg);

#ifdef __cplusplus
}
#endif


#endif

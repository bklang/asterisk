/*
 * Asterisk -- A telephony toolkit for Linux.
 *
 * Use /dev/dsp as an intercom.
 * 
 * Copyright (C) 1999, Mark Spencer
 *
 * Mark Spencer <markster@linux-support.net>
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License
 */
 
#include <asterisk/lock.h>
#include <asterisk/file.h>
#include <asterisk/frame.h>
#include <asterisk/logger.h>
#include <asterisk/channel.h>
#include <asterisk/pbx.h>
#include <asterisk/module.h>
#include <asterisk/translate.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/time.h>
#ifdef __linux__
#include <linux/soundcard.h>
#elif defined(__FreeBSD__)
#include <machine/soundcard.h>
#else
#include <soundcard.h>
#endif
#include <netinet/in.h>

#ifdef __OpenBSD__
#define DEV_DSP "/dev/audio"
#else
#define DEV_DSP "/dev/dsp"
#endif

/* Number of 32 byte buffers -- each buffer is 2 ms */
#define BUFFER_SIZE 32

static char *tdesc = "Intercom using /dev/dsp for output";

static char *app = "Intercom";

static char *synopsis = "(Obsolete) Send to Intercom";
static char *descrip = 
"  Intercom(): Sends the user to the intercom (i.e. /dev/dsp).  This program\n"
"is generally considered  obselete by the chan_oss module.  Returns 0 if the\n"
"user exits with a DTMF tone, or -1 if they hangup.\n";

STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

static ast_mutex_t sound_lock = AST_MUTEX_INITIALIZER;
static int sound = -1;

static int write_audio(short *data, int len)
{
	int res;
	struct audio_buf_info info;
	ast_mutex_lock(&sound_lock);
	if (sound < 0) {
		ast_log(LOG_WARNING, "Sound device closed?\n");
		ast_mutex_unlock(&sound_lock);
		return -1;
	}
    if (ioctl(sound, SNDCTL_DSP_GETOSPACE, &info)) {
		ast_log(LOG_WARNING, "Unable to read output space\n");
		ast_mutex_unlock(&sound_lock);
        return -1;
    }
	res = write(sound, data, len);
	ast_mutex_unlock(&sound_lock);
	return res;
}

static int create_audio(void)
{
	int fmt, desired, res, fd;
	fd = open(DEV_DSP, O_WRONLY);
	if (fd < 0) {
		ast_log(LOG_WARNING, "Unable to open %s: %s\n", DEV_DSP, strerror(errno));
		close(fd);
		return -1;
	}
	fmt = AFMT_S16_LE;
	res = ioctl(fd, SNDCTL_DSP_SETFMT, &fmt);
	if (res < 0) {
		ast_log(LOG_WARNING, "Unable to set format to 16-bit signed\n");
		close(fd);
		return -1;
	}
	fmt = 0;
	res = ioctl(fd, SNDCTL_DSP_STEREO, &fmt);
	if (res < 0) {
		ast_log(LOG_WARNING, "Failed to set audio device to mono\n");
		close(fd);
		return -1;
	}
	/* 8000 Hz desired */
	desired = 8000;
	fmt = desired;
	res = ioctl(fd, SNDCTL_DSP_SPEED, &fmt);
	if (res < 0) {
		ast_log(LOG_WARNING, "Failed to set audio device to mono\n");
		close(fd);
		return -1;
	}
	if (fmt != desired) {
		ast_log(LOG_WARNING, "Requested %d Hz, got %d Hz -- sound may be choppy\n", desired, fmt);
	}
#if 1
	/* 2 bytes * 15 units of 2^5 = 32 bytes per buffer */
	fmt = ((BUFFER_SIZE) << 16) | (0x0005);
	res = ioctl(fd, SNDCTL_DSP_SETFRAGMENT, &fmt);
	if (res < 0) {
		ast_log(LOG_WARNING, "Unable to set fragment size -- sound may be choppy\n");
	}
#endif
	sound = fd;
	return 0;
}

static int intercom_exec(struct ast_channel *chan, void *data)
{
	int res = 0;
	struct localuser *u;
	struct ast_frame *f;
	int oreadformat;
	LOCAL_USER_ADD(u);
	/* Remember original read format */
	oreadformat = chan->readformat;
	/* Set mode to signed linear */
	res = ast_set_read_format(chan, AST_FORMAT_SLINEAR);
	if (res < 0) {
		ast_log(LOG_WARNING, "Unable to set format to signed linear on channel %s\n", chan->name);
		return -1;
	}
	/* Read packets from the channel */
	while(!res) {
		res = ast_waitfor(chan, -1);
		if (res > 0) {
			res = 0;
			f = ast_read(chan);
			if (f) {
				if (f->frametype == AST_FRAME_DTMF) {
					ast_frfree(f);
					break;
				} else {
					if (f->frametype == AST_FRAME_VOICE) {
						if (f->subclass == AST_FORMAT_SLINEAR) {
							res = write_audio(f->data, f->datalen);
							if (res > 0)
								res = 0;
						} else
							ast_log(LOG_DEBUG, "Unable to handle non-signed linear frame (%d)\n", f->subclass);
					} 
				}
				ast_frfree(f);
			} else
				res = -1;
		}
	}
	LOCAL_USER_REMOVE(u);
	if (!res)
		ast_set_read_format(chan, oreadformat);
	return res;
}

int unload_module(void)
{
	STANDARD_HANGUP_LOCALUSERS;
	if (sound > -1)
		close(sound);
	return ast_unregister_application(app);
}

int load_module(void)
{
	if (create_audio())
		return -1;
	return ast_register_application(app, intercom_exec, synopsis, descrip);
}

char *description(void)
{
	return tdesc;
}

int usecount(void)
{
	int res;
	STANDARD_USECOUNT(res);
	return res;
}

char *key()
{
	return ASTERISK_GPL_KEY;
}

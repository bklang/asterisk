/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2011, Digium, Inc.
 *
 * Joshua Colp <jcolp@digium.com>
 * David Vossel <dvossel@digium.com>
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*! \file
 *
 * \brief Multi-party software based channel mixing
 *
 * \author Joshua Colp <jcolp@digium.com>
 * \author David Vossel <dvossel@digium.com>
 *
 * \ingroup bridges
 */

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>

#include "asterisk/module.h"
#include "asterisk/channel.h"
#include "asterisk/bridging.h"
#include "asterisk/bridging_technology.h"
#include "asterisk/frame.h"
#include "asterisk/options.h"
#include "asterisk/logger.h"
#include "asterisk/slinfactory.h"
#include "asterisk/astobj2.h"
#include "asterisk/timing.h"
#include "asterisk/translate.h"

#define MAX_DATALEN 8096

/*! \brief Interval at which mixing will take place. Valid options are 10, 20, and 40. */
#define DEFAULT_SOFTMIX_INTERVAL 20

/*! \brief Size of the buffer used for sample manipulation */
#define SOFTMIX_DATALEN(rate, interval) ((rate/50) * (interval / 10))

/*! \brief Number of samples we are dealing with */
#define SOFTMIX_SAMPLES(rate, interval) (SOFTMIX_DATALEN(rate, interval) / 2)

/*! \brief Number of mixing iterations to perform between gathering statistics. */
#define SOFTMIX_STAT_INTERVAL 100

/* This is the threshold in ms at which a channel's own audio will stop getting
 * mixed out its own write audio stream because it is not talking. */
#define DEFAULT_SOFTMIX_SILENCE_THRESHOLD 2500
#define DEFAULT_SOFTMIX_TALKING_THRESHOLD 160

/*! \brief Structure which contains per-channel mixing information */
struct softmix_channel {
	/*! Lock to protect this structure */
	ast_mutex_t lock;
	/*! Factory which contains audio read in from the channel */
	struct ast_slinfactory factory;
	/*! Frame that contains mixed audio to be written out to the channel */
	struct ast_frame write_frame;
	/*! Frame that contains mixed audio read from the channel */
	struct ast_frame read_frame;
	/*! DSP for detecting silence */
	struct ast_dsp *dsp;
	/*! Bit used to indicate if a channel is talking or not. This affects how
	 * the channel's audio is mixed back to it. */
	int talking:1;
	/*! Bit used to indicate that the channel provided audio for this mixing interval */
	int have_audio:1;
	/*! Bit used to indicate that a frame is available to be written out to the channel */
	int have_frame:1;
	/*! Buffer containing final mixed audio from all sources */
	short final_buf[MAX_DATALEN];
	/*! Buffer containing only the audio from the channel */
	short our_buf[MAX_DATALEN];
};

struct softmix_bridge_data {
	struct ast_timer *timer;
	unsigned int internal_rate;
	unsigned int internal_mixing_interval;
};

struct softmix_stats {
		/*! Each index represents a sample rate used above the internal rate. */
		unsigned int sample_rates[16];
		/*! Each index represents the number of channels using the same index in the sample_rates array.  */
		unsigned int num_channels[16];
		/*! the number of channels above the internal sample rate */
		unsigned int num_above_internal_rate;
		/*! the number of channels at the internal sample rate */
		unsigned int num_at_internal_rate;
		/*! the absolute highest sample rate supported by any channel in the bridge */
		unsigned int highest_supported_rate;
		/*! Is the sample rate locked by the bridge, if so what is that rate.*/
		unsigned int locked_rate;
};

struct softmix_mixing_array {
	int max_num_entries;
	int used_entries;
	int16_t **buffers;
};

struct softmix_translate_helper_entry {
	int num_times_requested; /*!< Once this entry is no longer requested, free the trans_pvt
	                              and re-init if it was usable. */
	struct ast_format dst_format; /*!< The destination format for this helper */
	struct ast_trans_pvt *trans_pvt; /*!< the translator for this slot. */
	struct ast_frame *out_frame; /*!< The output frame from the last translation */
	AST_LIST_ENTRY(softmix_translate_helper_entry) entry;
};

struct softmix_translate_helper {
	struct ast_format slin_src; /*!< the source format expected for all the translators */
	AST_LIST_HEAD_NOLOCK(, softmix_translate_helper_entry) entries;
};

static struct softmix_translate_helper_entry *softmix_translate_helper_entry_alloc(struct ast_format *dst)
{
	struct softmix_translate_helper_entry *entry;
	if (!(entry = ast_calloc(1, sizeof(*entry)))) {
		return NULL;
	}
	ast_format_copy(&entry->dst_format, dst);
	return entry;
}

static void *softmix_translate_helper_free_entry(struct softmix_translate_helper_entry *entry)
{
	if (entry->trans_pvt) {
		ast_translator_free_path(entry->trans_pvt);
	}
	if (entry->out_frame) {
		ast_frfree(entry->out_frame);
	}
	ast_free(entry);
	return NULL;
}

static void softmix_translate_helper_init(struct softmix_translate_helper *trans_helper, unsigned int sample_rate)
{
	memset(trans_helper, 0, sizeof(*trans_helper));
	ast_format_set(&trans_helper->slin_src, ast_format_slin_by_rate(sample_rate), 0);
}

static void softmix_translate_helper_destroy(struct softmix_translate_helper *trans_helper)
{
	struct softmix_translate_helper_entry *entry;

	while ((entry = AST_LIST_REMOVE_HEAD(&trans_helper->entries, entry))) {
		softmix_translate_helper_free_entry(entry);
	}
}

static void softmix_translate_helper_change_rate(struct softmix_translate_helper *trans_helper, unsigned int sample_rate)
{
	struct softmix_translate_helper_entry *entry;

	ast_format_set(&trans_helper->slin_src, ast_format_slin_by_rate(sample_rate), 0);
	AST_LIST_TRAVERSE_SAFE_BEGIN(&trans_helper->entries, entry, entry) {
		if (entry->trans_pvt) {
			ast_translator_free_path(entry->trans_pvt);
			if (!(entry->trans_pvt = ast_translator_build_path(&entry->dst_format, &trans_helper->slin_src))) {
				AST_LIST_REMOVE_CURRENT(entry);
				entry = softmix_translate_helper_free_entry(entry);
			}
		}
	}
	AST_LIST_TRAVERSE_SAFE_END;
}

/*!
 * \internal
 * \brief Get the next available audio on the softmix channel's read stream
 * and determine if it should be mixed out or not on the write stream. 
 *
 * \retval pointer to buffer containing the exact number of samples requested on success.
 * \retval NULL if no samples are present
 */
static int16_t *softmix_process_read_audio(struct softmix_channel *sc, unsigned int num_samples)
{
	if ((ast_slinfactory_available(&sc->factory) >= num_samples) &&
		ast_slinfactory_read(&sc->factory, sc->our_buf, num_samples)) {
		sc->have_audio = 1;
		return sc->our_buf;
	}
	sc->have_audio = 0;
	return NULL;
}

/*!
 * \internal
 * \brief Process a softmix channel's write audio
 *
 * \details This function will remove the channel's talking from its own audio if present and
 * possibly even do the channel's write translation for it depending on how many other
 * channels use the same write format.
 */
static void softmix_process_write_audio(struct softmix_translate_helper *trans_helper,
	struct ast_format *raw_write_fmt,
	struct softmix_channel *sc)
{
	struct softmix_translate_helper_entry *entry = NULL;
	int i;

	/* If we provided audio that was not determined to be silence,
	 * then take it out while in slinear format. */
	if (sc->have_audio && sc->talking) {
		for (i = 0; i < sc->write_frame.samples; i++) {
			ast_slinear_saturated_subtract(&sc->final_buf[i], &sc->our_buf[i]);
		}
		/* do not do any special write translate optimization if we had to make
		 * a special mix for them to remove their own audio. */
		return;
	}

	AST_LIST_TRAVERSE(&trans_helper->entries, entry, entry) {
		if (ast_format_cmp(&entry->dst_format, raw_write_fmt) == AST_FORMAT_CMP_EQUAL) {
			entry->num_times_requested++;
		} else {
			continue;
		}
		if (!entry->trans_pvt && (entry->num_times_requested > 1)) {
			entry->trans_pvt = ast_translator_build_path(&entry->dst_format, &trans_helper->slin_src);
		}
		if (entry->trans_pvt && !entry->out_frame) {
			entry->out_frame = ast_translate(entry->trans_pvt, &sc->write_frame, 0);
		}
		if (entry->out_frame && (entry->out_frame->datalen < MAX_DATALEN)) {
			ast_format_copy(&sc->write_frame.subclass.format, &entry->out_frame->subclass.format);
			memcpy(sc->final_buf, entry->out_frame->data.ptr, entry->out_frame->datalen);
			sc->write_frame.datalen = entry->out_frame->datalen;
			sc->write_frame.samples = entry->out_frame->samples;
		}
		break;
	}

	/* add new entry into list if this format destination was not matched. */
	if (!entry && (entry = softmix_translate_helper_entry_alloc(raw_write_fmt))) {
		AST_LIST_INSERT_HEAD(&trans_helper->entries, entry, entry);
	}
}

static void softmix_translate_helper_cleanup(struct softmix_translate_helper *trans_helper)
{
	struct softmix_translate_helper_entry *entry = NULL;
	AST_LIST_TRAVERSE(&trans_helper->entries, entry, entry) {
		if (entry->out_frame) {
			ast_frfree(entry->out_frame);
			entry->out_frame = NULL;
		}
		entry->num_times_requested = 0;
	}
}

static void softmix_bridge_data_destroy(void *obj)
{
	struct softmix_bridge_data *softmix_data = obj;
	ast_timer_close(softmix_data->timer);
}

/*! \brief Function called when a bridge is created */
static int softmix_bridge_create(struct ast_bridge *bridge)
{
	struct softmix_bridge_data *softmix_data;

	if (!(softmix_data = ao2_alloc(sizeof(*softmix_data), softmix_bridge_data_destroy))) {
		return -1;
	}
	if (!(softmix_data->timer = ast_timer_open())) {
		ao2_ref(softmix_data, -1);
		return -1;
	}

	/* start at 8khz, let it grow from there */
	softmix_data->internal_rate = 8000;
	softmix_data->internal_mixing_interval = DEFAULT_SOFTMIX_INTERVAL;

	bridge->bridge_pvt = softmix_data;
	return 0;
}

/*! \brief Function called when a bridge is destroyed */
static int softmix_bridge_destroy(struct ast_bridge *bridge)
{
	struct softmix_bridge_data *softmix_data = bridge->bridge_pvt;
	if (!bridge->bridge_pvt) {
		return -1;
	}
	ao2_ref(softmix_data, -1);
	bridge->bridge_pvt = NULL;
	return 0;
}

static void set_softmix_bridge_data(int rate, int interval, struct ast_bridge_channel *bridge_channel, int reset)
{
	struct softmix_channel *sc = bridge_channel->bridge_pvt;
	unsigned int channel_read_rate = ast_format_rate(&bridge_channel->chan->rawreadformat);

	ast_mutex_lock(&sc->lock);
	if (reset) {
		ast_slinfactory_destroy(&sc->factory);
		ast_dsp_free(sc->dsp);
	}
	/* Setup read/write frame parameters */
	sc->write_frame.frametype = AST_FRAME_VOICE;
	ast_format_set(&sc->write_frame.subclass.format, ast_format_slin_by_rate(rate), 0);
	sc->write_frame.data.ptr = sc->final_buf;
	sc->write_frame.datalen = SOFTMIX_DATALEN(rate, interval);
	sc->write_frame.samples = SOFTMIX_SAMPLES(rate, interval);

	sc->read_frame.frametype = AST_FRAME_VOICE;
	ast_format_set(&sc->read_frame.subclass.format, ast_format_slin_by_rate(channel_read_rate), 0);
	sc->read_frame.data.ptr = sc->our_buf;
	sc->read_frame.datalen = SOFTMIX_DATALEN(channel_read_rate, interval);
	sc->read_frame.samples = SOFTMIX_SAMPLES(channel_read_rate, interval);

	/* Setup smoother */
	ast_slinfactory_init_with_format(&sc->factory, &sc->write_frame.subclass.format);

	/* set new read and write formats on channel. */
	ast_set_read_format(bridge_channel->chan, &sc->read_frame.subclass.format);
	ast_set_write_format(bridge_channel->chan, &sc->write_frame.subclass.format);

	/* set up new DSP.  This is on the read side only right before the read frame enters the smoother.  */
	sc->dsp = ast_dsp_new_with_rate(channel_read_rate);
	/* we want to aggressively detect silence to avoid feedback */
	if (bridge_channel->tech_args.talking_threshold) {
		ast_dsp_set_threshold(sc->dsp, bridge_channel->tech_args.talking_threshold);
	} else {
		ast_dsp_set_threshold(sc->dsp, DEFAULT_SOFTMIX_TALKING_THRESHOLD);
	}

	ast_mutex_unlock(&sc->lock);
}

/*! \brief Function called when a channel is joined into the bridge */
static int softmix_bridge_join(struct ast_bridge *bridge, struct ast_bridge_channel *bridge_channel)
{
	struct softmix_channel *sc = NULL;
	struct softmix_bridge_data *softmix_data = bridge->bridge_pvt;

	/* Create a new softmix_channel structure and allocate various things on it */
	if (!(sc = ast_calloc(1, sizeof(*sc)))) {
		return -1;
	}

	/* Can't forget the lock */
	ast_mutex_init(&sc->lock);

	/* Can't forget to record our pvt structure within the bridged channel structure */
	bridge_channel->bridge_pvt = sc;

	set_softmix_bridge_data(softmix_data->internal_rate,
		softmix_data->internal_mixing_interval ? softmix_data->internal_mixing_interval : DEFAULT_SOFTMIX_INTERVAL,
		bridge_channel, 0);

	return 0;
}

/*! \brief Function called when a channel leaves the bridge */
static int softmix_bridge_leave(struct ast_bridge *bridge, struct ast_bridge_channel *bridge_channel)
{
	struct softmix_channel *sc = bridge_channel->bridge_pvt;

	if (!(bridge_channel->bridge_pvt)) {
		return 0;
	}
	bridge_channel->bridge_pvt = NULL;

	/* Drop mutex lock */
	ast_mutex_destroy(&sc->lock);

	/* Drop the factory */
	ast_slinfactory_destroy(&sc->factory);

	/* Drop the DSP */
	ast_dsp_free(sc->dsp);

	/* Eep! drop ourselves */
	ast_free(sc);

	return 0;
}

/*!
 * \internal
 * \brief If the bridging core passes DTMF to us, then they want it to be distributed out to all memebers. Do that here.
 */
static void softmix_pass_dtmf(struct ast_bridge *bridge, struct ast_bridge_channel *bridge_channel, struct ast_frame *frame)
{
	struct ast_bridge_channel *tmp;
	AST_LIST_TRAVERSE(&bridge->channels, tmp, entry) {
		if (tmp == bridge_channel) {
			continue;
		}
		ast_write(tmp->chan, frame);
	}
}

/*! \brief Function called when a channel writes a frame into the bridge */
static enum ast_bridge_write_result softmix_bridge_write(struct ast_bridge *bridge, struct ast_bridge_channel *bridge_channel, struct ast_frame *frame)
{
	struct softmix_channel *sc = bridge_channel->bridge_pvt;
	struct softmix_bridge_data *softmix_data = bridge->bridge_pvt;
	int totalsilence = 0;
	int silence_threshold = bridge_channel->tech_args.silence_threshold ?
		bridge_channel->tech_args.silence_threshold :
		DEFAULT_SOFTMIX_SILENCE_THRESHOLD;
	char update_talking = -1;  /* if this is set to 0 or 1, tell the bridge that the channel has started or stopped talking. */
	int res = AST_BRIDGE_WRITE_SUCCESS;

	/* Only accept audio frames, all others are unsupported */
	if (frame->frametype == AST_FRAME_DTMF_END || frame->frametype == AST_FRAME_DTMF_BEGIN) {
		softmix_pass_dtmf(bridge, bridge_channel, frame);
		goto no_audio;
	} else if (frame->frametype != AST_FRAME_VOICE) {
		res = AST_BRIDGE_WRITE_UNSUPPORTED;
		goto no_audio;
	} else if (frame->datalen == 0) {
		goto no_audio;
	}

	/* If we made it here, we are going to write the frame into the conference */
	ast_mutex_lock(&sc->lock);

	ast_dsp_silence(sc->dsp, frame, &totalsilence);
	if (totalsilence < silence_threshold) {
		if (!sc->talking) {
			update_talking = 1;
		}
		sc->talking = 1; /* tell the write process we have audio to be mixed out */
	} else {
		if (sc->talking) {
			update_talking = 0;
		}
		sc->talking = 0;
	}

	/* Before adding audio in, make sure we haven't fallen behind. If audio has fallen
	 * behind 4 times the amount of samples mixed on every iteration of the mixer, Re-sync
	 * the audio by flushing the buffer before adding new audio in. */
	if (ast_slinfactory_available(&sc->factory) > (4 * SOFTMIX_SAMPLES(softmix_data->internal_rate, softmix_data->internal_mixing_interval))) {
		ast_slinfactory_flush(&sc->factory);
	}

	/* If a frame was provided add it to the smoother, unless drop silence is enabled and this frame
	 * is not determined to be talking. */
	if (!(bridge_channel->tech_args.drop_silence && !sc->talking) &&
		(frame->frametype == AST_FRAME_VOICE && ast_format_is_slinear(&frame->subclass.format))) {
		ast_slinfactory_feed(&sc->factory, frame);
	}

	/* If a frame is ready to be written out, do so */
	if (sc->have_frame) {
		ast_write(bridge_channel->chan, &sc->write_frame);
		sc->have_frame = 0;
	}

	/* Alllll done */
	ast_mutex_unlock(&sc->lock);

	if (update_talking != -1) {
		ast_bridge_notify_talking(bridge, bridge_channel, update_talking);
	}

	return res;

no_audio:
	/* Even though the frame is not being written into the conference because it is not audio,
	 * we should use this opportunity to check to see if a frame is ready to be written out from
	 * the conference to the channel. */
	ast_mutex_lock(&sc->lock);
	if (sc->have_frame) {
		ast_write(bridge_channel->chan, &sc->write_frame);
		sc->have_frame = 0;
	}
	ast_mutex_unlock(&sc->lock);

	return res;
}

/*! \brief Function called when the channel's thread is poked */
static int softmix_bridge_poke(struct ast_bridge *bridge, struct ast_bridge_channel *bridge_channel)
{
	struct softmix_channel *sc = bridge_channel->bridge_pvt;

	ast_mutex_lock(&sc->lock);

	if (sc->have_frame) {
		ast_write(bridge_channel->chan, &sc->write_frame);
		sc->have_frame = 0;
	}

	ast_mutex_unlock(&sc->lock);

	return 0;
}

static void gather_softmix_stats(struct softmix_stats *stats,
	const struct softmix_bridge_data *softmix_data,
	struct ast_bridge_channel *bridge_channel)
{
	int channel_native_rate;
	int i;
	/* Gather stats about channel sample rates. */
	channel_native_rate = MAX(ast_format_rate(&bridge_channel->chan->rawwriteformat),
		ast_format_rate(&bridge_channel->chan->rawreadformat));

	if (channel_native_rate > stats->highest_supported_rate) {
		stats->highest_supported_rate = channel_native_rate;
	}
	if (channel_native_rate > softmix_data->internal_rate) {
		for (i = 0; i < ARRAY_LEN(stats->sample_rates); i++) {
			if (stats->sample_rates[i] == channel_native_rate) {
				stats->num_channels[i]++;
				break;
			} else if (!stats->sample_rates[i]) {
				stats->sample_rates[i] = channel_native_rate;
				stats->num_channels[i]++;
				break;
			}
		}
		stats->num_above_internal_rate++;
	} else if (channel_native_rate == softmix_data->internal_rate) {
		stats->num_at_internal_rate++;
	}
}
/*!
 * \internal
 * \brief Analyse mixing statistics and change bridges internal rate
 * if necessary.
 *
 * \retval 0, no changes to internal rate 
 * \ratval 1, internal rate was changed, update all the channels on the next mixing iteration.
 */
static unsigned int analyse_softmix_stats(struct softmix_stats *stats, struct softmix_bridge_data *softmix_data)
{
	int i;
	/* Re-adjust the internal bridge sample rate if
	 * 1. The bridge's internal sample rate is locked in at a sample
	 *    rate other than the current sample rate being used.
	 * 2. two or more channels support a higher sample rate
	 * 3. no channels support the current sample rate or a higher rate
	 */
	if (stats->locked_rate) {
		/* if the rate is locked by the bridge, only update it if it differs
		 * from the current rate we are using. */
		if (softmix_data->internal_rate != stats->locked_rate) {
			softmix_data->internal_rate = stats->locked_rate;
			ast_debug(1, " Bridge is locked in at sample rate %d\n", softmix_data->internal_rate);
			return 1;
		}
	} else if (stats->num_above_internal_rate >= 2) {
		/* the highest rate is just used as a starting point */
		unsigned int best_rate = stats->highest_supported_rate;
		int best_index = -1;

		for (i = 0; i < ARRAY_LEN(stats->num_channels); i++) {
			if (stats->num_channels[i]) {
				break;
			}
			/* best_rate starts out being the first sample rate
			 * greater than the internal sample rate that 2 or
			 * more channels support. */
			if (stats->num_channels[i] >= 2 && (best_index == -1)) {
				best_rate = stats->sample_rates[i];
				best_index = i;
			/* If it has been detected that multiple rates above
			 * the internal rate are present, compare those rates
			 * to each other and pick the highest one two or more
			 * channels support. */
			} else if (((best_index != -1) &&
				(stats->num_channels[i] >= 2) &&
				(stats->sample_rates[best_index] < stats->sample_rates[i]))) {
				best_rate = stats->sample_rates[i];
				best_index = i;
			/* It is possible that multiple channels exist with native sample
			 * rates above the internal sample rate, but none of those channels
			 * have the same rate in common.  In this case, the lowest sample
			 * rate among those channels is picked. Over time as additional
			 * statistic runs are made the internal sample rate number will
			 * adjust to the most optimal sample rate, but it may take multiple
			 * iterations. */
			} else if (best_index == -1) {
				best_rate = MIN(best_rate, stats->sample_rates[i]);
			}
		}

		ast_debug(1, " Bridge changed from %d To %d\n", softmix_data->internal_rate, best_rate);
		softmix_data->internal_rate = best_rate;
		return 1;
	} else if (!stats->num_at_internal_rate && !stats->num_above_internal_rate) {
		/* In this case, the highest supported rate is actually lower than the internal rate */
		softmix_data->internal_rate = stats->highest_supported_rate;
		ast_debug(1, " Bridge changed from %d to %d\n", softmix_data->internal_rate, stats->highest_supported_rate);
		return 1;
	}
	return 0;
}

static int softmix_mixing_array_init(struct softmix_mixing_array *mixing_array, unsigned int starting_num_entries)
{
	memset(mixing_array, 0, sizeof(*mixing_array));
	mixing_array->max_num_entries = starting_num_entries;
	if (!(mixing_array->buffers = ast_calloc(mixing_array->max_num_entries, sizeof(int16_t *)))) {
		ast_log(LOG_NOTICE, "Failed to allocate softmix mixing structure. \n");
		return -1;
	}
	return 0;
}

static void softmix_mixing_array_destroy(struct softmix_mixing_array *mixing_array)
{
	ast_free(mixing_array->buffers);
}

static int softmix_mixing_array_grow(struct softmix_mixing_array *mixing_array, unsigned int num_entries)
{
	int16_t **tmp;
	/* give it some room to grow since memory is cheap but allocations can be expensive */
	mixing_array->max_num_entries = num_entries;
	if (!(tmp = ast_realloc(mixing_array->buffers, (mixing_array->max_num_entries * sizeof(int16_t *))))) {
		ast_log(LOG_NOTICE, "Failed to re-allocate softmix mixing structure. \n");
		return -1;
	}
	mixing_array->buffers = tmp;
	return 0;
}

/*! \brief Function which acts as the mixing thread */
static int softmix_bridge_thread(struct ast_bridge *bridge)
{
	struct softmix_stats stats = { { 0 }, };
	struct softmix_mixing_array mixing_array;
	struct softmix_bridge_data *softmix_data = bridge->bridge_pvt;
	struct ast_timer *timer;
	struct softmix_translate_helper trans_helper;
	int16_t buf[MAX_DATALEN] = { 0, };
	unsigned int stat_iteration_counter = 0; /* counts down, gather stats at zero and reset. */
	int timingfd;
	int update_all_rates = 0; /* set this when the internal sample rate has changed */
	int i, x;
	int res = -1;

	if (!(softmix_data = bridge->bridge_pvt)) {
		goto softmix_cleanup;
	}

	ao2_ref(softmix_data, 1);
	timer = softmix_data->timer;
	timingfd = ast_timer_fd(timer);
	softmix_translate_helper_init(&trans_helper, softmix_data->internal_rate);
	ast_timer_set_rate(timer, (1000 / softmix_data->internal_mixing_interval));

	/* Give the mixing array room to grow, memory is cheap but allocations are expensive. */
	if (softmix_mixing_array_init(&mixing_array, bridge->num + 10)) {
		ast_log(LOG_NOTICE, "Failed to allocate softmix mixing structure. \n");
		goto softmix_cleanup;
	}

	while (!bridge->stop && !bridge->refresh && bridge->array_num) {
		struct ast_bridge_channel *bridge_channel = NULL;
		int timeout = -1;
		enum ast_format_id cur_slin_id = ast_format_slin_by_rate(softmix_data->internal_rate);
		unsigned int softmix_samples = SOFTMIX_SAMPLES(softmix_data->internal_rate, softmix_data->internal_mixing_interval);
		unsigned int softmix_datalen = SOFTMIX_DATALEN(softmix_data->internal_rate, softmix_data->internal_mixing_interval);

		if (softmix_datalen > MAX_DATALEN) {
			/* This should NEVER happen, but if it does we need to know about it. Almost
			 * all the memcpys used during this process depend on this assumption.  Rather
			 * than checking this over and over again through out the code, this single
			 * verification is done on each iteration. */
			ast_log(LOG_WARNING, "Conference mixing error, requested mixing length greater than mixing buffer.\n");
			goto softmix_cleanup;
		}

		/* Grow the mixing array buffer as participants are added. */
		if (mixing_array.max_num_entries < bridge->num && softmix_mixing_array_grow(&mixing_array, bridge->num + 5)) {
			goto softmix_cleanup;
		}

		/* init the number of buffers stored in the mixing array to 0.
		 * As buffers are added for mixing, this number is incremented. */
		mixing_array.used_entries = 0;

		/* These variables help determine if a rate change is required */
		if (!stat_iteration_counter) {
			memset(&stats, 0, sizeof(stats));
			stats.locked_rate = bridge->internal_sample_rate;
		}

		/* If the sample rate has changed, update the translator helper */
		if (update_all_rates) {
			softmix_translate_helper_change_rate(&trans_helper, softmix_data->internal_rate);
		}

		/* Go through pulling audio from each factory that has it available */
		AST_LIST_TRAVERSE(&bridge->channels, bridge_channel, entry) {
			struct softmix_channel *sc = bridge_channel->bridge_pvt;

			/* Update the sample rate to match the bridge's native sample rate if necessary. */
			if (update_all_rates) {
				set_softmix_bridge_data(softmix_data->internal_rate, softmix_data->internal_mixing_interval, bridge_channel, 1);
			}

			/* If stat_iteration_counter is 0, then collect statistics during this mixing interation */
			if (!stat_iteration_counter) {
				gather_softmix_stats(&stats, softmix_data, bridge_channel);
			}

			/* if the channel is suspended, don't check for audio, but still gather stats */
			if (bridge_channel->suspended) {
				continue;
			}

			/* Try to get audio from the factory if available */
			ast_mutex_lock(&sc->lock);
			if ((mixing_array.buffers[mixing_array.used_entries] = softmix_process_read_audio(sc, softmix_samples))) {
				mixing_array.used_entries++;
			}
			ast_mutex_unlock(&sc->lock);
		}

		/* mix it like crazy */
		memset(buf, 0, softmix_datalen);
		for (i = 0; i < mixing_array.used_entries; i++) {
			for (x = 0; x < softmix_samples; x++) {
				ast_slinear_saturated_add(buf + x, mixing_array.buffers[i] + x);
			}
		}

		/* Next step go through removing the channel's own audio and creating a good frame... */
		AST_LIST_TRAVERSE(&bridge->channels, bridge_channel, entry) {
			struct softmix_channel *sc = bridge_channel->bridge_pvt;

			if (bridge_channel->suspended) {
				continue;
			}

			ast_mutex_lock(&sc->lock);

			/* Make SLINEAR write frame from local buffer */
			if (sc->write_frame.subclass.format.id != cur_slin_id) {
				ast_format_set(&sc->write_frame.subclass.format, cur_slin_id, 0);
			}
			sc->write_frame.datalen = softmix_datalen;
			sc->write_frame.samples = softmix_samples;
			memcpy(sc->final_buf, buf, softmix_datalen);

			/* process the softmix channel's new write audio */
			softmix_process_write_audio(&trans_helper, &bridge_channel->chan->rawwriteformat, sc);

			/* The frame is now ready for use... */
			sc->have_frame = 1;

			ast_mutex_unlock(&sc->lock);

			/* Poke bridged channel thread just in case */
			pthread_kill(bridge_channel->thread, SIGURG);
		}

		update_all_rates = 0;
		if (!stat_iteration_counter) {
			update_all_rates = analyse_softmix_stats(&stats, softmix_data);
			stat_iteration_counter = SOFTMIX_STAT_INTERVAL;
		}
		stat_iteration_counter--;

		ao2_unlock(bridge);
		/* cleanup any translation frame data from the previous mixing iteration. */
		softmix_translate_helper_cleanup(&trans_helper);
		/* Wait for the timing source to tell us to wake up and get things done */
		ast_waitfor_n_fd(&timingfd, 1, &timeout, NULL);
		ast_timer_ack(timer, 1);
		ao2_lock(bridge);

		/* make sure to detect mixing interval changes if they occur. */
		if (bridge->internal_mixing_interval && (bridge->internal_mixing_interval != softmix_data->internal_mixing_interval)) {
			softmix_data->internal_mixing_interval = bridge->internal_mixing_interval;
			ast_timer_set_rate(timer, (1000 / softmix_data->internal_mixing_interval));
			update_all_rates = 1; /* if the interval changes, the rates must be adjusted as well just to be notified new interval.*/
		}
	}

	res = 0;

softmix_cleanup:
	softmix_translate_helper_destroy(&trans_helper);
	softmix_mixing_array_destroy(&mixing_array);
	if (softmix_data) {
		ao2_ref(softmix_data, -1);
	}
	return res;
}

static struct ast_bridge_technology softmix_bridge = {
	.name = "softmix",
	.capabilities = AST_BRIDGE_CAPABILITY_MULTIMIX | AST_BRIDGE_CAPABILITY_THREAD | AST_BRIDGE_CAPABILITY_MULTITHREADED | AST_BRIDGE_CAPABILITY_OPTIMIZE,
	.preference = AST_BRIDGE_PREFERENCE_LOW,
	.create = softmix_bridge_create,
	.destroy = softmix_bridge_destroy,
	.join = softmix_bridge_join,
	.leave = softmix_bridge_leave,
	.write = softmix_bridge_write,
	.thread = softmix_bridge_thread,
	.poke = softmix_bridge_poke,
};

static int unload_module(void)
{
	ast_format_cap_destroy(softmix_bridge.format_capabilities);
	return ast_bridge_technology_unregister(&softmix_bridge);
}

static int load_module(void)
{
	struct ast_format tmp;
	if (!(softmix_bridge.format_capabilities = ast_format_cap_alloc())) {
		return AST_MODULE_LOAD_DECLINE;
	}
	ast_format_cap_add(softmix_bridge.format_capabilities, ast_format_set(&tmp, AST_FORMAT_SLINEAR, 0));
	return ast_bridge_technology_register(&softmix_bridge);
}

AST_MODULE_INFO_STANDARD(ASTERISK_GPL_KEY, "Multi-party software based channel mixing");

/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2006, Digium, Inc.
 *
 * Joshua Colp <jcolp@digium.com>
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
 * \brief Generic Speech Recognition API
 *
 * \author Joshua Colp <jcolp@digium.com>
 */

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$");

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "asterisk/channel.h"
#include "asterisk/module.h"
#include "asterisk/lock.h"
#include "asterisk/linkedlists.h"
#include "asterisk/cli.h"
#include "asterisk/term.h"
#include "asterisk/options.h"
#include "asterisk/speech.h"


static AST_RWLIST_HEAD_STATIC(engines, ast_speech_engine);
static struct ast_speech_engine *default_engine = NULL;

/*! \brief Find a speech recognition engine of specified name, if NULL then use the default one */
static struct ast_speech_engine *find_engine(char *engine_name)
{
	struct ast_speech_engine *engine = NULL;

	/* If no name is specified -- use the default engine */
	if (ast_strlen_zero(engine_name))
		return default_engine;

	AST_RWLIST_RDLOCK(&engines);
	AST_RWLIST_TRAVERSE(&engines, engine, list) {
		if (!strcasecmp(engine->name, engine_name)) {
			break;
		}
	}
	AST_RWLIST_UNLOCK(&engines);

	return engine;
}

/*! \brief Activate a loaded (either local or global) grammar */
int ast_speech_grammar_activate(struct ast_speech *speech, char *grammar_name)
{
	return (speech->engine->activate ? speech->engine->activate(speech, grammar_name) : -1);
}

/*! \brief Deactivate a loaded grammar on a speech structure */
int ast_speech_grammar_deactivate(struct ast_speech *speech, char *grammar_name)
{
	return (speech->engine->deactivate ? speech->engine->deactivate(speech, grammar_name) : -1);
}

/*! \brief Load a local grammar on a speech structure */
int ast_speech_grammar_load(struct ast_speech *speech, char *grammar_name, char *grammar)
{
	return (speech->engine->load ? speech->engine->load(speech, grammar_name, grammar) : -1);
}

/*! \brief Unload a local grammar from a speech structure */
int ast_speech_grammar_unload(struct ast_speech *speech, char *grammar_name)
{
	return (speech->engine->unload ? speech->engine->unload(speech, grammar_name) : -1);
}

/*! \brief Return the results of a recognition from the speech structure */
struct ast_speech_result *ast_speech_results_get(struct ast_speech *speech)
{
	return (speech->engine->get ? speech->engine->get(speech) : NULL);
}

/*! \brief Free a list of results */
int ast_speech_results_free(struct ast_speech_result *result)
{
	struct ast_speech_result *current_result = result, *prev_result = NULL;
	int res = 0;

	while (current_result != NULL) {
		prev_result = current_result;
		/* Deallocate what we can */
		if (current_result->text != NULL) {
			ast_free(current_result->text);
			current_result->text = NULL;
		}
		if (current_result->grammar != NULL) {
			ast_free(current_result->grammar);
			current_result->grammar = NULL;
		}
		/* Move on and then free ourselves */
		current_result = AST_LIST_NEXT(current_result, list);
		ast_free(prev_result);
		prev_result = NULL;
	}

	return res;
}

/*! \brief Start speech recognition on a speech structure */
void ast_speech_start(struct ast_speech *speech)
{

	/* Clear any flags that may affect things */
	ast_clear_flag(speech, AST_SPEECH_SPOKE);
	ast_clear_flag(speech, AST_SPEECH_QUIET);
	ast_clear_flag(speech, AST_SPEECH_HAVE_RESULTS);

	/* If results are on the structure, free them since we are starting again */
	if (speech->results) {
		ast_speech_results_free(speech->results);
		speech->results = NULL;
	}

	/* If the engine needs to start stuff up, do it */
	if (speech->engine->start)
		speech->engine->start(speech);

	return;
}

/*! \brief Write in signed linear audio to be recognized */
int ast_speech_write(struct ast_speech *speech, void *data, int len)
{
	/* Make sure the speech engine is ready to accept audio */
	if (speech->state != AST_SPEECH_STATE_READY)
		return -1;

	return speech->engine->write(speech, data, len);
}

/*! \brief Signal to the engine that DTMF was received */
int ast_speech_dtmf(struct ast_speech *speech, const char *dtmf)
{
	int res = 0;

	if (speech->state != AST_SPEECH_STATE_READY)
		return -1;

	if (speech->engine->dtmf != NULL) {
		res = speech->engine->dtmf(speech, dtmf);
	}

	return res;
}

/*! \brief Change an engine specific attribute */
int ast_speech_change(struct ast_speech *speech, char *name, const char *value)
{
	return (speech->engine->change ? speech->engine->change(speech, name, value) : -1);
}

/*! \brief Create a new speech structure using the engine specified */
struct ast_speech *ast_speech_new(char *engine_name, int formats)
{
	struct ast_speech_engine *engine = NULL;
	struct ast_speech *new_speech = NULL;
	int format = AST_FORMAT_SLINEAR;

	/* Try to find the speech recognition engine that was requested */
	if (!(engine = find_engine(engine_name)))
		return NULL;

	/* Before even allocating the memory below do some codec negotiation, we choose the best codec possible and fall back to signed linear if possible */
	if ((format = (engine->formats & formats)))
		format = ast_best_codec(format);
	else if ((engine->formats & AST_FORMAT_SLINEAR))
		format = AST_FORMAT_SLINEAR;
	else
		return NULL;

	/* Allocate our own speech structure, and try to allocate a structure from the engine too */
	if (!(new_speech = ast_calloc(1, sizeof(*new_speech))))
		return NULL;

	/* Initialize the lock */
	ast_mutex_init(&new_speech->lock);

	/* Make sure no results are present */
	new_speech->results = NULL;

	/* Copy over our engine pointer */
	new_speech->engine = engine;

	/* Can't forget the format audio is going to be in */
	new_speech->format = format;

	/* We are not ready to accept audio yet */
	ast_speech_change_state(new_speech, AST_SPEECH_STATE_NOT_READY);

	/* Pass ourselves to the engine so they can set us up some more and if they error out then do not create a structure */
	if (engine->create(new_speech, format)) {
		ast_mutex_destroy(&new_speech->lock);
		ast_free(new_speech);
		new_speech = NULL;
	}

	return new_speech;
}

/*! \brief Destroy a speech structure */
int ast_speech_destroy(struct ast_speech *speech)
{
	int res = 0;

	/* Call our engine so we are destroyed properly */
	speech->engine->destroy(speech);

	/* Deinitialize the lock */
	ast_mutex_destroy(&speech->lock);

	/* If results exist on the speech structure, destroy them */
	if (speech->results)
		ast_speech_results_free(speech->results);

	/* If a processing sound is set - free the memory used by it */
	if (speech->processing_sound)
		ast_free(speech->processing_sound);

	/* Aloha we are done */
	ast_free(speech);

	return res;
}

/*! \brief Change state of a speech structure */
int ast_speech_change_state(struct ast_speech *speech, int state)
{
	int res = 0;

	switch (state) {
	case AST_SPEECH_STATE_WAIT:
		/* The engine heard audio, so they spoke */
		ast_set_flag(speech, AST_SPEECH_SPOKE);
	default:
		speech->state = state;
		break;
	}

	return res;
}

/*! \brief Change the type of results we want */
int ast_speech_change_results_type(struct ast_speech *speech, enum ast_speech_results_type results_type)
{
	speech->results_type = results_type;

	return (speech->engine->change_results_type ? speech->engine->change_results_type(speech, results_type) : 0);
}

/*! \brief Register a speech recognition engine */
int ast_speech_register(struct ast_speech_engine *engine)
{
	struct ast_speech_engine *existing_engine = NULL;
	int res = 0;

	/* Confirm the engine meets the minimum API requirements */
	if (!engine->create || !engine->write || !engine->destroy) {
		ast_log(LOG_WARNING, "Speech recognition engine '%s' did not meet minimum API requirements.\n", engine->name);
		return -1;
	}

	/* If an engine is already loaded with this name, error out */
	if ((existing_engine = find_engine(engine->name))) {
		ast_log(LOG_WARNING, "Speech recognition engine '%s' already exists.\n", engine->name);
		return -1;
	}

	ast_verb(2, "Registered speech recognition engine '%s'\n", engine->name);

	/* Add to the engine linked list and make default if needed */
	AST_RWLIST_WRLOCK(&engines);
	AST_RWLIST_INSERT_HEAD(&engines, engine, list);
	if (!default_engine) {
		default_engine = engine;
		ast_verb(2, "Made '%s' the default speech recognition engine\n", engine->name);
	}
	AST_RWLIST_UNLOCK(&engines);

	return res;
}

/*! \brief Unregister a speech recognition engine */
int ast_speech_unregister(char *engine_name)
{
	struct ast_speech_engine *engine = NULL;
	int res = -1;

	if (ast_strlen_zero(engine_name))
		return -1;

	AST_RWLIST_WRLOCK(&engines);
	AST_RWLIST_TRAVERSE_SAFE_BEGIN(&engines, engine, list) {
		if (!strcasecmp(engine->name, engine_name)) {
			/* We have our engine... removed it */
			AST_RWLIST_REMOVE_CURRENT(&engines, list);
			/* If this was the default engine, we need to pick a new one */
			if (!default_engine)
				default_engine = AST_RWLIST_FIRST(&engines);
			ast_verb(2, "Unregistered speech recognition engine '%s'\n", engine_name);
			/* All went well */
			res = 0;
			break;
		}
	}
	AST_RWLIST_TRAVERSE_SAFE_END
	AST_RWLIST_UNLOCK(&engines);

	return res;
}

static int unload_module(void)
{
	/* We can not be unloaded */
	return -1;
}

static int load_module(void)
{
	return 0;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_GLOBAL_SYMBOLS, "Generic Speech Recognition API",
		.load = load_module,
		.unload = unload_module,
		);

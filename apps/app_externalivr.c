/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Kevin P. Fleming <kpfleming@digium.com>
 *
 * Portions taken from the file-based music-on-hold work
 * created by Anthony Minessale II in res_musiconhold.c
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
 * \brief External IVR application interface
 *
 * \author Kevin P. Fleming <kpfleming@digium.com>
 *
 * \note Portions taken from the file-based music-on-hold work
 * created by Anthony Minessale II in res_musiconhold.c
 *
 * \ingroup applications
 */

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#include <signal.h>

#include "asterisk/lock.h"
#include "asterisk/file.h"
#include "asterisk/channel.h"
#include "asterisk/pbx.h"
#include "asterisk/module.h"
#include "asterisk/linkedlists.h"
#include "asterisk/app.h"
#include "asterisk/utils.h"

static const char *app = "ExternalIVR";

static const char *synopsis = "Interfaces with an external IVR application";

static const char *descrip =
"  ExternalIVR(command[,arg[,arg...]]): Forks a process to run the supplied command,\n"
"and starts a generator on the channel. The generator's play list is\n"
"controlled by the external application, which can add and clear entries\n"
"via simple commands issued over its stdout. The external application\n"
"will receive all DTMF events received on the channel, and notification\n"
"if the channel is hung up. The application will not be forcibly terminated\n"
"when the channel is hung up.\n"
"See doc/externalivr.txt for a protocol specification.\n";

/* XXX the parser in gcc 2.95 gets confused if you don't put a space between 'name' and the comma */
#define ast_chan_log(level, channel, format, ...) ast_log(level, "%s: " format, channel->name , ## __VA_ARGS__)

struct playlist_entry {
	AST_LIST_ENTRY(playlist_entry) list;
	char filename[1];
};

struct ivr_localuser {
	struct ast_channel *chan;
	AST_LIST_HEAD(playlist, playlist_entry) playlist;
	AST_LIST_HEAD(finishlist, playlist_entry) finishlist;
	int abort_current_sound;
	int playing_silence;
	int option_autoclear;
};


struct gen_state {
	struct ivr_localuser *u;
	struct ast_filestream *stream;
	struct playlist_entry *current;
	int sample_queue;
};

static int eivr_comm(struct ast_channel *chan, struct ivr_localuser *u, 
              int eivr_events_fd, int eivr_commands_fd, int eivr_errors_fd, 
              const char *args);

static void send_eivr_event(FILE *handle, const char event, const char *data,
	const struct ast_channel *chan)
{
	char tmp[256];

	if (!data) {
		snprintf(tmp, sizeof(tmp), "%c,%10d", event, (int)time(NULL));
	} else {
		snprintf(tmp, sizeof(tmp), "%c,%10d,%s", event, (int)time(NULL), data);
	}

	fprintf(handle, "%s\n", tmp);
	if (option_debug)
		ast_chan_log(LOG_DEBUG, chan, "sent '%s'\n", tmp);
}

static void *gen_alloc(struct ast_channel *chan, void *params)
{
	struct ivr_localuser *u = params;
	struct gen_state *state;

	if (!(state = ast_calloc(1, sizeof(*state))))
		return NULL;

	state->u = u;

	return state;
}

static void gen_closestream(struct gen_state *state)
{
	if (!state->stream)
		return;

	ast_closestream(state->stream);
	state->u->chan->stream = NULL;
	state->stream = NULL;
}

static void gen_release(struct ast_channel *chan, void *data)
{
	struct gen_state *state = data;

	gen_closestream(state);
	ast_free(data);
}

/* caller has the playlist locked */
static int gen_nextfile(struct gen_state *state)
{
	struct ivr_localuser *u = state->u;
	char *file_to_stream;

	u->abort_current_sound = 0;
	u->playing_silence = 0;
	gen_closestream(state);

	while (!state->stream) {
		state->current = AST_LIST_REMOVE_HEAD(&u->playlist, list);
		if (state->current) {
			file_to_stream = state->current->filename;
		} else {
			file_to_stream = "silence/10";
			u->playing_silence = 1;
		}

		if (!(state->stream = ast_openstream_full(u->chan, file_to_stream, u->chan->language, 1))) {
			ast_chan_log(LOG_WARNING, u->chan, "File '%s' could not be opened: %s\n", file_to_stream, strerror(errno));
			if (!u->playing_silence) {
				continue;
			} else {
				break;
			}
		}
	}

	return (!state->stream);
}

static struct ast_frame *gen_readframe(struct gen_state *state)
{
	struct ast_frame *f = NULL;
	struct ivr_localuser *u = state->u;

	if (u->abort_current_sound ||
		(u->playing_silence && AST_LIST_FIRST(&u->playlist))) {
		gen_closestream(state);
		AST_LIST_LOCK(&u->playlist);
		gen_nextfile(state);
		AST_LIST_UNLOCK(&u->playlist);
	}

	if (!(state->stream && (f = ast_readframe(state->stream)))) {
		if (state->current) {
			AST_LIST_LOCK(&u->finishlist);
			AST_LIST_INSERT_TAIL(&u->finishlist, state->current, list);
			AST_LIST_UNLOCK(&u->finishlist);
			state->current = NULL;
		}
		if (!gen_nextfile(state))
			f = ast_readframe(state->stream);
	}

	return f;
}

static int gen_generate(struct ast_channel *chan, void *data, int len, int samples)
{
	struct gen_state *state = data;
	struct ast_frame *f = NULL;
	int res = 0;

	state->sample_queue += samples;

	while (state->sample_queue > 0) {
		if (!(f = gen_readframe(state)))
			return -1;

		res = ast_write(chan, f);
		ast_frfree(f);
		if (res < 0) {
			ast_chan_log(LOG_WARNING, chan, "Failed to write frame: %s\n", strerror(errno));
			return -1;
		}
		state->sample_queue -= f->samples;
	}

	return res;
}

static struct ast_generator gen =
{
	alloc: gen_alloc,
	release: gen_release,
	generate: gen_generate,
};

static void ast_eivr_getvariable(struct ast_channel *chan, char *data, char *outbuf, int outbuflen)
{
	// original input data: "G,var1,var2,"
	// data passed as "data":  "var1,var2"
	char *inbuf, *variable;

	const char *value;
	char *saveptr;
	int j;

	outbuf[0] = 0;

	for (j = 1, inbuf = data; ; j++, inbuf = NULL) {
		variable = strtok_r(inbuf, ",", &saveptr);
		if (variable == NULL) {
			int outstrlen = strlen(outbuf);
			if(outstrlen && outbuf[outstrlen - 1] == ',') {
				outbuf[outstrlen - 1] = 0;
			}
			break;
		}
		
		value = pbx_builtin_getvar_helper(chan, variable);
		if(!value)
			value = "";
		strncat(outbuf,variable,outbuflen);
		strncat(outbuf,"=",outbuflen);
		strncat(outbuf,value,outbuflen);
		strncat(outbuf,",",outbuflen);
	}
};

static void ast_eivr_setvariable(struct ast_channel *chan, char *data)
{
	char buf[1024];
	char *value;

	char *inbuf, *variable;

	char *saveptr;
	int j;

	for(j=1, inbuf=data; ; j++, inbuf=NULL) {
		variable = strtok_r(inbuf, ",", &saveptr);
		ast_chan_log(LOG_DEBUG, chan, "Setting up a variable: %s\n", variable);
		if(variable) {
			//variable contains "varname=value"
			strncpy(buf, variable, sizeof(buf));
			value = strchr(buf, '=');
			if(!value) 
				value="";
			else {
				value[0] = 0;
				value++;
			}
			pbx_builtin_setvar_helper(chan, buf, value);
		}
		else break;

	}

};

static struct playlist_entry *make_entry(const char *filename)
{
	struct playlist_entry *entry;

	if (!(entry = ast_calloc(1, sizeof(*entry) + strlen(filename) + 10))) /* XXX why 10 ? */
		return NULL;

	strcpy(entry->filename, filename);

	return entry;
}

static int app_exec(struct ast_channel *chan, void *data)
{
	struct playlist_entry *entry;
	int child_stdin[2] = { 0,0 };
	int child_stdout[2] = { 0,0 };
	int child_stderr[2] = { 0,0 };
	int res = -1;
	int gen_active = 0;
	int pid;
	char *buf, *pipe_delim_argbuf, *pdargbuf_ptr;
	struct ivr_localuser foo = {
		.playlist = AST_LIST_HEAD_INIT_VALUE,
		.finishlist = AST_LIST_HEAD_INIT_VALUE,
	};
	struct ivr_localuser *u = &foo;
	sigset_t fullset, oldset;
	AST_DECLARE_APP_ARGS(args,
		AST_APP_ARG(cmd)[32];
	);

	sigfillset(&fullset);
	pthread_sigmask(SIG_BLOCK, &fullset, &oldset);

	u->abort_current_sound = 0;
	u->chan = chan;

	if (ast_strlen_zero(data)) {
		ast_log(LOG_WARNING, "ExternalIVR requires a command to execute\n");
		return -1;
	}

	buf = ast_strdupa(data);
	AST_STANDARD_APP_ARGS(args, buf);

	//copy args and replace commas with pipes
	pipe_delim_argbuf = ast_strdupa(data);
	while((pdargbuf_ptr = strchr(pipe_delim_argbuf, ',')) != NULL)
		pdargbuf_ptr[0] = '|';
	
	if (pipe(child_stdin)) {
		ast_chan_log(LOG_WARNING, chan, "Could not create pipe for child input: %s\n", strerror(errno));
		goto exit;
	}
	if (pipe(child_stdout)) {
		ast_chan_log(LOG_WARNING, chan, "Could not create pipe for child output: %s\n", strerror(errno));
		goto exit;
	}
	if (pipe(child_stderr)) {
		ast_chan_log(LOG_WARNING, chan, "Could not create pipe for child errors: %s\n", strerror(errno));
		goto exit;
	}
	if (chan->_state != AST_STATE_UP) {
		ast_answer(chan);
	}
	if (ast_activate_generator(chan, &gen, u) < 0) {
		ast_chan_log(LOG_WARNING, chan, "Failed to activate generator\n");
		goto exit;
	} else
		gen_active = 1;

	pid = fork();
	if (pid < 0) {
		ast_log(LOG_WARNING, "Failed to fork(): %s\n", strerror(errno));
		goto exit;
	}

	if (!pid) {
		/* child process */
		int i;

		signal(SIGPIPE, SIG_DFL);
		pthread_sigmask(SIG_UNBLOCK, &fullset, NULL);

		if (ast_opt_high_priority)
			ast_set_priority(0);

		dup2(child_stdin[0], STDIN_FILENO);
		dup2(child_stdout[1], STDOUT_FILENO);
		dup2(child_stderr[1], STDERR_FILENO);
		for (i = STDERR_FILENO + 1; i < 1024; i++)
			close(i);
		execv(args.cmd[0], args.cmd);
		fprintf(stderr, "Failed to execute '%s': %s\n", args.cmd[0], strerror(errno));
		_exit(1);
	} else {
		/* parent process */

		close(child_stdin[0]);
		child_stdin[0] = 0;
		close(child_stdout[1]);
		child_stdout[1] = 0;
		close(child_stderr[1]);
		child_stderr[1] = 0;
		res = eivr_comm(chan, u, child_stdin[1], child_stdout[0], child_stderr[0], pipe_delim_argbuf);

		exit:
		if (gen_active)
			ast_deactivate_generator(chan);

		if (child_stdin[0])
			close(child_stdin[0]);

		if (child_stdin[1])
			close(child_stdin[1]);

		if (child_stdout[0])
			close(child_stdout[0]);

		if (child_stdout[1])
			close(child_stdout[1]);

		if (child_stderr[0])
			close(child_stderr[0]);

		if (child_stderr[1])
			close(child_stderr[1]);

		while ((entry = AST_LIST_REMOVE_HEAD(&u->playlist, list)))
			ast_free(entry);

		return res;
	}
}

static int eivr_comm(struct ast_channel *chan, struct ivr_localuser *u, 
 				int eivr_events_fd, int eivr_commands_fd, int eivr_errors_fd, 
 				const char *args)
{
	struct playlist_entry *entry;
	struct ast_frame *f;
	int ms;
 	int exception;
 	int ready_fd;
 	int waitfds[2] = { eivr_commands_fd, eivr_errors_fd };
 	struct ast_channel *rchan;
 	char *command;
 	int res = -1;
  
 	FILE *eivr_commands = NULL;
 	FILE *eivr_errors = NULL;
 	FILE *eivr_events = NULL;
 
 	if (!(eivr_events = fdopen(eivr_events_fd, "w"))) {
 		ast_chan_log(LOG_WARNING, chan, "Could not open stream to send events\n");
 		goto exit;
 	}
 	if (!(eivr_commands = fdopen(eivr_commands_fd, "r"))) {
 		ast_chan_log(LOG_WARNING, chan, "Could not open stream to receive commands\n");
 		goto exit;
 	}
 	if(eivr_errors_fd) {  /*if opening a socket connection, error stream will not be used*/
 		if (!(eivr_errors = fdopen(eivr_errors_fd, "r"))) {
 			ast_chan_log(LOG_WARNING, chan, "Could not open stream to receive errors\n");
 			goto exit;
 		}
 	}
 
 	setvbuf(eivr_events, NULL, _IONBF, 0);
 	setvbuf(eivr_commands, NULL, _IONBF, 0);
 	if(eivr_errors)
		setvbuf(eivr_errors, NULL, _IONBF, 0);

	res = 0;
 
 	while (1) {
 		if (ast_test_flag(chan, AST_FLAG_ZOMBIE)) {
 			ast_chan_log(LOG_NOTICE, chan, "Is a zombie\n");
 			res = -1;
 			break;
 		}
 		if (ast_check_hangup(chan)) {
 			ast_chan_log(LOG_NOTICE, chan, "Got check_hangup\n");
 			send_eivr_event(eivr_events, 'H', NULL, chan);
 			res = -1;
 			break;
 		}
 
 		ready_fd = 0;
 		ms = 100;
 		errno = 0;
 		exception = 0;
 
 		rchan = ast_waitfor_nandfds(&chan, 1, waitfds, 2, &exception, &ready_fd, &ms);
 
 		if (!AST_LIST_EMPTY(&u->finishlist)) {
 			AST_LIST_LOCK(&u->finishlist);
 			while ((entry = AST_LIST_REMOVE_HEAD(&u->finishlist, list))) {
 				send_eivr_event(eivr_events, 'F', entry->filename, chan);
 				ast_free(entry);
 			}
 			AST_LIST_UNLOCK(&u->finishlist);
 		}
 
 		if (rchan) {
 			/* the channel has something */
 			f = ast_read(chan);
 			if (!f) {
 				ast_chan_log(LOG_NOTICE, chan, "Returned no frame\n");
 				send_eivr_event(eivr_events, 'H', NULL, chan);
 				res = -1;
 				break;
 			}
 			if (f->frametype == AST_FRAME_DTMF) {
 				send_eivr_event(eivr_events, f->subclass, NULL, chan);
 				if (u->option_autoclear) {
  					if (!u->abort_current_sound && !u->playing_silence)
 						send_eivr_event(eivr_events, 'T', NULL, chan);
  					AST_LIST_LOCK(&u->playlist);
  					while ((entry = AST_LIST_REMOVE_HEAD(&u->playlist, list))) {
 						send_eivr_event(eivr_events, 'D', entry->filename, chan);
  						ast_free(entry);
  					}
  					if (!u->playing_silence)
  						u->abort_current_sound = 1;
  					AST_LIST_UNLOCK(&u->playlist);
  				}
 			} else if ((f->frametype == AST_FRAME_CONTROL) && (f->subclass == AST_CONTROL_HANGUP)) {
 				ast_chan_log(LOG_NOTICE, chan, "Got AST_CONTROL_HANGUP\n");
 				send_eivr_event(eivr_events, 'H', NULL, chan);
 				ast_frfree(f);
 				res = -1;
 				break;
 			}
 			ast_frfree(f);
 		} else if (ready_fd == eivr_commands_fd) {
 			char input[1024];
 
 			if (exception || feof(eivr_commands)) {
 				ast_chan_log(LOG_WARNING, chan, "Child process went away\n");
 				res = -1;
  				break;
  			}
  
 			if (!fgets(input, sizeof(input), eivr_commands))
 				continue;
 
 			command = ast_strip(input);
  
 			if (option_debug)
 				ast_chan_log(LOG_DEBUG, chan, "got command '%s'\n", input);
  
 			if (strlen(input) < 4)
 				continue;
  
			if (input[0] == 'P') {
 				send_eivr_event(eivr_events, 'P', args, chan);
 
 			} else if (input[0] == 'S') {
 				if (ast_fileexists(&input[2], NULL, u->chan->language) == -1) {
 					ast_chan_log(LOG_WARNING, chan, "Unknown file requested '%s'\n", &input[2]);
 					send_eivr_event(eivr_events, 'Z', NULL, chan);
 					strcpy(&input[2], "exception");
 				}
 				if (!u->abort_current_sound && !u->playing_silence)
 					send_eivr_event(eivr_events, 'T', NULL, chan);
 				AST_LIST_LOCK(&u->playlist);
 				while ((entry = AST_LIST_REMOVE_HEAD(&u->playlist, list))) {
 					send_eivr_event(eivr_events, 'D', entry->filename, chan);
 					ast_free(entry);
 				}
 				if (!u->playing_silence)
 					u->abort_current_sound = 1;
 				entry = make_entry(&input[2]);
 				if (entry)
 					AST_LIST_INSERT_TAIL(&u->playlist, entry, list);
 				AST_LIST_UNLOCK(&u->playlist);
 			} else if (input[0] == 'A') {
 				if (ast_fileexists(&input[2], NULL, u->chan->language) == -1) {
 					ast_chan_log(LOG_WARNING, chan, "Unknown file requested '%s'\n", &input[2]);
 					send_eivr_event(eivr_events, 'Z', NULL, chan);
 					strcpy(&input[2], "exception");
 				}
 				entry = make_entry(&input[2]);
 				if (entry) {
 					AST_LIST_LOCK(&u->playlist);
 					AST_LIST_INSERT_TAIL(&u->playlist, entry, list);
 					AST_LIST_UNLOCK(&u->playlist);
 				}
 			} else if (input[0] == 'G') {
 				// A get variable message:  "G,variable1,variable2,..."
 				char response[2048];
 				ast_chan_log(LOG_NOTICE, chan, "Getting a Variable out of the channel: %s\n", &input[2]);
 				ast_eivr_getvariable(chan, &input[2], response, sizeof(response));
 				send_eivr_event(eivr_events, 'G', response, chan);
 			} else if (input[0] == 'V') {
 				// A set variable message:  "V,variablename=foo"
 				ast_chan_log(LOG_NOTICE, chan, "Setting a Variable up: %s\n", &input[2]);
 				ast_eivr_setvariable(chan, &input[2]);
 			} else if (input[0] == 'L') {
 				ast_chan_log(LOG_NOTICE, chan, "Log message from EIVR: %s\n", &input[2]);
 			} else if (input[0] == 'X') {
 				ast_chan_log(LOG_NOTICE, chan, "Exiting ExternalIVR: %s\n", &input[2]);
 				//TODO: add deprecation debug message for X command here
 				res = 0;
 				break;
			} else if (input[0] == 'E') {
 				ast_chan_log(LOG_NOTICE, chan, "Exiting: %s\n", &input[2]);
 				send_eivr_event(eivr_events, 'E', NULL, chan);
 				res = 0;
 				break;
 			} else if (input[0] == 'H') {
 				ast_chan_log(LOG_NOTICE, chan, "Hanging up: %s\n", &input[2]);
 				send_eivr_event(eivr_events, 'H', NULL, chan);
 				res = -1;
 				break;
 			} else if (input[0] == 'O') {
 				if (!strcasecmp(&input[2], "autoclear"))
 					u->option_autoclear = 1;
 				else if (!strcasecmp(&input[2], "noautoclear"))
 					u->option_autoclear = 0;
 				else
 					ast_chan_log(LOG_WARNING, chan, "Unknown option requested '%s'\n", &input[2]);
 			}
 		} else if (eivr_errors_fd && ready_fd == eivr_errors_fd) {
 			char input[1024];
  
 			if (exception || feof(eivr_errors)) {
 				ast_chan_log(LOG_WARNING, chan, "Child process went away\n");
 				res = -1;
 				break;
 			}
 			if (fgets(input, sizeof(input), eivr_errors)) {
 				command = ast_strip(input);
 				ast_chan_log(LOG_NOTICE, chan, "stderr: %s\n", command);
 			}
 		} else if ((ready_fd < 0) && ms) { 
 			if (errno == 0 || errno == EINTR)
 				continue;
 
 			ast_chan_log(LOG_WARNING, chan, "Wait failed (%s)\n", strerror(errno));
 			break;
 		}
 	}
  
 
exit:
 
 	if (eivr_events)
 		fclose(eivr_events);
 
 	if (eivr_commands)
		fclose(eivr_commands);

 	if (eivr_errors)
 		fclose(eivr_errors);
  
  	return res;
 
  }

static int unload_module(void)
{
	return ast_unregister_application(app);
}

static int load_module(void)
{
	return ast_register_application(app, app_exec, synopsis, descrip);
}

AST_MODULE_INFO_STANDARD(ASTERISK_GPL_KEY, "External IVR Interface Application");

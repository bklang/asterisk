/*
 * Asterisk -- A telephony toolkit for Linux.
 *
 * Trivial application to dial a channel and send an URL on answer
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
#include <asterisk/logger.h>
#include <asterisk/channel.h>
#include <asterisk/pbx.h>
#include <asterisk/options.h>
#include <asterisk/module.h>
#include <asterisk/translate.h>
#include <asterisk/say.h>
#include <asterisk/parking.h>
#include <asterisk/musiconhold.h>
#include <asterisk/callerid.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/time.h>
#include <sys/signal.h>
#include <netinet/in.h>

#include <pthread.h>

static char *tdesc = "Dialing Application";

static char *app = "Dial";

static char *synopsis = "Place a call and connect to the current channel";

static char *descrip =
"  Dial(Technology/resource[&Technology2/resource2...][|timeout][|options][|URL]):\n"
"Requests one or more channels and places specified outgoing calls on them.\n"
"As soon as a channel answers, the Dial app will answer the originating\n"
"channel (if it needs to be answered) and will bridge a call with the channel\n"
"which first answered. All other calls placed by the Dial app will be hung up\n"
"If a timeout is not specified, the Dial application will wait indefinitely\n"
"until either one of the called channels answers, the user hangs up, or all\n"
"channels return busy or error. In general, the dialler will return 0 if it\n"
"was unable to place the call, or the timeout expired. However, if all\n"
"channels were busy, and there exists an extension with priority n+101 (where\n"
"n is the priority of the dialler instance), then it will be the next\n"
"executed extension (this allows you to setup different behavior on busy from\n"
"no-answer).\n"
"  This application returns -1 if the originating channel hangs up, or if the\n"
"call is bridged and either of the parties in the bridge terminate the call.\n"
"The option string may contain zero or more of the following characters:\n"
"      't' -- allow the called user transfer the calling user\n"
"      'T' -- to allow the calling user to transfer the call.\n"
"      'r' -- indicate ringing to the calling party, pass no audio until answered.\n"
"      'm' -- provide hold music to the calling party until answered.\n"
"      'H' -- allow caller to hang up by hitting *.\n"
"      'C' -- reset call detail record for this call.\n"
"      'P[(x)]' -- privacy mode, using 'x' as database if provided.\n"
"      'g' -- goes on in context if the destination channel hangs up\n"
"      'A(x)' -- play an announcement to the called party, using x as file\n"
"  In addition to transferring the call, a call may be parked and then picked\n"
"up by another user.\n"
"  The optionnal URL will be sent to the called party if the channel supports\n"
"it.\n";

/* We define a customer "local user" structure because we
   use it not only for keeping track of what is in use but
   also for keeping track of who we're dialing. */

struct localuser {
	struct ast_channel *chan;
	int stillgoing;
	int allowredirect_in;
	int allowredirect_out;
	int ringbackonly;
	int musiconhold;
	int allowdisconnect;
	struct localuser *next;
};

LOCAL_USER_DECL;

static void hanguptree(struct localuser *outgoing, struct ast_channel *exception)
{
	/* Hang up a tree of stuff */
	struct localuser *oo;
	while(outgoing) {
		/* Hangup any existing lines we have open */
		if (outgoing->chan && (outgoing->chan != exception))
			ast_hangup(outgoing->chan);
		oo = outgoing;
		outgoing=outgoing->next;
		free(oo);
	}
}

#define MAX 256

static struct ast_channel *wait_for_answer(struct ast_channel *in, struct localuser *outgoing, int *to, int *allowredir_in, int *allowredir_out, int *allowdisconnect)
{
	struct localuser *o;
	int found;
	int numlines;
	int sentringing = 0;
	int numbusies = 0;
	int orig = *to;
	struct ast_frame *f;
	struct ast_channel *peer = NULL;
	struct ast_channel *watchers[MAX];
	int pos;
	int single;
	int moh=0;
	int ringind=0;
	struct ast_channel *winner;
	
	single = (outgoing && !outgoing->next && !outgoing->musiconhold && !outgoing->ringbackonly);
	
	if (single) {
		/* Turn off hold music, etc */
		ast_indicate(in, -1);
		/* If we are calling a single channel, make them compatible for in-band tone purpose */
		ast_channel_make_compatible(outgoing->chan, in);
	}
	
	if (outgoing) {
		moh = outgoing->musiconhold;
		ringind = outgoing->ringbackonly;
		if (outgoing->musiconhold) {
			ast_moh_start(in, NULL);
		} else if (outgoing->ringbackonly) {
			ast_indicate(in, AST_CONTROL_RINGING);
		}
	}
	
	while(*to && !peer) {
		o = outgoing;
		found = -1;
		pos = 1;
		numlines = 0;
		watchers[0] = in;
		while(o) {
			/* Keep track of important channels */
			if (o->stillgoing && o->chan) {
				watchers[pos++] = o->chan;
				found = 1;
			}
			o = o->next;
			numlines++;
		}
		if (found < 0) {
			if (numlines == numbusies) {
				if (option_verbose > 2)
					ast_verbose( VERBOSE_PREFIX_2 "Everyone is busy at this time\n");
				/* See if there is a special busy message */
				if (ast_exists_extension(in, in->context, in->exten, in->priority + 101, in->callerid)) 
					in->priority+=100;
			} else {
				if (option_verbose > 2)
					ast_verbose( VERBOSE_PREFIX_2 "No one is available to answer at this time\n");
			}
			*to = 0;
			/* if no one available we'd better stop MOH/ringing to */
			if (moh) {
				ast_moh_stop(in);
			} else if (ringind) {
				ast_indicate(in, -1);
			}
			return NULL;
		}
		winner = ast_waitfor_n(watchers, pos, to);
		o = outgoing;
		while(o) {
			if (o->stillgoing && o->chan && (o->chan->_state == AST_STATE_UP)) {
				if (!peer) {
					if (option_verbose > 2)
						ast_verbose( VERBOSE_PREFIX_3 "%s answered %s\n", o->chan->name, in->name);
					peer = o->chan;
					*allowredir_in = o->allowredirect_in;
					*allowredir_out = o->allowredirect_out;
					*allowdisconnect = o->allowdisconnect;
				}
			} else if (o->chan && (o->chan == winner)) {
				if (strlen(o->chan->call_forward)) {
					char tmpchan[256];
					/* Before processing channel, go ahead and check for forwarding */
					if (option_verbose > 2)
						ast_verbose(VERBOSE_PREFIX_3 "Now forwarding %s to '%s@%s' (thanks to %s)\n", in->name, o->chan->call_forward, o->chan->context, o->chan->name);
					/* Setup parameters */
					snprintf(tmpchan, sizeof(tmpchan),"%s@%s", o->chan->call_forward, o->chan->context);
					ast_hangup(o->chan);
					o->chan = ast_request("Local", in->nativeformats, tmpchan);
					if (!o->chan) {
						ast_log(LOG_NOTICE, "Unable to create local channel for call forward to '%s'\n", tmpchan);
						o->stillgoing = 0;
						numbusies++;
					} else {
						if (in->callerid) {
							if (o->chan->callerid)
								free(o->chan->callerid);
							o->chan->callerid = malloc(strlen(in->callerid) + 1);
							strncpy(o->chan->callerid, in->callerid, strlen(in->callerid) + 1);
						}
						if (in->ani) {
							if (o->chan->ani)
								free(o->chan->ani);
							o->chan->ani = malloc(strlen(in->ani) + 1);
							strncpy(o->chan->ani, in->ani, strlen(in->ani) + 1);
						}
						if (ast_call(o->chan, tmpchan, 0)) {
							ast_log(LOG_NOTICE, "Failed to dial on local channel for call forward to '%s'\n", tmpchan);
							o->stillgoing = 0;
							ast_hangup(o->chan);
							o->chan = NULL;
							numbusies++;
						}
					}
					continue;
				}
				f = ast_read(winner);
				if (f) {
					if (f->frametype == AST_FRAME_CONTROL) {
						switch(f->subclass) {
					    case AST_CONTROL_ANSWER:
							/* This is our guy if someone answered. */
							if (!peer) {
								if (option_verbose > 2)
									ast_verbose( VERBOSE_PREFIX_3 "%s answered %s\n", o->chan->name, in->name);
								peer = o->chan;
								*allowredir_in = o->allowredirect_in;
								*allowredir_out = o->allowredirect_out;
								*allowdisconnect = o->allowdisconnect;
							}
							break;
						case AST_CONTROL_BUSY:
							if (option_verbose > 2)
								ast_verbose( VERBOSE_PREFIX_3 "%s is busy\n", o->chan->name);
							in->hangupcause = o->chan->hangupcause;
							ast_hangup(o->chan);
							o->chan = NULL;
							o->stillgoing = 0;
							if (in->cdr)
								ast_cdr_busy(in->cdr);
							numbusies++;
							break;
						case AST_CONTROL_CONGESTION:
							if (option_verbose > 2)
								ast_verbose( VERBOSE_PREFIX_3 "%s is circuit-busy\n", o->chan->name);
							in->hangupcause = o->chan->hangupcause;
							ast_hangup(o->chan);
							o->chan = NULL;
							o->stillgoing = 0;
							if (in->cdr)
								ast_cdr_busy(in->cdr);
							numbusies++;
							break;
						case AST_CONTROL_RINGING:
							if (option_verbose > 2)
								ast_verbose( VERBOSE_PREFIX_3 "%s is ringing\n", o->chan->name);
							if (!sentringing && !moh) {
								ast_indicate(in, AST_CONTROL_RINGING);
								sentringing++;
								ringind++;
							}
							break;
						case AST_CONTROL_PROGRESS:
							if (option_verbose > 2)
								ast_verbose ( VERBOSE_PREFIX_3 "%s is making progress passing it to %s\n", o->chan->name,in->name);
							ast_indicate(in, AST_CONTROL_PROGRESS);
							break;
						case AST_CONTROL_OFFHOOK:
							/* Ignore going off hook */
							break;
						case -1:
							if (option_verbose > 2)
								ast_verbose( VERBOSE_PREFIX_3 "%s stopped sounds\n", o->chan->name);
							ast_indicate(in, -1);
							break;
						default:
							ast_log(LOG_DEBUG, "Dunno what to do with control type %d\n", f->subclass);
						}
					} else if (single && (f->frametype == AST_FRAME_VOICE) && 
								!(outgoing->ringbackonly || outgoing->musiconhold)) {
						if (ast_write(in, f)) 
							ast_log(LOG_WARNING, "Unable to forward frame\n");
					} else if (single && (f->frametype == AST_FRAME_IMAGE) && 
								!(outgoing->ringbackonly || outgoing->musiconhold)) {
						if (ast_write(in, f))
							ast_log(LOG_WARNING, "Unable to forward image\n");
					}
					ast_frfree(f);
				} else {
					in->hangupcause = o->chan->hangupcause;
					ast_hangup(o->chan);
					o->chan = NULL;
					o->stillgoing = 0;
				}
			}
			o = o->next;
		}
		if (winner == in) {
			f = ast_read(in);
#if 0
			if (f && (f->frametype != AST_FRAME_VOICE))
					printf("Frame type: %d, %d\n", f->frametype, f->subclass);
			else if (!f || (f->frametype != AST_FRAME_VOICE))
				printf("Hangup received on %s\n", in->name);
#endif
			if (!f || ((f->frametype == AST_FRAME_CONTROL) && (f->subclass == AST_CONTROL_HANGUP))) {
				/* Got hung up */
				*to=-1;
				return NULL;
			}
			if (f && (f->frametype == AST_FRAME_DTMF) && *allowdisconnect &&
				(f->subclass == '*')) {
			    if (option_verbose > 3)
				ast_verbose(VERBOSE_PREFIX_3 "User hit %c to disconnect call.\n", f->subclass);
				*to=0;
				return NULL;
			}
			if (single && ((f->frametype == AST_FRAME_VOICE) || (f->frametype == AST_FRAME_DTMF)))  {
				if (ast_write(outgoing->chan, f))
					ast_log(LOG_WARNING, "Unable to forward voice\n");
				ast_frfree(f);
			}
		}
		if (!*to && (option_verbose > 2))
			ast_verbose( VERBOSE_PREFIX_3 "Nobody picked up in %d ms\n", orig);
	}
	if (moh) {
		ast_moh_stop(in);
	} else if (ringind) {
		ast_indicate(in, -1);
	}

	return peer;
	
}

static int dial_exec(struct ast_channel *chan, void *data)
{
	int res=-1;
	struct localuser *u;
	char info[256], *peers, *timeout, *tech, *number, *rest, *cur;
	char  privdb[256] = "", *s;
	char  announcemsg[256] = "", *ann;
	struct localuser *outgoing=NULL, *tmp;
	struct ast_channel *peer;
	int to;
	int allowredir_in=0;
	int allowredir_out=0;
	int allowdisconnect=0;
	int privacy=0;
	int announce=0;
	int resetcdr=0;
	char numsubst[AST_MAX_EXTENSION];
	char restofit[AST_MAX_EXTENSION];
	char *transfer = NULL;
	char *newnum;
	char callerid[256], *l, *n;
	char *url=NULL; /* JDG */
	struct ast_var_t *current;
	struct varshead *headp, *newheadp;
	struct ast_var_t *newvar;
	int go_on=0;
	
	if (!data) {
		ast_log(LOG_WARNING, "Dial requires an argument (technology1/number1&technology2/number2...|optional timeout)\n");
		return -1;
	}
	
	LOCAL_USER_ADD(u);
	
	strncpy(info, (char *)data, sizeof(info) - 1);
	peers = info;
	if (peers) {
		timeout = strchr(info, '|');
		if (timeout) {
			*timeout = '\0';
			timeout++;
			transfer = strchr(timeout, '|');
			if (transfer) {
				*transfer = '\0';
				transfer++;
				/* JDG */
				url = strchr(transfer, '|');
				if (url) {
					*url = '\0';
					url++;
					ast_log(LOG_DEBUG, "DIAL WITH URL=%s_\n", url);
				} else 
					ast_log(LOG_DEBUG, "SIMPLE DIAL (NO URL)\n");
				/* /JDG */
			}
		}
	} else
		timeout = NULL;
	if (!peers || !strlen(peers)) {
		ast_log(LOG_WARNING, "Dial argument takes format (technology1/number1&technology2/number2...|optional timeout)\n");
		goto out;
	}
	

	if (transfer) {
		/* XXX ANNOUNCE SUPPORT */
		if ((ann = strstr(transfer, "A("))) {
			announce = 1;
			strncpy(announcemsg, ann + 2, sizeof(announcemsg) - 1);
			/* Overwrite with X's what was the announce info */
			while(*ann && (*ann != ')')) 
				*(ann++) = 'X';
			if (*ann)
				*ann = 'X';
			/* Now find the end of the privdb */
			ann = strchr(announcemsg, ')');
			if (ann)
				*ann = '\0';
			else {
				ast_log(LOG_WARNING, "Transfer with Announce spec lacking trailing ')'\n");
				announce = 0;
			}
		}
		/* Extract privacy info from transfer */
		if ((s = strstr(transfer, "P("))) {
			privacy = 1;
			strncpy(privdb, s + 2, sizeof(privdb) - 1);
			/* Overwrite with X's what was the privacy info */
			while(*s && (*s != ')')) 
				*(s++) = 'X';
			if (*s)
				*s = 'X';
			/* Now find the end of the privdb */
			s = strchr(privdb, ')');
			if (s)
				*s = '\0';
			else {
				ast_log(LOG_WARNING, "Transfer with privacy lacking trailing ')'\n");
				privacy = 0;
			}
		} else if (strchr(transfer, 'P')) {
			/* No specified privdb */
			privacy = 1;
		} else if (strchr(transfer, 'C')) {
			resetcdr = 1;
		}
	}
	if (resetcdr && chan->cdr)
		ast_cdr_reset(chan->cdr, 0);
	if (!strlen(privdb) && privacy) {
		/* If privdb is not specified and we are using privacy, copy from extension */
		strncpy(privdb, chan->exten, sizeof(privdb) - 1);
	}
	if (privacy) {
		if (chan->callerid)
			strncpy(callerid, chan->callerid, sizeof(callerid));
		else
			strcpy(callerid, "");
		ast_callerid_parse(callerid, &n, &l);
		if (l) {
			ast_shrink_phone_number(l);
		} else
			l = "";
		ast_log(LOG_NOTICE, "Privacy DB is '%s', privacy is %d, clid is '%s'\n", privdb, privacy, l);
	}
	cur = peers;
	do {
		/* Remember where to start next time */
		rest = strchr(cur, '&');
		if (rest) {
			*rest = 0;
			rest++;
		}
		/* Get a technology/[device:]number pair */
		tech = cur;
		number = strchr(tech, '/');
		if (!number) {
			ast_log(LOG_WARNING, "Dial argument takes format (technology1/[device:]number1&technology2/[device:]number2...|optional timeout)\n");
			goto out;
		}
		*number = '\0';
		number++;
		tmp = malloc(sizeof(struct localuser));
		if (!tmp) {
			ast_log(LOG_WARNING, "Out of memory\n");
			goto out;
		}
		memset(tmp, 0, sizeof(struct localuser));
		if (transfer) {
			if (strchr(transfer, 't'))
				tmp->allowredirect_in = 1;
                        else    tmp->allowredirect_in = 0;
			if (strchr(transfer, 'T'))
				tmp->allowredirect_out = 1;
                        else    tmp->allowredirect_out = 0;
			if (strchr(transfer, 'r'))
				tmp->ringbackonly = 1;
                        else    tmp->ringbackonly = 0;
			if (strchr(transfer, 'm'))
				tmp->musiconhold = 1;
                        else    tmp->musiconhold = 0;
			if (strchr(transfer, 'H'))
				allowdisconnect = tmp->allowdisconnect = 1;
                        else    allowdisconnect = tmp->allowdisconnect = 0;
			if(strchr(transfer, 'g'))
				go_on=1;
		}
		strncpy(numsubst, number, sizeof(numsubst)-1);
		/* If we're dialing by extension, look at the extension to know what to dial */
		if ((newnum = strstr(numsubst, "BYEXTENSION"))) {
			strncpy(restofit, newnum + strlen("BYEXTENSION"), sizeof(restofit)-1);
			snprintf(newnum, sizeof(numsubst) - (newnum - numsubst), "%s%s", chan->exten,restofit);
			if (option_debug)
				ast_log(LOG_DEBUG, "Dialing by extension %s\n", numsubst);
		}
		/* Request the peer */
		tmp->chan = ast_request(tech, chan->nativeformats, numsubst);
		if (!tmp->chan) {
			/* If we can't, just go on to the next call */
			ast_log(LOG_NOTICE, "Unable to create channel of type '%s'\n", tech);
			if (chan->cdr)
				ast_cdr_busy(chan->cdr);
			free(tmp);
			cur = rest;
			continue;
		}
		if (strlen(tmp->chan->call_forward)) {
			char tmpchan[256];
			if (option_verbose > 2)
				ast_verbose(VERBOSE_PREFIX_3 "Forwarding call to '%s@%s'\n", tmp->chan->call_forward, tmp->chan->context);
			snprintf(tmpchan, sizeof(tmpchan),"%s@%s", tmp->chan->call_forward, tmp->chan->context);
			ast_hangup(tmp->chan);
			tmp->chan = ast_request("Local", chan->nativeformats, tmpchan);
			if (!tmp->chan) {
				ast_log(LOG_NOTICE, "Unable to create local channel for call forward to '%s'\n", tmpchan);
				free(tmp);
				cur = rest;
				continue;
			}
		}
		/* If creating a SIP channel, look for a variable called */
		/* VXML_URL in the calling channel and copy it to the    */
		/* new channel.                                          */
		if (strcasecmp(tech,"SIP")==0)
		{
			headp=&chan->varshead;
			AST_LIST_TRAVERSE(headp,current,entries) {
				if (strcasecmp(ast_var_name(current),"VXML_URL")==0)
				{
					newvar=ast_var_assign(ast_var_name(current),ast_var_value(current));
					newheadp=&tmp->chan->varshead;
					AST_LIST_INSERT_HEAD(newheadp,newvar,entries);
					break;
				}
			}
		}
		/* Check for ALERT_INFO in the SetVar list.  This is for   */
		/* SIP distinctive ring as per the RFC.  For Cisco 7960s,  */
		/* SetVar(ALERT_INFO=<x>) where x is an integer value 1-5. */
		/* However, the RFC says it should be a URL.  -km-         */

		if (strcasecmp(tech,"SIP")==0)
		{
			headp=&chan->varshead;
			AST_LIST_TRAVERSE(headp,current,entries) {
				/* Search for ALERT_INFO */
				if (strcasecmp(ast_var_name(current),"ALERT_INFO")==0)
				{
					newvar=ast_var_assign(ast_var_name(current),ast_var_value(current));
					newheadp=&tmp->chan->varshead;
					AST_LIST_INSERT_HEAD(newheadp,newvar,entries);
					break;
				}
			}
		}
		
		tmp->chan->appl = "AppDial";
		tmp->chan->data = "(Outgoing Line)";
		tmp->chan->whentohangup = 0;
		if (tmp->chan->callerid)
			free(tmp->chan->callerid);
		if (tmp->chan->ani)
			free(tmp->chan->ani);
		if (chan->callerid)
			tmp->chan->callerid = strdup(chan->callerid);
		else
			tmp->chan->callerid = NULL;
		/* Copy language from incoming to outgoing */
		strcpy(tmp->chan->language, chan->language);
		if (!strlen(tmp->chan->musicclass))
			strncpy(tmp->chan->musicclass, chan->musicclass, sizeof(tmp->chan->musicclass) - 1);
		if (chan->ani)
			tmp->chan->ani = strdup(chan->ani);
		else
			tmp->chan->ani = NULL;
		/* Pass hidecallerid setting */
		tmp->chan->restrictcid = chan->restrictcid;
		/* Pass callingpres setting */
		tmp->chan->callingpres = chan->callingpres;
		/* Presense of ADSI CPE on outgoing channel follows ours */
		tmp->chan->adsicpe = chan->adsicpe;
		/* pass the digital flag */
		ast_dup_flag(tmp->chan, chan, AST_FLAG_DIGITAL);
		/* Place the call, but don't wait on the answer */
		res = ast_call(tmp->chan, numsubst, 0);

		/* Save the info in cdr's that we called them */
		if (chan->cdr)
			ast_cdr_setdestchan(chan->cdr, tmp->chan->name);

		/* check the restuls of ast_call */
		if (res) {
			/* Again, keep going even if there's an error */
			if (option_debug)
				ast_log(LOG_DEBUG, "ast call on peer returned %d\n", res);
			else if (option_verbose > 2)
				ast_verbose(VERBOSE_PREFIX_3 "Couldn't call %s\n", numsubst);
			ast_hangup(tmp->chan);
			free(tmp);
			cur = rest;
			continue;
		} else
			if (option_verbose > 2)
				ast_verbose(VERBOSE_PREFIX_3 "Called %s\n", numsubst);
		/* Put them in the list of outgoing thingies...  We're ready now. 
		   XXX If we're forcibly removed, these outgoing calls won't get
		   hung up XXX */
		tmp->stillgoing = -1;
		tmp->next = outgoing;
		outgoing = tmp;
		/* If this line is up, don't try anybody else */
		if (outgoing->chan->_state == AST_STATE_UP)
			break;
		cur = rest;
	} while(cur);
	
	if (timeout && strlen(timeout))
		to = atoi(timeout) * 1000;
	else
		to = -1;
	peer = wait_for_answer(chan, outgoing, &to, &allowredir_in, &allowredir_out, &allowdisconnect);
	if (!peer) {
		if (to) 
			/* Musta gotten hung up */
			res = -1;
		 else 
		 	/* Nobody answered, next please? */
			res=0;
		
		goto out;
	}
	if (peer) {
		/* Ah ha!  Someone answered within the desired timeframe.  Of course after this
		   we will always return with -1 so that it is hung up properly after the 
		   conversation.  */
		hanguptree(outgoing, peer);
		outgoing = NULL;
		/* If appropriate, log that we have a destination channel */
		if (chan->cdr)
			ast_cdr_setdestchan(chan->cdr, peer->name);
		if (peer->name)
			pbx_builtin_setvar_helper(chan, "DIALEDPEERNAME", peer->name);
		if (numsubst)
			pbx_builtin_setvar_helper(chan, "DIALEDPEERNUMBER", numsubst);
		/* Make sure channels are compatible */
		res = ast_channel_make_compatible(chan, peer);
		if (res < 0) {
			ast_log(LOG_WARNING, "Had to drop call because I couldn't make %s compatible with %s\n", chan->name, peer->name);
			ast_hangup(peer);
			return -1;
		}
 		/* JDG: sendurl */
 		if( url && strlen(url) && ast_channel_supports_html(peer) ) {
 			ast_log(LOG_DEBUG, "app_dial: sendurl=%s.\n", url);
 			ast_channel_sendurl( peer, url );
 		} /* /JDG */
		if (announce && announcemsg)
		{
			int res2;
			// Start autoservice on the other chan
			res2 = ast_autoservice_start(chan);
			// Now Stream the File
			if (!res2)
				res2 = ast_streamfile(peer,announcemsg,peer->language);
			if (!res2)
				res2 = ast_waitstream(peer,"");
			// Ok, done. stop autoservice
			res2 = ast_autoservice_stop(chan);
		}
		res = ast_bridge_call(chan, peer, allowredir_in, allowredir_out, allowdisconnect);

		if (res != AST_PBX_NO_HANGUP_PEER)
			ast_hangup(peer);
	}	
out:
	hanguptree(outgoing, NULL);
	LOCAL_USER_REMOVE(u);
	
	if((go_on>0) && (!chan->_softhangup))
	    res=0;
	    
	return res;
}

int unload_module(void)
{
	STANDARD_HANGUP_LOCALUSERS;
	return ast_unregister_application(app);
}

int load_module(void)
{
	int res;
	res = ast_register_application(app, dial_exec, synopsis, descrip);
	return res;
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

/*
 * Asterisk -- A telephony toolkit for Linux.
 *
 * Real-time Transport Protocol support
 * 
 * Copyright (C) 1999, Mark Spencer
 *
 * Mark Spencer <markster@linux-support.net>
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License
 */

#ifndef _ASTERISK_RTP_H
#define _ASTERISK_RTP_H

#include <asterisk/frame.h>
#include <asterisk/io.h>
#include <asterisk/sched.h>
#include <asterisk/channel.h>

#include <netinet/in.h>

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

struct ast_rtp_protocol {
	struct ast_rtp *(*get_rtp_info)(struct ast_channel *chan);				/* Get RTP struct, or NULL if unwilling to transfer */
	int (*set_rtp_peer)(struct ast_channel *chan, struct ast_rtp *peer);	/* Set RTP peer */
	int (*get_rtp_willing)(struct ast_channel *chan);		/* Willing to native bridge */
	char *type;
	struct ast_rtp_protocol *next;
};

struct ast_rtp;

typedef int (*ast_rtp_callback)(struct ast_rtp *rtp, struct ast_frame *f, void *data);

struct ast_rtp *ast_rtp_new(struct sched_context *sched, struct io_context *io);

void ast_rtp_set_peer(struct ast_rtp *rtp, struct sockaddr_in *them);

void ast_rtp_get_peer(struct ast_rtp *rpt, struct sockaddr_in *them);

void ast_rtp_get_us(struct ast_rtp *rtp, struct sockaddr_in *us);

void ast_rtp_destroy(struct ast_rtp *rtp);

void ast_rtp_set_callback(struct ast_rtp *rtp, ast_rtp_callback callback);

void ast_rtp_set_data(struct ast_rtp *rtp, void *data);

int ast_rtp_write(struct ast_rtp *rtp, struct ast_frame *f);

struct ast_frame *ast_rtp_read(struct ast_rtp *rtp);

int ast_rtp_fd(struct ast_rtp *rtp);

int ast_rtp_senddigit(struct ast_rtp *rtp, char digit);

int ast_rtp_settos(struct ast_rtp *rtp, int tos);

int ast2rtp(int id);

int rtp2ast(int id);

char *ast2rtpn(int id);

int ast_rtp_bridge(struct ast_channel *c0, struct ast_channel *c1, int flags, struct ast_frame **fo, struct ast_channel **rc);

int ast_rtp_proto_register(struct ast_rtp_protocol *proto);

void ast_rtp_proto_unregister(struct ast_rtp_protocol *proto);

#if defined(__cplusplus) || defined(c_plusplus)
}
#endif

#endif

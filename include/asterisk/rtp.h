/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2006, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
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

/*!
 * \file rtp.h
 * \brief Supports RTP and RTCP with Symmetric RTP support for NAT traversal.
 *
 * RTP is defined in RFC 3550.
 */

#ifndef _ASTERISK_RTP_H
#define _ASTERISK_RTP_H

#include "asterisk/network.h"

#include "asterisk/frame.h"
#include "asterisk/io.h"
#include "asterisk/sched.h"
#include "asterisk/channel.h"
#include "asterisk/linkedlists.h"

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

/* Codes for RTP-specific data - not defined by our AST_FORMAT codes */
/*! DTMF (RFC2833) */
#define AST_RTP_DTMF            	(1 << 0)
/*! 'Comfort Noise' (RFC3389) */
#define AST_RTP_CN              	(1 << 1)
/*! DTMF (Cisco Proprietary) */
#define AST_RTP_CISCO_DTMF      	(1 << 2)
/*! Maximum RTP-specific code */
#define AST_RTP_MAX             	AST_RTP_CISCO_DTMF

/*! Maxmum number of payload defintions for a RTP session */
#define MAX_RTP_PT			256

#define FLAG_3389_WARNING		(1 << 0)

enum ast_rtp_options {
	AST_RTP_OPT_G726_NONSTANDARD = (1 << 0),
};

enum ast_rtp_get_result {
	/*! Failed to find the RTP structure */
	AST_RTP_GET_FAILED = 0,
	/*! RTP structure exists but true native bridge can not occur so try partial */
	AST_RTP_TRY_PARTIAL,
	/*! RTP structure exists and native bridge can occur */
	AST_RTP_TRY_NATIVE,
};

struct ast_rtp;

/*! \brief This is the structure that binds a channel (SIP/Jingle/H.323) to the RTP subsystem 
*/
struct ast_rtp_protocol {
	/*! Get RTP struct, or NULL if unwilling to transfer */
	enum ast_rtp_get_result (* const get_rtp_info)(struct ast_channel *chan, struct ast_rtp **rtp);
	/*! Get RTP struct, or NULL if unwilling to transfer */
	enum ast_rtp_get_result (* const get_vrtp_info)(struct ast_channel *chan, struct ast_rtp **rtp);
	/*! Get RTP struct, or NULL if unwilling to transfer */
	enum ast_rtp_get_result (* const get_trtp_info)(struct ast_channel *chan, struct ast_rtp **rtp);
	/*! Set RTP peer */
	int (* const set_rtp_peer)(struct ast_channel *chan, struct ast_rtp *peer, struct ast_rtp *vpeer, struct ast_rtp *tpeer, int codecs, int nat_active);
	int (* const get_codec)(struct ast_channel *chan);
	const char * const type;
	AST_LIST_ENTRY(ast_rtp_protocol) list;
};

/*! \brief RTCP quality report storage */
struct ast_rtp_quality {
	unsigned int local_ssrc;          /*!< Our SSRC */
	unsigned int local_lostpackets;   /*!< Our lost packets */
	double       local_jitter;        /*!< Our calculated jitter */
	unsigned int local_count;         /*!< Number of received packets */
	unsigned int remote_ssrc;         /*!< Their SSRC */
	unsigned int remote_lostpackets;  /*!< Their lost packets */
	double       remote_jitter;       /*!< Their reported jitter */
	unsigned int remote_count;        /*!< Number of transmitted packets */
	double       rtt;                 /*!< Round trip time */
};

/*! RTP callback structure */
typedef int (*ast_rtp_callback)(struct ast_rtp *rtp, struct ast_frame *f, void *data);

/*!
 * \brief Get the amount of space required to hold an RTP session
 * \return number of bytes required
 */
size_t ast_rtp_alloc_size(void);

/*!
 * \brief Initializate a RTP session.
 *
 * \param sched
 * \param io
 * \param rtcpenable
 * \param callbackmode
 * \returns A representation (structure) of an RTP session.
 */
struct ast_rtp *ast_rtp_new(struct sched_context *sched, struct io_context *io, int rtcpenable, int callbackmode);

/*!
 * \brief Initializate a RTP session using an in_addr structure.
 *
 * This fuction gets called by ast_rtp_new().
 *
 * \param sched
 * \param io
 * \param rtcpenable
 * \param callbackmode
 * \param in
 * \returns A representation (structure) of an RTP session.
 */
struct ast_rtp *ast_rtp_new_with_bindaddr(struct sched_context *sched, struct io_context *io, int rtcpenable, int callbackmode, struct in_addr in);

void ast_rtp_set_peer(struct ast_rtp *rtp, struct sockaddr_in *them);

/* Copies from rtp to them and returns 1 if there was a change or 0 if it was already the same */
int ast_rtp_get_peer(struct ast_rtp *rtp, struct sockaddr_in *them);

void ast_rtp_get_us(struct ast_rtp *rtp, struct sockaddr_in *us);

struct ast_rtp *ast_rtp_get_bridged(struct ast_rtp *rtp);

/*! Destroy RTP session */
void ast_rtp_destroy(struct ast_rtp *rtp);

void ast_rtp_reset(struct ast_rtp *rtp);

/*! Stop RTP session, do not destroy structure */
void ast_rtp_stop(struct ast_rtp *rtp);

void ast_rtp_set_callback(struct ast_rtp *rtp, ast_rtp_callback callback);

void ast_rtp_set_data(struct ast_rtp *rtp, void *data);

int ast_rtp_write(struct ast_rtp *rtp, struct ast_frame *f);

struct ast_frame *ast_rtp_read(struct ast_rtp *rtp);

struct ast_frame *ast_rtcp_read(struct ast_rtp *rtp);

int ast_rtp_fd(struct ast_rtp *rtp);

int ast_rtcp_fd(struct ast_rtp *rtp);

int ast_rtp_senddigit_begin(struct ast_rtp *rtp, char digit);

int ast_rtp_senddigit_end(struct ast_rtp *rtp, char digit);

int ast_rtp_sendcng(struct ast_rtp *rtp, int level);

int ast_rtp_setqos(struct ast_rtp *rtp, int tos, int cos);

/*! \brief  Setting RTP payload types from lines in a SDP description: */
void ast_rtp_pt_clear(struct ast_rtp* rtp);
/*! \brief Set payload types to defaults */
void ast_rtp_pt_default(struct ast_rtp* rtp);

/*! \brief Copy payload types between RTP structures */
void ast_rtp_pt_copy(struct ast_rtp *dest, struct ast_rtp *src);

void ast_rtp_set_m_type(struct ast_rtp* rtp, int pt);
void ast_rtp_set_rtpmap_type(struct ast_rtp* rtp, int pt,
			     char *mimeType, char *mimeSubtype,
			     enum ast_rtp_options options);

/*! \brief  Mapping between RTP payload format codes and Asterisk codes: */
struct rtpPayloadType ast_rtp_lookup_pt(struct ast_rtp* rtp, int pt);
int ast_rtp_lookup_code(struct ast_rtp* rtp, int isAstFormat, int code);

void ast_rtp_get_current_formats(struct ast_rtp* rtp,
			     int* astFormats, int* nonAstFormats);

/*! \brief  Mapping an Asterisk code into a MIME subtype (string): */
const char *ast_rtp_lookup_mime_subtype(int isAstFormat, int code,
					enum ast_rtp_options options);

/*! \brief Build a string of MIME subtype names from a capability list */
char *ast_rtp_lookup_mime_multiple(char *buf, size_t size, const int capability,
				   const int isAstFormat, enum ast_rtp_options options);

void ast_rtp_setnat(struct ast_rtp *rtp, int nat);

int ast_rtp_getnat(struct ast_rtp *rtp);

/*! \brief Indicate whether this RTP session is carrying DTMF or not */
void ast_rtp_setdtmf(struct ast_rtp *rtp, int dtmf);

/*! \brief Compensate for devices that send RFC2833 packets all at once */
void ast_rtp_setdtmfcompensate(struct ast_rtp *rtp, int compensate);

/*! \brief Enable STUN capability */
void ast_rtp_setstun(struct ast_rtp *rtp, int stun_enable);

/*! \brief Generic STUN request
 * send a generic stun request to the server specified.
 * \param s the socket used to send the request
 * \param dst the address of the STUN server
 * \param username if non null, add the username in the request
 * \param answer if non null, the function waits for a response and
 *    puts here the externally visible address.
 * \return 0 on success, other values on error.
 * The interface it may change in the future.
 */
int ast_stun_request(int s, struct sockaddr_in *dst,
	const char *username, struct sockaddr_in *answer);

/*! \brief Send STUN request for an RTP socket
 * Deprecated, this is just a wrapper for ast_rtp_stun_request()
 */
void ast_rtp_stun_request(struct ast_rtp *rtp, struct sockaddr_in *suggestion, const char *username);

/*! \brief The RTP bridge.
	\arg \ref AstRTPbridge
*/
int ast_rtp_bridge(struct ast_channel *c0, struct ast_channel *c1, int flags, struct ast_frame **fo, struct ast_channel **rc, int timeoutms);

/*! \brief Register an RTP channel client */
int ast_rtp_proto_register(struct ast_rtp_protocol *proto);

/*! \brief Unregister an RTP channel client */
void ast_rtp_proto_unregister(struct ast_rtp_protocol *proto);

int ast_rtp_make_compatible(struct ast_channel *dest, struct ast_channel *src, int media);

/*! \brief If possible, create an early bridge directly between the devices without
           having to send a re-invite later */
int ast_rtp_early_bridge(struct ast_channel *c0, struct ast_channel *c1);

/*! \brief Return RTCP quality string */
char *ast_rtp_get_quality(struct ast_rtp *rtp, struct ast_rtp_quality *qual);

/*! \brief Send an H.261 fast update request. Some devices need this rather than the XML message  in SIP */
int ast_rtcp_send_h261fur(void *data);

void ast_rtp_init(void);                                      /*! Initialize RTP subsystem */
int ast_rtp_reload(void);                                     /*! reload rtp configuration */
void ast_rtp_new_init(struct ast_rtp *rtp);

/*! Set codec preference */
int ast_rtp_codec_setpref(struct ast_rtp *rtp, struct ast_codec_pref *prefs);

/*! Get codec preference */
struct ast_codec_pref *ast_rtp_codec_getpref(struct ast_rtp *rtp);

/*! get format from predefined dynamic payload format */
int ast_rtp_codec_getformat(int pt);

/*! \brief Set rtp timeout */
void ast_rtp_set_rtptimeout(struct ast_rtp *rtp, int timeout);
/*! \brief Set rtp hold timeout */
void ast_rtp_set_rtpholdtimeout(struct ast_rtp *rtp, int timeout);
/*! \brief set RTP keepalive interval */
void ast_rtp_set_rtpkeepalive(struct ast_rtp *rtp, int period);
/*! \brief Get RTP keepalive interval */
int ast_rtp_get_rtpkeepalive(struct ast_rtp *rtp);
/*! \brief Get rtp hold timeout */
int ast_rtp_get_rtpholdtimeout(struct ast_rtp *rtp);
/*! \brief Get rtp timeout */
int ast_rtp_get_rtptimeout(struct ast_rtp *rtp);
/* \brief Put RTP timeout timers on hold during another transaction, like T.38 */
void ast_rtp_set_rtptimers_onhold(struct ast_rtp *rtp);

#if defined(__cplusplus) || defined(c_plusplus)
}
#endif

#endif /* _ASTERISK_RTP_H */

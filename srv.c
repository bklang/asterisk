/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
 *
 * Funding provided by nic.at
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
 * \brief DNS SRV Record Lookup Support for Asterisk
 *
 * \author Mark Spencer <markster@digium.com>
 * 
 * \arg See also \ref AstENUM
 *
 * \note Funding provided by nic.at
 */

#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/nameser.h>
#if __APPLE_CC__ >= 1495
#include <arpa/nameser_compat.h>
#endif
#include <resolv.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#include "asterisk/channel.h"
#include "asterisk/logger.h"
#include "asterisk/srv.h"
#include "asterisk/dns.h"
#include "asterisk/options.h"
#include "asterisk/utils.h"

#ifdef __APPLE__
#undef T_SRV
#define T_SRV 33
#endif

struct srv {
	unsigned short priority;
	unsigned short weight;
	unsigned short portnum;
} __attribute__ ((__packed__));

static int parse_srv(char *host, int hostlen, int *portno, char *answer, int len, char *msg)
{
	int res = 0;
	struct srv *srv = (struct srv *)answer;
	char repl[256] = "";

	if (len < sizeof(struct srv)) {
		printf("Length too short\n");
		return -1;
	}
	answer += sizeof(struct srv);
	len -= sizeof(struct srv);

	if ((res = dn_expand((unsigned char *)msg, (unsigned char *)answer + len, (unsigned char *)answer, repl, sizeof(repl) - 1)) < 0) {
		ast_log(LOG_WARNING, "Failed to expand hostname\n");
		return -1;
	}
	if (res && strcmp(repl, ".")) {
		if (option_verbose > 3)
			ast_verbose( VERBOSE_PREFIX_3 "parse_srv: SRV mapped to host %s, port %d\n", repl, ntohs(srv->portnum));
		if (host) {
			ast_copy_string(host, repl, hostlen);
			host[hostlen-1] = '\0';
		}
		if (portno)
			*portno = ntohs(srv->portnum);
		return 0;
	}
	return -1;
}

struct srv_context {
	char *host;
	int hostlen;
	int *port;
};

static int srv_callback(void *context, char *answer, int len, char *fullanswer)
{
	struct srv_context *c = (struct srv_context *)context;

	if (parse_srv(c->host, c->hostlen, c->port, answer, len, fullanswer)) {
		ast_log(LOG_WARNING, "Failed to parse srv\n");
		return -1;
	}

	if (!ast_strlen_zero(c->host))
		return 1;

	return 0;
}

int ast_get_srv(struct ast_channel *chan, char *host, int hostlen, int *port, const char *service)
{
	struct srv_context context;
	int ret;

	context.host = host;
	context.hostlen = hostlen;
	context.port = port;

	if (chan && ast_autoservice_start(chan) < 0)
		return -1;

	ret = ast_search_dns(&context, service, C_IN, T_SRV, srv_callback);

	if (chan)
		ret |= ast_autoservice_stop(chan);

	if (ret <= 0) {
		host[0] = '\0';
		*port = -1;
		return ret;
	}
	return ret;
}

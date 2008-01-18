/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2007 - 2008, Digium, Inc.
 *
 * Luigi Rizzo (TCP and TLS server code)
 * Brett Bryant <brettbryant@gmail.com> (updated for client support)
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
 * \file
 * \brief Code to support TCP and TLS server/client
 *
 * \author Luigi Rizzo
 * \author Brett Bryant <brettbryant@gmail.com>
 */

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif

#include <sys/signal.h>

#include "asterisk/compat.h"
#include "asterisk/tcptls.h"
#include "asterisk/http.h"
#include "asterisk/utils.h"
#include "asterisk/strings.h"
#include "asterisk/options.h"
#include "asterisk/manager.h"

/*!
 * replacement read/write functions for SSL support.
 * We use wrappers rather than SSL_read/SSL_write directly so
 * we can put in some debugging.
 */

#ifdef DO_SSL
static HOOK_T ssl_read(void *cookie, char *buf, LEN_T len)
{
	int i = SSL_read(cookie, buf, len-1);
#if 0
	if (i >= 0)
		buf[i] = '\0';
	ast_verbose("ssl read size %d returns %d <%s>\n", (int)len, i, buf);
#endif
	return i;
}

static HOOK_T ssl_write(void *cookie, const char *buf, LEN_T len)
{
#if 0
	char *s = alloca(len+1);
	strncpy(s, buf, len);
	s[len] = '\0';
	ast_verbose("ssl write size %d <%s>\n", (int)len, s);
#endif
	return SSL_write(cookie, buf, len);
}

static int ssl_close(void *cookie)
{
	close(SSL_get_fd(cookie));
	SSL_shutdown(cookie);
	SSL_free(cookie);
	return 0;
}
#endif	/* DO_SSL */

HOOK_T server_read(struct server_instance *ser, void *buf, size_t count)
{
	if (!ser->ssl) 
		return read(ser->fd, buf, count);
#ifdef DO_SSL
	else
		return ssl_read(ser->ssl, buf, count);
#endif
}

HOOK_T server_write(struct server_instance *ser, void *buf, size_t count)
{
	if (!ser->ssl) 
		return write(ser->fd, buf, count);
#ifdef DO_SSL
	else
		return ssl_write(ser->ssl, buf, count);
#endif
}

void *server_root(void *data)
{
	struct server_args *desc = data;
	int fd;
	struct sockaddr_in sin;
	socklen_t sinlen;
	struct server_instance *ser;
	pthread_t launched;
	
	for (;;) {
		int i, flags;

		if (desc->periodic_fn)
			desc->periodic_fn(desc);
		i = ast_wait_for_input(desc->accept_fd, desc->poll_timeout);
		if (i <= 0)
			continue;
		sinlen = sizeof(sin);
		fd = accept(desc->accept_fd, (struct sockaddr *)&sin, &sinlen);
		if (fd < 0) {
			if ((errno != EAGAIN) && (errno != EINTR))
				ast_log(LOG_WARNING, "Accept failed: %s\n", strerror(errno));
			continue;
		}
		ser = ast_calloc(1, sizeof(*ser));
		if (!ser) {
			ast_log(LOG_WARNING, "No memory for new session: %s\n", strerror(errno));
			close(fd);
			continue;
		}
		flags = fcntl(fd, F_GETFL);
		fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
		ser->fd = fd;
		ser->parent = desc;
		memcpy(&ser->requestor, &sin, sizeof(ser->requestor));

		ser->client = 0;
			
		if (ast_pthread_create_detached_background(&launched, NULL, ast_make_file_from_fd, ser)) {
			ast_log(LOG_WARNING, "Unable to launch helper thread: %s\n", strerror(errno));
			close(ser->fd);
			ast_free(ser);
		}
	}
	return NULL;
}

static int __ssl_setup(struct ast_tls_config *cfg, int client)
{
#ifndef DO_SSL
	cfg->enabled = 0;
	return 0;
#else
	if (!cfg->enabled)
		return 0;

	SSL_load_error_strings();
	SSLeay_add_ssl_algorithms();

	if (!(cfg->ssl_ctx = SSL_CTX_new( client ? SSLv23_client_method() : SSLv23_server_method() ))) {
		ast_log(LOG_DEBUG, "Sorry, SSL_CTX_new call returned null...\n");
		cfg->enabled = 0;
		return 0;
	}
	if (!ast_strlen_zero(cfg->certfile)) {
		if (SSL_CTX_use_certificate_file(cfg->ssl_ctx, cfg->certfile, SSL_FILETYPE_PEM) == 0 ||
		    SSL_CTX_use_PrivateKey_file(cfg->ssl_ctx, cfg->certfile, SSL_FILETYPE_PEM) == 0 ||
		    SSL_CTX_check_private_key(cfg->ssl_ctx) == 0 ) {
			if (!client) {
				/* Clients don't need a certificate, but if its setup we can use it */
				ast_verbose("ssl cert error <%s>", cfg->certfile);
				sleep(2);
				cfg->enabled = 0;
				return 0;
			}
		}
	}
	if (!ast_strlen_zero(cfg->cipher)) {
		if (SSL_CTX_set_cipher_list(cfg->ssl_ctx, cfg->cipher) == 0 ) {
			if (!client) {
				ast_verbose("ssl cipher error <%s>", cfg->cipher);
				sleep(2);
				cfg->enabled = 0;
				return 0;
			}
		}
	}
	if (!ast_strlen_zero(cfg->cafile) || !ast_strlen_zero(cfg->capath)) {
		if (SSL_CTX_load_verify_locations(cfg->ssl_ctx, S_OR(cfg->cafile, NULL), S_OR(cfg->capath,NULL)) == 0)
			ast_verbose("ssl CA file(%s)/path(%s) error\n", cfg->cafile, cfg->capath);
	}

	ast_verbose("ssl cert ok\n");
	return 1;
#endif
}

int ssl_setup(struct ast_tls_config *cfg)
{
	return __ssl_setup(cfg, 0);
}

/*! A generic client routine for a TCP client
 *  and starts a thread for handling accept()
 */
struct server_instance *client_start(struct server_args *desc)
{
	int flags;
	struct server_instance *ser = NULL;

	/* Do nothing if nothing has changed */
	if(!memcmp(&desc->oldsin, &desc->sin, sizeof(desc->oldsin))) {
		if (option_debug)
			ast_log(LOG_DEBUG, "Nothing changed in %s\n", desc->name);
		return NULL;
	}

	desc->oldsin = desc->sin;

	if (desc->accept_fd != -1)
		close(desc->accept_fd);

	desc->accept_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (desc->accept_fd < 0) {
		ast_log(LOG_WARNING, "Unable to allocate socket for %s: %s\n",
			desc->name, strerror(errno));
		return NULL;
	}

	if (connect(desc->accept_fd, (const struct sockaddr *)&desc->sin, sizeof(desc->sin))) {
		ast_log(LOG_NOTICE, "Unable to connect %s to %s:%d: %s\n",
			desc->name,
			ast_inet_ntoa(desc->sin.sin_addr), ntohs(desc->sin.sin_port),
			strerror(errno));
		goto error;
	}

	if (!(ser = ast_calloc(1, sizeof(*ser))))
		goto error;

	flags = fcntl(desc->accept_fd, F_GETFL);
	fcntl(desc->accept_fd, F_SETFL, flags & ~O_NONBLOCK);

	ser->fd = desc->accept_fd;
	ser->parent = desc;
	ser->parent->worker_fn = NULL;
	memcpy(&ser->requestor, &desc->sin, sizeof(ser->requestor));

	ser->client = 1;

	if (desc->tls_cfg) {
		desc->tls_cfg->enabled = 1;
		__ssl_setup(desc->tls_cfg, 1);
	}

	if (!ast_make_file_from_fd(ser))
		goto error;

	return ser;

error:
	close(desc->accept_fd);
	desc->accept_fd = -1;
	if (ser)
		ast_free(ser);
	return NULL;
}

/*!
 * This is a generic (re)start routine for a TCP server,
 * which does the socket/bind/listen and starts a thread for handling
 * accept().
 */

void server_start(struct server_args *desc)
{
	int flags;
	int x = 1;
	
	/* Do nothing if nothing has changed */
	if (!memcmp(&desc->oldsin, &desc->sin, sizeof(desc->oldsin))) {
		if (option_debug)
			ast_log(LOG_DEBUG, "Nothing changed in %s\n", desc->name);
		return;
	}
	
	desc->oldsin = desc->sin;
	
	/* Shutdown a running server if there is one */
	if (desc->master != AST_PTHREADT_NULL) {
		pthread_cancel(desc->master);
		pthread_kill(desc->master, SIGURG);
		pthread_join(desc->master, NULL);
	}
	
	if (desc->accept_fd != -1)
		close(desc->accept_fd);

	/* If there's no new server, stop here */
	if (desc->sin.sin_family == 0)
		return;

	desc->accept_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (desc->accept_fd < 0) {
		ast_log(LOG_WARNING, "Unable to allocate socket for %s: %s\n",
			desc->name, strerror(errno));
		return;
	}
	
	setsockopt(desc->accept_fd, SOL_SOCKET, SO_REUSEADDR, &x, sizeof(x));
	if (bind(desc->accept_fd, (struct sockaddr *)&desc->sin, sizeof(desc->sin))) {
		ast_log(LOG_NOTICE, "Unable to bind %s to %s:%d: %s\n",
			desc->name,
			ast_inet_ntoa(desc->sin.sin_addr), ntohs(desc->sin.sin_port),
			strerror(errno));
		goto error;
	}
	if (listen(desc->accept_fd, 10)) {
		ast_log(LOG_NOTICE, "Unable to listen for %s!\n", desc->name);
		goto error;
	}
	flags = fcntl(desc->accept_fd, F_GETFL);
	fcntl(desc->accept_fd, F_SETFL, flags | O_NONBLOCK);
	if (ast_pthread_create_background(&desc->master, NULL, desc->accept_fn, desc)) {
		ast_log(LOG_NOTICE, "Unable to launch %s on %s:%d: %s\n",
			desc->name,
			ast_inet_ntoa(desc->sin.sin_addr), ntohs(desc->sin.sin_port),
			strerror(errno));
		goto error;
	}
	return;

error:
	close(desc->accept_fd);
	desc->accept_fd = -1;
}

void server_stop(struct server_args *desc)
{
	/* Shutdown a running server if there is one */
	if (desc->master != AST_PTHREADT_NULL) {
		pthread_cancel(desc->master);
		pthread_kill(desc->master, SIGURG);
		pthread_join(desc->master, NULL);
	}
	if (desc->accept_fd != -1)
		close(desc->accept_fd);
	desc->accept_fd = -1;
}

/*!
* creates a FILE * from the fd passed by the accept thread.
* This operation is potentially expensive (certificate verification),
* so we do it in the child thread context.
*/
void *ast_make_file_from_fd(void *data)
{
	struct server_instance *ser = data;
	int (*ssl_setup)(SSL *) = (ser->client) ? SSL_connect : SSL_accept;
	int ret;
	char err[256];

	/*
	* open a FILE * as appropriate.
	*/
	if (!ser->parent->tls_cfg)
		ser->f = fdopen(ser->fd, "w+");
#ifdef DO_SSL
	else if ( (ser->ssl = SSL_new(ser->parent->tls_cfg->ssl_ctx)) ) {
		SSL_set_fd(ser->ssl, ser->fd);
		if ((ret = ssl_setup(ser->ssl)) <= 0) {
			if(option_verbose > 1)
				ast_verbose(VERBOSE_PREFIX_2 "Problem setting up ssl connection: %s\n", ERR_error_string(ERR_get_error(), err));
		} else {
#if defined(HAVE_FUNOPEN)	/* the BSD interface */
			ser->f = funopen(ser->ssl, ssl_read, ssl_write, NULL, ssl_close);

#elif defined(HAVE_FOPENCOOKIE)	/* the glibc/linux interface */
			static const cookie_io_functions_t cookie_funcs = {
				ssl_read, ssl_write, NULL, ssl_close
			};
			ser->f = fopencookie(ser->ssl, "w+", cookie_funcs);
#else
			/* could add other methods here */
			ast_log(LOG_WARNING, "no ser->f methods attempted!");
#endif
			if ((ser->client && !ast_test_flag(&ser->parent->tls_cfg->flags, AST_SSL_DONT_VERIFY_SERVER))
				|| (!ser->client && ast_test_flag(&ser->parent->tls_cfg->flags, AST_SSL_VERIFY_CLIENT))) {
				X509 *peer;
				long res;
				peer = SSL_get_peer_certificate(ser->ssl);
				if (!peer)
					ast_log(LOG_WARNING, "No peer certificate\n");
				res = SSL_get_verify_result(ser->ssl);
				if (res != X509_V_OK)
					ast_log(LOG_WARNING, "Certificate did not verify: %s\n", X509_verify_cert_error_string(res));
				if (!ast_test_flag(&ser->parent->tls_cfg->flags, AST_SSL_IGNORE_COMMON_NAME)) {
					ASN1_STRING *str;
					unsigned char *str2;
					X509_NAME *name = X509_get_subject_name(peer);
					int pos = -1;
					int found = 0;
				
					for (;;) {
						/* Walk the certificate to check all available "Common Name" */
						/* XXX Probably should do a gethostbyname on the hostname and compare that as well */
						pos = X509_NAME_get_index_by_NID(name, NID_commonName, pos);
						if (pos < 0)
							break;
						str = X509_NAME_ENTRY_get_data(X509_NAME_get_entry(name, pos));
						ASN1_STRING_to_UTF8(&str2, str);
						if (str2) {
							if (!strcasecmp(ser->parent->hostname, (char *) str2))
								found = 1;
							ast_log(LOG_DEBUG, "SSL Common Name compare s1='%s' s2='%s'\n", ser->parent->hostname, str2);
							OPENSSL_free(str2);
						}
						if (found)
							break;
					}
					if (!found) {
						ast_log(LOG_WARNING, "Certificate common name did not match (%s)\n", ser->parent->hostname);
						if (peer)
							X509_free(peer);
						fclose(ser->f);
						return NULL;
					}
				}
				if (peer)
					X509_free(peer);
			}
		}
		if (!ser->f)	/* no success opening descriptor stacking */
			SSL_free(ser->ssl);
   }
#endif /* DO_SSL */

	if (!ser->f) {
		close(ser->fd);
		ast_log(LOG_WARNING, "FILE * open failed!\n");
		ast_free(ser);
		return NULL;
	}

	if (ser && ser->parent->worker_fn)
		return ser->parent->worker_fn(ser);
	else
		return ser;
}

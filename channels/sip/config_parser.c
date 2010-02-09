/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2010, Digium, Inc.
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
 * \brief sip config parsing functions and unit tests
 */

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#include "include/sip.h"
#include "include/config_parser.h"
#include "include/sip_utils.h"

/*! \brief Parse register=> line in sip.conf
 *
 * \retval 0 on success
 * \retval -1 on failure
 */
int sip_parse_register_line(struct sip_registry *reg, int default_expiry, const char *value, int lineno)
{
	int portnum = 0;
	enum sip_transport transport = SIP_TRANSPORT_UDP;
	char buf[256] = "";
	char *userpart = NULL, *hostpart = NULL;
	/* register => [peer?][transport://]user[@domain][:secret[:authuser]]@host[:port][/extension][~expiry] */
	AST_DECLARE_APP_ARGS(pre1,
		AST_APP_ARG(peer);
		AST_APP_ARG(userpart);
	);
	AST_DECLARE_APP_ARGS(pre2,
		AST_APP_ARG(transport);
		AST_APP_ARG(blank);
		AST_APP_ARG(userpart);
	);
	AST_DECLARE_APP_ARGS(user1,
		AST_APP_ARG(userpart);
		AST_APP_ARG(secret);
		AST_APP_ARG(authuser);
	);
	AST_DECLARE_APP_ARGS(host1,
		AST_APP_ARG(hostpart);
		AST_APP_ARG(expiry);
	);
	AST_DECLARE_APP_ARGS(host2,
		AST_APP_ARG(hostpart);
		AST_APP_ARG(extension);
	);
	AST_DECLARE_APP_ARGS(host3,
		AST_APP_ARG(host);
		AST_APP_ARG(port);
	);

	if (!value) {
		return -1;
	}

	if (!reg) {
		return -1;
	}
	ast_copy_string(buf, value, sizeof(buf));

	/*! register => [peer?][transport://]user[@domain][:secret[:authuser]]@host[:port][/extension][~expiry]
	 * becomes
	 *   userpart => [peer?][transport://]user[@domain][:secret[:authuser]]
	 *   hostpart => host[:port][/extension][~expiry]
	 */
	if ((hostpart = strrchr(buf, '@'))) {
		*hostpart++ = '\0';
		userpart = buf;
	}

	if (ast_strlen_zero(userpart) || ast_strlen_zero(hostpart)) {
		ast_log(LOG_WARNING, "Format for registration is [peer?][transport://]user[@domain][:secret[:authuser]]@host[:port][/extension][~expiry] at line %d\n", lineno);
		return -1;
	}

	/*!
	 * pre1.peer => peer
	 * pre1.userpart => [transport://]user[@domain][:secret[:authuser]]
	 * hostpart => host[:port][/extension][~expiry]
	 */
	AST_NONSTANDARD_RAW_ARGS(pre1, userpart, '?');
	if (ast_strlen_zero(pre1.userpart)) {
		pre1.userpart = pre1.peer;
		pre1.peer = NULL;
	}

	/*!
	 * pre1.peer => peer
	 * pre2.transport = transport
	 * pre2.userpart => user[@domain][:secret[:authuser]]
	 * hostpart => host[:port][/extension][~expiry]
	 */
	AST_NONSTANDARD_RAW_ARGS(pre2, pre1.userpart, '/');
	if (ast_strlen_zero(pre2.userpart)) {
		pre2.userpart = pre2.transport;
		pre2.transport = NULL;
	} else {
		pre2.transport[strlen(pre2.transport) - 1] = '\0'; /* Remove trailing : */
	}

	if (!ast_strlen_zero(pre2.blank)) {
		ast_log(LOG_WARNING, "Format for registration is [peer?][transport://]user[@domain][:secret[:authuser]]@host[:port][/extension][~expiry] at line %d\n", lineno);
		return -1;
	}

	/*!
	 * pre1.peer => peer
	 * pre2.transport = transport
	 * user1.userpart => user[@domain]
	 * user1.secret => secret
	 * user1.authuser => authuser
	 * hostpart => host[:port][/extension][~expiry]
	 */
	AST_NONSTANDARD_RAW_ARGS(user1, pre2.userpart, ':');

	/*!
	 * pre1.peer => peer
	 * pre2.transport = transport
	 * user1.userpart => user[@domain]
	 * user1.secret => secret
	 * user1.authuser => authuser
	 * host1.hostpart => host[:port][/extension]
	 * host1.expiry => [expiry]
	 */
	AST_NONSTANDARD_RAW_ARGS(host1, hostpart, '~');

	/*!
	 * pre1.peer => peer
	 * pre2.transport = transport
	 * user1.userpart => user[@domain]
	 * user1.secret => secret
	 * user1.authuser => authuser
	 * host2.hostpart => host[:port]
	 * host2.extension => [extension]
	 * host1.expiry => [expiry]
	 */
	AST_NONSTANDARD_RAW_ARGS(host2, host1.hostpart, '/');

	/*!
	 * pre1.peer => peer
	 * pre2.transport = transport
	 * user1.userpart => user[@domain]
	 * user1.secret => secret
	 * user1.authuser => authuser
	 * host3.host => host
	 * host3.port => port
	 * host2.extension => extension
	 * host1.expiry => expiry
	 */
	AST_NONSTANDARD_RAW_ARGS(host3, host2.hostpart, ':');

	if (host3.port) {
		if (!(portnum = port_str2int(host3.port, 0))) {
			ast_log(LOG_NOTICE, "'%s' is not a valid port number on line %d of sip.conf. using default.\n", host3.port, lineno);
		}
	}

	/* set transport type */
	if (!pre2.transport) {
		transport = SIP_TRANSPORT_UDP;
	} else if (!strncasecmp(pre2.transport, "tcp", 3)) {
		transport = SIP_TRANSPORT_TCP;
	} else if (!strncasecmp(pre2.transport, "tls", 3)) {
		transport = SIP_TRANSPORT_TLS;
	} else if (!strncasecmp(pre2.transport, "udp", 3)) {
		transport = SIP_TRANSPORT_UDP;
	} else {
		transport = SIP_TRANSPORT_UDP;
		ast_log(LOG_NOTICE, "'%.3s' is not a valid transport type on line %d of sip.conf. defaulting to udp.\n", pre2.transport, lineno);
	}

	/* if no portnum specified, set default for transport */
	if (!portnum) {
		if (transport == SIP_TRANSPORT_TLS) {
			portnum = STANDARD_TLS_PORT;
		} else {
			portnum = STANDARD_SIP_PORT;
		}
	}

	/* copy into sip_registry object */
	ast_string_field_set(reg, callback, ast_strip_quoted(S_OR(host2.extension, "s"), "\"", "\""));
	ast_string_field_set(reg, username, ast_strip_quoted(S_OR(user1.userpart, ""), "\"", "\""));
	ast_string_field_set(reg, hostname, ast_strip_quoted(S_OR(host3.host, ""), "\"", "\""));
	ast_string_field_set(reg, authuser, ast_strip_quoted(S_OR(user1.authuser, ""), "\"", "\""));
	ast_string_field_set(reg, secret, ast_strip_quoted(S_OR(user1.secret, ""), "\"", "\""));
	ast_string_field_set(reg, peername, ast_strip_quoted(S_OR(pre1.peer, ""), "\"", "\""));

	reg->transport = transport;
	reg->timeout = reg->expire = -1;
	reg->portno = portnum;
	reg->callid_valid = FALSE;
	reg->ocseq = INITIAL_CSEQ;
	reg->refresh = reg->expiry = reg->configured_expiry = (host1.expiry ? atoi(ast_strip_quoted(host1.expiry, "\"", "\"")) : default_expiry);

	return 0;
}

AST_TEST_DEFINE(sip_parse_register_line_test)
{
	int res = AST_TEST_PASS;
	struct sip_registry *reg;
	int default_expiry = 120;
	const char *reg1 = "name@domain";
	const char *reg2 = "name:pass@domain";
	const char *reg3 = "name@namedomain:pass:authuser@domain";
	const char *reg4 = "name@namedomain:pass:authuser@domain/extension";
	const char *reg5 = "tcp://name@namedomain:pass:authuser@domain/extension";
	const char *reg6 = "tls://name@namedomain:pass:authuser@domain/extension~111";
	const char *reg7 = "peer?tcp://name@namedomain:pass:authuser@domain:1234/extension~111";
	const char *reg8 = "peer?name@namedomain:pass:authuser@domain:1234/extension~111";
	const char *reg9 = "peer?name:pass:authuser:1234/extension~111";
	const char *reg10 = "@domin:1234";

	switch (cmd) {
	case TEST_INIT:
		info->name = "sip_parse_register_line_test";
		info->category = "channels/chan_sip/";
		info->summary = "tests sip register line parsing";
		info->description =
							" Tests parsing of various register line configurations."
							" Verifies output matches expected behavior.";
		return AST_TEST_NOT_RUN;
	case TEST_EXECUTE:
		break;
	}

	/* ---Test reg 1, simple config --- */
	if (!(reg = ast_calloc_with_stringfields(1, struct sip_registry, 256))) {
		goto alloc_fail;
	} else if (
	    sip_parse_register_line(reg, default_expiry, reg1, 1) ||
		strcmp(reg->callback, "s")           ||
		strcmp(reg->username, "name")       ||
		strcmp(reg->hostname, "domain")     ||
		strcmp(reg->authuser, "")           ||
		strcmp(reg->secret, "")             ||
		strcmp(reg->peername, "")           ||
		reg->transport != SIP_TRANSPORT_UDP ||
		reg->timeout != -1                  ||
		reg->expire != -1                   ||
		reg->refresh != default_expiry ||
		reg->expiry != default_expiry ||
		reg->configured_expiry != default_expiry ||
		reg->portno != STANDARD_SIP_PORT    ||
		reg->callid_valid != FALSE          ||
		reg->ocseq != INITIAL_CSEQ) {

		ast_test_status_update(test, "Test 1: simple config failed\n");
		res = AST_TEST_FAIL;
	}
	ast_string_field_free_memory(reg);
	ast_free(reg);

	/* ---Test reg 2, add secret --- */
	if (!(reg = ast_calloc_with_stringfields(1, struct sip_registry, 256))) {
		goto alloc_fail;
	} else if (
	    sip_parse_register_line(reg, default_expiry, reg2, 1) ||
		strcmp(reg->callback, "s")           ||
		strcmp(reg->username, "name")       ||
		strcmp(reg->hostname, "domain")     ||
		strcmp(reg->authuser, "")           ||
		strcmp(reg->secret, "pass")         ||
		strcmp(reg->peername, "")           ||
		reg->transport != SIP_TRANSPORT_UDP ||
		reg->timeout != -1                  ||
		reg->expire != -1                   ||
		reg->refresh != default_expiry ||
		reg->expiry != default_expiry ||
		reg->configured_expiry != default_expiry ||
		reg->portno != STANDARD_SIP_PORT    ||
		reg->callid_valid != FALSE          ||
		reg->ocseq != INITIAL_CSEQ) {

		ast_test_status_update(test,  "Test 2: add secret failed\n");
		res = AST_TEST_FAIL;
	}
	ast_string_field_free_memory(reg);
	ast_free(reg);

	/* ---Test reg 3, add userdomain and authuser --- */
	if (!(reg = ast_calloc_with_stringfields(1, struct sip_registry, 256))) {
		goto alloc_fail;
	} else if (
	    sip_parse_register_line(reg, default_expiry, reg3, 1) ||
		strcmp(reg->callback, "s")           ||
		strcmp(reg->username, "name@namedomain") ||
		strcmp(reg->hostname, "domain")     ||
		strcmp(reg->authuser, "authuser")           ||
		strcmp(reg->secret, "pass")         ||
		strcmp(reg->peername, "")           ||
		reg->transport != SIP_TRANSPORT_UDP ||
		reg->timeout != -1                  ||
		reg->expire != -1                   ||
		reg->refresh != default_expiry ||
		reg->expiry != default_expiry ||
		reg->configured_expiry != default_expiry ||
		reg->portno != STANDARD_SIP_PORT    ||
		reg->callid_valid != FALSE          ||
		reg->ocseq != INITIAL_CSEQ) {

		ast_test_status_update(test, "Test 3: add userdomain and authuser failed\n");
		res = AST_TEST_FAIL;
	}
	ast_string_field_free_memory(reg);
	ast_free(reg);

	/* ---Test reg 4, add callback extension --- */
	if (!(reg = ast_calloc_with_stringfields(1, struct sip_registry, 256))) {
		goto alloc_fail;
	} else if (
	    sip_parse_register_line(reg, default_expiry, reg4, 1) ||
		strcmp(reg->callback, "extension")           ||
		strcmp(reg->username, "name@namedomain") ||
		strcmp(reg->hostname, "domain")     ||
		strcmp(reg->authuser, "authuser")           ||
		strcmp(reg->secret, "pass")         ||
		strcmp(reg->peername, "")           ||
		reg->transport != SIP_TRANSPORT_UDP ||
		reg->timeout != -1                  ||
		reg->expire != -1                   ||
		reg->refresh != default_expiry ||
		reg->expiry != default_expiry ||
		reg->configured_expiry != default_expiry ||
		reg->portno != STANDARD_SIP_PORT    ||
		reg->callid_valid != FALSE          ||
		reg->ocseq != INITIAL_CSEQ) {

		ast_test_status_update(test, "Test 4: add callback extension failed\n");
		res = AST_TEST_FAIL;
	}
	ast_string_field_free_memory(reg);
	ast_free(reg);

	/* ---Test reg 5, add transport --- */
	if (!(reg = ast_calloc_with_stringfields(1, struct sip_registry, 256))) {
		goto alloc_fail;
	} else if (
	    sip_parse_register_line(reg, default_expiry, reg5, 1) ||
		strcmp(reg->callback, "extension")           ||
		strcmp(reg->username, "name@namedomain") ||
		strcmp(reg->hostname, "domain")     ||
		strcmp(reg->authuser, "authuser")           ||
		strcmp(reg->secret, "pass")         ||
		strcmp(reg->peername, "")           ||
		reg->transport != SIP_TRANSPORT_TCP ||
		reg->timeout != -1                  ||
		reg->expire != -1                   ||
		reg->refresh != default_expiry ||
		reg->expiry != default_expiry ||
		reg->configured_expiry != default_expiry ||
		reg->portno != STANDARD_SIP_PORT    ||
		reg->callid_valid != FALSE          ||
		reg->ocseq != INITIAL_CSEQ) {

		ast_test_status_update(test, "Test 5: add transport failed\n");
		res = AST_TEST_FAIL;
	}
	ast_string_field_free_memory(reg);
	ast_free(reg);

	/* ---Test reg 6, change to tls transport, add expiry  --- */
	if (!(reg = ast_calloc_with_stringfields(1, struct sip_registry, 256))) {
		goto alloc_fail;
	} else if (
	    sip_parse_register_line(reg, default_expiry, reg6, 1) ||
		strcmp(reg->callback, "extension")           ||
		strcmp(reg->username, "name@namedomain") ||
		strcmp(reg->hostname, "domain")     ||
		strcmp(reg->authuser, "authuser")           ||
		strcmp(reg->secret, "pass")         ||
		strcmp(reg->peername, "")           ||
		reg->transport != SIP_TRANSPORT_TLS ||
		reg->timeout != -1                  ||
		reg->expire != -1                   ||
		reg->refresh != 111 ||
		reg->expiry != 111 ||
		reg->configured_expiry != 111 ||
		reg->portno != STANDARD_TLS_PORT    ||
		reg->callid_valid != FALSE          ||
		reg->ocseq != INITIAL_CSEQ) {

		ast_test_status_update(test, "Test 6: change to tls transport and add expiry failed\n");
		res = AST_TEST_FAIL;
	}
	ast_string_field_free_memory(reg);
	ast_free(reg);

	/* ---Test reg 7, change transport to tcp, add custom port, and add peer --- */
	if (!(reg = ast_calloc_with_stringfields(1, struct sip_registry, 256))) {
		goto alloc_fail;
	} else if (
	    sip_parse_register_line(reg, default_expiry, reg7, 1) ||
		strcmp(reg->callback, "extension")           ||
		strcmp(reg->username, "name@namedomain") ||
		strcmp(reg->hostname, "domain")     ||
		strcmp(reg->authuser, "authuser")           ||
		strcmp(reg->secret, "pass")         ||
		strcmp(reg->peername, "peer")           ||
		reg->transport != SIP_TRANSPORT_TCP ||
		reg->timeout != -1                  ||
		reg->expire != -1                   ||
		reg->refresh != 111 ||
		reg->expiry != 111 ||
		reg->configured_expiry != 111 ||
		reg->portno != 1234    ||
		reg->callid_valid != FALSE          ||
		reg->ocseq != INITIAL_CSEQ) {

		ast_test_status_update(test, "Test 7, change transport to tcp, add custom port, and add peer failed.\n");
		res = AST_TEST_FAIL;
	}
	ast_string_field_free_memory(reg);
	ast_free(reg);

	/* ---Test reg 8, remove transport --- */
	if (!(reg = ast_calloc_with_stringfields(1, struct sip_registry, 256))) {
		goto alloc_fail;
	} else if (
	    sip_parse_register_line(reg, default_expiry, reg8, 1) ||
		strcmp(reg->callback, "extension")           ||
		strcmp(reg->username, "name@namedomain") ||
		strcmp(reg->hostname, "domain")     ||
		strcmp(reg->authuser, "authuser")           ||
		strcmp(reg->secret, "pass")         ||
		strcmp(reg->peername, "peer")           ||
		reg->transport != SIP_TRANSPORT_UDP ||
		reg->timeout != -1                  ||
		reg->expire != -1                   ||
		reg->refresh != 111 ||
		reg->expiry != 111 ||
		reg->configured_expiry != 111 ||
		reg->portno != 1234    ||
		reg->callid_valid != FALSE          ||
		reg->ocseq != INITIAL_CSEQ) {

		ast_test_status_update(test, "Test 8, remove transport failed.\n");
		res = AST_TEST_FAIL;
	}
	ast_string_field_free_memory(reg);
	ast_free(reg);

	/* ---Test reg 9, missing domain, expected to fail --- */
	if (!(reg = ast_calloc_with_stringfields(1, struct sip_registry, 256))) {
		goto alloc_fail;
	} else if (!sip_parse_register_line(reg, default_expiry, reg9, 1)) {
		ast_test_status_update(test,
				"Test 9, missing domain, expected to fail but did not.\n");
		res = AST_TEST_FAIL;
	}
	ast_string_field_free_memory(reg);
	ast_free(reg);

	/* ---Test reg 10,  missing user, expected to fail --- */
	if (!(reg = ast_calloc_with_stringfields(1, struct sip_registry, 256))) {
		goto alloc_fail;
	} else if (!sip_parse_register_line(reg, default_expiry, reg10, 1)) {
		ast_test_status_update(test,
				"Test 10, missing user expected to fail but did not\n");
		res = AST_TEST_FAIL;
	}
	ast_string_field_free_memory(reg);
	ast_free(reg);

	/* ---Test reg 11, no registry object, expected to fail--- */
	if (!sip_parse_register_line(NULL, default_expiry, reg1, 1)) {
		ast_test_status_update(test,
				"Test 11, no registery object, expected to fail but did not.\n");
		res = AST_TEST_FAIL;
	}

	/* ---Test reg 11,  no registry line, expected to fail --- */
	if (!(reg = ast_calloc_with_stringfields(1, struct sip_registry, 256))) {
		goto alloc_fail;
	} else if (!sip_parse_register_line(reg, default_expiry, NULL, 1)) {

		ast_test_status_update(test,
				"Test 11, NULL register line expected to fail but did not.\n");
		res = AST_TEST_FAIL;
	}
	ast_string_field_free_memory(reg);
	ast_free(reg);


	return res;

alloc_fail:
	ast_test_status_update(test, "Out of memory. \n");
	return res;
}

int sip_parse_host(char *line, int lineno, char **hostname, int *portnum, enum sip_transport *transport)
{
	char *port;

	if (ast_strlen_zero(line)) {
		return -1;
	}
	if ((*hostname = strstr(line, "://"))) {
		*hostname += 3;

		if (!strncasecmp(line, "tcp", 3))
			*transport = SIP_TRANSPORT_TCP;
		else if (!strncasecmp(line, "tls", 3))
			*transport = SIP_TRANSPORT_TLS;
		else if (!strncasecmp(line, "udp", 3))
			*transport = SIP_TRANSPORT_UDP;
		else
			ast_log(LOG_NOTICE, "'%.3s' is not a valid transport type on line %d of sip.conf. defaulting to udp.\n", line, lineno);
	} else {
		*hostname = line;
		*transport = SIP_TRANSPORT_UDP;
	}

	if ((line = strrchr(*hostname, '@')))
		line++;
	else
		line = *hostname;

	if ((port = strrchr(line, ':'))) {
		*port++ = '\0';

		if (!sscanf(port, "%5u", portnum)) {
			ast_log(LOG_NOTICE, "'%s' is not a valid port number on line %d of sip.conf. using default.\n", port, lineno);
			port = NULL;
		}
	}

	if (!port) {
		if (*transport & SIP_TRANSPORT_TLS) {
			*portnum = STANDARD_TLS_PORT;
		} else {
			*portnum = STANDARD_SIP_PORT;
		}
	}

	return 0;
}

AST_TEST_DEFINE(sip_parse_host_line_test)
{
	int res = AST_TEST_PASS;
	char *host;
	int port;
	enum sip_transport transport;
	char host1[] = "www.blah.com";
	char host2[] = "tcp://www.blah.com";
	char host3[] = "tls://10.10.10.10";
	char host4[] = "tls://10.10.10.10:1234";
	char host5[] = "10.10.10.10:1234";

	switch (cmd) {
	case TEST_INIT:
		info->name = "sip_parse_host_line_test";
		info->category = "channels/chan_sip/";
		info->summary = "tests sip.conf host line parsing";
		info->description =
							" Tests parsing of various host line configurations."
							" Verifies output matches expected behavior.";
		return AST_TEST_NOT_RUN;
	case TEST_EXECUTE:
		break;
	}

	/* test 1, simple host */
	sip_parse_host(host1, 1, &host, &port, &transport);
	if (port != STANDARD_SIP_PORT ||
			ast_strlen_zero(host) || strcmp(host, "www.blah.com") ||
			transport != SIP_TRANSPORT_UDP) {
		ast_test_status_update(test, "Test 1: simple host failed.\n");
		res = AST_TEST_FAIL;
	}

	/* test 2, add tcp transport */
	sip_parse_host(host2, 1, &host, &port, &transport);
	if (port != STANDARD_SIP_PORT ||
			ast_strlen_zero(host) || strcmp(host, "www.blah.com") ||
			transport != SIP_TRANSPORT_TCP) {
		ast_test_status_update(test, "Test 2: tcp host failed.\n");
		res = AST_TEST_FAIL;
	}

	/* test 3, add tls transport */
	sip_parse_host(host3, 1, &host, &port, &transport);
	if (port != STANDARD_TLS_PORT ||
			ast_strlen_zero(host) || strcmp(host, "10.10.10.10") ||
			transport != SIP_TRANSPORT_TLS) {
		ast_test_status_update(test, "Test 3: tls host failed. \n");
		res = AST_TEST_FAIL;
	}

	/* test 4, add custom port with tls */
	sip_parse_host(host4, 1, &host, &port, &transport);
	if (port != 1234 || ast_strlen_zero(host) ||
			strcmp(host, "10.10.10.10") ||
			transport != SIP_TRANSPORT_TLS) {
		ast_test_status_update(test, "Test 4: tls host with custom port failed.\n");
		res = AST_TEST_FAIL;
	}

	/* test 5, simple host with custom port */
	sip_parse_host(host5, 1, &host, &port, &transport);
	if (port != 1234 || ast_strlen_zero(host) ||
			strcmp(host, "10.10.10.10") ||
			transport != SIP_TRANSPORT_UDP) {
		ast_test_status_update(test, "Test 5: simple host with custom port failed.\n");
		res = AST_TEST_FAIL;
	}

	/* test 6, expected failure with NULL input */
	if (!sip_parse_host(NULL, 1, &host, &port, &transport)) {
		ast_test_status_update(test, "Test 6: expected error on NULL input did not occur.\n");
		res = AST_TEST_FAIL;
	}

	return res;

}

/*! \brief SIP test registration */
void sip_config_parser_register_tests(void)
{
	AST_TEST_REGISTER(sip_parse_register_line_test);
	AST_TEST_REGISTER(sip_parse_host_line_test);
}

/*! \brief SIP test registration */
void sip_config_parser_unregister_tests(void)
{
	AST_TEST_UNREGISTER(sip_parse_register_line_test);
	AST_TEST_UNREGISTER(sip_parse_host_line_test);
}


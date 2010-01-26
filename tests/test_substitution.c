/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2009, Digium, Inc.
 *
 * Tilghman Lesher <tlesher AT digium DOT com>
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
 * \brief Substitution Test
 *
 * \author\verbatim Tilghman Lesher <tlesher AT digium DOT com> \endverbatim
 * 
 * \ingroup tests
 */

/*** MODULEINFO
	<depend>TEST_FRAMEWORK</depend>
 ***/

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#include "asterisk/file.h"
#include "asterisk/channel.h"
#include "asterisk/pbx.h"
#include "asterisk/module.h"
#include "asterisk/lock.h"
#include "asterisk/app.h"
#include "asterisk/strings.h"
#include "asterisk/stringfields.h"
#include "asterisk/threadstorage.h"
#include "asterisk/test.h"

static enum ast_test_result_state test_chan_integer(void *status, struct ast_str **err,
		struct ast_channel *c, int *ifield, const char *expression)
{
	int i, okay = 1, value1 = -1, value2 = -1;
	char workspace[4096];
	struct ast_str *str = ast_str_create(16);

	ast_test_status_update(status, "Testing '%s' . . . . . %s\n", expression, okay ? "passed" : "FAILED");
	for (i = 0; i < 256; i++) {
		*ifield = i;
		ast_str_substitute_variables(&str, 0, c, expression);
		pbx_substitute_variables_helper(c, expression, workspace, sizeof(workspace));
		if (sscanf(workspace, "%d", &value1) != 1 || value1 != i || sscanf(ast_str_buffer(str), "%d", &value2) != 1 || value2 != i) {
			ast_test_status_update(status, "%s != %s and/or %d != %d != %d\n", ast_str_buffer(str), workspace, value1, value2, i);
			ast_str_set(err, 0, "%s: '%s' != '%s' and/or %d != %d != %d\n",
					__PRETTY_FUNCTION__, ast_str_buffer(str),
					workspace, value1, value2, i);
			okay = 0;
		}
	}

	ast_free(str);

	return okay ? AST_TEST_PASS : AST_TEST_FAIL;
}

static enum ast_test_result_state test_chan_string(void *status, struct ast_str **err,
		struct ast_channel *c, char *cfield, size_t cfieldsize,
		const char *expression)
{
	const char *values[] = { "one", "three", "reallylongdinosaursoundingthingwithwordsinit" };
	int i, okay = 1;
	char workspace[4096];
	struct ast_str *str = ast_str_create(16);

	for (i = 0; i < ARRAY_LEN(values); i++) {
		ast_copy_string(cfield, values[i], cfieldsize);
		ast_str_substitute_variables(&str, 0, c, expression);
		pbx_substitute_variables_helper(c, expression, workspace, sizeof(workspace));
		ast_test_status_update(status, "Testing '%s' . . . . . %s\n", expression, okay ? "passed" : "FAILED");
		if (strcmp(cfield, ast_str_buffer(str)) != 0 || strcmp(cfield, workspace) != 0) {
			ast_test_status_update(status, "%s != %s != %s\n", cfield, ast_str_buffer(str), workspace);
			ast_str_set(err, 0, "%s: '%s' != '%s' != '%s'\n",
					__PRETTY_FUNCTION__, cfield, ast_str_buffer(str), workspace);
			okay = 0;
		}
	}

	ast_free(str);

	return okay ? AST_TEST_PASS : AST_TEST_FAIL;
}

static enum ast_test_result_state test_chan_variable(void *status, struct ast_str **err,
		struct ast_channel *c, const char *varname)
{
	const char *values[] = { "one", "three", "reallylongdinosaursoundingthingwithwordsinit" };
	int i, okay = 1;
	char workspace[4096];
	struct ast_str *str = ast_str_create(16);
	struct ast_str *var = ast_str_create(16);

	ast_str_set(&var, 0, "${%s}", varname);
	for (i = 0; i < ARRAY_LEN(values); i++) {
		pbx_builtin_setvar_helper(c, varname, values[i]);
		ast_str_substitute_variables(&str, 0, c, ast_str_buffer(var));
		pbx_substitute_variables_helper(c, ast_str_buffer(var), workspace, sizeof(workspace));
		ast_test_status_update(status, "Testing '%s' . . . . . %s\n", ast_str_buffer(var), okay ? "passed" : "FAILED");
		if (strcmp(values[i], ast_str_buffer(str)) != 0 || strcmp(values[i], workspace) != 0) {
			ast_test_status_update(status, "%s != %s != %s\n", values[i], ast_str_buffer(str), workspace);
			ast_str_set(err, 0, "%s: '%s' != '%s' != '%s'\n",
					__PRETTY_FUNCTION__, values[i], ast_str_buffer(str), workspace);
			okay = 0;
		}
	}

	ast_free(str);
	ast_free(var);

	return okay ? AST_TEST_PASS : AST_TEST_FAIL;
}

static enum ast_test_result_state test_chan_function(void *status, struct ast_str **err,
		struct ast_channel *c, const char *expression)
{
	int okay = 1;
	char workspace[4096];
	struct ast_str *str = ast_str_create(16);

	ast_str_substitute_variables(&str, 0, c, expression);
	pbx_substitute_variables_helper(c, expression, workspace, sizeof(workspace));
	ast_test_status_update(status, "Testing '%s' . . . . . %s\n", expression, okay ? "passed" : "FAILED");
	if (strcmp(workspace, ast_str_buffer(str)) != 0) {
		ast_test_status_update(status, "test_chan_function, expr: '%s' ... %s != %s\n",
				expression, ast_str_buffer(str), workspace);
		ast_str_set(err, 0, "%s: expr: '%s' ... '%s' != '%s'\n",
				__PRETTY_FUNCTION__, expression, ast_str_buffer(str), workspace);
		okay = 0;
	}

	ast_free(str);

	return okay ? AST_TEST_PASS : AST_TEST_FAIL;
}

static enum ast_test_result_state test_2way_function(void *status, struct ast_str **err,
		struct ast_channel *c, const char *encode1, const char *encode2,
		const char *decode1, const char *decode2)
{
	struct ast_str *str = ast_str_create(16), *expression = ast_str_alloca(120);
	int okay;

	ast_str_set(&expression, 0, "%s%s%s", encode1, "foobarbaz", encode2);
	ast_str_substitute_variables(&str, 0, c, ast_str_buffer(expression));
	ast_str_set(&expression, 0, "%s%s%s", decode1, ast_str_buffer(str), decode2);
	ast_str_substitute_variables(&str, 0, c, ast_str_buffer(expression));

	okay = !strcmp(ast_str_buffer(str), "foobarbaz");

	ast_test_status_update(status, "Testing '%s%s' and '%s%s' . . . . . %s\n",
			encode1, encode2, decode1, decode2,
			okay ? "passed" : "FAILED");

	if (!okay) {
		ast_test_status_update(status, "  '%s' != 'foobarbaz'\n", ast_str_buffer(str));
		ast_str_set(err, 0, "%s: '%s' != 'foobarbaz'\n",
				__PRETTY_FUNCTION__, ast_str_buffer(str));
	}

	ast_free(str);

	return okay ? AST_TEST_PASS : AST_TEST_FAIL;
}

static enum ast_test_result_state test_expected_result(void *status, struct ast_str **err,
		struct ast_channel *c, const char *expression, const char *result)
{
	struct ast_str *str = ast_str_create(16);
	int okay;

	ast_str_substitute_variables(&str, 0, c, expression);
	okay = !strcmp(ast_str_buffer(str), result);

	ast_test_status_update(status, "Testing '%s' ('%s') == '%s' . . . . . %s\n",
			ast_str_buffer(str), expression, result,
			okay ? "passed" : "FAILED");

	if (!okay) {
		ast_test_status_update(status, "test_expected_result: '%s' != '%s'\n",
				ast_str_buffer(str), result);
		ast_str_set(err, 0, "%s: '%s' != '%s'\n", __PRETTY_FUNCTION__,
				ast_str_buffer(str), result);
	}

	ast_free(str);

	return okay ? AST_TEST_PASS : AST_TEST_FAIL;
}

AST_TEST_DEFINE(test_substitution)
{
	struct ast_channel *c;
	int i;
	enum ast_test_result_state res = AST_TEST_PASS;

	switch (cmd) {
	case TEST_INIT:
		info->name = "test_substitution";
		info->category = "main/pbx/";
		info->summary = "Test variable and function substitution";
		info->description =
			"This test executes a variety of variable and function substitutions "
			"and ensures that the expected results are received.";
		return AST_TEST_NOT_RUN;
	case TEST_EXECUTE:
		break;
	}

	ast_test_status_update(&args->status_update, "Testing variable substitution ...\n");

	c = ast_channel_alloc(0, 0, "", "", "", "", "", "", 0, "Test/substitution");

#define TEST(t) if (t == AST_TEST_FAIL) { res = AST_TEST_FAIL; }
	TEST(test_chan_integer(&args->status_update, &args->ast_test_error_str, c, &c->cid.cid_pres, "${CALLINGPRES}"));
	TEST(test_chan_integer(&args->status_update, &args->ast_test_error_str, c, &c->cid.cid_ani2, "${CALLINGANI2}"));
	TEST(test_chan_integer(&args->status_update, &args->ast_test_error_str, c, &c->cid.cid_ton, "${CALLINGTON}"));
	TEST(test_chan_integer(&args->status_update, &args->ast_test_error_str, c, &c->cid.cid_tns, "${CALLINGTNS}"));
	TEST(test_chan_integer(&args->status_update, &args->ast_test_error_str, c, &c->hangupcause, "${HANGUPCAUSE}"));
	TEST(test_chan_integer(&args->status_update, &args->ast_test_error_str, c, &c->priority, "${PRIORITY}"));
	TEST(test_chan_string(&args->status_update, &args->ast_test_error_str, c, c->context, sizeof(c->context), "${CONTEXT}"));
	TEST(test_chan_string(&args->status_update, &args->ast_test_error_str, c, c->exten, sizeof(c->exten), "${EXTEN}"));
	TEST(test_chan_variable(&args->status_update, &args->ast_test_error_str, c, "CHANNEL(language)"));
	TEST(test_chan_variable(&args->status_update, &args->ast_test_error_str, c, "CHANNEL(musicclass)"));
	TEST(test_chan_variable(&args->status_update, &args->ast_test_error_str, c, "CHANNEL(parkinglot)"));
	TEST(test_chan_variable(&args->status_update, &args->ast_test_error_str, c, "CALLERID(name)"));
	TEST(test_chan_variable(&args->status_update, &args->ast_test_error_str, c, "CURLOPT(proxyuserpwd)"));
	TEST(test_chan_variable(&args->status_update, &args->ast_test_error_str, c, "CDR(foo)"));
	TEST(test_chan_variable(&args->status_update, &args->ast_test_error_str, c, "ENV(foo)"));
	TEST(test_chan_variable(&args->status_update, &args->ast_test_error_str, c, "GLOBAL(foo)"));
	TEST(test_chan_variable(&args->status_update, &args->ast_test_error_str, c, "GROUP()"));
	TEST(test_2way_function(&args->status_update, &args->ast_test_error_str, c, "${AES_ENCRYPT(abcdefghijklmnop,", ")}", "${AES_DECRYPT(abcdefghijklmnop,", ")}"));
	TEST(test_2way_function(&args->status_update, &args->ast_test_error_str, c, "${BASE64_ENCODE(", ")}", "${BASE64_DECODE(", ")}"));
	pbx_builtin_setvar_helper(c, "foo", "123");
	pbx_builtin_setvar_helper(c, "bar", "foo");
	pbx_builtin_setvar_helper(c, "baz", "fo");
	TEST(test_expected_result(&args->status_update, &args->ast_test_error_str, c, "${foo}${foo}", "123123"));
	TEST(test_expected_result(&args->status_update, &args->ast_test_error_str, c, "A${foo}A${foo}A", "A123A123A"));
	TEST(test_expected_result(&args->status_update, &args->ast_test_error_str, c, "A${${bar}}A", "A123A"));
	TEST(test_expected_result(&args->status_update, &args->ast_test_error_str, c, "A${${baz}o}A", "A123A"));
	TEST(test_expected_result(&args->status_update, &args->ast_test_error_str, c, "A${${baz}o:1}A", "A23A"));
	TEST(test_expected_result(&args->status_update, &args->ast_test_error_str, c, "A${${baz}o:1:1}A", "A2A"));
	TEST(test_expected_result(&args->status_update, &args->ast_test_error_str, c, "A${${baz}o:1:-1}A", "A2A"));
	TEST(test_expected_result(&args->status_update, &args->ast_test_error_str, c, "A${${baz}o:-1:1}A", "A3A"));
	TEST(test_expected_result(&args->status_update, &args->ast_test_error_str, c, "A${${baz}o:-2:1}A", "A2A"));
	TEST(test_expected_result(&args->status_update, &args->ast_test_error_str, c, "A${${baz}o:-2:-1}A", "A2A"));
#undef TEST

	/* For testing dialplan functions */
	for (i = 0; ; i++) {
		char *cmd = ast_cli_generator("core show function", "", i);
		if (cmd == NULL) {
			break;
		}
		if (strcmp(cmd, "CHANNEL") && strcmp(cmd, "CALLERID") && strncmp(cmd, "CURL", 4) &&
				strncmp(cmd, "AES", 3) && strncmp(cmd, "BASE64", 6) &&
				strcmp(cmd, "CDR") && strcmp(cmd, "ENV") && strcmp(cmd, "GLOBAL") &&
				strcmp(cmd, "GROUP") && strcmp(cmd, "CUT") && strcmp(cmd, "LISTFILTER") &&
				strcmp(cmd, "PP_EACH_EXTENSION") && strcmp(cmd, "SET")) {
			struct ast_custom_function *acf = ast_custom_function_find(cmd);
			if (acf->read && acf->read2) {
				char expression[80];
				snprintf(expression, sizeof(expression), "${%s(foo)}", cmd);
				if (AST_TEST_FAIL == test_chan_function(&args->status_update, &args->ast_test_error_str,c, expression)) {
					res = AST_TEST_FAIL;
				}
			}
		}
		ast_free(cmd);
	}

	ast_hangup(c);
	return res;
}

static int unload_module(void)
{
	AST_TEST_UNREGISTER(test_substitution);
	return 0;
}

static int load_module(void)
{
	AST_TEST_REGISTER(test_substitution);
	return AST_MODULE_LOAD_SUCCESS;
}

AST_MODULE_INFO_STANDARD(ASTERISK_GPL_KEY, "Substitution tests");

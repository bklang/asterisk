/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2008, Eliel C. Sardanons (LU1ALY) <eliels@gmail.com>
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
 * \brief XML abstraction layer
 *
 * \author Eliel C. Sardanons (LU1ALY) <eliels@gmail.com>
 */

#include "asterisk.h"
#include "asterisk/xml.h"
#include "asterisk/logger.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#if defined(HAVE_LIBXML2)
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xinclude.h>
/* libxml2 ast_xml implementation. */


int ast_xml_init(void)
{
	LIBXML_TEST_VERSION

	return 0;
}

int ast_xml_finish(void)
{
	xmlCleanupParser();

	return 0;
}

struct ast_xml_doc *ast_xml_open(char *filename)
{
	xmlDoc *doc;

	if (!filename) {
		return NULL;
	}

	doc = xmlReadFile(filename, NULL, XML_PARSE_RECOVER);
	if (doc) {
		/* process xinclude elements. */
		if (xmlXIncludeProcess(doc) < 0) {
			xmlFreeDoc(doc);
			return NULL;
		}
	}

	return (struct ast_xml_doc *) doc;
}

struct ast_xml_doc *ast_xml_new(void)
{
	xmlDoc *doc;

	doc = xmlNewDoc((const xmlChar *) "1.0");
	return (struct ast_xml_doc *) doc;
}

struct ast_xml_node *ast_xml_new_node(const char *name)
{
	xmlNode *node;
	if (!name) {
		return NULL;
	}

	node = xmlNewNode(NULL, (const xmlChar *) name);

	return (struct ast_xml_node *) node;
}

struct ast_xml_node *ast_xml_new_child(struct ast_xml_node *parent, const char *child_name)
{
	xmlNode *child;

	if (!parent || !child_name) {
		return NULL;
	}

	child = xmlNewChild((xmlNode *) parent, NULL, (const xmlChar *) child_name, NULL);
	return (struct ast_xml_node *) child;
}

struct ast_xml_node *ast_xml_add_child(struct ast_xml_node *parent, struct ast_xml_node *child)
{
	if (!parent || !child) {
		return NULL;
	}
	return (struct ast_xml_node *) xmlAddChild((xmlNode *) parent, (xmlNode *) child);
}

struct ast_xml_doc *ast_xml_read_memory(char *buffer, size_t size)
{
	xmlDoc *doc;

	if (!buffer) {
		return NULL;
	}

	if (!(doc = xmlParseMemory(buffer, (int) size))) {
		/* process xinclude elements. */
		if (xmlXIncludeProcess(doc) < 0) {
			xmlFreeDoc(doc);
			return NULL;
		}
	}

	return (struct ast_xml_doc *) doc;
}

void ast_xml_close(struct ast_xml_doc *doc)
{
	if (!doc) {
		return;
	}

	xmlFreeDoc((xmlDoc *) doc);
	doc = NULL;
}

void ast_xml_set_root(struct ast_xml_doc *doc, struct ast_xml_node *node)
{
	if (!doc || !node) {
		return;
	}

	xmlDocSetRootElement((xmlDoc *) doc, (xmlNode *) node);
}

struct ast_xml_node *ast_xml_get_root(struct ast_xml_doc *doc)
{
	xmlNode *root_node;

	if (!doc) {
		return NULL;
	}

	root_node = xmlDocGetRootElement((xmlDoc *) doc);

	return (struct ast_xml_node *) root_node;
}

void ast_xml_free_node(struct ast_xml_node *node)
{
	if (!node) {
		return;
	}

	xmlFreeNode((xmlNode *) node);
	node = NULL;
}

void ast_xml_free_attr(const char *attribute)
{
	if (attribute) {
		xmlFree((char *) attribute);
	}
}

void ast_xml_free_text(const char *text)
{
	if (text) {
		xmlFree((char *) text);
	}
}

const char *ast_xml_get_attribute(struct ast_xml_node *node, const char *attrname)
{
	xmlChar *attrvalue;

	if (!node) {
		return NULL;
	}

	if (!attrname) {
		return NULL;
	}

	attrvalue = xmlGetProp((xmlNode *) node, (xmlChar *) attrname);

	return (const char *) attrvalue;
}

int ast_xml_set_attribute(struct ast_xml_node *node, const char *name, const char *value)
{
	if (!name || !value) {
		return -1;
	}

	if (!xmlSetProp((xmlNode *) node, (xmlChar *) name, (xmlChar *) value)) {
		return -1;
	}

	return 0;
}

struct ast_xml_node *ast_xml_find_element(struct ast_xml_node *root_node, const char *name, const char *attrname, const char *attrvalue)
{
	struct ast_xml_node *cur;
	const char *attr;

	if (!root_node) {
		return NULL;
	}

	for (cur = root_node; cur; cur = ast_xml_node_get_next(cur)) {
		/* Check if the name matchs */
		if (strcmp(ast_xml_node_get_name(cur), name)) {
			continue;
		}
		/* We need to check for a specific attribute name? */
		if (!attrname || !attrvalue) {
			return cur;
		}
		/* Get the attribute, we need to compare it. */
		if ((attr = ast_xml_get_attribute(cur, attrname))) {
			/* does attribute name/value matches? */
			if (!strcmp(attr, attrvalue)) {
				ast_xml_free_attr(attr);
				return cur;
			}
			ast_xml_free_attr(attr);
		}
	}

	return NULL;
}

struct ast_xml_doc *ast_xml_get_doc(struct ast_xml_node *node)
{
	if (!node) {
		return NULL;
	}

	return (struct ast_xml_doc *) ((xmlNode *)node)->doc;
}

struct ast_xml_ns *ast_xml_find_namespace(struct ast_xml_doc *doc, struct ast_xml_node *node, const char *ns_name) {
	xmlNsPtr ns = xmlSearchNs((xmlDocPtr) doc, (xmlNodePtr) node, (xmlChar *) ns_name);
	return (struct ast_xml_ns *) ns;
}

const char *ast_xml_get_ns_href(struct ast_xml_ns *ns)
{
	return (const char *) ((xmlNsPtr) ns)->href;
}

const char *ast_xml_get_text(struct ast_xml_node *node)
{
	if (!node) {
		return NULL;
	}

	return (const char *) xmlNodeGetContent((xmlNode *) node);
}

void ast_xml_set_text(struct ast_xml_node *node, const char *content)
{
	if (!node || !content) {
		return;
	}

	xmlNodeSetContent((xmlNode *) node, (const xmlChar *) content);
}

int ast_xml_doc_dump_file(FILE *output, struct ast_xml_doc *doc)
{
	return xmlDocDump(output, (xmlDocPtr)doc);
}

const char *ast_xml_node_get_name(struct ast_xml_node *node)
{
	return (const char *) ((xmlNode *) node)->name;
}

struct ast_xml_node *ast_xml_node_get_children(struct ast_xml_node *node)
{
	return (struct ast_xml_node *) ((xmlNode *) node)->children;
}

struct ast_xml_node *ast_xml_node_get_next(struct ast_xml_node *node)
{
	return (struct ast_xml_node *) ((xmlNode *) node)->next;
}

struct ast_xml_node *ast_xml_node_get_prev(struct ast_xml_node *node)
{
	return (struct ast_xml_node *) ((xmlNode *) node)->prev;
}

struct ast_xml_node *ast_xml_node_get_parent(struct ast_xml_node *node)
{
	return (struct ast_xml_node *) ((xmlNode *) node)->parent;
}

#endif /* defined(HAVE_LIBXML2) */


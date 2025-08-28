/* (C) Copyright 2016
 * Stefano Babic, stefano.babic@swupdate.org.
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <assert.h>
#include "generated/autoconf.h"
#include "bsdqueue.h"
#include "util.h"
#include "parsers.h"
#include "parselib.h"
#include "parselib-private.h"

#define MAX_URL_LENGTH 2048

static json_type map_field_type(field_type_t type)
{
	switch (type) {
	case TYPE_INT:
	case TYPE_INT64:
		return json_type_int;
	case TYPE_BOOL:
		return json_type_boolean;
	case TYPE_DOUBLE:
		return json_type_double;
	default: /* not supported in SWUpdate */
		return json_type_null;
	}
}

json_object *find_json_recursive_node(json_object *root, const char **names)
{
	json_object *node = root;

	while (*names) {
		const char *n = *names;
		json_object *cnode = NULL;

		if (json_object_object_get_ex(node, n, &cnode))
			node = cnode;
		else
			return NULL;
		names++;
	}

	return node;
}

void *get_child_json(json_object *e, const char *path)
{
	json_object *node = NULL;

	if (path) {
		if (!json_object_object_get_ex(e, path, &node))
			return NULL;
	}

	return node;
}

void iterate_field_json(json_object *e, iterate_callback cb, void *data)
{
	json_object *subnode;
	const char *str;
	size_t i;

	if (!cb || json_object_get_type(e) != json_type_object)
		return;

	json_object_object_foreach(e, key, node) {
		switch (json_object_get_type(node)) {
		case json_type_string:
			str = json_object_get_string(node);
			cb(key, str, data);
			break;
		case json_type_array:
			for (i = 0; i < json_object_array_length(node); i++) {
				subnode = json_object_array_get_idx(node, i);
				if (json_object_get_type(subnode) != json_type_string)
					continue;

				str = json_object_get_string(subnode);
				cb(key, str, data);
			}
			break;
		default:
			break;
		}
	}
}

const char *get_field_string_json(json_object *e, const char *path)
{
	const char *str;
	json_object *node;

	if (path) {
		if (!json_object_object_get_ex(e, path, &node))
			return NULL;
	} else
		node = e;

	if (json_object_get_type(node) == json_type_string) {
		str = json_object_get_string(node);

		return str;
	}

	return NULL;
}

static void get_value_json(json_object *e, const char *path, void *dest, field_type_t expected_type)
{
	enum json_type parsed_type;
	parsed_type = json_object_get_type(e);
	if (parsed_type != map_field_type(expected_type)) {
		WARN("Type mismatch for %s field \"%s\"", SW_DESCRIPTION_FILENAME, path);
		return;
	}
	switch (expected_type) {
	case TYPE_BOOL:
		*(bool *)dest = json_object_get_boolean(e);
		break;
	case TYPE_INT:
		*(int *)dest = json_object_get_int(e);
		break;
	case TYPE_INT64:
		*(long long *)dest = json_object_get_int(e);
		break;
	case TYPE_DOUBLE:
		*(double *)dest = json_object_get_double(e);
		break;
		/* Do nothing, add if needed */
	}
}

bool is_field_numeric_json(json_object *e, const char *path)
{
	enum json_type type;
	json_object *fld = NULL;

	if (path) {
		if (!json_object_object_get_ex(e, path, &fld))
			return false;
	} else {
		fld = e;
	}

	type = json_object_get_type(fld);
	return type == json_type_int ||
	       type == json_type_double;
}

void get_field_json(json_object *e, const char *path, void *dest, field_type_t type)
{
	json_object *fld = NULL;

	if (path) {
		if (json_object_object_get_ex(e, path, &fld))
			get_value_json(fld, path, dest, type);
	} else {
		get_value_json(e, path, dest, type);
	}
}

json_object *json_get_key(json_object *json_root, const char *key)
{
	json_object *json_child;
	if (json_object_object_get_ex(json_root, key, &json_child)) {
		return json_child;
	}
	return NULL;
}

const char *json_get_value(struct json_object *json_root,
			   const char *key)
{
	json_object *json_data = json_get_key(json_root, key);

	if (json_data == NULL)
		return "";

	return json_object_get_string(json_data);
}

json_object *json_get_path_key(json_object *json_root, const char **json_path)
{
	json_object *json_data = json_root;
	while (*json_path) {
		const char *key = *json_path;
		json_data = json_get_key(json_data, key);
		if (json_data == NULL) {
			return NULL;
		}
		json_path++;
	}
	return json_data;
}

char *json_get_data_url(json_object *json_root, const char *key)
{
	json_object *json_data = json_get_path_key(
	    json_root, (const char *[]){"_links", key, "href", NULL});
	return json_data == NULL
		   ? NULL
		   : strndup(json_object_get_string(json_data), MAX_URL_LENGTH);
}

void *find_root_json(json_object *root, const char **nodes, unsigned int depth)
{
	json_object *node;
	enum json_type type;
	const char *str;

	/*
	 * check for deadlock links, block recursion
	 */
	if (!(--depth))
		return NULL;

	node = find_json_recursive_node(root, nodes);

	if (node) {
		type = json_object_get_type(node);

		if (type == json_type_object || type == json_type_array) {
			str = get_field_string_json(node, "ref");
			if (str) {
				if (!set_find_path(nodes, str))
					return NULL;
				node = find_root_json(root, nodes, depth);
			}
		}
	}
	return node;
}

void *get_node_json(json_object *root, const char **nodes)
{
	return find_json_recursive_node(root, nodes);
}



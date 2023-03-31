/*-------------------------------------------------------------------------
 *
 * jsonfuncs.h
 *	  Functions to process JSON data types.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/jsonfuncs.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef JSONFUNCS_H
#define JSONFUNCS_H

#include "common/jsonapi.h"
#include "utils/jsonb.h"

/*
 * Flag types for iterate_json(b)_values to specify what elements from a
 * json(b) document we want to iterate.
 */
typedef enum JsonToIndex
{
	jtiKey = 0x01,
	jtiString = 0x02,
	jtiNumeric = 0x04,
	jtiBool = 0x08,
	jtiAll = jtiKey | jtiString | jtiNumeric | jtiBool
} JsonToIndex;

/* an action that will be applied to each value in iterate_json(b)_values functions */
typedef void (*JsonIterateStringValuesAction) (void *state, char *elem_value, int elem_len);

/* an action that will be applied to each value in transform_json(b)_values functions */
typedef text *(*JsonTransformStringValuesAction) (void *state, char *elem_value, int elem_len);

/* build a JsonLexContext from a text datum */
extern JsonLexContext *makeJsonLexContext(text *json, bool need_escapes);

/* try to parse json, and errsave(escontext) on failure */
extern bool pg_parse_json_or_errsave(JsonLexContext *lex, JsonSemAction *sem,
									 struct Node *escontext);

#define pg_parse_json_or_ereport(lex, sem) \
	(void) pg_parse_json_or_errsave(lex, sem, NULL)

/* save an error during json lexing or parsing */
extern void json_errsave_error(JsonParseErrorType error, JsonLexContext *lex,
							   struct Node *escontext);

/* get first JSON token */
extern JsonTokenType json_get_first_token(text *json, bool throw_error);

extern uint32 parse_jsonb_index_flags(Jsonb *jb);
extern void iterate_jsonb_values(Jsonb *jb, uint32 flags, void *state,
								 JsonIterateStringValuesAction action);
extern void iterate_json_values(text *json, uint32 flags, void *action_state,
								JsonIterateStringValuesAction action);
extern Jsonb *transform_jsonb_string_values(Jsonb *jsonb, void *action_state,
											JsonTransformStringValuesAction transform_action);
extern text *transform_json_string_values(text *json, void *action_state,
										  JsonTransformStringValuesAction transform_action);

#endif

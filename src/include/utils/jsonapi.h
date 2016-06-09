/*-------------------------------------------------------------------------
 *
 * jsonapi.h
 *	  Declarations for JSON API support.
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/jsonapi.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef JSONAPI_H
#define JSONAPI_H

#include "lib/stringinfo.h"

typedef enum
{
	JSON_TOKEN_INVALID,
	JSON_TOKEN_STRING,
	JSON_TOKEN_NUMBER,
	JSON_TOKEN_OBJECT_START,
	JSON_TOKEN_OBJECT_END,
	JSON_TOKEN_ARRAY_START,
	JSON_TOKEN_ARRAY_END,
	JSON_TOKEN_COMMA,
	JSON_TOKEN_COLON,
	JSON_TOKEN_TRUE,
	JSON_TOKEN_FALSE,
	JSON_TOKEN_NULL,
	JSON_TOKEN_END
} JsonTokenType;


/*
 * All the fields in this structure should be treated as read-only.
 *
 * If strval is not null, then it should contain the de-escaped value
 * of the lexeme if it's a string. Otherwise most of these field names
 * should be self-explanatory.
 *
 * line_number and line_start are principally for use by the parser's
 * error reporting routines.
 * token_terminator and prev_token_terminator point to the character
 * AFTER the end of the token, i.e. where there would be a nul byte
 * if we were using nul-terminated strings.
 */
typedef struct JsonLexContext
{
	char	   *input;
	int			input_length;
	char	   *token_start;
	char	   *token_terminator;
	char	   *prev_token_terminator;
	JsonTokenType token_type;
	int			lex_level;
	int			line_number;
	char	   *line_start;
	StringInfo	strval;
} JsonLexContext;

typedef void (*json_struct_action) (void *state);
typedef void (*json_ofield_action) (void *state, char *fname, bool isnull);
typedef void (*json_aelem_action) (void *state, bool isnull);
typedef void (*json_scalar_action) (void *state, char *token, JsonTokenType tokentype);


/*
 * Semantic Action structure for use in parsing json.
 * Any of these actions can be NULL, in which case nothing is done at that
 * point, Likewise, semstate can be NULL. Using an all-NULL structure amounts
 * to doing a pure parse with no side-effects, and is therefore exactly
 * what the json input routines do.
 *
 * The 'fname' and 'token' strings passed to these actions are palloc'd.
 * They are not free'd or used further by the parser, so the action function
 * is free to do what it wishes with them.
 */
typedef struct JsonSemAction
{
	void	   *semstate;
	json_struct_action object_start;
	json_struct_action object_end;
	json_struct_action array_start;
	json_struct_action array_end;
	json_ofield_action object_field_start;
	json_ofield_action object_field_end;
	json_aelem_action array_element_start;
	json_aelem_action array_element_end;
	json_scalar_action scalar;
} JsonSemAction;

/*
 * parse_json will parse the string in the lex calling the
 * action functions in sem at the appropriate points. It is
 * up to them to keep what state they need	in semstate. If they
 * need access to the state of the lexer, then its pointer
 * should be passed to them as a member of whatever semstate
 * points to. If the action pointers are NULL the parser
 * does nothing and just continues.
 */
extern void pg_parse_json(JsonLexContext *lex, JsonSemAction *sem);

/*
 * json_count_array_elements performs a fast secondary parse to determine the
 * number of elements in passed array lex context. It should be called from an
 * array_start action.
 */
extern int	json_count_array_elements(JsonLexContext *lex);

/*
 * constructors for JsonLexContext, with or without strval element.
 * If supplied, the strval element will contain a de-escaped version of
 * the lexeme. However, doing this imposes a performance penalty, so
 * it should be avoided if the de-escaped lexeme is not required.
 *
 * If you already have the json as a text* value, use the first of these
 * functions, otherwise use  makeJsonLexContextCstringLen().
 */
extern JsonLexContext *makeJsonLexContext(text *json, bool need_escapes);
extern JsonLexContext *makeJsonLexContextCstringLen(char *json,
							 int len,
							 bool need_escapes);

/*
 * Utility function to check if a string is a valid JSON number.
 *
 * str agrument does not need to be nul-terminated.
 */
extern bool IsValidJsonNumber(const char *str, int len);

#endif   /* JSONAPI_H */

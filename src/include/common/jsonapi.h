/*-------------------------------------------------------------------------
 *
 * jsonapi.h
 *	  Declarations for JSON API support.
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/common/jsonapi.h
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

typedef enum
{
	JSON_SUCCESS,
	JSON_ESCAPING_INVALID,
	JSON_ESCAPING_REQUIRED,
	JSON_EXPECTED_ARRAY_FIRST,
	JSON_EXPECTED_ARRAY_NEXT,
	JSON_EXPECTED_COLON,
	JSON_EXPECTED_END,
	JSON_EXPECTED_JSON,
	JSON_EXPECTED_MORE,
	JSON_EXPECTED_OBJECT_FIRST,
	JSON_EXPECTED_OBJECT_NEXT,
	JSON_EXPECTED_STRING,
	JSON_INVALID_TOKEN,
	JSON_UNICODE_CODE_POINT_ZERO,
	JSON_UNICODE_ESCAPE_FORMAT,
	JSON_UNICODE_HIGH_ESCAPE,
	JSON_UNICODE_HIGH_SURROGATE,
	JSON_UNICODE_LOW_SURROGATE
} JsonParseErrorType;


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
	int			input_encoding;
	char	   *token_start;
	char	   *token_terminator;
	char	   *prev_token_terminator;
	JsonTokenType token_type;
	int			lex_level;
	int			line_number;	/* line number, starting from 1 */
	char	   *line_start;		/* where that line starts within input */
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
 * pg_parse_json will parse the string in the lex calling the
 * action functions in sem at the appropriate points. It is
 * up to them to keep what state they need	in semstate. If they
 * need access to the state of the lexer, then its pointer
 * should be passed to them as a member of whatever semstate
 * points to. If the action pointers are NULL the parser
 * does nothing and just continues.
 */
extern JsonParseErrorType pg_parse_json(JsonLexContext *lex,
										JsonSemAction *sem);

/* the null action object used for pure validation */
extern PGDLLIMPORT JsonSemAction nullSemAction;

/*
 * json_count_array_elements performs a fast secondary parse to determine the
 * number of elements in passed array lex context. It should be called from an
 * array_start action.
 *
 * The return value indicates whether any error occurred, while the number
 * of elements is stored into *elements (but only if the return value is
 * JSON_SUCCESS).
 */
extern JsonParseErrorType json_count_array_elements(JsonLexContext *lex,
													int *elements);

/*
 * constructor for JsonLexContext, with or without strval element.
 * If supplied, the strval element will contain a de-escaped version of
 * the lexeme. However, doing this imposes a performance penalty, so
 * it should be avoided if the de-escaped lexeme is not required.
 */
extern JsonLexContext *makeJsonLexContextCstringLen(char *json,
													int len,
													int encoding,
													bool need_escapes);

/* lex one token */
extern JsonParseErrorType json_lex(JsonLexContext *lex);

/* construct an error detail string for a json error */
extern char *json_errdetail(JsonParseErrorType error, JsonLexContext *lex);

/*
 * Utility function to check if a string is a valid JSON number.
 *
 * str argument does not need to be nul-terminated.
 */
extern bool IsValidJsonNumber(const char *str, int len);

#endif							/* JSONAPI_H */

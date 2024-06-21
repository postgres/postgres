/*-------------------------------------------------------------------------
 *
 * jsonapi.h
 *	  Declarations for JSON API support.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/common/jsonapi.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef JSONAPI_H
#define JSONAPI_H

#include "lib/stringinfo.h"

typedef enum JsonTokenType
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
	JSON_TOKEN_END,
} JsonTokenType;

typedef enum JsonParseErrorType
{
	JSON_SUCCESS,
	JSON_INCOMPLETE,
	JSON_INVALID_LEXER_TYPE,
	JSON_NESTING_TOO_DEEP,
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
	JSON_UNICODE_UNTRANSLATABLE,
	JSON_UNICODE_HIGH_SURROGATE,
	JSON_UNICODE_LOW_SURROGATE,
	JSON_SEM_ACTION_FAILED,		/* error should already be reported */
} JsonParseErrorType;

/* Parser state private to jsonapi.c */
typedef struct JsonParserStack JsonParserStack;
typedef struct JsonIncrementalState JsonIncrementalState;

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
 *
 * The prev_token_terminator field should not be used when incremental is
 * true, as the previous token might have started in a previous piece of input,
 * and thus it can't be used in any pointer arithmetic or other operations in
 * conjunction with token_start.
 *
 * JSONLEX_FREE_STRUCT/STRVAL are used to drive freeJsonLexContext.
 */
#define JSONLEX_FREE_STRUCT			(1 << 0)
#define JSONLEX_FREE_STRVAL			(1 << 1)
typedef struct JsonLexContext
{
	const char *input;
	size_t		input_length;
	int			input_encoding;
	const char *token_start;
	const char *token_terminator;
	const char *prev_token_terminator;
	bool		incremental;
	JsonTokenType token_type;
	int			lex_level;
	bits32		flags;
	int			line_number;	/* line number, starting from 1 */
	const char *line_start;		/* where that line starts within input */
	JsonParserStack *pstack;
	JsonIncrementalState *inc_state;
	StringInfo	strval;
	StringInfo	errormsg;
} JsonLexContext;

typedef JsonParseErrorType (*json_struct_action) (void *state);
typedef JsonParseErrorType (*json_ofield_action) (void *state, char *fname, bool isnull);
typedef JsonParseErrorType (*json_aelem_action) (void *state, bool isnull);
typedef JsonParseErrorType (*json_scalar_action) (void *state, char *token, JsonTokenType tokentype);


/*
 * Semantic Action structure for use in parsing json.
 *
 * Any of these actions can be NULL, in which case nothing is done at that
 * point, Likewise, semstate can be NULL. Using an all-NULL structure amounts
 * to doing a pure parse with no side-effects, and is therefore exactly
 * what the json input routines do.
 *
 * The 'fname' and 'token' strings passed to these actions are palloc'd.
 * They are not free'd or used further by the parser, so the action function
 * is free to do what it wishes with them.
 *
 * All action functions return JsonParseErrorType.  If the result isn't
 * JSON_SUCCESS, the parse is abandoned and that error code is returned.
 * If it is JSON_SEM_ACTION_FAILED, the action function is responsible
 * for having reported the error in some appropriate way.
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
 * up to them to keep what state they need in semstate. If they
 * need access to the state of the lexer, then its pointer
 * should be passed to them as a member of whatever semstate
 * points to. If the action pointers are NULL the parser
 * does nothing and just continues.
 */
extern JsonParseErrorType pg_parse_json(JsonLexContext *lex,
										JsonSemAction *sem);

extern JsonParseErrorType pg_parse_json_incremental(JsonLexContext *lex,
													JsonSemAction *sem,
													const char *json,
													size_t len,
													bool is_last);

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
 * initializer for JsonLexContext.
 *
 * If a valid 'lex' pointer is given, it is initialized.  This can be used
 * for stack-allocated structs, saving overhead.  If NULL is given, a new
 * struct is allocated.
 *
 * If need_escapes is true, ->strval stores the unescaped lexemes.
 * Unescaping is expensive, so only request it when necessary.
 *
 * If need_escapes is true or lex was given as NULL, then the caller is
 * responsible for freeing the returned struct, either by calling
 * freeJsonLexContext() or (in backend environment) via memory context
 * cleanup.
 */
extern JsonLexContext *makeJsonLexContextCstringLen(JsonLexContext *lex,
													const char *json,
													size_t len,
													int encoding,
													bool need_escapes);

/*
 * make a JsonLexContext suitable for incremental parsing.
 * the string chunks will be handed to pg_parse_json_incremental,
 * so there's no need for them here.
 */
extern JsonLexContext *makeJsonLexContextIncremental(JsonLexContext *lex,
													 int encoding,
													 bool need_escapes);

extern void freeJsonLexContext(JsonLexContext *lex);

/* lex one token */
extern JsonParseErrorType json_lex(JsonLexContext *lex);

/* construct an error detail string for a json error */
extern char *json_errdetail(JsonParseErrorType error, JsonLexContext *lex);

/*
 * Utility function to check if a string is a valid JSON number.
 *
 * str argument does not need to be nul-terminated.
 */
extern bool IsValidJsonNumber(const char *str, size_t len);

#endif							/* JSONAPI_H */

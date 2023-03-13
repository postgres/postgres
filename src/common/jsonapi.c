/*-------------------------------------------------------------------------
 *
 * jsonapi.c
 *		JSON parser and lexer interfaces
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/common/jsonapi.c
 *
 *-------------------------------------------------------------------------
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "common/jsonapi.h"
#include "mb/pg_wchar.h"

#ifndef FRONTEND
#include "miscadmin.h"
#endif

/*
 * The context of the parser is maintained by the recursive descent
 * mechanism, but is passed explicitly to the error reporting routine
 * for better diagnostics.
 */
typedef enum					/* contexts of JSON parser */
{
	JSON_PARSE_VALUE,			/* expecting a value */
	JSON_PARSE_STRING,			/* expecting a string (for a field name) */
	JSON_PARSE_ARRAY_START,		/* saw '[', expecting value or ']' */
	JSON_PARSE_ARRAY_NEXT,		/* saw array element, expecting ',' or ']' */
	JSON_PARSE_OBJECT_START,	/* saw '{', expecting label or '}' */
	JSON_PARSE_OBJECT_LABEL,	/* saw object label, expecting ':' */
	JSON_PARSE_OBJECT_NEXT,		/* saw object value, expecting ',' or '}' */
	JSON_PARSE_OBJECT_COMMA,	/* saw object ',', expecting next label */
	JSON_PARSE_END				/* saw the end of a document, expect nothing */
} JsonParseContext;

static inline JsonParseErrorType json_lex_string(JsonLexContext *lex);
static inline JsonParseErrorType json_lex_number(JsonLexContext *lex, char *s,
												 bool *num_err, int *total_len);
static inline JsonParseErrorType parse_scalar(JsonLexContext *lex, JsonSemAction *sem);
static JsonParseErrorType parse_object_field(JsonLexContext *lex, JsonSemAction *sem);
static JsonParseErrorType parse_object(JsonLexContext *lex, JsonSemAction *sem);
static JsonParseErrorType parse_array_element(JsonLexContext *lex, JsonSemAction *sem);
static JsonParseErrorType parse_array(JsonLexContext *lex, JsonSemAction *sem);
static JsonParseErrorType report_parse_error(JsonParseContext ctx, JsonLexContext *lex);

/* the null action object used for pure validation */
JsonSemAction nullSemAction =
{
	NULL, NULL, NULL, NULL, NULL,
	NULL, NULL, NULL, NULL, NULL
};

/* Recursive Descent parser support routines */

/*
 * lex_peek
 *
 * what is the current look_ahead token?
*/
static inline JsonTokenType
lex_peek(JsonLexContext *lex)
{
	return lex->token_type;
}

/*
 * lex_expect
 *
 * move the lexer to the next token if the current look_ahead token matches
 * the parameter token. Otherwise, report an error.
 */
static inline JsonParseErrorType
lex_expect(JsonParseContext ctx, JsonLexContext *lex, JsonTokenType token)
{
	if (lex_peek(lex) == token)
		return json_lex(lex);
	else
		return report_parse_error(ctx, lex);
}

/* chars to consider as part of an alphanumeric token */
#define JSON_ALPHANUMERIC_CHAR(c)  \
	(((c) >= 'a' && (c) <= 'z') || \
	 ((c) >= 'A' && (c) <= 'Z') || \
	 ((c) >= '0' && (c) <= '9') || \
	 (c) == '_' || \
	 IS_HIGHBIT_SET(c))

/*
 * Utility function to check if a string is a valid JSON number.
 *
 * str is of length len, and need not be null-terminated.
 */
bool
IsValidJsonNumber(const char *str, int len)
{
	bool		numeric_error;
	int			total_len;
	JsonLexContext dummy_lex;

	if (len <= 0)
		return false;

	/*
	 * json_lex_number expects a leading  '-' to have been eaten already.
	 *
	 * having to cast away the constness of str is ugly, but there's not much
	 * easy alternative.
	 */
	if (*str == '-')
	{
		dummy_lex.input = unconstify(char *, str) + 1;
		dummy_lex.input_length = len - 1;
	}
	else
	{
		dummy_lex.input = unconstify(char *, str);
		dummy_lex.input_length = len;
	}

	json_lex_number(&dummy_lex, dummy_lex.input, &numeric_error, &total_len);

	return (!numeric_error) && (total_len == dummy_lex.input_length);
}

/*
 * makeJsonLexContextCstringLen
 *
 * lex constructor, with or without StringInfo object for de-escaped lexemes.
 *
 * Without is better as it makes the processing faster, so only make one
 * if really required.
 */
JsonLexContext *
makeJsonLexContextCstringLen(char *json, int len, int encoding, bool need_escapes)
{
	JsonLexContext *lex = palloc0(sizeof(JsonLexContext));

	lex->input = lex->token_terminator = lex->line_start = json;
	lex->line_number = 1;
	lex->input_length = len;
	lex->input_encoding = encoding;
	if (need_escapes)
		lex->strval = makeStringInfo();
	return lex;
}

/*
 * pg_parse_json
 *
 * Publicly visible entry point for the JSON parser.
 *
 * lex is a lexing context, set up for the json to be processed by calling
 * makeJsonLexContext(). sem is a structure of function pointers to semantic
 * action routines to be called at appropriate spots during parsing, and a
 * pointer to a state object to be passed to those routines.
 */
JsonParseErrorType
pg_parse_json(JsonLexContext *lex, JsonSemAction *sem)
{
	JsonTokenType tok;
	JsonParseErrorType result;

	/* get the initial token */
	result = json_lex(lex);
	if (result != JSON_SUCCESS)
		return result;

	tok = lex_peek(lex);

	/* parse by recursive descent */
	switch (tok)
	{
		case JSON_TOKEN_OBJECT_START:
			result = parse_object(lex, sem);
			break;
		case JSON_TOKEN_ARRAY_START:
			result = parse_array(lex, sem);
			break;
		default:
			result = parse_scalar(lex, sem);	/* json can be a bare scalar */
	}

	if (result == JSON_SUCCESS)
		result = lex_expect(JSON_PARSE_END, lex, JSON_TOKEN_END);

	return result;
}

/*
 * json_count_array_elements
 *
 * Returns number of array elements in lex context at start of array token
 * until end of array token at same nesting level.
 *
 * Designed to be called from array_start routines.
 */
JsonParseErrorType
json_count_array_elements(JsonLexContext *lex, int *elements)
{
	JsonLexContext copylex;
	int			count;
	JsonParseErrorType result;

	/*
	 * It's safe to do this with a shallow copy because the lexical routines
	 * don't scribble on the input. They do scribble on the other pointers
	 * etc, so doing this with a copy makes that safe.
	 */
	memcpy(&copylex, lex, sizeof(JsonLexContext));
	copylex.strval = NULL;		/* not interested in values here */
	copylex.lex_level++;

	count = 0;
	result = lex_expect(JSON_PARSE_ARRAY_START, &copylex,
						JSON_TOKEN_ARRAY_START);
	if (result != JSON_SUCCESS)
		return result;
	if (lex_peek(&copylex) != JSON_TOKEN_ARRAY_END)
	{
		while (1)
		{
			count++;
			result = parse_array_element(&copylex, &nullSemAction);
			if (result != JSON_SUCCESS)
				return result;
			if (copylex.token_type != JSON_TOKEN_COMMA)
				break;
			result = json_lex(&copylex);
			if (result != JSON_SUCCESS)
				return result;
		}
	}
	result = lex_expect(JSON_PARSE_ARRAY_NEXT, &copylex,
						JSON_TOKEN_ARRAY_END);
	if (result != JSON_SUCCESS)
		return result;

	*elements = count;
	return JSON_SUCCESS;
}

/*
 *	Recursive Descent parse routines. There is one for each structural
 *	element in a json document:
 *	  - scalar (string, number, true, false, null)
 *	  - array  ( [ ] )
 *	  - array element
 *	  - object ( { } )
 *	  - object field
 */
static inline JsonParseErrorType
parse_scalar(JsonLexContext *lex, JsonSemAction *sem)
{
	char	   *val = NULL;
	json_scalar_action sfunc = sem->scalar;
	JsonTokenType tok = lex_peek(lex);
	JsonParseErrorType result;

	/* a scalar must be a string, a number, true, false, or null */
	if (tok != JSON_TOKEN_STRING && tok != JSON_TOKEN_NUMBER &&
		tok != JSON_TOKEN_TRUE && tok != JSON_TOKEN_FALSE &&
		tok != JSON_TOKEN_NULL)
		return report_parse_error(JSON_PARSE_VALUE, lex);

	/* if no semantic function, just consume the token */
	if (sfunc == NULL)
		return json_lex(lex);

	/* extract the de-escaped string value, or the raw lexeme */
	if (lex_peek(lex) == JSON_TOKEN_STRING)
	{
		if (lex->strval != NULL)
			val = pstrdup(lex->strval->data);
	}
	else
	{
		int			len = (lex->token_terminator - lex->token_start);

		val = palloc(len + 1);
		memcpy(val, lex->token_start, len);
		val[len] = '\0';
	}

	/* consume the token */
	result = json_lex(lex);
	if (result != JSON_SUCCESS)
		return result;

	/* invoke the callback */
	(*sfunc) (sem->semstate, val, tok);

	return JSON_SUCCESS;
}

static JsonParseErrorType
parse_object_field(JsonLexContext *lex, JsonSemAction *sem)
{
	/*
	 * An object field is "fieldname" : value where value can be a scalar,
	 * object or array.  Note: in user-facing docs and error messages, we
	 * generally call a field name a "key".
	 */

	char	   *fname = NULL;	/* keep compiler quiet */
	json_ofield_action ostart = sem->object_field_start;
	json_ofield_action oend = sem->object_field_end;
	bool		isnull;
	JsonTokenType tok;
	JsonParseErrorType result;

	if (lex_peek(lex) != JSON_TOKEN_STRING)
		return report_parse_error(JSON_PARSE_STRING, lex);
	if ((ostart != NULL || oend != NULL) && lex->strval != NULL)
		fname = pstrdup(lex->strval->data);
	result = json_lex(lex);
	if (result != JSON_SUCCESS)
		return result;

	result = lex_expect(JSON_PARSE_OBJECT_LABEL, lex, JSON_TOKEN_COLON);
	if (result != JSON_SUCCESS)
		return result;

	tok = lex_peek(lex);
	isnull = tok == JSON_TOKEN_NULL;

	if (ostart != NULL)
		(*ostart) (sem->semstate, fname, isnull);

	switch (tok)
	{
		case JSON_TOKEN_OBJECT_START:
			result = parse_object(lex, sem);
			break;
		case JSON_TOKEN_ARRAY_START:
			result = parse_array(lex, sem);
			break;
		default:
			result = parse_scalar(lex, sem);
	}
	if (result != JSON_SUCCESS)
		return result;

	if (oend != NULL)
		(*oend) (sem->semstate, fname, isnull);
	return JSON_SUCCESS;
}

static JsonParseErrorType
parse_object(JsonLexContext *lex, JsonSemAction *sem)
{
	/*
	 * an object is a possibly empty sequence of object fields, separated by
	 * commas and surrounded by curly braces.
	 */
	json_struct_action ostart = sem->object_start;
	json_struct_action oend = sem->object_end;
	JsonTokenType tok;
	JsonParseErrorType result;

#ifndef FRONTEND
	check_stack_depth();
#endif

	if (ostart != NULL)
		(*ostart) (sem->semstate);

	/*
	 * Data inside an object is at a higher nesting level than the object
	 * itself. Note that we increment this after we call the semantic routine
	 * for the object start and restore it before we call the routine for the
	 * object end.
	 */
	lex->lex_level++;

	Assert(lex_peek(lex) == JSON_TOKEN_OBJECT_START);
	result = json_lex(lex);
	if (result != JSON_SUCCESS)
		return result;

	tok = lex_peek(lex);
	switch (tok)
	{
		case JSON_TOKEN_STRING:
			result = parse_object_field(lex, sem);
			while (result == JSON_SUCCESS && lex_peek(lex) == JSON_TOKEN_COMMA)
			{
				result = json_lex(lex);
				if (result != JSON_SUCCESS)
					break;
				result = parse_object_field(lex, sem);
			}
			break;
		case JSON_TOKEN_OBJECT_END:
			break;
		default:
			/* case of an invalid initial token inside the object */
			result = report_parse_error(JSON_PARSE_OBJECT_START, lex);
	}
	if (result != JSON_SUCCESS)
		return result;

	result = lex_expect(JSON_PARSE_OBJECT_NEXT, lex, JSON_TOKEN_OBJECT_END);
	if (result != JSON_SUCCESS)
		return result;

	lex->lex_level--;

	if (oend != NULL)
		(*oend) (sem->semstate);

	return JSON_SUCCESS;
}

static JsonParseErrorType
parse_array_element(JsonLexContext *lex, JsonSemAction *sem)
{
	json_aelem_action astart = sem->array_element_start;
	json_aelem_action aend = sem->array_element_end;
	JsonTokenType tok = lex_peek(lex);
	JsonParseErrorType result;

	bool		isnull;

	isnull = tok == JSON_TOKEN_NULL;

	if (astart != NULL)
		(*astart) (sem->semstate, isnull);

	/* an array element is any object, array or scalar */
	switch (tok)
	{
		case JSON_TOKEN_OBJECT_START:
			result = parse_object(lex, sem);
			break;
		case JSON_TOKEN_ARRAY_START:
			result = parse_array(lex, sem);
			break;
		default:
			result = parse_scalar(lex, sem);
	}

	if (result != JSON_SUCCESS)
		return result;

	if (aend != NULL)
		(*aend) (sem->semstate, isnull);

	return JSON_SUCCESS;
}

static JsonParseErrorType
parse_array(JsonLexContext *lex, JsonSemAction *sem)
{
	/*
	 * an array is a possibly empty sequence of array elements, separated by
	 * commas and surrounded by square brackets.
	 */
	json_struct_action astart = sem->array_start;
	json_struct_action aend = sem->array_end;
	JsonParseErrorType result;

#ifndef FRONTEND
	check_stack_depth();
#endif

	if (astart != NULL)
		(*astart) (sem->semstate);

	/*
	 * Data inside an array is at a higher nesting level than the array
	 * itself. Note that we increment this after we call the semantic routine
	 * for the array start and restore it before we call the routine for the
	 * array end.
	 */
	lex->lex_level++;

	result = lex_expect(JSON_PARSE_ARRAY_START, lex, JSON_TOKEN_ARRAY_START);
	if (result == JSON_SUCCESS && lex_peek(lex) != JSON_TOKEN_ARRAY_END)
	{
		result = parse_array_element(lex, sem);

		while (result == JSON_SUCCESS && lex_peek(lex) == JSON_TOKEN_COMMA)
		{
			result = json_lex(lex);
			if (result != JSON_SUCCESS)
				break;
			result = parse_array_element(lex, sem);
		}
	}
	if (result != JSON_SUCCESS)
		return result;

	result = lex_expect(JSON_PARSE_ARRAY_NEXT, lex, JSON_TOKEN_ARRAY_END);
	if (result != JSON_SUCCESS)
		return result;

	lex->lex_level--;

	if (aend != NULL)
		(*aend) (sem->semstate);

	return JSON_SUCCESS;
}

/*
 * Lex one token from the input stream.
 */
JsonParseErrorType
json_lex(JsonLexContext *lex)
{
	char	   *s;
	int			len;
	JsonParseErrorType result;

	/* Skip leading whitespace. */
	s = lex->token_terminator;
	len = s - lex->input;
	while (len < lex->input_length &&
		   (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r'))
	{
		if (*s++ == '\n')
		{
			++lex->line_number;
			lex->line_start = s;
		}
		len++;
	}
	lex->token_start = s;

	/* Determine token type. */
	if (len >= lex->input_length)
	{
		lex->token_start = NULL;
		lex->prev_token_terminator = lex->token_terminator;
		lex->token_terminator = s;
		lex->token_type = JSON_TOKEN_END;
	}
	else
	{
		switch (*s)
		{
				/* Single-character token, some kind of punctuation mark. */
			case '{':
				lex->prev_token_terminator = lex->token_terminator;
				lex->token_terminator = s + 1;
				lex->token_type = JSON_TOKEN_OBJECT_START;
				break;
			case '}':
				lex->prev_token_terminator = lex->token_terminator;
				lex->token_terminator = s + 1;
				lex->token_type = JSON_TOKEN_OBJECT_END;
				break;
			case '[':
				lex->prev_token_terminator = lex->token_terminator;
				lex->token_terminator = s + 1;
				lex->token_type = JSON_TOKEN_ARRAY_START;
				break;
			case ']':
				lex->prev_token_terminator = lex->token_terminator;
				lex->token_terminator = s + 1;
				lex->token_type = JSON_TOKEN_ARRAY_END;
				break;
			case ',':
				lex->prev_token_terminator = lex->token_terminator;
				lex->token_terminator = s + 1;
				lex->token_type = JSON_TOKEN_COMMA;
				break;
			case ':':
				lex->prev_token_terminator = lex->token_terminator;
				lex->token_terminator = s + 1;
				lex->token_type = JSON_TOKEN_COLON;
				break;
			case '"':
				/* string */
				result = json_lex_string(lex);
				if (result != JSON_SUCCESS)
					return result;
				lex->token_type = JSON_TOKEN_STRING;
				break;
			case '-':
				/* Negative number. */
				result = json_lex_number(lex, s + 1, NULL, NULL);
				if (result != JSON_SUCCESS)
					return result;
				lex->token_type = JSON_TOKEN_NUMBER;
				break;
			case '0':
			case '1':
			case '2':
			case '3':
			case '4':
			case '5':
			case '6':
			case '7':
			case '8':
			case '9':
				/* Positive number. */
				result = json_lex_number(lex, s, NULL, NULL);
				if (result != JSON_SUCCESS)
					return result;
				lex->token_type = JSON_TOKEN_NUMBER;
				break;
			default:
				{
					char	   *p;

					/*
					 * We're not dealing with a string, number, legal
					 * punctuation mark, or end of string.  The only legal
					 * tokens we might find here are true, false, and null,
					 * but for error reporting purposes we scan until we see a
					 * non-alphanumeric character.  That way, we can report
					 * the whole word as an unexpected token, rather than just
					 * some unintuitive prefix thereof.
					 */
					for (p = s; p - s < lex->input_length - len && JSON_ALPHANUMERIC_CHAR(*p); p++)
						 /* skip */ ;

					/*
					 * We got some sort of unexpected punctuation or an
					 * otherwise unexpected character, so just complain about
					 * that one character.
					 */
					if (p == s)
					{
						lex->prev_token_terminator = lex->token_terminator;
						lex->token_terminator = s + 1;
						return JSON_INVALID_TOKEN;
					}

					/*
					 * We've got a real alphanumeric token here.  If it
					 * happens to be true, false, or null, all is well.  If
					 * not, error out.
					 */
					lex->prev_token_terminator = lex->token_terminator;
					lex->token_terminator = p;
					if (p - s == 4)
					{
						if (memcmp(s, "true", 4) == 0)
							lex->token_type = JSON_TOKEN_TRUE;
						else if (memcmp(s, "null", 4) == 0)
							lex->token_type = JSON_TOKEN_NULL;
						else
							return JSON_INVALID_TOKEN;
					}
					else if (p - s == 5 && memcmp(s, "false", 5) == 0)
						lex->token_type = JSON_TOKEN_FALSE;
					else
						return JSON_INVALID_TOKEN;
				}
		}						/* end of switch */
	}

	return JSON_SUCCESS;
}

/*
 * The next token in the input stream is known to be a string; lex it.
 *
 * If lex->strval isn't NULL, fill it with the decoded string.
 * Set lex->token_terminator to the end of the decoded input, and in
 * success cases, transfer its previous value to lex->prev_token_terminator.
 * Return JSON_SUCCESS or an error code.
 *
 * Note: be careful that all error exits advance lex->token_terminator
 * to the point after the character we detected the error on.
 */
static inline JsonParseErrorType
json_lex_string(JsonLexContext *lex)
{
	char	   *s;
	int			len;
	int			hi_surrogate = -1;

	/* Convenience macros for error exits */
#define FAIL_AT_CHAR_START(code) \
	do { \
		lex->token_terminator = s; \
		return code; \
	} while (0)
#define FAIL_AT_CHAR_END(code) \
	do { \
		lex->token_terminator = \
			s + pg_encoding_mblen_bounded(lex->input_encoding, s); \
		return code; \
	} while (0)

	if (lex->strval != NULL)
		resetStringInfo(lex->strval);

	Assert(lex->input_length > 0);
	s = lex->token_start;
	len = lex->token_start - lex->input;
	for (;;)
	{
		s++;
		len++;
		/* Premature end of the string. */
		if (len >= lex->input_length)
			FAIL_AT_CHAR_START(JSON_INVALID_TOKEN);
		else if (*s == '"')
			break;
		else if ((unsigned char) *s < 32)
		{
			/* Per RFC4627, these characters MUST be escaped. */
			/* Since *s isn't printable, exclude it from the context string */
			FAIL_AT_CHAR_START(JSON_ESCAPING_REQUIRED);
		}
		else if (*s == '\\')
		{
			/* OK, we have an escape character. */
			s++;
			len++;
			if (len >= lex->input_length)
				FAIL_AT_CHAR_START(JSON_INVALID_TOKEN);
			else if (*s == 'u')
			{
				int			i;
				int			ch = 0;

				for (i = 1; i <= 4; i++)
				{
					s++;
					len++;
					if (len >= lex->input_length)
						FAIL_AT_CHAR_START(JSON_INVALID_TOKEN);
					else if (*s >= '0' && *s <= '9')
						ch = (ch * 16) + (*s - '0');
					else if (*s >= 'a' && *s <= 'f')
						ch = (ch * 16) + (*s - 'a') + 10;
					else if (*s >= 'A' && *s <= 'F')
						ch = (ch * 16) + (*s - 'A') + 10;
					else
						FAIL_AT_CHAR_END(JSON_UNICODE_ESCAPE_FORMAT);
				}
				if (lex->strval != NULL)
				{
					/*
					 * Combine surrogate pairs.
					 */
					if (is_utf16_surrogate_first(ch))
					{
						if (hi_surrogate != -1)
							FAIL_AT_CHAR_END(JSON_UNICODE_HIGH_SURROGATE);
						hi_surrogate = ch;
						continue;
					}
					else if (is_utf16_surrogate_second(ch))
					{
						if (hi_surrogate == -1)
							FAIL_AT_CHAR_END(JSON_UNICODE_LOW_SURROGATE);
						ch = surrogate_pair_to_codepoint(hi_surrogate, ch);
						hi_surrogate = -1;
					}

					if (hi_surrogate != -1)
						FAIL_AT_CHAR_END(JSON_UNICODE_LOW_SURROGATE);

					/*
					 * Reject invalid cases.  We can't have a value above
					 * 0xFFFF here (since we only accepted 4 hex digits
					 * above), so no need to test for out-of-range chars.
					 */
					if (ch == 0)
					{
						/* We can't allow this, since our TEXT type doesn't */
						FAIL_AT_CHAR_END(JSON_UNICODE_CODE_POINT_ZERO);
					}

					/*
					 * Add the represented character to lex->strval.  In the
					 * backend, we can let pg_unicode_to_server() handle any
					 * required character set conversion; in frontend, we can
					 * only deal with trivial conversions.
					 *
					 * Note: pg_unicode_to_server() will throw an error for a
					 * conversion failure, rather than returning a failure
					 * indication.  That seems OK.
					 */
#ifndef FRONTEND
					{
						char		cbuf[MAX_UNICODE_EQUIVALENT_STRING + 1];

						pg_unicode_to_server(ch, (unsigned char *) cbuf);
						appendStringInfoString(lex->strval, cbuf);
					}
#else
					if (lex->input_encoding == PG_UTF8)
					{
						/* OK, we can map the code point to UTF8 easily */
						char		utf8str[5];
						int			utf8len;

						unicode_to_utf8(ch, (unsigned char *) utf8str);
						utf8len = pg_utf_mblen((unsigned char *) utf8str);
						appendBinaryStringInfo(lex->strval, utf8str, utf8len);
					}
					else if (ch <= 0x007f)
					{
						/* The ASCII range is the same in all encodings */
						appendStringInfoChar(lex->strval, (char) ch);
					}
					else
						FAIL_AT_CHAR_END(JSON_UNICODE_HIGH_ESCAPE);
#endif							/* FRONTEND */
				}
			}
			else if (lex->strval != NULL)
			{
				if (hi_surrogate != -1)
					FAIL_AT_CHAR_END(JSON_UNICODE_LOW_SURROGATE);

				switch (*s)
				{
					case '"':
					case '\\':
					case '/':
						appendStringInfoChar(lex->strval, *s);
						break;
					case 'b':
						appendStringInfoChar(lex->strval, '\b');
						break;
					case 'f':
						appendStringInfoChar(lex->strval, '\f');
						break;
					case 'n':
						appendStringInfoChar(lex->strval, '\n');
						break;
					case 'r':
						appendStringInfoChar(lex->strval, '\r');
						break;
					case 't':
						appendStringInfoChar(lex->strval, '\t');
						break;
					default:

						/*
						 * Not a valid string escape, so signal error.  We
						 * adjust token_start so that just the escape sequence
						 * is reported, not the whole string.
						 */
						lex->token_start = s;
						FAIL_AT_CHAR_END(JSON_ESCAPING_INVALID);
				}
			}
			else if (strchr("\"\\/bfnrt", *s) == NULL)
			{
				/*
				 * Simpler processing if we're not bothered about de-escaping
				 *
				 * It's very tempting to remove the strchr() call here and
				 * replace it with a switch statement, but testing so far has
				 * shown it's not a performance win.
				 */
				lex->token_start = s;
				FAIL_AT_CHAR_END(JSON_ESCAPING_INVALID);
			}
		}
		else if (lex->strval != NULL)
		{
			if (hi_surrogate != -1)
				FAIL_AT_CHAR_END(JSON_UNICODE_LOW_SURROGATE);

			appendStringInfoChar(lex->strval, *s);
		}
	}

	if (hi_surrogate != -1)
	{
		lex->token_terminator = s + 1;
		return JSON_UNICODE_LOW_SURROGATE;
	}

	/* Hooray, we found the end of the string! */
	lex->prev_token_terminator = lex->token_terminator;
	lex->token_terminator = s + 1;
	return JSON_SUCCESS;

#undef FAIL_AT_CHAR_START
#undef FAIL_AT_CHAR_END
}

/*
 * The next token in the input stream is known to be a number; lex it.
 *
 * In JSON, a number consists of four parts:
 *
 * (1) An optional minus sign ('-').
 *
 * (2) Either a single '0', or a string of one or more digits that does not
 *	   begin with a '0'.
 *
 * (3) An optional decimal part, consisting of a period ('.') followed by
 *	   one or more digits.  (Note: While this part can be omitted
 *	   completely, it's not OK to have only the decimal point without
 *	   any digits afterwards.)
 *
 * (4) An optional exponent part, consisting of 'e' or 'E', optionally
 *	   followed by '+' or '-', followed by one or more digits.  (Note:
 *	   As with the decimal part, if 'e' or 'E' is present, it must be
 *	   followed by at least one digit.)
 *
 * The 's' argument to this function points to the ostensible beginning
 * of part 2 - i.e. the character after any optional minus sign, or the
 * first character of the string if there is none.
 *
 * If num_err is not NULL, we return an error flag to *num_err rather than
 * raising an error for a badly-formed number.  Also, if total_len is not NULL
 * the distance from lex->input to the token end+1 is returned to *total_len.
 */
static inline JsonParseErrorType
json_lex_number(JsonLexContext *lex, char *s,
				bool *num_err, int *total_len)
{
	bool		error = false;
	int			len = s - lex->input;

	/* Part (1): leading sign indicator. */
	/* Caller already did this for us; so do nothing. */

	/* Part (2): parse main digit string. */
	if (len < lex->input_length && *s == '0')
	{
		s++;
		len++;
	}
	else if (len < lex->input_length && *s >= '1' && *s <= '9')
	{
		do
		{
			s++;
			len++;
		} while (len < lex->input_length && *s >= '0' && *s <= '9');
	}
	else
		error = true;

	/* Part (3): parse optional decimal portion. */
	if (len < lex->input_length && *s == '.')
	{
		s++;
		len++;
		if (len == lex->input_length || *s < '0' || *s > '9')
			error = true;
		else
		{
			do
			{
				s++;
				len++;
			} while (len < lex->input_length && *s >= '0' && *s <= '9');
		}
	}

	/* Part (4): parse optional exponent. */
	if (len < lex->input_length && (*s == 'e' || *s == 'E'))
	{
		s++;
		len++;
		if (len < lex->input_length && (*s == '+' || *s == '-'))
		{
			s++;
			len++;
		}
		if (len == lex->input_length || *s < '0' || *s > '9')
			error = true;
		else
		{
			do
			{
				s++;
				len++;
			} while (len < lex->input_length && *s >= '0' && *s <= '9');
		}
	}

	/*
	 * Check for trailing garbage.  As in json_lex(), any alphanumeric stuff
	 * here should be considered part of the token for error-reporting
	 * purposes.
	 */
	for (; len < lex->input_length && JSON_ALPHANUMERIC_CHAR(*s); s++, len++)
		error = true;

	if (total_len != NULL)
		*total_len = len;

	if (num_err != NULL)
	{
		/* let the caller handle any error */
		*num_err = error;
	}
	else
	{
		/* return token endpoint */
		lex->prev_token_terminator = lex->token_terminator;
		lex->token_terminator = s;
		/* handle error if any */
		if (error)
			return JSON_INVALID_TOKEN;
	}

	return JSON_SUCCESS;
}

/*
 * Report a parse error.
 *
 * lex->token_start and lex->token_terminator must identify the current token.
 */
static JsonParseErrorType
report_parse_error(JsonParseContext ctx, JsonLexContext *lex)
{
	/* Handle case where the input ended prematurely. */
	if (lex->token_start == NULL || lex->token_type == JSON_TOKEN_END)
		return JSON_EXPECTED_MORE;

	/* Otherwise choose the error type based on the parsing context. */
	switch (ctx)
	{
		case JSON_PARSE_END:
			return JSON_EXPECTED_END;
		case JSON_PARSE_VALUE:
			return JSON_EXPECTED_JSON;
		case JSON_PARSE_STRING:
			return JSON_EXPECTED_STRING;
		case JSON_PARSE_ARRAY_START:
			return JSON_EXPECTED_ARRAY_FIRST;
		case JSON_PARSE_ARRAY_NEXT:
			return JSON_EXPECTED_ARRAY_NEXT;
		case JSON_PARSE_OBJECT_START:
			return JSON_EXPECTED_OBJECT_FIRST;
		case JSON_PARSE_OBJECT_LABEL:
			return JSON_EXPECTED_COLON;
		case JSON_PARSE_OBJECT_NEXT:
			return JSON_EXPECTED_OBJECT_NEXT;
		case JSON_PARSE_OBJECT_COMMA:
			return JSON_EXPECTED_STRING;
	}

	/*
	 * We don't use a default: case, so that the compiler will warn about
	 * unhandled enum values.
	 */
	Assert(false);
	return JSON_SUCCESS;		/* silence stupider compilers */
}


#ifndef FRONTEND
/*
 * Extract the current token from a lexing context, for error reporting.
 */
static char *
extract_token(JsonLexContext *lex)
{
	int			toklen = lex->token_terminator - lex->token_start;
	char	   *token = palloc(toklen + 1);

	memcpy(token, lex->token_start, toklen);
	token[toklen] = '\0';
	return token;
}

/*
 * Construct an (already translated) detail message for a JSON error.
 *
 * Note that the error message generated by this routine may not be
 * palloc'd, making it unsafe for frontend code as there is no way to
 * know if this can be safely pfree'd or not.
 */
char *
json_errdetail(JsonParseErrorType error, JsonLexContext *lex)
{
	switch (error)
	{
		case JSON_SUCCESS:
			/* fall through to the error code after switch */
			break;
		case JSON_ESCAPING_INVALID:
			return psprintf(_("Escape sequence \"\\%s\" is invalid."),
							extract_token(lex));
		case JSON_ESCAPING_REQUIRED:
			return psprintf(_("Character with value 0x%02x must be escaped."),
							(unsigned char) *(lex->token_terminator));
		case JSON_EXPECTED_END:
			return psprintf(_("Expected end of input, but found \"%s\"."),
							extract_token(lex));
		case JSON_EXPECTED_ARRAY_FIRST:
			return psprintf(_("Expected array element or \"]\", but found \"%s\"."),
							extract_token(lex));
		case JSON_EXPECTED_ARRAY_NEXT:
			return psprintf(_("Expected \",\" or \"]\", but found \"%s\"."),
							extract_token(lex));
		case JSON_EXPECTED_COLON:
			return psprintf(_("Expected \":\", but found \"%s\"."),
							extract_token(lex));
		case JSON_EXPECTED_JSON:
			return psprintf(_("Expected JSON value, but found \"%s\"."),
							extract_token(lex));
		case JSON_EXPECTED_MORE:
			return _("The input string ended unexpectedly.");
		case JSON_EXPECTED_OBJECT_FIRST:
			return psprintf(_("Expected string or \"}\", but found \"%s\"."),
							extract_token(lex));
		case JSON_EXPECTED_OBJECT_NEXT:
			return psprintf(_("Expected \",\" or \"}\", but found \"%s\"."),
							extract_token(lex));
		case JSON_EXPECTED_STRING:
			return psprintf(_("Expected string, but found \"%s\"."),
							extract_token(lex));
		case JSON_INVALID_TOKEN:
			return psprintf(_("Token \"%s\" is invalid."),
							extract_token(lex));
		case JSON_UNICODE_CODE_POINT_ZERO:
			return _("\\u0000 cannot be converted to text.");
		case JSON_UNICODE_ESCAPE_FORMAT:
			return _("\"\\u\" must be followed by four hexadecimal digits.");
		case JSON_UNICODE_HIGH_ESCAPE:
			/* note: this case is only reachable in frontend not backend */
			return _("Unicode escape values cannot be used for code point values above 007F when the encoding is not UTF8.");
		case JSON_UNICODE_HIGH_SURROGATE:
			return _("Unicode high surrogate must not follow a high surrogate.");
		case JSON_UNICODE_LOW_SURROGATE:
			return _("Unicode low surrogate must follow a high surrogate.");
	}

	/*
	 * We don't use a default: case, so that the compiler will warn about
	 * unhandled enum values.  But this needs to be here anyway to cover the
	 * possibility of an incorrect input.
	 */
	elog(ERROR, "unexpected json parse error type: %d", (int) error);
	return NULL;
}
#endif

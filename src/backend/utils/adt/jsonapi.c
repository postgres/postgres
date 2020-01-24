/*-------------------------------------------------------------------------
 *
 * jsonapi.c
 *		JSON parser and lexer interfaces
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/jsonapi.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/jsonapi.h"

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

static inline void json_lex_string(JsonLexContext *lex);
static inline void json_lex_number(JsonLexContext *lex, char *s,
								   bool *num_err, int *total_len);
static inline void parse_scalar(JsonLexContext *lex, JsonSemAction *sem);
static void parse_object_field(JsonLexContext *lex, JsonSemAction *sem);
static void parse_object(JsonLexContext *lex, JsonSemAction *sem);
static void parse_array_element(JsonLexContext *lex, JsonSemAction *sem);
static void parse_array(JsonLexContext *lex, JsonSemAction *sem);
static void report_parse_error(JsonParseContext ctx, JsonLexContext *lex) pg_attribute_noreturn();
static void report_invalid_token(JsonLexContext *lex) pg_attribute_noreturn();
static int	report_json_context(JsonLexContext *lex);
static char *extract_mb_char(char *s);

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
 * lex_accept
 *
 * accept the look_ahead token and move the lexer to the next token if the
 * look_ahead token matches the token parameter. In that case, and if required,
 * also hand back the de-escaped lexeme.
 *
 * returns true if the token matched, false otherwise.
 */
static inline bool
lex_accept(JsonLexContext *lex, JsonTokenType token, char **lexeme)
{
	if (lex->token_type == token)
	{
		if (lexeme != NULL)
		{
			if (lex->token_type == JSON_TOKEN_STRING)
			{
				if (lex->strval != NULL)
					*lexeme = pstrdup(lex->strval->data);
			}
			else
			{
				int			len = (lex->token_terminator - lex->token_start);
				char	   *tokstr = palloc(len + 1);

				memcpy(tokstr, lex->token_start, len);
				tokstr[len] = '\0';
				*lexeme = tokstr;
			}
		}
		json_lex(lex);
		return true;
	}
	return false;
}

/*
 * lex_accept
 *
 * move the lexer to the next token if the current look_ahead token matches
 * the parameter token. Otherwise, report an error.
 */
static inline void
lex_expect(JsonParseContext ctx, JsonLexContext *lex, JsonTokenType token)
{
	if (!lex_accept(lex, token, NULL))
		report_parse_error(ctx, lex);
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
		dummy_lex.input = unconstify(char *, str) +1;
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
 * makeJsonLexContext
 *
 * lex constructor, with or without StringInfo object
 * for de-escaped lexemes.
 *
 * Without is better as it makes the processing faster, so only make one
 * if really required.
 *
 * If you already have the json as a text* value, use the first of these
 * functions, otherwise use  makeJsonLexContextCstringLen().
 */
JsonLexContext *
makeJsonLexContext(text *json, bool need_escapes)
{
	return makeJsonLexContextCstringLen(VARDATA_ANY(json),
										VARSIZE_ANY_EXHDR(json),
										need_escapes);
}

JsonLexContext *
makeJsonLexContextCstringLen(char *json, int len, bool need_escapes)
{
	JsonLexContext *lex = palloc0(sizeof(JsonLexContext));

	lex->input = lex->token_terminator = lex->line_start = json;
	lex->line_number = 1;
	lex->input_length = len;
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
void
pg_parse_json(JsonLexContext *lex, JsonSemAction *sem)
{
	JsonTokenType tok;

	/* get the initial token */
	json_lex(lex);

	tok = lex_peek(lex);

	/* parse by recursive descent */
	switch (tok)
	{
		case JSON_TOKEN_OBJECT_START:
			parse_object(lex, sem);
			break;
		case JSON_TOKEN_ARRAY_START:
			parse_array(lex, sem);
			break;
		default:
			parse_scalar(lex, sem); /* json can be a bare scalar */
	}

	lex_expect(JSON_PARSE_END, lex, JSON_TOKEN_END);

}

/*
 * json_count_array_elements
 *
 * Returns number of array elements in lex context at start of array token
 * until end of array token at same nesting level.
 *
 * Designed to be called from array_start routines.
 */
int
json_count_array_elements(JsonLexContext *lex)
{
	JsonLexContext copylex;
	int			count;

	/*
	 * It's safe to do this with a shallow copy because the lexical routines
	 * don't scribble on the input. They do scribble on the other pointers
	 * etc, so doing this with a copy makes that safe.
	 */
	memcpy(&copylex, lex, sizeof(JsonLexContext));
	copylex.strval = NULL;		/* not interested in values here */
	copylex.lex_level++;

	count = 0;
	lex_expect(JSON_PARSE_ARRAY_START, &copylex, JSON_TOKEN_ARRAY_START);
	if (lex_peek(&copylex) != JSON_TOKEN_ARRAY_END)
	{
		do
		{
			count++;
			parse_array_element(&copylex, &nullSemAction);
		}
		while (lex_accept(&copylex, JSON_TOKEN_COMMA, NULL));
	}
	lex_expect(JSON_PARSE_ARRAY_NEXT, &copylex, JSON_TOKEN_ARRAY_END);

	return count;
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
static inline void
parse_scalar(JsonLexContext *lex, JsonSemAction *sem)
{
	char	   *val = NULL;
	json_scalar_action sfunc = sem->scalar;
	char	  **valaddr;
	JsonTokenType tok = lex_peek(lex);

	valaddr = sfunc == NULL ? NULL : &val;

	/* a scalar must be a string, a number, true, false, or null */
	switch (tok)
	{
		case JSON_TOKEN_TRUE:
			lex_accept(lex, JSON_TOKEN_TRUE, valaddr);
			break;
		case JSON_TOKEN_FALSE:
			lex_accept(lex, JSON_TOKEN_FALSE, valaddr);
			break;
		case JSON_TOKEN_NULL:
			lex_accept(lex, JSON_TOKEN_NULL, valaddr);
			break;
		case JSON_TOKEN_NUMBER:
			lex_accept(lex, JSON_TOKEN_NUMBER, valaddr);
			break;
		case JSON_TOKEN_STRING:
			lex_accept(lex, JSON_TOKEN_STRING, valaddr);
			break;
		default:
			report_parse_error(JSON_PARSE_VALUE, lex);
	}

	if (sfunc != NULL)
		(*sfunc) (sem->semstate, val, tok);
}

static void
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
	char	  **fnameaddr = NULL;
	JsonTokenType tok;

	if (ostart != NULL || oend != NULL)
		fnameaddr = &fname;

	if (!lex_accept(lex, JSON_TOKEN_STRING, fnameaddr))
		report_parse_error(JSON_PARSE_STRING, lex);

	lex_expect(JSON_PARSE_OBJECT_LABEL, lex, JSON_TOKEN_COLON);

	tok = lex_peek(lex);
	isnull = tok == JSON_TOKEN_NULL;

	if (ostart != NULL)
		(*ostart) (sem->semstate, fname, isnull);

	switch (tok)
	{
		case JSON_TOKEN_OBJECT_START:
			parse_object(lex, sem);
			break;
		case JSON_TOKEN_ARRAY_START:
			parse_array(lex, sem);
			break;
		default:
			parse_scalar(lex, sem);
	}

	if (oend != NULL)
		(*oend) (sem->semstate, fname, isnull);
}

static void
parse_object(JsonLexContext *lex, JsonSemAction *sem)
{
	/*
	 * an object is a possibly empty sequence of object fields, separated by
	 * commas and surrounded by curly braces.
	 */
	json_struct_action ostart = sem->object_start;
	json_struct_action oend = sem->object_end;
	JsonTokenType tok;

	check_stack_depth();

	if (ostart != NULL)
		(*ostart) (sem->semstate);

	/*
	 * Data inside an object is at a higher nesting level than the object
	 * itself. Note that we increment this after we call the semantic routine
	 * for the object start and restore it before we call the routine for the
	 * object end.
	 */
	lex->lex_level++;

	/* we know this will succeed, just clearing the token */
	lex_expect(JSON_PARSE_OBJECT_START, lex, JSON_TOKEN_OBJECT_START);

	tok = lex_peek(lex);
	switch (tok)
	{
		case JSON_TOKEN_STRING:
			parse_object_field(lex, sem);
			while (lex_accept(lex, JSON_TOKEN_COMMA, NULL))
				parse_object_field(lex, sem);
			break;
		case JSON_TOKEN_OBJECT_END:
			break;
		default:
			/* case of an invalid initial token inside the object */
			report_parse_error(JSON_PARSE_OBJECT_START, lex);
	}

	lex_expect(JSON_PARSE_OBJECT_NEXT, lex, JSON_TOKEN_OBJECT_END);

	lex->lex_level--;

	if (oend != NULL)
		(*oend) (sem->semstate);
}

static void
parse_array_element(JsonLexContext *lex, JsonSemAction *sem)
{
	json_aelem_action astart = sem->array_element_start;
	json_aelem_action aend = sem->array_element_end;
	JsonTokenType tok = lex_peek(lex);

	bool		isnull;

	isnull = tok == JSON_TOKEN_NULL;

	if (astart != NULL)
		(*astart) (sem->semstate, isnull);

	/* an array element is any object, array or scalar */
	switch (tok)
	{
		case JSON_TOKEN_OBJECT_START:
			parse_object(lex, sem);
			break;
		case JSON_TOKEN_ARRAY_START:
			parse_array(lex, sem);
			break;
		default:
			parse_scalar(lex, sem);
	}

	if (aend != NULL)
		(*aend) (sem->semstate, isnull);
}

static void
parse_array(JsonLexContext *lex, JsonSemAction *sem)
{
	/*
	 * an array is a possibly empty sequence of array elements, separated by
	 * commas and surrounded by square brackets.
	 */
	json_struct_action astart = sem->array_start;
	json_struct_action aend = sem->array_end;

	check_stack_depth();

	if (astart != NULL)
		(*astart) (sem->semstate);

	/*
	 * Data inside an array is at a higher nesting level than the array
	 * itself. Note that we increment this after we call the semantic routine
	 * for the array start and restore it before we call the routine for the
	 * array end.
	 */
	lex->lex_level++;

	lex_expect(JSON_PARSE_ARRAY_START, lex, JSON_TOKEN_ARRAY_START);
	if (lex_peek(lex) != JSON_TOKEN_ARRAY_END)
	{

		parse_array_element(lex, sem);

		while (lex_accept(lex, JSON_TOKEN_COMMA, NULL))
			parse_array_element(lex, sem);
	}

	lex_expect(JSON_PARSE_ARRAY_NEXT, lex, JSON_TOKEN_ARRAY_END);

	lex->lex_level--;

	if (aend != NULL)
		(*aend) (sem->semstate);
}

/*
 * Lex one token from the input stream.
 */
void
json_lex(JsonLexContext *lex)
{
	char	   *s;
	int			len;

	/* Skip leading whitespace. */
	s = lex->token_terminator;
	len = s - lex->input;
	while (len < lex->input_length &&
		   (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r'))
	{
		if (*s == '\n')
			++lex->line_number;
		++s;
		++len;
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
				json_lex_string(lex);
				lex->token_type = JSON_TOKEN_STRING;
				break;
			case '-':
				/* Negative number. */
				json_lex_number(lex, s + 1, NULL, NULL);
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
				json_lex_number(lex, s, NULL, NULL);
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
						report_invalid_token(lex);
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
							report_invalid_token(lex);
					}
					else if (p - s == 5 && memcmp(s, "false", 5) == 0)
						lex->token_type = JSON_TOKEN_FALSE;
					else
						report_invalid_token(lex);

				}
		}						/* end of switch */
}

/*
 * The next token in the input stream is known to be a string; lex it.
 */
static inline void
json_lex_string(JsonLexContext *lex)
{
	char	   *s;
	int			len;
	int			hi_surrogate = -1;

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
		{
			lex->token_terminator = s;
			report_invalid_token(lex);
		}
		else if (*s == '"')
			break;
		else if ((unsigned char) *s < 32)
		{
			/* Per RFC4627, these characters MUST be escaped. */
			/* Since *s isn't printable, exclude it from the context string */
			lex->token_terminator = s;
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for type %s", "json"),
					 errdetail("Character with value 0x%02x must be escaped.",
							   (unsigned char) *s),
					 report_json_context(lex)));
		}
		else if (*s == '\\')
		{
			/* OK, we have an escape character. */
			s++;
			len++;
			if (len >= lex->input_length)
			{
				lex->token_terminator = s;
				report_invalid_token(lex);
			}
			else if (*s == 'u')
			{
				int			i;
				int			ch = 0;

				for (i = 1; i <= 4; i++)
				{
					s++;
					len++;
					if (len >= lex->input_length)
					{
						lex->token_terminator = s;
						report_invalid_token(lex);
					}
					else if (*s >= '0' && *s <= '9')
						ch = (ch * 16) + (*s - '0');
					else if (*s >= 'a' && *s <= 'f')
						ch = (ch * 16) + (*s - 'a') + 10;
					else if (*s >= 'A' && *s <= 'F')
						ch = (ch * 16) + (*s - 'A') + 10;
					else
					{
						lex->token_terminator = s + pg_mblen(s);
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
								 errmsg("invalid input syntax for type %s",
										"json"),
								 errdetail("\"\\u\" must be followed by four hexadecimal digits."),
								 report_json_context(lex)));
					}
				}
				if (lex->strval != NULL)
				{
					char		utf8str[5];
					int			utf8len;

					if (ch >= 0xd800 && ch <= 0xdbff)
					{
						if (hi_surrogate != -1)
							ereport(ERROR,
									(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
									 errmsg("invalid input syntax for type %s",
											"json"),
									 errdetail("Unicode high surrogate must not follow a high surrogate."),
									 report_json_context(lex)));
						hi_surrogate = (ch & 0x3ff) << 10;
						continue;
					}
					else if (ch >= 0xdc00 && ch <= 0xdfff)
					{
						if (hi_surrogate == -1)
							ereport(ERROR,
									(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
									 errmsg("invalid input syntax for type %s", "json"),
									 errdetail("Unicode low surrogate must follow a high surrogate."),
									 report_json_context(lex)));
						ch = 0x10000 + hi_surrogate + (ch & 0x3ff);
						hi_surrogate = -1;
					}

					if (hi_surrogate != -1)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
								 errmsg("invalid input syntax for type %s", "json"),
								 errdetail("Unicode low surrogate must follow a high surrogate."),
								 report_json_context(lex)));

					/*
					 * For UTF8, replace the escape sequence by the actual
					 * utf8 character in lex->strval. Do this also for other
					 * encodings if the escape designates an ASCII character,
					 * otherwise raise an error.
					 */

					if (ch == 0)
					{
						/* We can't allow this, since our TEXT type doesn't */
						ereport(ERROR,
								(errcode(ERRCODE_UNTRANSLATABLE_CHARACTER),
								 errmsg("unsupported Unicode escape sequence"),
								 errdetail("\\u0000 cannot be converted to text."),
								 report_json_context(lex)));
					}
					else if (GetDatabaseEncoding() == PG_UTF8)
					{
						unicode_to_utf8(ch, (unsigned char *) utf8str);
						utf8len = pg_utf_mblen((unsigned char *) utf8str);
						appendBinaryStringInfo(lex->strval, utf8str, utf8len);
					}
					else if (ch <= 0x007f)
					{
						/*
						 * This is the only way to designate things like a
						 * form feed character in JSON, so it's useful in all
						 * encodings.
						 */
						appendStringInfoChar(lex->strval, (char) ch);
					}
					else
					{
						ereport(ERROR,
								(errcode(ERRCODE_UNTRANSLATABLE_CHARACTER),
								 errmsg("unsupported Unicode escape sequence"),
								 errdetail("Unicode escape values cannot be used for code point values above 007F when the server encoding is not UTF8."),
								 report_json_context(lex)));
					}

				}
			}
			else if (lex->strval != NULL)
			{
				if (hi_surrogate != -1)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
							 errmsg("invalid input syntax for type %s",
									"json"),
							 errdetail("Unicode low surrogate must follow a high surrogate."),
							 report_json_context(lex)));

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
						/* Not a valid string escape, so error out. */
						lex->token_terminator = s + pg_mblen(s);
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
								 errmsg("invalid input syntax for type %s",
										"json"),
								 errdetail("Escape sequence \"\\%s\" is invalid.",
										   extract_mb_char(s)),
								 report_json_context(lex)));
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
				lex->token_terminator = s + pg_mblen(s);
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("invalid input syntax for type %s", "json"),
						 errdetail("Escape sequence \"\\%s\" is invalid.",
								   extract_mb_char(s)),
						 report_json_context(lex)));
			}

		}
		else if (lex->strval != NULL)
		{
			if (hi_surrogate != -1)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("invalid input syntax for type %s", "json"),
						 errdetail("Unicode low surrogate must follow a high surrogate."),
						 report_json_context(lex)));

			appendStringInfoChar(lex->strval, *s);
		}

	}

	if (hi_surrogate != -1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type %s", "json"),
				 errdetail("Unicode low surrogate must follow a high surrogate."),
				 report_json_context(lex)));

	/* Hooray, we found the end of the string! */
	lex->prev_token_terminator = lex->token_terminator;
	lex->token_terminator = s + 1;
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
static inline void
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
			report_invalid_token(lex);
	}
}

/*
 * Report a parse error.
 *
 * lex->token_start and lex->token_terminator must identify the current token.
 */
static void
report_parse_error(JsonParseContext ctx, JsonLexContext *lex)
{
	char	   *token;
	int			toklen;

	/* Handle case where the input ended prematurely. */
	if (lex->token_start == NULL || lex->token_type == JSON_TOKEN_END)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type %s", "json"),
				 errdetail("The input string ended unexpectedly."),
				 report_json_context(lex)));

	/* Separate out the current token. */
	toklen = lex->token_terminator - lex->token_start;
	token = palloc(toklen + 1);
	memcpy(token, lex->token_start, toklen);
	token[toklen] = '\0';

	/* Complain, with the appropriate detail message. */
	if (ctx == JSON_PARSE_END)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type %s", "json"),
				 errdetail("Expected end of input, but found \"%s\".",
						   token),
				 report_json_context(lex)));
	else
	{
		switch (ctx)
		{
			case JSON_PARSE_VALUE:
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("invalid input syntax for type %s", "json"),
						 errdetail("Expected JSON value, but found \"%s\".",
								   token),
						 report_json_context(lex)));
				break;
			case JSON_PARSE_STRING:
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("invalid input syntax for type %s", "json"),
						 errdetail("Expected string, but found \"%s\".",
								   token),
						 report_json_context(lex)));
				break;
			case JSON_PARSE_ARRAY_START:
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("invalid input syntax for type %s", "json"),
						 errdetail("Expected array element or \"]\", but found \"%s\".",
								   token),
						 report_json_context(lex)));
				break;
			case JSON_PARSE_ARRAY_NEXT:
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("invalid input syntax for type %s", "json"),
						 errdetail("Expected \",\" or \"]\", but found \"%s\".",
								   token),
						 report_json_context(lex)));
				break;
			case JSON_PARSE_OBJECT_START:
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("invalid input syntax for type %s", "json"),
						 errdetail("Expected string or \"}\", but found \"%s\".",
								   token),
						 report_json_context(lex)));
				break;
			case JSON_PARSE_OBJECT_LABEL:
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("invalid input syntax for type %s", "json"),
						 errdetail("Expected \":\", but found \"%s\".",
								   token),
						 report_json_context(lex)));
				break;
			case JSON_PARSE_OBJECT_NEXT:
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("invalid input syntax for type %s", "json"),
						 errdetail("Expected \",\" or \"}\", but found \"%s\".",
								   token),
						 report_json_context(lex)));
				break;
			case JSON_PARSE_OBJECT_COMMA:
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("invalid input syntax for type %s", "json"),
						 errdetail("Expected string, but found \"%s\".",
								   token),
						 report_json_context(lex)));
				break;
			default:
				elog(ERROR, "unexpected json parse state: %d", ctx);
		}
	}
}

/*
 * Report an invalid input token.
 *
 * lex->token_start and lex->token_terminator must identify the token.
 */
static void
report_invalid_token(JsonLexContext *lex)
{
	char	   *token;
	int			toklen;

	/* Separate out the offending token. */
	toklen = lex->token_terminator - lex->token_start;
	token = palloc(toklen + 1);
	memcpy(token, lex->token_start, toklen);
	token[toklen] = '\0';

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
			 errmsg("invalid input syntax for type %s", "json"),
			 errdetail("Token \"%s\" is invalid.", token),
			 report_json_context(lex)));
}

/*
 * Report a CONTEXT line for bogus JSON input.
 *
 * lex->token_terminator must be set to identify the spot where we detected
 * the error.  Note that lex->token_start might be NULL, in case we recognized
 * error at EOF.
 *
 * The return value isn't meaningful, but we make it non-void so that this
 * can be invoked inside ereport().
 */
static int
report_json_context(JsonLexContext *lex)
{
	const char *context_start;
	const char *context_end;
	const char *line_start;
	int			line_number;
	char	   *ctxt;
	int			ctxtlen;
	const char *prefix;
	const char *suffix;

	/* Choose boundaries for the part of the input we will display */
	context_start = lex->input;
	context_end = lex->token_terminator;
	line_start = context_start;
	line_number = 1;
	for (;;)
	{
		/* Always advance over newlines */
		if (context_start < context_end && *context_start == '\n')
		{
			context_start++;
			line_start = context_start;
			line_number++;
			continue;
		}
		/* Otherwise, done as soon as we are close enough to context_end */
		if (context_end - context_start < 50)
			break;
		/* Advance to next multibyte character */
		if (IS_HIGHBIT_SET(*context_start))
			context_start += pg_mblen(context_start);
		else
			context_start++;
	}

	/*
	 * We add "..." to indicate that the excerpt doesn't start at the
	 * beginning of the line ... but if we're within 3 characters of the
	 * beginning of the line, we might as well just show the whole line.
	 */
	if (context_start - line_start <= 3)
		context_start = line_start;

	/* Get a null-terminated copy of the data to present */
	ctxtlen = context_end - context_start;
	ctxt = palloc(ctxtlen + 1);
	memcpy(ctxt, context_start, ctxtlen);
	ctxt[ctxtlen] = '\0';

	/*
	 * Show the context, prefixing "..." if not starting at start of line, and
	 * suffixing "..." if not ending at end of line.
	 */
	prefix = (context_start > line_start) ? "..." : "";
	suffix = (lex->token_type != JSON_TOKEN_END && context_end - lex->input < lex->input_length && *context_end != '\n' && *context_end != '\r') ? "..." : "";

	return errcontext("JSON data, line %d: %s%s%s",
					  line_number, prefix, ctxt, suffix);
}

/*
 * Extract a single, possibly multi-byte char from the input string.
 */
static char *
extract_mb_char(char *s)
{
	char	   *res;
	int			len;

	len = pg_mblen(s);
	res = palloc(len + 1);
	memcpy(res, s, len);
	res[len] = '\0';

	return res;
}

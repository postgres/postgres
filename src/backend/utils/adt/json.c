/*-------------------------------------------------------------------------
 *
 * json.c
 *		JSON data type support.
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/json.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "lib/stringinfo.h"
#include "libpq/pqformat.h"
#include "mb/pg_wchar.h"
#include "parser/parse_coerce.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/json.h"
#include "utils/typcache.h"

typedef enum					/* types of JSON values */
{
	JSON_VALUE_INVALID,			/* non-value tokens are reported as this */
	JSON_VALUE_STRING,
	JSON_VALUE_NUMBER,
	JSON_VALUE_OBJECT,
	JSON_VALUE_ARRAY,
	JSON_VALUE_TRUE,
	JSON_VALUE_FALSE,
	JSON_VALUE_NULL
} JsonValueType;

typedef struct					/* state of JSON lexer */
{
	char	   *input;			/* whole string being parsed */
	char	   *token_start;	/* start of current token within input */
	char	   *token_terminator; /* end of previous or current token */
	JsonValueType token_type;	/* type of current token, once it's known */
} JsonLexContext;

typedef enum					/* states of JSON parser */
{
	JSON_PARSE_VALUE,			/* expecting a value */
	JSON_PARSE_ARRAY_START,		/* saw '[', expecting value or ']' */
	JSON_PARSE_ARRAY_NEXT,		/* saw array element, expecting ',' or ']' */
	JSON_PARSE_OBJECT_START,	/* saw '{', expecting label or '}' */
	JSON_PARSE_OBJECT_LABEL,	/* saw object label, expecting ':' */
	JSON_PARSE_OBJECT_NEXT,		/* saw object value, expecting ',' or '}' */
	JSON_PARSE_OBJECT_COMMA		/* saw object ',', expecting next label */
} JsonParseState;

typedef struct JsonParseStack	/* the parser state has to be stackable */
{
	JsonParseState state;
	/* currently only need the state enum, but maybe someday more stuff */
} JsonParseStack;

typedef enum					/* required operations on state stack */
{
	JSON_STACKOP_NONE,			/* no-op */
	JSON_STACKOP_PUSH,			/* push new JSON_PARSE_VALUE stack item */
	JSON_STACKOP_PUSH_WITH_PUSHBACK, /* push, then rescan current token */
	JSON_STACKOP_POP			/* pop, or expect end of input if no stack */
} JsonStackOp;

static void json_validate_cstring(char *input);
static void json_lex(JsonLexContext *lex);
static void json_lex_string(JsonLexContext *lex);
static void json_lex_number(JsonLexContext *lex, char *s, bool *num_err);
static void report_parse_error(JsonParseStack *stack, JsonLexContext *lex);
static void report_invalid_token(JsonLexContext *lex);
static int report_json_context(JsonLexContext *lex);
static char *extract_mb_char(char *s);
static void composite_to_json(Datum composite, StringInfo result,
							  bool use_line_feeds);
static void array_dim_to_json(StringInfo result, int dim, int ndims, int *dims,
				  Datum *vals, bool *nulls, int *valcount,
				  TYPCATEGORY tcategory, Oid typoutputfunc,
				  bool use_line_feeds);
static void array_to_json_internal(Datum array, StringInfo result,
								   bool use_line_feeds);

/* fake type category for JSON so we can distinguish it in datum_to_json */
#define TYPCATEGORY_JSON 'j'
/* chars to consider as part of an alphanumeric token */
#define JSON_ALPHANUMERIC_CHAR(c)  \
	(((c) >= 'a' && (c) <= 'z') || \
	 ((c) >= 'A' && (c) <= 'Z') || \
	 ((c) >= '0' && (c) <= '9') || \
	 (c) == '_' || \
	 IS_HIGHBIT_SET(c))


/*
 * Input.
 */
Datum
json_in(PG_FUNCTION_ARGS)
{
	char	   *text = PG_GETARG_CSTRING(0);

	json_validate_cstring(text);

	/* Internal representation is the same as text, for now */
	PG_RETURN_TEXT_P(cstring_to_text(text));
}

/*
 * Output.
 */
Datum
json_out(PG_FUNCTION_ARGS)
{
	/* we needn't detoast because text_to_cstring will handle that */
	Datum		txt = PG_GETARG_DATUM(0);

	PG_RETURN_CSTRING(TextDatumGetCString(txt));
}

/*
 * Binary send.
 */
Datum
json_send(PG_FUNCTION_ARGS)
{
	text	   *t = PG_GETARG_TEXT_PP(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendtext(&buf, VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t));
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*
 * Binary receive.
 */
Datum
json_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	text	   *result;
	char	   *str;
	int			nbytes;

	str = pq_getmsgtext(buf, buf->len - buf->cursor, &nbytes);

	/*
	 * We need a null-terminated string to pass to json_validate_cstring().
	 * Rather than make a separate copy, make the temporary result one byte
	 * bigger than it needs to be.
	 */
	result = palloc(nbytes + 1 + VARHDRSZ);
	SET_VARSIZE(result, nbytes + VARHDRSZ);
	memcpy(VARDATA(result), str, nbytes);
	str = VARDATA(result);
	str[nbytes] = '\0';

	/* Validate it. */
	json_validate_cstring(str);

	PG_RETURN_TEXT_P(result);
}

/*
 * Check whether supplied input is valid JSON.
 */
static void
json_validate_cstring(char *input)
{
	JsonLexContext lex;
	JsonParseStack *stack,
			   *stacktop;
	int			stacksize;

	/* Set up lexing context. */
	lex.input = input;
	lex.token_terminator = lex.input;

	/* Set up parse stack. */
	stacksize = 32;
	stacktop = (JsonParseStack *) palloc(sizeof(JsonParseStack) * stacksize);
	stack = stacktop;
	stack->state = JSON_PARSE_VALUE;

	/* Main parsing loop. */
	for (;;)
	{
		JsonStackOp op;

		/* Fetch next token. */
		json_lex(&lex);

		/* Check for unexpected end of input. */
		if (lex.token_start == NULL)
			report_parse_error(stack, &lex);

redo:
		/* Figure out what to do with this token. */
		op = JSON_STACKOP_NONE;
		switch (stack->state)
		{
			case JSON_PARSE_VALUE:
				if (lex.token_type != JSON_VALUE_INVALID)
					op = JSON_STACKOP_POP;
				else if (lex.token_start[0] == '[')
					stack->state = JSON_PARSE_ARRAY_START;
				else if (lex.token_start[0] == '{')
					stack->state = JSON_PARSE_OBJECT_START;
				else
					report_parse_error(stack, &lex);
				break;
			case JSON_PARSE_ARRAY_START:
				if (lex.token_type != JSON_VALUE_INVALID)
					stack->state = JSON_PARSE_ARRAY_NEXT;
				else if (lex.token_start[0] == ']')
					op = JSON_STACKOP_POP;
				else if (lex.token_start[0] == '[' ||
						 lex.token_start[0] == '{')
				{
					stack->state = JSON_PARSE_ARRAY_NEXT;
					op = JSON_STACKOP_PUSH_WITH_PUSHBACK;
				}
				else
					report_parse_error(stack, &lex);
				break;
			case JSON_PARSE_ARRAY_NEXT:
				if (lex.token_type != JSON_VALUE_INVALID)
					report_parse_error(stack, &lex);
				else if (lex.token_start[0] == ']')
					op = JSON_STACKOP_POP;
				else if (lex.token_start[0] == ',')
					op = JSON_STACKOP_PUSH;
				else
					report_parse_error(stack, &lex);
				break;
			case JSON_PARSE_OBJECT_START:
				if (lex.token_type == JSON_VALUE_STRING)
					stack->state = JSON_PARSE_OBJECT_LABEL;
				else if (lex.token_type == JSON_VALUE_INVALID &&
						 lex.token_start[0] == '}')
					op = JSON_STACKOP_POP;
				else
					report_parse_error(stack, &lex);
				break;
			case JSON_PARSE_OBJECT_LABEL:
				if (lex.token_type == JSON_VALUE_INVALID &&
					lex.token_start[0] == ':')
				{
					stack->state = JSON_PARSE_OBJECT_NEXT;
					op = JSON_STACKOP_PUSH;
				}
				else
					report_parse_error(stack, &lex);
				break;
			case JSON_PARSE_OBJECT_NEXT:
				if (lex.token_type != JSON_VALUE_INVALID)
					report_parse_error(stack, &lex);
				else if (lex.token_start[0] == '}')
					op = JSON_STACKOP_POP;
				else if (lex.token_start[0] == ',')
					stack->state = JSON_PARSE_OBJECT_COMMA;
				else
					report_parse_error(stack, &lex);
				break;
			case JSON_PARSE_OBJECT_COMMA:
				if (lex.token_type == JSON_VALUE_STRING)
					stack->state = JSON_PARSE_OBJECT_LABEL;
				else
					report_parse_error(stack, &lex);
				break;
			default:
				elog(ERROR, "unexpected json parse state: %d",
					 (int) stack->state);
		}

		/* Push or pop the state stack, if needed. */
		switch (op)
		{
			case JSON_STACKOP_PUSH:
			case JSON_STACKOP_PUSH_WITH_PUSHBACK:
				stack++;
				if (stack >= &stacktop[stacksize])
				{
					/* Need to enlarge the stack. */
					int			stackoffset = stack - stacktop;

					stacksize += 32;
					stacktop = (JsonParseStack *)
						repalloc(stacktop,
								 sizeof(JsonParseStack) * stacksize);
					stack = stacktop + stackoffset;
				}
				stack->state = JSON_PARSE_VALUE;
				if (op == JSON_STACKOP_PUSH_WITH_PUSHBACK)
					goto redo;
				break;
			case JSON_STACKOP_POP:
				if (stack == stacktop)
				{
					/* Expect end of input. */
					json_lex(&lex);
					if (lex.token_start != NULL)
						report_parse_error(NULL, &lex);
					return;
				}
				stack--;
				break;
			case JSON_STACKOP_NONE:
				/* nothing to do */
				break;
		}
	}
}

/*
 * Lex one token from the input stream.
 */
static void
json_lex(JsonLexContext *lex)
{
	char	   *s;

	/* Skip leading whitespace. */
	s = lex->token_terminator;
	while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
		s++;
	lex->token_start = s;

	/* Determine token type. */
	if (strchr("{}[],:", s[0]) != NULL)
	{
		/* strchr() is willing to match a zero byte, so test for that. */
		if (s[0] == '\0')
		{
			/* End of string. */
			lex->token_start = NULL;
			lex->token_terminator = s;
		}
		else
		{
			/* Single-character token, some kind of punctuation mark. */
			lex->token_terminator = s + 1;
		}
		lex->token_type = JSON_VALUE_INVALID;
	}
	else if (*s == '"')
	{
		/* String. */
		json_lex_string(lex);
		lex->token_type = JSON_VALUE_STRING;
	}
	else if (*s == '-')
	{
		/* Negative number. */
		json_lex_number(lex, s + 1, NULL);
		lex->token_type = JSON_VALUE_NUMBER;
	}
	else if (*s >= '0' && *s <= '9')
	{
		/* Positive number. */
		json_lex_number(lex, s, NULL);
		lex->token_type = JSON_VALUE_NUMBER;
	}
	else
	{
		char	   *p;

		/*
		 * We're not dealing with a string, number, legal punctuation mark, or
		 * end of string.  The only legal tokens we might find here are true,
		 * false, and null, but for error reporting purposes we scan until we
		 * see a non-alphanumeric character.  That way, we can report the
		 * whole word as an unexpected token, rather than just some
		 * unintuitive prefix thereof.
		 */
		for (p = s; JSON_ALPHANUMERIC_CHAR(*p); p++)
			/* skip */ ;

		if (p == s)
		{
			/*
			 * We got some sort of unexpected punctuation or an otherwise
			 * unexpected character, so just complain about that one
			 * character.  (It can't be multibyte because the above loop
			 * will advance over any multibyte characters.)
			 */
			lex->token_terminator = s + 1;
			report_invalid_token(lex);
		}

		/*
		 * We've got a real alphanumeric token here.  If it happens to be
		 * true, false, or null, all is well.  If not, error out.
		 */
		lex->token_terminator = p;
		if (p - s == 4)
		{
			if (memcmp(s, "true", 4) == 0)
				lex->token_type = JSON_VALUE_TRUE;
			else if (memcmp(s, "null", 4) == 0)
				lex->token_type = JSON_VALUE_NULL;
			else
				report_invalid_token(lex);
		}
		else if (p - s == 5 && memcmp(s, "false", 5) == 0)
			lex->token_type = JSON_VALUE_FALSE;
		else
			report_invalid_token(lex);
	}
}

/*
 * The next token in the input stream is known to be a string; lex it.
 */
static void
json_lex_string(JsonLexContext *lex)
{
	char	   *s;

	for (s = lex->token_start + 1; *s != '"'; s++)
	{
		/* Per RFC4627, these characters MUST be escaped. */
		if ((unsigned char) *s < 32)
		{
			/* A NUL byte marks the (premature) end of the string. */
			if (*s == '\0')
			{
				lex->token_terminator = s;
				report_invalid_token(lex);
			}
			/* Since *s isn't printable, exclude it from the context string */
			lex->token_terminator = s;
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for type json"),
					 errdetail("Character with value 0x%02x must be escaped.",
							   (unsigned char) *s),
					 report_json_context(lex)));
		}
		else if (*s == '\\')
		{
			/* OK, we have an escape character. */
			s++;
			if (*s == '\0')
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
					if (*s == '\0')
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
								 errmsg("invalid input syntax for type json"),
								 errdetail("\"\\u\" must be followed by four hexadecimal digits."),
								 report_json_context(lex)));
					}
				}
			}
			else if (strchr("\"\\/bfnrt", *s) == NULL)
			{
				/* Not a valid string escape, so error out. */
				lex->token_terminator = s + pg_mblen(s);
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("invalid input syntax for type json"),
						 errdetail("Escape sequence \"\\%s\" is invalid.",
								   extract_mb_char(s)),
						 report_json_context(lex)));
			}
		}
	}

	/* Hooray, we found the end of the string! */
	lex->token_terminator = s + 1;
}

/*-------------------------------------------------------------------------
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
 * of part 2 - i.e. the character after any optional minus sign, and the
 * first character of the string if there is none.
 *
 *-------------------------------------------------------------------------
 */
static void
json_lex_number(JsonLexContext *lex, char *s, bool *num_err)
{
	bool		error = false;
	char	   *p;

	/* Part (1): leading sign indicator. */
	/* Caller already did this for us; so do nothing. */

	/* Part (2): parse main digit string. */
	if (*s == '0')
		s++;
	else if (*s >= '1' && *s <= '9')
	{
		do
		{
			s++;
		} while (*s >= '0' && *s <= '9');
	}
	else
		error = true;

	/* Part (3): parse optional decimal portion. */
	if (*s == '.')
	{
		s++;
		if (*s < '0' || *s > '9')
			error = true;
		else
		{
			do
			{
				s++;
			} while (*s >= '0' && *s <= '9');
		}
	}

	/* Part (4): parse optional exponent. */
	if (*s == 'e' || *s == 'E')
	{
		s++;
		if (*s == '+' || *s == '-')
			s++;
		if (*s < '0' || *s > '9')
			error = true;
		else
		{
			do
			{
				s++;
			} while (*s >= '0' && *s <= '9');
		}
	}

	/*
	 * Check for trailing garbage.  As in json_lex(), any alphanumeric stuff
	 * here should be considered part of the token for error-reporting
	 * purposes.
	 */
	for (p = s; JSON_ALPHANUMERIC_CHAR(*p); p++)
		error = true;

	if (num_err != NULL)
	{
		/* let the caller handle the error */
		*num_err = error;
	}
	else
	{
		lex->token_terminator = p;
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
report_parse_error(JsonParseStack *stack, JsonLexContext *lex)
{
	char	   *token;
	int			toklen;

	/* Handle case where the input ended prematurely. */
	if (lex->token_start == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type json"),
				 errdetail("The input string ended unexpectedly."),
				 report_json_context(lex)));

	/* Separate out the current token. */
	toklen = lex->token_terminator - lex->token_start;
	token = palloc(toklen + 1);
	memcpy(token, lex->token_start, toklen);
	token[toklen] = '\0';

	/* Complain, with the appropriate detail message. */
	if (stack == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type json"),
				 errdetail("Expected end of input, but found \"%s\".",
						   token),
				 report_json_context(lex)));
	else
	{
		switch (stack->state)
		{
			case JSON_PARSE_VALUE:
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("invalid input syntax for type json"),
						 errdetail("Expected JSON value, but found \"%s\".",
								   token),
						 report_json_context(lex)));
				break;
			case JSON_PARSE_ARRAY_START:
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("invalid input syntax for type json"),
						 errdetail("Expected array element or \"]\", but found \"%s\".",
								   token),
						 report_json_context(lex)));
				break;
			case JSON_PARSE_ARRAY_NEXT:
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("invalid input syntax for type json"),
						 errdetail("Expected \",\" or \"]\", but found \"%s\".",
								   token),
						 report_json_context(lex)));
				break;
			case JSON_PARSE_OBJECT_START:
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("invalid input syntax for type json"),
						 errdetail("Expected string or \"}\", but found \"%s\".",
								   token),
						 report_json_context(lex)));
				break;
			case JSON_PARSE_OBJECT_LABEL:
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("invalid input syntax for type json"),
						 errdetail("Expected \":\", but found \"%s\".",
								   token),
						 report_json_context(lex)));
				break;
			case JSON_PARSE_OBJECT_NEXT:
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("invalid input syntax for type json"),
						 errdetail("Expected \",\" or \"}\", but found \"%s\".",
								   token),
						 report_json_context(lex)));
				break;
			case JSON_PARSE_OBJECT_COMMA:
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("invalid input syntax for type json"),
						 errdetail("Expected string, but found \"%s\".",
								   token),
						 report_json_context(lex)));
				break;
			default:
				elog(ERROR, "unexpected json parse state: %d",
					 (int) stack->state);
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
			 errmsg("invalid input syntax for type json"),
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
		/* Always advance over newlines (context_end test is just paranoia) */
		if (*context_start == '\n' && context_start < context_end)
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
	suffix = (*context_end != '\0' && *context_end != '\n' && *context_end != '\r') ? "..." : "";

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

/*
 * Turn a scalar Datum into JSON, appending the string to "result".
 *
 * Hand off a non-scalar datum to composite_to_json or array_to_json_internal
 * as appropriate.
 */
static void
datum_to_json(Datum val, bool is_null, StringInfo result,
			  TYPCATEGORY tcategory, Oid typoutputfunc)
{
	char	   *outputstr;
	bool		numeric_error;
	JsonLexContext dummy_lex;

	if (is_null)
	{
		appendStringInfoString(result, "null");
		return;
	}

	switch (tcategory)
	{
		case TYPCATEGORY_ARRAY:
			array_to_json_internal(val, result, false);
			break;
		case TYPCATEGORY_COMPOSITE:
			composite_to_json(val, result, false);
			break;
		case TYPCATEGORY_BOOLEAN:
			if (DatumGetBool(val))
				appendStringInfoString(result, "true");
			else
				appendStringInfoString(result, "false");
			break;
		case TYPCATEGORY_NUMERIC:
			outputstr = OidOutputFunctionCall(typoutputfunc, val);

			/*
			 * Don't call escape_json here if it's a valid JSON number.
			 */
			dummy_lex.input = *outputstr == '-' ? outputstr + 1 : outputstr;
			json_lex_number(&dummy_lex, dummy_lex.input, &numeric_error);
			if (!numeric_error)
				appendStringInfoString(result, outputstr);
			else
				escape_json(result, outputstr);
			pfree(outputstr);
			break;
		case TYPCATEGORY_JSON:
			/* JSON will already be escaped */
			outputstr = OidOutputFunctionCall(typoutputfunc, val);
			appendStringInfoString(result, outputstr);
			pfree(outputstr);
			break;
		default:
			outputstr = OidOutputFunctionCall(typoutputfunc, val);
			escape_json(result, outputstr);
			pfree(outputstr);
			break;
	}
}

/*
 * Process a single dimension of an array.
 * If it's the innermost dimension, output the values, otherwise call
 * ourselves recursively to process the next dimension.
 */
static void
array_dim_to_json(StringInfo result, int dim, int ndims, int *dims, Datum *vals,
				  bool *nulls, int *valcount, TYPCATEGORY tcategory,
				  Oid typoutputfunc, bool use_line_feeds)
{
	int			i;
	const char *sep;

	Assert(dim < ndims);

	sep = use_line_feeds ? ",\n " : ",";

	appendStringInfoChar(result, '[');

	for (i = 1; i <= dims[dim]; i++)
	{
		if (i > 1)
			appendStringInfoString(result, sep);

		if (dim + 1 == ndims)
		{
			datum_to_json(vals[*valcount], nulls[*valcount], result, tcategory,
						  typoutputfunc);
			(*valcount)++;
		}
		else
		{
			/*
			 * Do we want line feeds on inner dimensions of arrays? For now
			 * we'll say no.
			 */
			array_dim_to_json(result, dim + 1, ndims, dims, vals, nulls,
							  valcount, tcategory, typoutputfunc, false);
		}
	}

	appendStringInfoChar(result, ']');
}

/*
 * Turn an array into JSON.
 */
static void
array_to_json_internal(Datum array, StringInfo result, bool use_line_feeds)
{
	ArrayType  *v = DatumGetArrayTypeP(array);
	Oid			element_type = ARR_ELEMTYPE(v);
	int		   *dim;
	int			ndim;
	int			nitems;
	int			count = 0;
	Datum	   *elements;
	bool	   *nulls;
	int16		typlen;
	bool		typbyval;
	char		typalign,
				typdelim;
	Oid			typioparam;
	Oid			typoutputfunc;
	TYPCATEGORY tcategory;

	ndim = ARR_NDIM(v);
	dim = ARR_DIMS(v);
	nitems = ArrayGetNItems(ndim, dim);

	if (nitems <= 0)
	{
		appendStringInfoString(result, "[]");
		return;
	}

	get_type_io_data(element_type, IOFunc_output,
					 &typlen, &typbyval, &typalign,
					 &typdelim, &typioparam, &typoutputfunc);

	deconstruct_array(v, element_type, typlen, typbyval,
					  typalign, &elements, &nulls,
					  &nitems);

	if (element_type == RECORDOID)
		tcategory = TYPCATEGORY_COMPOSITE;
	else if (element_type == JSONOID)
		tcategory = TYPCATEGORY_JSON;
	else
		tcategory = TypeCategory(element_type);

	array_dim_to_json(result, 0, ndim, dim, elements, nulls, &count, tcategory,
					  typoutputfunc, use_line_feeds);

	pfree(elements);
	pfree(nulls);
}

/*
 * Turn a composite / record into JSON.
 */
static void
composite_to_json(Datum composite, StringInfo result, bool use_line_feeds)
{
	HeapTupleHeader td;
	Oid			tupType;
	int32		tupTypmod;
	TupleDesc	tupdesc;
	HeapTupleData tmptup,
			   *tuple;
	int			i;
	bool		needsep = false;
	const char *sep;

	sep = use_line_feeds ? ",\n " : ",";

	td = DatumGetHeapTupleHeader(composite);

	/* Extract rowtype info and find a tupdesc */
	tupType = HeapTupleHeaderGetTypeId(td);
	tupTypmod = HeapTupleHeaderGetTypMod(td);
	tupdesc = lookup_rowtype_tupdesc(tupType, tupTypmod);

	/* Build a temporary HeapTuple control structure */
	tmptup.t_len = HeapTupleHeaderGetDatumLength(td);
	tmptup.t_data = td;
	tuple = &tmptup;

	appendStringInfoChar(result, '{');

	for (i = 0; i < tupdesc->natts; i++)
	{
		Datum		val,
					origval;
		bool		isnull;
		char	   *attname;
		TYPCATEGORY tcategory;
		Oid			typoutput;
		bool		typisvarlena;

		if (tupdesc->attrs[i]->attisdropped)
			continue;

		if (needsep)
			appendStringInfoString(result, sep);
		needsep = true;

		attname = NameStr(tupdesc->attrs[i]->attname);
		escape_json(result, attname);
		appendStringInfoChar(result, ':');

		origval = heap_getattr(tuple, i + 1, tupdesc, &isnull);

		if (tupdesc->attrs[i]->atttypid == RECORDARRAYOID)
			tcategory = TYPCATEGORY_ARRAY;
		else if (tupdesc->attrs[i]->atttypid == RECORDOID)
			tcategory = TYPCATEGORY_COMPOSITE;
		else if (tupdesc->attrs[i]->atttypid == JSONOID)
			tcategory = TYPCATEGORY_JSON;
		else
			tcategory = TypeCategory(tupdesc->attrs[i]->atttypid);

		getTypeOutputInfo(tupdesc->attrs[i]->atttypid,
						  &typoutput, &typisvarlena);

		/*
		 * If we have a toasted datum, forcibly detoast it here to avoid
		 * memory leakage inside the type's output routine.
		 */
		if (typisvarlena && !isnull)
			val = PointerGetDatum(PG_DETOAST_DATUM(origval));
		else
			val = origval;

		datum_to_json(val, isnull, result, tcategory, typoutput);

		/* Clean up detoasted copy, if any */
		if (val != origval)
			pfree(DatumGetPointer(val));
	}

	appendStringInfoChar(result, '}');
	ReleaseTupleDesc(tupdesc);
}

/*
 * SQL function array_to_json(row)
 */
extern Datum
array_to_json(PG_FUNCTION_ARGS)
{
	Datum		array = PG_GETARG_DATUM(0);
	StringInfo	result;

	result = makeStringInfo();

	array_to_json_internal(array, result, false);

	PG_RETURN_TEXT_P(cstring_to_text(result->data));
}

/*
 * SQL function array_to_json(row, prettybool)
 */
extern Datum
array_to_json_pretty(PG_FUNCTION_ARGS)
{
	Datum		array = PG_GETARG_DATUM(0);
	bool		use_line_feeds = PG_GETARG_BOOL(1);
	StringInfo	result;

	result = makeStringInfo();

	array_to_json_internal(array, result, use_line_feeds);

	PG_RETURN_TEXT_P(cstring_to_text(result->data));
}

/*
 * SQL function row_to_json(row)
 */
extern Datum
row_to_json(PG_FUNCTION_ARGS)
{
	Datum		array = PG_GETARG_DATUM(0);
	StringInfo	result;

	result = makeStringInfo();

	composite_to_json(array, result, false);

	PG_RETURN_TEXT_P(cstring_to_text(result->data));
}

/*
 * SQL function row_to_json(row, prettybool)
 */
extern Datum
row_to_json_pretty(PG_FUNCTION_ARGS)
{
	Datum		array = PG_GETARG_DATUM(0);
	bool		use_line_feeds = PG_GETARG_BOOL(1);
	StringInfo	result;

	result = makeStringInfo();

	composite_to_json(array, result, use_line_feeds);

	PG_RETURN_TEXT_P(cstring_to_text(result->data));
}

/*
 * Produce a JSON string literal, properly escaping characters in the text.
 */
void
escape_json(StringInfo buf, const char *str)
{
	const char *p;

	appendStringInfoCharMacro(buf, '\"');
	for (p = str; *p; p++)
	{
		switch (*p)
		{
			case '\b':
				appendStringInfoString(buf, "\\b");
				break;
			case '\f':
				appendStringInfoString(buf, "\\f");
				break;
			case '\n':
				appendStringInfoString(buf, "\\n");
				break;
			case '\r':
				appendStringInfoString(buf, "\\r");
				break;
			case '\t':
				appendStringInfoString(buf, "\\t");
				break;
			case '"':
				appendStringInfoString(buf, "\\\"");
				break;
			case '\\':
				appendStringInfoString(buf, "\\\\");
				break;
			default:
				if ((unsigned char) *p < ' ')
					appendStringInfo(buf, "\\u%04x", (int) *p);
				else
					appendStringInfoCharMacro(buf, *p);
				break;
		}
	}
	appendStringInfoCharMacro(buf, '\"');
}

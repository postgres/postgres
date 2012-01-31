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

#include "lib/stringinfo.h"
#include "libpq/pqformat.h"
#include "mb/pg_wchar.h"
#include "utils/builtins.h"
#include "utils/json.h"

typedef enum
{
	JSON_VALUE_INVALID,
	JSON_VALUE_STRING,
	JSON_VALUE_NUMBER,
	JSON_VALUE_OBJECT,
	JSON_VALUE_ARRAY,
	JSON_VALUE_TRUE,
	JSON_VALUE_FALSE,
	JSON_VALUE_NULL
} JsonValueType;

typedef struct
{
	char	   *input;
	char	   *token_start;
	char	   *token_terminator;
	JsonValueType	token_type;
	int			line_number;
	char	   *line_start;
} JsonLexContext;

typedef enum
{
	JSON_PARSE_VALUE,			/* expecting a value */
	JSON_PARSE_ARRAY_START,		/* saw '[', expecting value or ']' */
	JSON_PARSE_ARRAY_NEXT,		/* saw array element, expecting ',' or ']' */
	JSON_PARSE_OBJECT_START,	/* saw '{', expecting label or '}' */
	JSON_PARSE_OBJECT_LABEL,	/* saw object label, expecting ':' */
	JSON_PARSE_OBJECT_NEXT,		/* saw object value, expecting ',' or '}' */
	JSON_PARSE_OBJECT_COMMA		/* saw object ',', expecting next label */
} JsonParseState;

typedef struct JsonParseStack
{
	JsonParseState	state;
} JsonParseStack;

typedef enum
{
	JSON_STACKOP_NONE,
	JSON_STACKOP_PUSH,
	JSON_STACKOP_PUSH_WITH_PUSHBACK,
	JSON_STACKOP_POP
} JsonStackOp;

static void json_validate_cstring(char *input);
static void json_lex(JsonLexContext *lex);
static void json_lex_string(JsonLexContext *lex);
static void json_lex_number(JsonLexContext *lex, char *s);
static void report_parse_error(JsonParseStack *stack, JsonLexContext *lex);
static void report_invalid_token(JsonLexContext *lex);
static char *extract_mb_char(char *s);

extern Datum json_in(PG_FUNCTION_ARGS);

/*
 * Input.
 */
Datum
json_in(PG_FUNCTION_ARGS)
{
	char    *text = PG_GETARG_CSTRING(0);

	json_validate_cstring(text);

	PG_RETURN_TEXT_P(cstring_to_text(text));
}

/*
 * Output.
 */
Datum
json_out(PG_FUNCTION_ARGS)
{
	Datum	txt = PG_GETARG_DATUM(0);

	PG_RETURN_CSTRING(TextDatumGetCString(txt));
}

/*
 * Binary send.
 */
Datum
json_send(PG_FUNCTION_ARGS)
{
	StringInfoData buf;
	text   *t = PG_GETARG_TEXT_PP(0);

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
	JsonLexContext	lex;
	JsonParseStack *stack,
				   *stacktop;
	int				stacksize;

	/* Set up lexing context. */
	lex.input = input;
	lex.token_terminator = lex.input;
	lex.line_number = 1;
	lex.line_start = input;

	/* Set up parse stack. */
	stacksize = 32;
	stacktop = palloc(sizeof(JsonParseStack) * stacksize);
	stack = stacktop;
	stack->state = JSON_PARSE_VALUE;

	/* Main parsing loop. */
	for (;;)
	{
		JsonStackOp	op;

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
				else if (lex.token_start[0] == '['
					|| lex.token_start[0] == '{')
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
				else if (lex.token_type == JSON_VALUE_INVALID
					&& lex.token_start[0] == '}')
					op = JSON_STACKOP_POP;
				else
					report_parse_error(stack, &lex);
				break;
			case JSON_PARSE_OBJECT_LABEL:
				if (lex.token_type == JSON_VALUE_INVALID
					&& lex.token_start[0] == ':')
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

		/* Push or pop the stack, if needed. */
		switch (op)
		{
			case JSON_STACKOP_PUSH:
			case JSON_STACKOP_PUSH_WITH_PUSHBACK:
				++stack;
				if (stack >= &stacktop[stacksize])
				{
					int		stackoffset = stack - stacktop;
					stacksize = stacksize + 32;
					stacktop = repalloc(stacktop,
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
				--stack;
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
	{
		if (*s == '\n')
			++lex->line_number;
		++s;
	}
	lex->token_start = s;

	/* Determine token type. */
	if (strchr("{}[],:", s[0]))
	{
		/* strchr() doesn't return false on a NUL input. */
		if (s[0] == '\0')
		{
			/* End of string. */
			lex->token_start = NULL;
			lex->token_terminator = NULL;
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
		json_lex_number(lex, s + 1);
		lex->token_type = JSON_VALUE_NUMBER;
	}
	else if (*s >= '0' && *s <= '9')
	{
		/* Positive number. */
		json_lex_number(lex, s);
		lex->token_type = JSON_VALUE_NUMBER;
	}
	else
	{
		char   *p;

		/*
		 * We're not dealing with a string, number, legal punctuation mark,
		 * or end of string.  The only legal tokens we might find here are
		 * true, false, and null, but for error reporting purposes we scan
		 * until we see a non-alphanumeric character.  That way, we can report
		 * the whole word as an unexpected token, rather than just some
		 * unintuitive prefix thereof.
		 */
 		for (p = s; (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')
			|| (*p >= '0' && *p <= '9') || *p == '_' || IS_HIGHBIT_SET(*p);
			++p)
			;

		/*
		 * We got some sort of unexpected punctuation or an otherwise
		 * unexpected character, so just complain about that one character.
		 */
		if (p == s)
		{
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
	char	   *s = lex->token_start + 1;

	for (s = lex->token_start + 1; *s != '"'; ++s)
	{
		/* Per RFC4627, these characters MUST be escaped. */
		if (*s < 32)
		{
			/* A NUL byte marks the (premature) end of the string. */
			if (*s == '\0')
			{
				lex->token_terminator = s;
				report_invalid_token(lex);
			}
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for type json"),
					 errdetail_internal("line %d: Character \"%c\" must be escaped.",
						lex->line_number, *s)));
		}
		else if (*s == '\\')
		{
			/* OK, we have an escape character. */
			++s;
			if (*s == '\0')
			{
				lex->token_terminator = s;
				report_invalid_token(lex);
			}
			else if (*s == 'u')
			{
				int		i;
				int		ch = 0;

				for (i = 1; i <= 4; ++i)
				{
					if (s[i] == '\0')
					{
						lex->token_terminator = s + i;
						report_invalid_token(lex);
					}
					else if (s[i] >= '0' && s[i] <= '9')
						ch = (ch * 16) + (s[i] - '0');
					else if (s[i] >= 'a' && s[i] <= 'f')
						ch = (ch * 16) + (s[i] - 'a') + 10;
					else if (s[i] >= 'A' && s[i] <= 'F')
						ch = (ch * 16) + (s[i] - 'A') + 10;
					else
					{
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
								 errmsg("invalid input syntax for type json"),
								 errdetail_internal("line %d: \"\\u\" must be followed by four hexadecimal digits.",
									lex->line_number)));
					}
				}

				/* Account for the four additional bytes we just parsed. */
				s += 4;
			}
			else if (!strchr("\"\\/bfnrt", *s))
			{
				/* Error out. */
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("invalid input syntax for type json"),
						 errdetail_internal("line %d: Invalid escape \"\\%s\".",
							lex->line_number, extract_mb_char(s))));
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
 *     begin with a '0'.
 *
 * (3) An optional decimal part, consisting of a period ('.') followed by
 *     one or more digits.  (Note: While this part can be omitted
 *     completely, it's not OK to have only the decimal point without
 *     any digits afterwards.)
 *
 * (4) An optional exponent part, consisting of 'e' or 'E', optionally
 *     followed by '+' or '-', followed by one or more digits.  (Note:
 *     As with the decimal part, if 'e' or 'E' is present, it must be
 *     followed by at least one digit.)
 *
 * The 's' argument to this function points to the ostensible beginning
 * of part 2 - i.e. the character after any optional minus sign, and the
 * first character of the string if there is none.
 *
 *-------------------------------------------------------------------------
 */
static void
json_lex_number(JsonLexContext *lex, char *s)
{
	bool	error = false;
	char   *p;

	/* Part (1): leading sign indicator. */
	/* Caller already did this for us; so do nothing. */

	/* Part (2): parse main digit string. */
	if (*s == '0')
		++s;
	else if (*s >= '1' && *s <= '9')
	{
		do
		{
			++s;
		} while (*s >= '0' && *s <= '9');
	}
	else
		error = true;

	/* Part (3): parse optional decimal portion. */
	if (*s == '.')
	{
		++s;
		if (*s < '0' && *s > '9')
			error = true;
		else
		{
			do
			{
				++s;
			} while (*s >= '0' && *s <= '9');
		}
	}

	/* Part (4): parse optional exponent. */
	if (*s == 'e' || *s == 'E')
	{
		++s;
		if (*s == '+' || *s == '-')
			++s;
		if (*s < '0' && *s > '9')
			error = true;
		else
		{
			do
			{
				++s;
			} while (*s >= '0' && *s <= '9');
		}
	}

	/* Check for trailing garbage. */
	for (p = s; (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')
		|| (*p >= '0' && *p <= '9') || *p == '_' || IS_HIGHBIT_SET(*p); ++p)
		;
	lex->token_terminator = p;
	if (p > s || error)
		report_invalid_token(lex);
}

/*
 * Report a parse error.
 */
static void
report_parse_error(JsonParseStack *stack, JsonLexContext *lex)
{
	char   *detail = NULL;
	char   *token = NULL;
	int		toklen;

	/* Handle case where the input ended prematurely. */
	if (lex->token_start == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type json: \"%s\"",
					lex->input),
	 			 errdetail_internal("The input string ended unexpectedly.")));

	/* Work out the offending token. */
	toklen = lex->token_terminator - lex->token_start;
	token = palloc(toklen + 1);
	memcpy(token, lex->token_start, toklen);
	token[toklen] = '\0';

	/* Select correct detail message. */
	if (stack == NULL)
		detail = "line %d: Expected end of input, but found \"%s\".";
	else
	{
		switch (stack->state)
		{
			case JSON_PARSE_VALUE:
				detail = "line %d: Expected string, number, object, array, true, false, or null, but found \"%s\".";
				break;
			case JSON_PARSE_ARRAY_START:
				detail = "line %d: Expected array element or \"]\", but found \"%s\".";
				break;
			case JSON_PARSE_ARRAY_NEXT:
				detail = "line %d: Expected \",\" or \"]\", but found \"%s\".";
				break;
			case JSON_PARSE_OBJECT_START:
				detail = "line %d: Expected string or \"}\", but found \"%s\".";
				break;
			case JSON_PARSE_OBJECT_LABEL:
				detail = "line %d: Expected \":\", but found \"%s\".";
				break;
			case JSON_PARSE_OBJECT_NEXT:
				detail = "line %d: Expected \",\" or \"}\", but found \"%s\".";
				break;
			case JSON_PARSE_OBJECT_COMMA:
				detail = "line %d: Expected string, but found \"%s\".";
				break;
		}
	}

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
			 errmsg("invalid input syntax for type json: \"%s\"",
				lex->input),
 			 errdetail_internal(detail, lex->line_number, token)));
}

/*
 * Report an invalid input token.
 */
static void
report_invalid_token(JsonLexContext *lex)
{
	char   *token;
	int		toklen;

	toklen = lex->token_terminator - lex->token_start;
	token = palloc(toklen + 1);
	memcpy(token, lex->token_start, toklen);
	token[toklen] = '\0';

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
			 errmsg("invalid input syntax for type json"),
			 errdetail_internal("line %d: Token \"%s\" is invalid.",
				lex->line_number, token)));
}

/*
 * Extract a single, possibly multi-byte char from the input string.
 */
static char *
extract_mb_char(char *s)
{
	char   *res;
	int		len;

	len = pg_mblen(s);
	res = palloc(len + 1);
	memcpy(res, s, len);
	res[len] = '\0';

	return res;
}

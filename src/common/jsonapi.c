/*-------------------------------------------------------------------------
 *
 * jsonapi.c
 *		JSON parser and lexer interfaces
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
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
#include "port/pg_lfind.h"

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
	JSON_PARSE_END,				/* saw the end of a document, expect nothing */
} JsonParseContext;

/*
 * Setup for table-driven parser.
 * These enums need to be separate from the JsonTokenType and from each other
 * so we can have all of them on the prediction stack, which consists of
 * tokens, non-terminals, and semantic action markers.
 */

enum JsonNonTerminal
{
	JSON_NT_JSON = 32,
	JSON_NT_ARRAY_ELEMENTS,
	JSON_NT_MORE_ARRAY_ELEMENTS,
	JSON_NT_KEY_PAIRS,
	JSON_NT_MORE_KEY_PAIRS,
};

enum JsonParserSem
{
	JSON_SEM_OSTART = 64,
	JSON_SEM_OEND,
	JSON_SEM_ASTART,
	JSON_SEM_AEND,
	JSON_SEM_OFIELD_INIT,
	JSON_SEM_OFIELD_START,
	JSON_SEM_OFIELD_END,
	JSON_SEM_AELEM_START,
	JSON_SEM_AELEM_END,
	JSON_SEM_SCALAR_INIT,
	JSON_SEM_SCALAR_CALL,
};

/*
 * struct containing the 3 stacks used in non-recursive parsing,
 * and the token and value for scalars that need to be preserved
 * across calls.
 *
 * typedef appears in jsonapi.h
 */
struct JsonParserStack
{
	int			stack_size;
	char	   *prediction;
	size_t		pred_index;
	/* these two are indexed by lex_level */
	char	  **fnames;
	bool	   *fnull;
	JsonTokenType scalar_tok;
	char	   *scalar_val;
};

/*
 * struct containing state used when there is a possible partial token at the
 * end of a json chunk when we are doing incremental parsing.
 *
 * typedef appears in jsonapi.h
 */
struct JsonIncrementalState
{
	bool		is_last_chunk;
	bool		partial_completed;
	StringInfoData partial_token;
};

/*
 * constants and macros used in the nonrecursive parser
 */
#define JSON_NUM_TERMINALS 13
#define JSON_NUM_NONTERMINALS 5
#define JSON_NT_OFFSET JSON_NT_JSON
/* for indexing the table */
#define OFS(NT) (NT) - JSON_NT_OFFSET
/* classify items we get off the stack */
#define IS_SEM(x) ((x) & 0x40)
#define IS_NT(x)  ((x) & 0x20)

/*
 * These productions are stored in reverse order right to left so that when
 * they are pushed on the stack what we expect next is at the top of the stack.
 */
static char JSON_PROD_EPSILON[] = {0};	/* epsilon - an empty production */

/* JSON -> string */
static char JSON_PROD_SCALAR_STRING[] = {JSON_SEM_SCALAR_CALL, JSON_TOKEN_STRING, JSON_SEM_SCALAR_INIT, 0};

/* JSON -> number */
static char JSON_PROD_SCALAR_NUMBER[] = {JSON_SEM_SCALAR_CALL, JSON_TOKEN_NUMBER, JSON_SEM_SCALAR_INIT, 0};

/* JSON -> 'true' */
static char JSON_PROD_SCALAR_TRUE[] = {JSON_SEM_SCALAR_CALL, JSON_TOKEN_TRUE, JSON_SEM_SCALAR_INIT, 0};

/* JSON -> 'false' */
static char JSON_PROD_SCALAR_FALSE[] = {JSON_SEM_SCALAR_CALL, JSON_TOKEN_FALSE, JSON_SEM_SCALAR_INIT, 0};

/* JSON -> 'null' */
static char JSON_PROD_SCALAR_NULL[] = {JSON_SEM_SCALAR_CALL, JSON_TOKEN_NULL, JSON_SEM_SCALAR_INIT, 0};

/* JSON -> '{' KEY_PAIRS '}' */
static char JSON_PROD_OBJECT[] = {JSON_SEM_OEND, JSON_TOKEN_OBJECT_END, JSON_NT_KEY_PAIRS, JSON_TOKEN_OBJECT_START, JSON_SEM_OSTART, 0};

/* JSON -> '[' ARRAY_ELEMENTS ']' */
static char JSON_PROD_ARRAY[] = {JSON_SEM_AEND, JSON_TOKEN_ARRAY_END, JSON_NT_ARRAY_ELEMENTS, JSON_TOKEN_ARRAY_START, JSON_SEM_ASTART, 0};

/* ARRAY_ELEMENTS -> JSON MORE_ARRAY_ELEMENTS */
static char JSON_PROD_ARRAY_ELEMENTS[] = {JSON_NT_MORE_ARRAY_ELEMENTS, JSON_SEM_AELEM_END, JSON_NT_JSON, JSON_SEM_AELEM_START, 0};

/* MORE_ARRAY_ELEMENTS -> ',' JSON MORE_ARRAY_ELEMENTS */
static char JSON_PROD_MORE_ARRAY_ELEMENTS[] = {JSON_NT_MORE_ARRAY_ELEMENTS, JSON_SEM_AELEM_END, JSON_NT_JSON, JSON_SEM_AELEM_START, JSON_TOKEN_COMMA, 0};

/* KEY_PAIRS -> string ':' JSON MORE_KEY_PAIRS */
static char JSON_PROD_KEY_PAIRS[] = {JSON_NT_MORE_KEY_PAIRS, JSON_SEM_OFIELD_END, JSON_NT_JSON, JSON_SEM_OFIELD_START, JSON_TOKEN_COLON, JSON_TOKEN_STRING, JSON_SEM_OFIELD_INIT, 0};

/* MORE_KEY_PAIRS -> ',' string ':'  JSON MORE_KEY_PAIRS */
static char JSON_PROD_MORE_KEY_PAIRS[] = {JSON_NT_MORE_KEY_PAIRS, JSON_SEM_OFIELD_END, JSON_NT_JSON, JSON_SEM_OFIELD_START, JSON_TOKEN_COLON, JSON_TOKEN_STRING, JSON_SEM_OFIELD_INIT, JSON_TOKEN_COMMA, 0};

/*
 * Note: there are also epsilon productions for ARRAY_ELEMENTS,
 * MORE_ARRAY_ELEMENTS, KEY_PAIRS and MORE_KEY_PAIRS
 * They are all the same as none require any semantic actions.
 */

/*
 * Table connecting the productions with their director sets of
 * terminal symbols.
 * Any combination not specified here represents an error.
 */

typedef struct
{
	size_t		len;
	char	   *prod;
} td_entry;

#define TD_ENTRY(PROD) { sizeof(PROD) - 1, (PROD) }

static td_entry td_parser_table[JSON_NUM_NONTERMINALS][JSON_NUM_TERMINALS] =
{
	/* JSON */
	[OFS(JSON_NT_JSON)][JSON_TOKEN_STRING] = TD_ENTRY(JSON_PROD_SCALAR_STRING),
	[OFS(JSON_NT_JSON)][JSON_TOKEN_NUMBER] = TD_ENTRY(JSON_PROD_SCALAR_NUMBER),
	[OFS(JSON_NT_JSON)][JSON_TOKEN_TRUE] = TD_ENTRY(JSON_PROD_SCALAR_TRUE),
	[OFS(JSON_NT_JSON)][JSON_TOKEN_FALSE] = TD_ENTRY(JSON_PROD_SCALAR_FALSE),
	[OFS(JSON_NT_JSON)][JSON_TOKEN_NULL] = TD_ENTRY(JSON_PROD_SCALAR_NULL),
	[OFS(JSON_NT_JSON)][JSON_TOKEN_ARRAY_START] = TD_ENTRY(JSON_PROD_ARRAY),
	[OFS(JSON_NT_JSON)][JSON_TOKEN_OBJECT_START] = TD_ENTRY(JSON_PROD_OBJECT),
	/* ARRAY_ELEMENTS */
	[OFS(JSON_NT_ARRAY_ELEMENTS)][JSON_TOKEN_ARRAY_START] = TD_ENTRY(JSON_PROD_ARRAY_ELEMENTS),
	[OFS(JSON_NT_ARRAY_ELEMENTS)][JSON_TOKEN_OBJECT_START] = TD_ENTRY(JSON_PROD_ARRAY_ELEMENTS),
	[OFS(JSON_NT_ARRAY_ELEMENTS)][JSON_TOKEN_STRING] = TD_ENTRY(JSON_PROD_ARRAY_ELEMENTS),
	[OFS(JSON_NT_ARRAY_ELEMENTS)][JSON_TOKEN_NUMBER] = TD_ENTRY(JSON_PROD_ARRAY_ELEMENTS),
	[OFS(JSON_NT_ARRAY_ELEMENTS)][JSON_TOKEN_TRUE] = TD_ENTRY(JSON_PROD_ARRAY_ELEMENTS),
	[OFS(JSON_NT_ARRAY_ELEMENTS)][JSON_TOKEN_FALSE] = TD_ENTRY(JSON_PROD_ARRAY_ELEMENTS),
	[OFS(JSON_NT_ARRAY_ELEMENTS)][JSON_TOKEN_NULL] = TD_ENTRY(JSON_PROD_ARRAY_ELEMENTS),
	[OFS(JSON_NT_ARRAY_ELEMENTS)][JSON_TOKEN_ARRAY_END] = TD_ENTRY(JSON_PROD_EPSILON),
	/* MORE_ARRAY_ELEMENTS */
	[OFS(JSON_NT_MORE_ARRAY_ELEMENTS)][JSON_TOKEN_COMMA] = TD_ENTRY(JSON_PROD_MORE_ARRAY_ELEMENTS),
	[OFS(JSON_NT_MORE_ARRAY_ELEMENTS)][JSON_TOKEN_ARRAY_END] = TD_ENTRY(JSON_PROD_EPSILON),
	/* KEY_PAIRS */
	[OFS(JSON_NT_KEY_PAIRS)][JSON_TOKEN_STRING] = TD_ENTRY(JSON_PROD_KEY_PAIRS),
	[OFS(JSON_NT_KEY_PAIRS)][JSON_TOKEN_OBJECT_END] = TD_ENTRY(JSON_PROD_EPSILON),
	/* MORE_KEY_PAIRS */
	[OFS(JSON_NT_MORE_KEY_PAIRS)][JSON_TOKEN_COMMA] = TD_ENTRY(JSON_PROD_MORE_KEY_PAIRS),
	[OFS(JSON_NT_MORE_KEY_PAIRS)][JSON_TOKEN_OBJECT_END] = TD_ENTRY(JSON_PROD_EPSILON),
};

/* the GOAL production. Not stored in the table, but will be the initial contents of the prediction stack */
static char JSON_PROD_GOAL[] = {JSON_TOKEN_END, JSON_NT_JSON, 0};

static inline JsonParseErrorType json_lex_string(JsonLexContext *lex);
static inline JsonParseErrorType json_lex_number(JsonLexContext *lex, const char *s,
												 bool *num_err, size_t *total_len);
static inline JsonParseErrorType parse_scalar(JsonLexContext *lex, const JsonSemAction *sem);
static JsonParseErrorType parse_object_field(JsonLexContext *lex, const JsonSemAction *sem);
static JsonParseErrorType parse_object(JsonLexContext *lex, const JsonSemAction *sem);
static JsonParseErrorType parse_array_element(JsonLexContext *lex, const JsonSemAction *sem);
static JsonParseErrorType parse_array(JsonLexContext *lex, const JsonSemAction *sem);
static JsonParseErrorType report_parse_error(JsonParseContext ctx, JsonLexContext *lex);

/* the null action object used for pure validation */
const JsonSemAction nullSemAction =
{
	NULL, NULL, NULL, NULL, NULL,
	NULL, NULL, NULL, NULL, NULL
};

/* Parser support routines */

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
IsValidJsonNumber(const char *str, size_t len)
{
	bool		numeric_error;
	size_t		total_len;
	JsonLexContext dummy_lex;

	if (len <= 0)
		return false;

	dummy_lex.incremental = false;
	dummy_lex.inc_state = NULL;
	dummy_lex.pstack = NULL;

	/*
	 * json_lex_number expects a leading  '-' to have been eaten already.
	 *
	 * having to cast away the constness of str is ugly, but there's not much
	 * easy alternative.
	 */
	if (*str == '-')
	{
		dummy_lex.input = str + 1;
		dummy_lex.input_length = len - 1;
	}
	else
	{
		dummy_lex.input = str;
		dummy_lex.input_length = len;
	}

	dummy_lex.token_start = dummy_lex.input;

	json_lex_number(&dummy_lex, dummy_lex.input, &numeric_error, &total_len);

	return (!numeric_error) && (total_len == dummy_lex.input_length);
}

/*
 * makeJsonLexContextCstringLen
 *		Initialize the given JsonLexContext object, or create one
 *
 * If a valid 'lex' pointer is given, it is initialized.  This can
 * be used for stack-allocated structs, saving overhead.  If NULL is
 * given, a new struct is allocated.
 *
 * If need_escapes is true, ->strval stores the unescaped lexemes.
 * Unescaping is expensive, so only request it when necessary.
 *
 * If need_escapes is true or lex was given as NULL, then caller is
 * responsible for freeing the returned struct, either by calling
 * freeJsonLexContext() or (in backend environment) via memory context
 * cleanup.
 */
JsonLexContext *
makeJsonLexContextCstringLen(JsonLexContext *lex, const char *json,
							 size_t len, int encoding, bool need_escapes)
{
	if (lex == NULL)
	{
		lex = palloc0(sizeof(JsonLexContext));
		lex->flags |= JSONLEX_FREE_STRUCT;
	}
	else
		memset(lex, 0, sizeof(JsonLexContext));

	lex->errormsg = NULL;
	lex->input = lex->token_terminator = lex->line_start = json;
	lex->line_number = 1;
	lex->input_length = len;
	lex->input_encoding = encoding;
	if (need_escapes)
	{
		lex->strval = makeStringInfo();
		lex->flags |= JSONLEX_FREE_STRVAL;
	}

	return lex;
}


/*
 * makeJsonLexContextIncremental
 *
 * Similar to above but set up for use in incremental parsing. That means we
 * need explicit stacks for predictions, field names and null indicators, but
 * we don't need the input, that will be handed in bit by bit to the
 * parse routine. We also need an accumulator for partial tokens in case
 * the boundary between chunks happens to fall in the middle of a token.
 */
#define JS_STACK_CHUNK_SIZE 64
#define JS_MAX_PROD_LEN 10		/* more than we need */
#define JSON_TD_MAX_STACK 6400	/* hard coded for now - this is a REALLY high
								 * number */

JsonLexContext *
makeJsonLexContextIncremental(JsonLexContext *lex, int encoding,
							  bool need_escapes)
{
	if (lex == NULL)
	{
		lex = palloc0(sizeof(JsonLexContext));
		lex->flags |= JSONLEX_FREE_STRUCT;
	}
	else
		memset(lex, 0, sizeof(JsonLexContext));

	lex->line_number = 1;
	lex->input_encoding = encoding;
	lex->incremental = true;
	lex->inc_state = palloc0(sizeof(JsonIncrementalState));
	initStringInfo(&(lex->inc_state->partial_token));
	lex->pstack = palloc(sizeof(JsonParserStack));
	lex->pstack->stack_size = JS_STACK_CHUNK_SIZE;
	lex->pstack->prediction = palloc(JS_STACK_CHUNK_SIZE * JS_MAX_PROD_LEN);
	lex->pstack->pred_index = 0;
	lex->pstack->fnames = palloc(JS_STACK_CHUNK_SIZE * sizeof(char *));
	lex->pstack->fnull = palloc(JS_STACK_CHUNK_SIZE * sizeof(bool));
	if (need_escapes)
	{
		lex->strval = makeStringInfo();
		lex->flags |= JSONLEX_FREE_STRVAL;
	}
	return lex;
}

static inline void
inc_lex_level(JsonLexContext *lex)
{
	lex->lex_level += 1;

	if (lex->incremental && lex->lex_level >= lex->pstack->stack_size)
	{
		lex->pstack->stack_size += JS_STACK_CHUNK_SIZE;
		lex->pstack->prediction =
			repalloc(lex->pstack->prediction,
					 lex->pstack->stack_size * JS_MAX_PROD_LEN);
		if (lex->pstack->fnames)
			lex->pstack->fnames =
				repalloc(lex->pstack->fnames,
						 lex->pstack->stack_size * sizeof(char *));
		if (lex->pstack->fnull)
			lex->pstack->fnull =
				repalloc(lex->pstack->fnull, lex->pstack->stack_size * sizeof(bool));
	}
}

static inline void
dec_lex_level(JsonLexContext *lex)
{
	lex->lex_level -= 1;
}

static inline void
push_prediction(JsonParserStack *pstack, td_entry entry)
{
	memcpy(pstack->prediction + pstack->pred_index, entry.prod, entry.len);
	pstack->pred_index += entry.len;
}

static inline char
pop_prediction(JsonParserStack *pstack)
{
	Assert(pstack->pred_index > 0);
	return pstack->prediction[--pstack->pred_index];
}

static inline char
next_prediction(JsonParserStack *pstack)
{
	Assert(pstack->pred_index > 0);
	return pstack->prediction[pstack->pred_index - 1];
}

static inline bool
have_prediction(JsonParserStack *pstack)
{
	return pstack->pred_index > 0;
}

static inline void
set_fname(JsonLexContext *lex, char *fname)
{
	lex->pstack->fnames[lex->lex_level] = fname;
}

static inline char *
get_fname(JsonLexContext *lex)
{
	return lex->pstack->fnames[lex->lex_level];
}

static inline void
set_fnull(JsonLexContext *lex, bool fnull)
{
	lex->pstack->fnull[lex->lex_level] = fnull;
}

static inline bool
get_fnull(JsonLexContext *lex)
{
	return lex->pstack->fnull[lex->lex_level];
}

/*
 * Free memory in a JsonLexContext.
 *
 * There's no need for this if a *lex pointer was given when the object was
 * made, need_escapes was false, and json_errdetail() was not called; or if (in
 * backend environment) a memory context delete/reset is imminent.
 */
void
freeJsonLexContext(JsonLexContext *lex)
{
	if (lex->flags & JSONLEX_FREE_STRVAL)
		destroyStringInfo(lex->strval);

	if (lex->errormsg)
		destroyStringInfo(lex->errormsg);

	if (lex->incremental)
	{
		pfree(lex->inc_state->partial_token.data);
		pfree(lex->inc_state);
		pfree(lex->pstack->prediction);
		pfree(lex->pstack->fnames);
		pfree(lex->pstack->fnull);
		pfree(lex->pstack);
	}

	if (lex->flags & JSONLEX_FREE_STRUCT)
		pfree(lex);
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
 *
 * If FORCE_JSON_PSTACK is defined then the routine will call the non-recursive
 * JSON parser. This is a useful way to validate that it's doing the right
 * thing at least for non-incremental cases. If this is on we expect to see
 * regression diffs relating to error messages about stack depth, but no
 * other differences.
 */
JsonParseErrorType
pg_parse_json(JsonLexContext *lex, const JsonSemAction *sem)
{
#ifdef FORCE_JSON_PSTACK

	lex->incremental = true;
	lex->inc_state = palloc0(sizeof(JsonIncrementalState));

	/*
	 * We don't need partial token processing, there is only one chunk. But we
	 * still need to init the partial token string so that freeJsonLexContext
	 * works.
	 */
	initStringInfo(&(lex->inc_state->partial_token));
	lex->pstack = palloc(sizeof(JsonParserStack));
	lex->pstack->stack_size = JS_STACK_CHUNK_SIZE;
	lex->pstack->prediction = palloc(JS_STACK_CHUNK_SIZE * JS_MAX_PROD_LEN);
	lex->pstack->pred_index = 0;
	lex->pstack->fnames = palloc(JS_STACK_CHUNK_SIZE * sizeof(char *));
	lex->pstack->fnull = palloc(JS_STACK_CHUNK_SIZE * sizeof(bool));

	return pg_parse_json_incremental(lex, sem, lex->input, lex->input_length, true);

#else

	JsonTokenType tok;
	JsonParseErrorType result;

	if (lex->incremental)
		return JSON_INVALID_LEXER_TYPE;

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
#endif
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
 * pg_parse_json_incremental
 *
 * Routine for incremental parsing of json. This uses the non-recursive top
 * down method of the Dragon Book Algorithm 4.3. It's somewhat slower than
 * the Recursive Descent pattern used above, so we only use it for incremental
 * parsing of JSON.
 *
 * The lexing context needs to be set up by a call to
 * makeJsonLexContextIncremental(). sem is a structure of function pointers
 * to semantic action routines, which should function exactly as those used
 * in the recursive descent parser.
 *
 * This routine can be called repeatedly with chunks of JSON. On the final
 * chunk is_last must be set to true. len is the length of the json chunk,
 * which does not need to be null terminated.
 */
JsonParseErrorType
pg_parse_json_incremental(JsonLexContext *lex,
						  const JsonSemAction *sem,
						  const char *json,
						  size_t len,
						  bool is_last)
{
	JsonTokenType tok;
	JsonParseErrorType result;
	JsonParseContext ctx = JSON_PARSE_VALUE;
	JsonParserStack *pstack = lex->pstack;


	if (!lex->incremental)
		return JSON_INVALID_LEXER_TYPE;

	lex->input = lex->token_terminator = lex->line_start = json;
	lex->input_length = len;
	lex->inc_state->is_last_chunk = is_last;

	/* get the initial token */
	result = json_lex(lex);
	if (result != JSON_SUCCESS)
		return result;

	tok = lex_peek(lex);

	/* use prediction stack for incremental parsing */

	if (!have_prediction(pstack))
	{
		td_entry	goal = TD_ENTRY(JSON_PROD_GOAL);

		push_prediction(pstack, goal);
	}

	while (have_prediction(pstack))
	{
		char		top = pop_prediction(pstack);
		td_entry	entry;

		/*
		 * these first two branches are the guts of the Table Driven method
		 */
		if (top == tok)
		{
			/*
			 * tok can only be a terminal symbol, so top must be too. the
			 * token matches the top of the stack, so get the next token.
			 */
			if (tok < JSON_TOKEN_END)
			{
				result = json_lex(lex);
				if (result != JSON_SUCCESS)
					return result;
				tok = lex_peek(lex);
			}
		}
		else if (IS_NT(top) && (entry = td_parser_table[OFS(top)][tok]).prod != NULL)
		{
			/*
			 * the token is in the director set for a production of the
			 * non-terminal at the top of the stack, so push the reversed RHS
			 * of the production onto the stack.
			 */
			push_prediction(pstack, entry);
		}
		else if (IS_SEM(top))
		{
			/*
			 * top is a semantic action marker, so take action accordingly.
			 * It's important to have these markers in the prediction stack
			 * before any token they might need so we don't advance the token
			 * prematurely. Note in a couple of cases we need to do something
			 * both before and after the token.
			 */
			switch (top)
			{
				case JSON_SEM_OSTART:
					{
						json_struct_action ostart = sem->object_start;

						if (lex->lex_level >= JSON_TD_MAX_STACK)
							return JSON_NESTING_TOO_DEEP;

						if (ostart != NULL)
						{
							result = (*ostart) (sem->semstate);
							if (result != JSON_SUCCESS)
								return result;
						}
						inc_lex_level(lex);
					}
					break;
				case JSON_SEM_OEND:
					{
						json_struct_action oend = sem->object_end;

						dec_lex_level(lex);
						if (oend != NULL)
						{
							result = (*oend) (sem->semstate);
							if (result != JSON_SUCCESS)
								return result;
						}
					}
					break;
				case JSON_SEM_ASTART:
					{
						json_struct_action astart = sem->array_start;

						if (lex->lex_level >= JSON_TD_MAX_STACK)
							return JSON_NESTING_TOO_DEEP;

						if (astart != NULL)
						{
							result = (*astart) (sem->semstate);
							if (result != JSON_SUCCESS)
								return result;
						}
						inc_lex_level(lex);
					}
					break;
				case JSON_SEM_AEND:
					{
						json_struct_action aend = sem->array_end;

						dec_lex_level(lex);
						if (aend != NULL)
						{
							result = (*aend) (sem->semstate);
							if (result != JSON_SUCCESS)
								return result;
						}
					}
					break;
				case JSON_SEM_OFIELD_INIT:
					{
						/*
						 * all we do here is save out the field name. We have
						 * to wait to get past the ':' to see if the next
						 * value is null so we can call the semantic routine
						 */
						char	   *fname = NULL;
						json_ofield_action ostart = sem->object_field_start;
						json_ofield_action oend = sem->object_field_end;

						if ((ostart != NULL || oend != NULL) && lex->strval != NULL)
						{
							fname = pstrdup(lex->strval->data);
						}
						set_fname(lex, fname);
					}
					break;
				case JSON_SEM_OFIELD_START:
					{
						/*
						 * the current token should be the first token of the
						 * value
						 */
						bool		isnull = tok == JSON_TOKEN_NULL;
						json_ofield_action ostart = sem->object_field_start;

						set_fnull(lex, isnull);

						if (ostart != NULL)
						{
							char	   *fname = get_fname(lex);

							result = (*ostart) (sem->semstate, fname, isnull);
							if (result != JSON_SUCCESS)
								return result;
						}
					}
					break;
				case JSON_SEM_OFIELD_END:
					{
						json_ofield_action oend = sem->object_field_end;

						if (oend != NULL)
						{
							char	   *fname = get_fname(lex);
							bool		isnull = get_fnull(lex);

							result = (*oend) (sem->semstate, fname, isnull);
							if (result != JSON_SUCCESS)
								return result;
						}
					}
					break;
				case JSON_SEM_AELEM_START:
					{
						json_aelem_action astart = sem->array_element_start;
						bool		isnull = tok == JSON_TOKEN_NULL;

						set_fnull(lex, isnull);

						if (astart != NULL)
						{
							result = (*astart) (sem->semstate, isnull);
							if (result != JSON_SUCCESS)
								return result;
						}
					}
					break;
				case JSON_SEM_AELEM_END:
					{
						json_aelem_action aend = sem->array_element_end;

						if (aend != NULL)
						{
							bool		isnull = get_fnull(lex);

							result = (*aend) (sem->semstate, isnull);
							if (result != JSON_SUCCESS)
								return result;
						}
					}
					break;
				case JSON_SEM_SCALAR_INIT:
					{
						json_scalar_action sfunc = sem->scalar;

						pstack->scalar_val = NULL;

						if (sfunc != NULL)
						{
							/*
							 * extract the de-escaped string value, or the raw
							 * lexeme
							 */
							/*
							 * XXX copied from RD parser but looks like a
							 * buglet
							 */
							if (tok == JSON_TOKEN_STRING)
							{
								if (lex->strval != NULL)
									pstack->scalar_val = pstrdup(lex->strval->data);
							}
							else
							{
								ptrdiff_t	tlen = (lex->token_terminator - lex->token_start);

								pstack->scalar_val = palloc(tlen + 1);
								memcpy(pstack->scalar_val, lex->token_start, tlen);
								pstack->scalar_val[tlen] = '\0';
							}
							pstack->scalar_tok = tok;
						}
					}
					break;
				case JSON_SEM_SCALAR_CALL:
					{
						/*
						 * We'd like to be able to get rid of this business of
						 * two bits of scalar action, but we can't. It breaks
						 * certain semantic actions which expect that when
						 * called the lexer has consumed the item. See for
						 * example get_scalar() in jsonfuncs.c.
						 */
						json_scalar_action sfunc = sem->scalar;

						if (sfunc != NULL)
						{
							result = (*sfunc) (sem->semstate, pstack->scalar_val, pstack->scalar_tok);
							if (result != JSON_SUCCESS)
								return result;
						}
					}
					break;
				default:
					/* should not happen */
					break;
			}
		}
		else
		{
			/*
			 * The token didn't match the stack top if it's a terminal nor a
			 * production for the stack top if it's a non-terminal.
			 *
			 * Various cases here are Asserted to be not possible, as the
			 * token would not appear at the top of the prediction stack
			 * unless the lookahead matched.
			 */
			switch (top)
			{
				case JSON_TOKEN_STRING:
					if (next_prediction(pstack) == JSON_TOKEN_COLON)
						ctx = JSON_PARSE_STRING;
					else
					{
						Assert(false);
						ctx = JSON_PARSE_VALUE;
					}
					break;
				case JSON_TOKEN_NUMBER:
				case JSON_TOKEN_TRUE:
				case JSON_TOKEN_FALSE:
				case JSON_TOKEN_NULL:
				case JSON_TOKEN_ARRAY_START:
				case JSON_TOKEN_OBJECT_START:
					Assert(false);
					ctx = JSON_PARSE_VALUE;
					break;
				case JSON_TOKEN_ARRAY_END:
					Assert(false);
					ctx = JSON_PARSE_ARRAY_NEXT;
					break;
				case JSON_TOKEN_OBJECT_END:
					Assert(false);
					ctx = JSON_PARSE_OBJECT_NEXT;
					break;
				case JSON_TOKEN_COMMA:
					Assert(false);
					if (next_prediction(pstack) == JSON_TOKEN_STRING)
						ctx = JSON_PARSE_OBJECT_NEXT;
					else
						ctx = JSON_PARSE_ARRAY_NEXT;
					break;
				case JSON_TOKEN_COLON:
					ctx = JSON_PARSE_OBJECT_LABEL;
					break;
				case JSON_TOKEN_END:
					ctx = JSON_PARSE_END;
					break;
				case JSON_NT_MORE_ARRAY_ELEMENTS:
					ctx = JSON_PARSE_ARRAY_NEXT;
					break;
				case JSON_NT_ARRAY_ELEMENTS:
					ctx = JSON_PARSE_ARRAY_START;
					break;
				case JSON_NT_MORE_KEY_PAIRS:
					ctx = JSON_PARSE_OBJECT_NEXT;
					break;
				case JSON_NT_KEY_PAIRS:
					ctx = JSON_PARSE_OBJECT_START;
					break;
				default:
					ctx = JSON_PARSE_VALUE;
			}
			return report_parse_error(ctx, lex);
		}
	}

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
parse_scalar(JsonLexContext *lex, const JsonSemAction *sem)
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
	result = (*sfunc) (sem->semstate, val, tok);

	return result;
}

static JsonParseErrorType
parse_object_field(JsonLexContext *lex, const JsonSemAction *sem)
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
	{
		result = (*ostart) (sem->semstate, fname, isnull);
		if (result != JSON_SUCCESS)
			return result;
	}

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
	{
		result = (*oend) (sem->semstate, fname, isnull);
		if (result != JSON_SUCCESS)
			return result;
	}

	return JSON_SUCCESS;
}

static JsonParseErrorType
parse_object(JsonLexContext *lex, const JsonSemAction *sem)
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
	{
		result = (*ostart) (sem->semstate);
		if (result != JSON_SUCCESS)
			return result;
	}

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
	{
		result = (*oend) (sem->semstate);
		if (result != JSON_SUCCESS)
			return result;
	}

	return JSON_SUCCESS;
}

static JsonParseErrorType
parse_array_element(JsonLexContext *lex, const JsonSemAction *sem)
{
	json_aelem_action astart = sem->array_element_start;
	json_aelem_action aend = sem->array_element_end;
	JsonTokenType tok = lex_peek(lex);
	JsonParseErrorType result;
	bool		isnull;

	isnull = tok == JSON_TOKEN_NULL;

	if (astart != NULL)
	{
		result = (*astart) (sem->semstate, isnull);
		if (result != JSON_SUCCESS)
			return result;
	}

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
	{
		result = (*aend) (sem->semstate, isnull);
		if (result != JSON_SUCCESS)
			return result;
	}

	return JSON_SUCCESS;
}

static JsonParseErrorType
parse_array(JsonLexContext *lex, const JsonSemAction *sem)
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
	{
		result = (*astart) (sem->semstate);
		if (result != JSON_SUCCESS)
			return result;
	}

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
	{
		result = (*aend) (sem->semstate);
		if (result != JSON_SUCCESS)
			return result;
	}

	return JSON_SUCCESS;
}

/*
 * Lex one token from the input stream.
 *
 * When doing incremental parsing, we can reach the end of the input string
 * without having (or knowing we have) a complete token. If it's not the
 * final chunk of input, the partial token is then saved to the lex
 * structure's ptok StringInfo. On subsequent calls input is appended to this
 * buffer until we have something that we think is a complete token,
 * which is then lexed using a recursive call to json_lex. Processing then
 * continues as normal on subsequent calls.
 *
 * Note than when doing incremental processing, the lex.prev_token_terminator
 * should not be relied on. It could point into a previous input chunk or
 * worse.
 */
JsonParseErrorType
json_lex(JsonLexContext *lex)
{
	const char *s;
	const char *const end = lex->input + lex->input_length;
	JsonParseErrorType result;

	if (lex->incremental && lex->inc_state->partial_completed)
	{
		/*
		 * We just lexed a completed partial token on the last call, so reset
		 * everything
		 */
		resetStringInfo(&(lex->inc_state->partial_token));
		lex->token_terminator = lex->input;
		lex->inc_state->partial_completed = false;
	}

	s = lex->token_terminator;

	if (lex->incremental && lex->inc_state->partial_token.len)
	{
		/*
		 * We have a partial token. Extend it and if completed lex it by a
		 * recursive call
		 */
		StringInfo	ptok = &(lex->inc_state->partial_token);
		size_t		added = 0;
		bool		tok_done = false;
		JsonLexContext dummy_lex;
		JsonParseErrorType partial_result;

		if (ptok->data[0] == '"')
		{
			/*
			 * It's a string. Accumulate characters until we reach an
			 * unescaped '"'.
			 */
			int			escapes = 0;

			for (int i = ptok->len - 1; i > 0; i--)
			{
				/* count the trailing backslashes on the partial token */
				if (ptok->data[i] == '\\')
					escapes++;
				else
					break;
			}

			for (size_t i = 0; i < lex->input_length; i++)
			{
				char		c = lex->input[i];

				appendStringInfoCharMacro(ptok, c);
				added++;
				if (c == '"' && escapes % 2 == 0)
				{
					tok_done = true;
					break;
				}
				if (c == '\\')
					escapes++;
				else
					escapes = 0;
			}
		}
		else
		{
			/* not a string */
			char		c = ptok->data[0];

			if (c == '-' || (c >= '0' && c <= '9'))
			{
				/* for numbers look for possible numeric continuations */

				bool		numend = false;

				for (size_t i = 0; i < lex->input_length && !numend; i++)
				{
					char		cc = lex->input[i];

					switch (cc)
					{
						case '+':
						case '-':
						case 'e':
						case 'E':
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
							{
								appendStringInfoCharMacro(ptok, cc);
								added++;
							}
							break;
						default:
							numend = true;
					}
				}
			}

			/*
			 * Add any remaining alphanumeric chars. This takes care of the
			 * {null, false, true} literals as well as any trailing
			 * alphanumeric junk on non-string tokens.
			 */
			for (size_t i = added; i < lex->input_length; i++)
			{
				char		cc = lex->input[i];

				if (JSON_ALPHANUMERIC_CHAR(cc))
				{
					appendStringInfoCharMacro(ptok, cc);
					added++;
				}
				else
				{
					tok_done = true;
					break;
				}
			}
			if (added == lex->input_length &&
				lex->inc_state->is_last_chunk)
			{
				tok_done = true;
			}
		}

		if (!tok_done)
		{
			/* We should have consumed the whole chunk in this case. */
			Assert(added == lex->input_length);

			if (!lex->inc_state->is_last_chunk)
				return JSON_INCOMPLETE;

			/* json_errdetail() needs access to the accumulated token. */
			lex->token_start = ptok->data;
			lex->token_terminator = ptok->data + ptok->len;
			return JSON_INVALID_TOKEN;
		}

		/*
		 * Everything up to lex->input[added] has been added to the partial
		 * token, so move the input past it.
		 */
		lex->input += added;
		lex->input_length -= added;

		dummy_lex.input = dummy_lex.token_terminator =
			dummy_lex.line_start = ptok->data;
		dummy_lex.line_number = lex->line_number;
		dummy_lex.input_length = ptok->len;
		dummy_lex.input_encoding = lex->input_encoding;
		dummy_lex.incremental = false;
		dummy_lex.strval = lex->strval;

		partial_result = json_lex(&dummy_lex);

		/*
		 * We either have a complete token or an error. In either case we need
		 * to point to the partial token data for the semantic or error
		 * routines. If it's not an error we'll readjust on the next call to
		 * json_lex.
		 */
		lex->token_type = dummy_lex.token_type;
		lex->line_number = dummy_lex.line_number;

		/*
		 * We know the prev_token_terminator must be back in some previous
		 * piece of input, so we just make it NULL.
		 */
		lex->prev_token_terminator = NULL;

		/*
		 * Normally token_start would be ptok->data, but it could be later,
		 * see json_lex_string's handling of invalid escapes.
		 */
		lex->token_start = dummy_lex.token_start;
		lex->token_terminator = dummy_lex.token_terminator;
		if (partial_result == JSON_SUCCESS)
		{
			/* make sure we've used all the input */
			if (lex->token_terminator - lex->token_start != ptok->len)
			{
				Assert(false);
				return JSON_INVALID_TOKEN;
			}

			lex->inc_state->partial_completed = true;
		}
		return partial_result;
		/* end of partial token processing */
	}

	/* Skip leading whitespace. */
	while (s < end && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r'))
	{
		if (*s++ == '\n')
		{
			++lex->line_number;
			lex->line_start = s;
		}
	}
	lex->token_start = s;

	/* Determine token type. */
	if (s >= end)
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
					const char *p;

					/*
					 * We're not dealing with a string, number, legal
					 * punctuation mark, or end of string.  The only legal
					 * tokens we might find here are true, false, and null,
					 * but for error reporting purposes we scan until we see a
					 * non-alphanumeric character.  That way, we can report
					 * the whole word as an unexpected token, rather than just
					 * some unintuitive prefix thereof.
					 */
					for (p = s; p < end && JSON_ALPHANUMERIC_CHAR(*p); p++)
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

					if (lex->incremental && !lex->inc_state->is_last_chunk &&
						p == lex->input + lex->input_length)
					{
						appendBinaryStringInfo(
											   &(lex->inc_state->partial_token), s, end - s);
						return JSON_INCOMPLETE;
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

	if (lex->incremental && lex->token_type == JSON_TOKEN_END && !lex->inc_state->is_last_chunk)
		return JSON_INCOMPLETE;
	else
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
	const char *s;
	const char *const end = lex->input + lex->input_length;
	int			hi_surrogate = -1;

	/* Convenience macros for error exits */
#define FAIL_OR_INCOMPLETE_AT_CHAR_START(code) \
	do { \
		if (lex->incremental && !lex->inc_state->is_last_chunk) \
		{ \
			appendBinaryStringInfo(&lex->inc_state->partial_token, \
								   lex->token_start, end - lex->token_start); \
			return JSON_INCOMPLETE; \
		} \
		lex->token_terminator = s; \
		return code; \
	} while (0)
#define FAIL_AT_CHAR_END(code) \
	do { \
		const char	   *term = s + pg_encoding_mblen(lex->input_encoding, s); \
		lex->token_terminator = (term <= end) ? term : end; \
		return code; \
	} while (0)

	if (lex->strval != NULL)
		resetStringInfo(lex->strval);

	Assert(lex->input_length > 0);
	s = lex->token_start;
	for (;;)
	{
		s++;
		/* Premature end of the string. */
		if (s >= end)
			FAIL_OR_INCOMPLETE_AT_CHAR_START(JSON_INVALID_TOKEN);
		else if (*s == '"')
			break;
		else if (*s == '\\')
		{
			/* OK, we have an escape character. */
			s++;
			if (s >= end)
				FAIL_OR_INCOMPLETE_AT_CHAR_START(JSON_INVALID_TOKEN);
			else if (*s == 'u')
			{
				int			i;
				int			ch = 0;

				for (i = 1; i <= 4; i++)
				{
					s++;
					if (s >= end)
						FAIL_OR_INCOMPLETE_AT_CHAR_START(JSON_INVALID_TOKEN);
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
					 * backend, we can let pg_unicode_to_server_noerror()
					 * handle any required character set conversion; in
					 * frontend, we can only deal with trivial conversions.
					 */
#ifndef FRONTEND
					{
						char		cbuf[MAX_UNICODE_EQUIVALENT_STRING + 1];

						if (!pg_unicode_to_server_noerror(ch, (unsigned char *) cbuf))
							FAIL_AT_CHAR_END(JSON_UNICODE_UNTRANSLATABLE);
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
		else
		{
			const char *p = s;

			if (hi_surrogate != -1)
				FAIL_AT_CHAR_END(JSON_UNICODE_LOW_SURROGATE);

			/*
			 * Skip to the first byte that requires special handling, so we
			 * can batch calls to appendBinaryStringInfo.
			 */
			while (p < end - sizeof(Vector8) &&
				   !pg_lfind8('\\', (uint8 *) p, sizeof(Vector8)) &&
				   !pg_lfind8('"', (uint8 *) p, sizeof(Vector8)) &&
				   !pg_lfind8_le(31, (uint8 *) p, sizeof(Vector8)))
				p += sizeof(Vector8);

			for (; p < end; p++)
			{
				if (*p == '\\' || *p == '"')
					break;
				else if ((unsigned char) *p <= 31)
				{
					/* Per RFC4627, these characters MUST be escaped. */
					/*
					 * Since *p isn't printable, exclude it from the context
					 * string
					 */
					lex->token_terminator = p;
					return JSON_ESCAPING_REQUIRED;
				}
			}

			if (lex->strval != NULL)
				appendBinaryStringInfo(lex->strval, s, p - s);

			/*
			 * s will be incremented at the top of the loop, so set it to just
			 * behind our lookahead position
			 */
			s = p - 1;
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

#undef FAIL_OR_INCOMPLETE_AT_CHAR_START
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
json_lex_number(JsonLexContext *lex, const char *s,
				bool *num_err, size_t *total_len)
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

	if (lex->incremental && !lex->inc_state->is_last_chunk &&
		len >= lex->input_length)
	{
		appendBinaryStringInfo(&lex->inc_state->partial_token,
							   lex->token_start, s - lex->token_start);
		if (num_err != NULL)
			*num_err = error;

		return JSON_INCOMPLETE;
	}
	else if (num_err != NULL)
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

/*
 * Construct an (already translated) detail message for a JSON error.
 *
 * The returned pointer should not be freed, the allocation is either static
 * or owned by the JsonLexContext.
 */
char *
json_errdetail(JsonParseErrorType error, JsonLexContext *lex)
{
	if (lex->errormsg)
		resetStringInfo(lex->errormsg);
	else
		lex->errormsg = makeStringInfo();

	/*
	 * A helper for error messages that should print the current token. The
	 * format must contain exactly one %.*s specifier.
	 */
#define json_token_error(lex, format) \
	appendStringInfo((lex)->errormsg, _(format), \
					 (int) ((lex)->token_terminator - (lex)->token_start), \
					 (lex)->token_start);

	switch (error)
	{
		case JSON_INCOMPLETE:
		case JSON_SUCCESS:
			/* fall through to the error code after switch */
			break;
		case JSON_INVALID_LEXER_TYPE:
			if (lex->incremental)
				return _("Recursive descent parser cannot use incremental lexer.");
			else
				return _("Incremental parser requires incremental lexer.");
		case JSON_NESTING_TOO_DEEP:
			return (_("JSON nested too deep, maximum permitted depth is 6400."));
		case JSON_ESCAPING_INVALID:
			json_token_error(lex, "Escape sequence \"\\%.*s\" is invalid.");
			break;
		case JSON_ESCAPING_REQUIRED:
			appendStringInfo(lex->errormsg,
							 _("Character with value 0x%02x must be escaped."),
							 (unsigned char) *(lex->token_terminator));
			break;
		case JSON_EXPECTED_END:
			json_token_error(lex, "Expected end of input, but found \"%.*s\".");
			break;
		case JSON_EXPECTED_ARRAY_FIRST:
			json_token_error(lex, "Expected array element or \"]\", but found \"%.*s\".");
			break;
		case JSON_EXPECTED_ARRAY_NEXT:
			json_token_error(lex, "Expected \",\" or \"]\", but found \"%.*s\".");
			break;
		case JSON_EXPECTED_COLON:
			json_token_error(lex, "Expected \":\", but found \"%.*s\".");
			break;
		case JSON_EXPECTED_JSON:
			json_token_error(lex, "Expected JSON value, but found \"%.*s\".");
			break;
		case JSON_EXPECTED_MORE:
			return _("The input string ended unexpectedly.");
		case JSON_EXPECTED_OBJECT_FIRST:
			json_token_error(lex, "Expected string or \"}\", but found \"%.*s\".");
			break;
		case JSON_EXPECTED_OBJECT_NEXT:
			json_token_error(lex, "Expected \",\" or \"}\", but found \"%.*s\".");
			break;
		case JSON_EXPECTED_STRING:
			json_token_error(lex, "Expected string, but found \"%.*s\".");
			break;
		case JSON_INVALID_TOKEN:
			json_token_error(lex, "Token \"%.*s\" is invalid.");
			break;
		case JSON_UNICODE_CODE_POINT_ZERO:
			return _("\\u0000 cannot be converted to text.");
		case JSON_UNICODE_ESCAPE_FORMAT:
			return _("\"\\u\" must be followed by four hexadecimal digits.");
		case JSON_UNICODE_HIGH_ESCAPE:
			/* note: this case is only reachable in frontend not backend */
			return _("Unicode escape values cannot be used for code point values above 007F when the encoding is not UTF8.");
		case JSON_UNICODE_UNTRANSLATABLE:

			/*
			 * Note: this case is only reachable in backend and not frontend.
			 * #ifdef it away so the frontend doesn't try to link against
			 * backend functionality.
			 */
#ifndef FRONTEND
			return psprintf(_("Unicode escape value could not be translated to the server's encoding %s."),
							GetDatabaseEncodingName());
#else
			Assert(false);
			break;
#endif
		case JSON_UNICODE_HIGH_SURROGATE:
			return _("Unicode high surrogate must not follow a high surrogate.");
		case JSON_UNICODE_LOW_SURROGATE:
			return _("Unicode low surrogate must follow a high surrogate.");
		case JSON_SEM_ACTION_FAILED:
			/* fall through to the error code after switch */
			break;
	}
#undef json_token_error

	/*
	 * We don't use a default: case, so that the compiler will warn about
	 * unhandled enum values.  But this needs to be here anyway to cover the
	 * possibility of an incorrect input.
	 */
	if (lex->errormsg->len == 0)
		appendStringInfo(lex->errormsg,
						 "unexpected json parse error type: %d",
						 (int) error);

	return lex->errormsg->data;
}

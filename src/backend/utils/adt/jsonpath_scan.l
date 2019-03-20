%{
/*-------------------------------------------------------------------------
 *
 * jsonpath_scan.l
 *	Lexical parser for jsonpath datatype
 *
 * Copyright (c) 2019, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	src/backend/utils/adt/jsonpath_scan.l
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "mb/pg_wchar.h"
#include "nodes/pg_list.h"

static JsonPathString scanstring;

/* No reason to constrain amount of data slurped */
/* #define YY_READ_BUF_SIZE 16777216 */

/* Handles to the buffer that the lexer uses internally */
static YY_BUFFER_STATE scanbufhandle;
static char *scanbuf;
static int	scanbuflen;

static void addstring(bool init, char *s, int l);
static void addchar(bool init, char s);
static int checkSpecialVal(void); /* examine scanstring for the special
								   * value */

static void parseUnicode(char *s, int l);
static void parseHexChars(char *s, int l);

/* Avoid exit() on fatal scanner errors (a bit ugly -- see yy_fatal_error) */
#undef fprintf
#define fprintf(file, fmt, msg)  fprintf_to_ereport(fmt, msg)

static void
fprintf_to_ereport(const char *fmt, const char *msg)
{
	ereport(ERROR, (errmsg_internal("%s", msg)));
}

%}

%option 8bit
%option never-interactive
%option nodefault
%option noinput
%option nounput
%option noyywrap
%option warn
%option prefix="jsonpath_yy"
%option bison-bridge
%option noyyalloc
%option noyyrealloc
%option noyyfree

%x xQUOTED
%x xNONQUOTED
%x xVARQUOTED
%x xSINGLEQUOTED
%x xCOMMENT

special		 [\?\%\$\.\[\]\{\}\(\)\|\&\!\=\<\>\@\#\,\*:\-\+\/]
any			[^\?\%\$\.\[\]\{\}\(\)\|\&\!\=\<\>\@\#\,\*:\-\+\/\\\"\' \t\n\r\f]
blank		[ \t\n\r\f]
hex_dig		[0-9A-Fa-f]
unicode		\\u({hex_dig}{4}|\{{hex_dig}{1,6}\})
hex_char	\\x{hex_dig}{2}


%%

<INITIAL>\&\&					{ return AND_P; }

<INITIAL>\|\|					{ return OR_P; }

<INITIAL>\!						{ return NOT_P; }

<INITIAL>\*\*					{ return ANY_P; }

<INITIAL>\<						{ return LESS_P; }

<INITIAL>\<\=					{ return LESSEQUAL_P; }

<INITIAL>\=\=					{ return EQUAL_P; }

<INITIAL>\<\>					{ return NOTEQUAL_P; }

<INITIAL>\!\=					{ return NOTEQUAL_P; }

<INITIAL>\>\=					{ return GREATEREQUAL_P; }

<INITIAL>\>						{ return GREATER_P; }

<INITIAL>\${any}+				{
									addstring(true, yytext + 1, yyleng - 1);
									addchar(false, '\0');
									yylval->str = scanstring;
									return VARIABLE_P;
								}

<INITIAL>\$\"					{
									addchar(true, '\0');
									BEGIN xVARQUOTED;
								}

<INITIAL>{special}				{ return *yytext; }

<INITIAL>{blank}+				{ /* ignore */ }

<INITIAL>\/\*					{
									addchar(true, '\0');
									BEGIN xCOMMENT;
								}

<INITIAL>[0-9]+(\.[0-9]+)?[eE][+-]?[0-9]+  /* float */  {
									addstring(true, yytext, yyleng);
									addchar(false, '\0');
									yylval->str = scanstring;
									return NUMERIC_P;
								}

<INITIAL>\.[0-9]+[eE][+-]?[0-9]+  /* float */  {
									addstring(true, yytext, yyleng);
									addchar(false, '\0');
									yylval->str = scanstring;
									return NUMERIC_P;
								}

<INITIAL>([0-9]+)?\.[0-9]+		{
									addstring(true, yytext, yyleng);
									addchar(false, '\0');
									yylval->str = scanstring;
									return NUMERIC_P;
								}

<INITIAL>[0-9]+					{
									addstring(true, yytext, yyleng);
									addchar(false, '\0');
									yylval->str = scanstring;
									return INT_P;
								}

<INITIAL>{any}+					{
									addstring(true, yytext, yyleng);
									BEGIN xNONQUOTED;
								}

<INITIAL>\"						{
									addchar(true, '\0');
									BEGIN xQUOTED;
								}

<INITIAL>\'						{
									addchar(true, '\0');
									BEGIN xSINGLEQUOTED;
								}

<INITIAL>\\						{
									yyless(0);
									addchar(true, '\0');
									BEGIN xNONQUOTED;
								}

<xNONQUOTED>{any}+				{
									addstring(false, yytext, yyleng);
								}

<xNONQUOTED>{blank}+			{
									yylval->str = scanstring;
									BEGIN INITIAL;
									return checkSpecialVal();
								}


<xNONQUOTED>\/\*				{
									yylval->str = scanstring;
									BEGIN xCOMMENT;
								}

<xNONQUOTED>({special}|\"|\')	{
									yylval->str = scanstring;
									yyless(0);
									BEGIN INITIAL;
									return checkSpecialVal();
								}

<xNONQUOTED><<EOF>>				{
									yylval->str = scanstring;
									BEGIN INITIAL;
									return checkSpecialVal();
								}

<xNONQUOTED,xQUOTED,xVARQUOTED,xSINGLEQUOTED>\\[\"\'\\]	{ addchar(false, yytext[1]); }

<xNONQUOTED,xQUOTED,xVARQUOTED,xSINGLEQUOTED>\\b	{ addchar(false, '\b'); }

<xNONQUOTED,xQUOTED,xVARQUOTED,xSINGLEQUOTED>\\f	{ addchar(false, '\f'); }

<xNONQUOTED,xQUOTED,xVARQUOTED,xSINGLEQUOTED>\\n	{ addchar(false, '\n'); }

<xNONQUOTED,xQUOTED,xVARQUOTED,xSINGLEQUOTED>\\r	{ addchar(false, '\r'); }

<xNONQUOTED,xQUOTED,xVARQUOTED,xSINGLEQUOTED>\\t	{ addchar(false, '\t'); }

<xNONQUOTED,xQUOTED,xVARQUOTED,xSINGLEQUOTED>\\v	{ addchar(false, '\v'); }

<xNONQUOTED,xQUOTED,xVARQUOTED,xSINGLEQUOTED>{unicode}+		{ parseUnicode(yytext, yyleng); }

<xNONQUOTED,xQUOTED,xVARQUOTED,xSINGLEQUOTED>{hex_char}+	{ parseHexChars(yytext, yyleng); }

<xNONQUOTED,xQUOTED,xVARQUOTED,xSINGLEQUOTED>\\x	{ yyerror(NULL, "Hex character sequence is invalid"); }

<xNONQUOTED,xQUOTED,xVARQUOTED,xSINGLEQUOTED>\\u	{ yyerror(NULL, "Unicode sequence is invalid"); }

<xNONQUOTED,xQUOTED,xVARQUOTED,xSINGLEQUOTED>\\.	{ yyerror(NULL, "Escape sequence is invalid"); }

<xNONQUOTED,xQUOTED,xVARQUOTED,xSINGLEQUOTED>\\		{ yyerror(NULL, "Unexpected end after backslash"); }

<xQUOTED,xVARQUOTED,xSINGLEQUOTED><<EOF>>			{ yyerror(NULL, "Unexpected end of quoted string"); }

<xQUOTED>\"						{
									yylval->str = scanstring;
									BEGIN INITIAL;
									return STRING_P;
								}

<xVARQUOTED>\"					{
									yylval->str = scanstring;
									BEGIN INITIAL;
									return VARIABLE_P;
								}

<xSINGLEQUOTED>\'				{
									yylval->str = scanstring;
									BEGIN INITIAL;
									return STRING_P;
								}

<xQUOTED,xVARQUOTED>[^\\\"]+	{ addstring(false, yytext, yyleng); }

<xSINGLEQUOTED>[^\\\']+			{ addstring(false, yytext, yyleng); }

<INITIAL><<EOF>>				{ yyterminate(); }

<xCOMMENT>\*\/					{ BEGIN INITIAL; }

<xCOMMENT>[^\*]+				{ }

<xCOMMENT>\*					{ }

<xCOMMENT><<EOF>>				{ yyerror(NULL, "Unexpected end of comment"); }

%%

void
jsonpath_yyerror(JsonPathParseResult **result, const char *message)
{
	if (*yytext == YY_END_OF_BUFFER_CHAR)
	{
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("bad jsonpath representation"),
				 /* translator: %s is typically "syntax error" */
				 errdetail("%s at end of input", message)));
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("bad jsonpath representation"),
				 /* translator: first %s is typically "syntax error" */
				 errdetail("%s at or near \"%s\"", message, yytext)));
	}
}

typedef struct JsonPathKeyword
{
	int16		len;
	bool		lowercase;
	int			val;
	const char *keyword;
} JsonPathKeyword;

/*
 * Array of key words should be sorted by length and then
 * alphabetical order
 */

static const JsonPathKeyword keywords[] = {
	{ 2, false,	IS_P,		"is"},
	{ 2, false,	TO_P,		"to"},
	{ 3, false,	ABS_P,		"abs"},
	{ 3, false,	LAX_P,		"lax"},
	{ 4, false,	FLAG_P,		"flag"},
	{ 4, false,	LAST_P,		"last"},
	{ 4, true,	NULL_P,		"null"},
	{ 4, false,	SIZE_P,		"size"},
	{ 4, true,	TRUE_P,		"true"},
	{ 4, false,	TYPE_P,		"type"},
	{ 4, false,	WITH_P,		"with"},
	{ 5, true,	FALSE_P,	"false"},
	{ 5, false,	FLOOR_P,	"floor"},
	{ 6, false,	DOUBLE_P,	"double"},
	{ 6, false,	EXISTS_P,	"exists"},
	{ 6, false,	STARTS_P,	"starts"},
	{ 6, false,	STRICT_P,	"strict"},
	{ 7, false,	CEILING_P,	"ceiling"},
	{ 7, false,	UNKNOWN_P,	"unknown"},
	{ 8, false,	KEYVALUE_P,	"keyvalue"},
	{ 10,false, LIKE_REGEX_P, "like_regex"},
};

static int
checkSpecialVal()
{
	int						res = IDENT_P;
	int						diff;
	const JsonPathKeyword  *StopLow = keywords,
						   *StopHigh = keywords + lengthof(keywords),
						   *StopMiddle;

	if (scanstring.len > keywords[lengthof(keywords) - 1].len)
		return res;

	while(StopLow < StopHigh)
	{
		StopMiddle = StopLow + ((StopHigh - StopLow) >> 1);

		if (StopMiddle->len == scanstring.len)
			diff = pg_strncasecmp(StopMiddle->keyword, scanstring.val,
								  scanstring.len);
		else
			diff = StopMiddle->len - scanstring.len;

		if (diff < 0)
			StopLow = StopMiddle + 1;
		else if (diff > 0)
			StopHigh = StopMiddle;
		else
		{
			if (StopMiddle->lowercase)
				diff = strncmp(StopMiddle->keyword, scanstring.val,
							   scanstring.len);

			if (diff == 0)
				res = StopMiddle->val;

			break;
		}
	}

	return res;
}

/*
 * Called before any actual parsing is done
 */
static void
jsonpath_scanner_init(const char *str, int slen)
{
	if (slen <= 0)
		slen = strlen(str);

	/*
	 * Might be left over after ereport()
	 */
	yy_init_globals();

	/*
	 * Make a scan buffer with special termination needed by flex.
	 */

	scanbuflen = slen;
	scanbuf = palloc(slen + 2);
	memcpy(scanbuf, str, slen);
	scanbuf[slen] = scanbuf[slen + 1] = YY_END_OF_BUFFER_CHAR;
	scanbufhandle = yy_scan_buffer(scanbuf, slen + 2);

	BEGIN(INITIAL);
}


/*
 * Called after parsing is done to clean up after jsonpath_scanner_init()
 */
static void
jsonpath_scanner_finish(void)
{
	yy_delete_buffer(scanbufhandle);
	pfree(scanbuf);
}

static void
addstring(bool init, char *s, int l)
{
	if (init)
	{
		scanstring.total = 32;
		scanstring.val = palloc(scanstring.total);
		scanstring.len = 0;
	}

	if (s && l)
	{
		while(scanstring.len + l + 1 >= scanstring.total)
		{
			scanstring.total *= 2;
			scanstring.val = repalloc(scanstring.val, scanstring.total);
		}

		memcpy(scanstring.val + scanstring.len, s, l);
		scanstring.len += l;
	}
}

static void
addchar(bool init, char s)
{
	if (init)
	{
		scanstring.total = 32;
		scanstring.val = palloc(scanstring.total);
		scanstring.len = 0;
	}
	else if(scanstring.len + 1 >= scanstring.total)
	{
		scanstring.total *= 2;
		scanstring.val = repalloc(scanstring.val, scanstring.total);
	}

	scanstring.val[ scanstring.len ] = s;
	if (s != '\0')
		scanstring.len++;
}

JsonPathParseResult *
parsejsonpath(const char *str, int len)
{
	JsonPathParseResult	*parseresult;

	jsonpath_scanner_init(str, len);

	if (jsonpath_yyparse((void*)&parseresult) != 0)
		jsonpath_yyerror(NULL, "bugus input");

	jsonpath_scanner_finish();

	return parseresult;
}

static int
hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 0xA;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 0xA;
	elog(ERROR, "invalid hexadecimal digit");
	return 0; /* not reached */
}

static void
addUnicodeChar(int ch)
{
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
				  errdetail("\\u0000 cannot be converted to text.")));
	}
	else if (GetDatabaseEncoding() == PG_UTF8)
	{
		char utf8str[5];
		int utf8len;

		unicode_to_utf8(ch, (unsigned char *) utf8str);
		utf8len = pg_utf_mblen((unsigned char *) utf8str);
		addstring(false, utf8str, utf8len);
	}
	else if (ch <= 0x007f)
	{
		/*
		 * This is the only way to designate things like a
		 * form feed character in JSON, so it's useful in all
		 * encodings.
		 */
		addchar(false, (char) ch);
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type jsonpath"),
				 errdetail("Unicode escape values cannot be used for code "
						   "point values above 007F when the server encoding "
						   "is not UTF8.")));
	}
}

static void
addUnicode(int ch, int *hi_surrogate)
{
	if (ch >= 0xd800 && ch <= 0xdbff)
	{
		if (*hi_surrogate != -1)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for type jsonpath"),
					 errdetail("Unicode high surrogate must not follow "
							   "a high surrogate.")));
		*hi_surrogate = (ch & 0x3ff) << 10;
		return;
	}
	else if (ch >= 0xdc00 && ch <= 0xdfff)
	{
		if (*hi_surrogate == -1)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for type jsonpath"),
					 errdetail("Unicode low surrogate must follow a high "
							   "surrogate.")));
		ch = 0x10000 + *hi_surrogate + (ch & 0x3ff);
		*hi_surrogate = -1;
	}
	else if (*hi_surrogate != -1)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type jsonpath"),
				 errdetail("Unicode low surrogate must follow a high "
						   "surrogate.")));
	}

	addUnicodeChar(ch);
}

/*
 * parseUnicode was adopted from json_lex_string() in
 * src/backend/utils/adt/json.c
 */
static void
parseUnicode(char *s, int l)
{
	int			i;
	int			hi_surrogate = -1;

	for (i = 2; i < l; i += 2)	/* skip '\u' */
	{
		int			ch = 0;
		int			j;

		if (s[i] == '{')	/* parse '\u{XX...}' */
		{
			while (s[++i] != '}' && i < l)
				ch = (ch << 4) | hexval(s[i]);
			i++;	/* ski p '}' */
		}
		else		/* parse '\uXXXX' */
		{
			for (j = 0; j < 4 && i < l; j++)
				ch = (ch << 4) | hexval(s[i++]);
		}

		addUnicode(ch, &hi_surrogate);
	}

	if (hi_surrogate != -1)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type jsonpath"),
				 errdetail("Unicode low surrogate must follow a high "
						   "surrogate.")));
	}
}

static void
parseHexChars(char *s, int l)
{
	int i;

	Assert(l % 4 /* \xXX */ == 0);

	for (i = 0; i < l / 4; i++)
	{
		int			ch = (hexval(s[i * 4 + 2]) << 4) | hexval(s[i * 4 + 3]);

		addUnicodeChar(ch);
	}
}

/*
 * Interface functions to make flex use palloc() instead of malloc().
 * It'd be better to make these static, but flex insists otherwise.
 */

void *
jsonpath_yyalloc(yy_size_t bytes)
{
	return palloc(bytes);
}

void *
jsonpath_yyrealloc(void *ptr, yy_size_t bytes)
{
	if (ptr)
		return repalloc(ptr, bytes);
	else
		return palloc(bytes);
}

void
jsonpath_yyfree(void *ptr)
{
	if (ptr)
		pfree(ptr);
}

%{
/*-------------------------------------------------------------------------
 *
 * jsonpath_scan.l
 *	Lexical parser for jsonpath datatype
 *
 * Splits jsonpath string into tokens represented as JsonPathString structs.
 * Decodes unicode and hex escaped strings.
 *
 * Copyright (c) 2019-2022, PostgreSQL Global Development Group
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

/* Handles to the buffer that the lexer uses internally */
static YY_BUFFER_STATE scanbufhandle;
static char *scanbuf;
static int	scanbuflen;

static void addstring(bool init, char *s, int l);
static void addchar(bool init, char s);
static enum yytokentype checkKeyword(void);
static void parseUnicode(char *s, int l);
static void parseHexChar(char *s);

/* Avoid exit() on fatal scanner errors (a bit ugly -- see yy_fatal_error) */
#undef fprintf
#define fprintf(file, fmt, msg)  fprintf_to_ereport(fmt, msg)

static void
fprintf_to_ereport(const char *fmt, const char *msg)
{
	ereport(ERROR, (errmsg_internal("%s", msg)));
}

/* LCOV_EXCL_START */

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

/*
 * We use exclusive states for quoted and non-quoted strings,
 * quoted variable names and C-style comments.
 * Exclusive states:
 *  <xq> - quoted strings
 *  <xnq> - non-quoted strings
 *  <xvq> - quoted variable names
 *  <xc> - C-style comment
 */

%x xq
%x xnq
%x xvq
%x xc

special		[\?\%\$\.\[\]\{\}\(\)\|\&\!\=\<\>\@\#\,\*:\-\+\/]
blank		[ \t\n\r\f]
/* "other" means anything that's not special, blank, or '\' or '"' */
other		[^\?\%\$\.\[\]\{\}\(\)\|\&\!\=\<\>\@\#\,\*:\-\+\/\\\" \t\n\r\f]

digit		[0-9]
integer		(0|[1-9]{digit}*)
decimal		({integer}\.{digit}*|\.{digit}+)
real		({integer}|{decimal})[Ee][-+]?{digit}+
realfail	({integer}|{decimal})[Ee][-+]

integer_junk	{integer}{other}
decimal_junk	{decimal}{other}
real_junk		{real}{other}

hex_dig		[0-9A-Fa-f]
unicode		\\u({hex_dig}{4}|\{{hex_dig}{1,6}\})
unicodefail	\\u({hex_dig}{0,3}|\{{hex_dig}{0,6})
hex_char	\\x{hex_dig}{2}
hex_fail	\\x{hex_dig}{0,1}

%%

<xnq>{other}+					{
									addstring(false, yytext, yyleng);
								}

<xnq>{blank}+					{
									yylval->str = scanstring;
									BEGIN INITIAL;
									return checkKeyword();
								}

<xnq>\/\*						{
									yylval->str = scanstring;
									BEGIN xc;
								}

<xnq>({special}|\")				{
									yylval->str = scanstring;
									yyless(0);
									BEGIN INITIAL;
									return checkKeyword();
								}

<xnq><<EOF>>					{
									yylval->str = scanstring;
									BEGIN INITIAL;
									return checkKeyword();
								}

<xnq,xq,xvq>\\b				{ addchar(false, '\b'); }

<xnq,xq,xvq>\\f				{ addchar(false, '\f'); }

<xnq,xq,xvq>\\n				{ addchar(false, '\n'); }

<xnq,xq,xvq>\\r				{ addchar(false, '\r'); }

<xnq,xq,xvq>\\t				{ addchar(false, '\t'); }

<xnq,xq,xvq>\\v				{ addchar(false, '\v'); }

<xnq,xq,xvq>{unicode}+		{ parseUnicode(yytext, yyleng); }

<xnq,xq,xvq>{hex_char}		{ parseHexChar(yytext); }

<xnq,xq,xvq>{unicode}*{unicodefail}	{ yyerror(NULL, "invalid unicode sequence"); }

<xnq,xq,xvq>{hex_fail}		{ yyerror(NULL, "invalid hex character sequence"); }

<xnq,xq,xvq>{unicode}+\\	{
								/* throw back the \\, and treat as unicode */
								yyless(yyleng - 1);
								parseUnicode(yytext, yyleng);
							}

<xnq,xq,xvq>\\.				{ addchar(false, yytext[1]); }

<xnq,xq,xvq>\\				{ yyerror(NULL, "unexpected end after backslash"); }

<xq,xvq><<EOF>>				{ yyerror(NULL, "unexpected end of quoted string"); }

<xq>\"							{
									yylval->str = scanstring;
									BEGIN INITIAL;
									return STRING_P;
								}

<xvq>\"							{
									yylval->str = scanstring;
									BEGIN INITIAL;
									return VARIABLE_P;
								}

<xq,xvq>[^\\\"]+				{ addstring(false, yytext, yyleng); }

<xc>\*\/						{ BEGIN INITIAL; }

<xc>[^\*]+						{ }

<xc>\*							{ }

<xc><<EOF>>						{ yyerror(NULL, "unexpected end of comment"); }

\&\&							{ return AND_P; }

\|\|							{ return OR_P; }

\!								{ return NOT_P; }

\*\*							{ return ANY_P; }

\<								{ return LESS_P; }

\<\=							{ return LESSEQUAL_P; }

\=\=							{ return EQUAL_P; }

\<\>							{ return NOTEQUAL_P; }

\!\=							{ return NOTEQUAL_P; }

\>\=							{ return GREATEREQUAL_P; }

\>								{ return GREATER_P; }

\${other}+						{
									addstring(true, yytext + 1, yyleng - 1);
									addchar(false, '\0');
									yylval->str = scanstring;
									return VARIABLE_P;
								}

\$\"							{
									addchar(true, '\0');
									BEGIN xvq;
								}

{special}						{ return *yytext; }

{blank}+						{ /* ignore */ }

\/\*							{
									addchar(true, '\0');
									BEGIN xc;
								}

{real}							{
									addstring(true, yytext, yyleng);
									addchar(false, '\0');
									yylval->str = scanstring;
									return NUMERIC_P;
								}

{decimal}						{
									addstring(true, yytext, yyleng);
									addchar(false, '\0');
									yylval->str = scanstring;
									return NUMERIC_P;
								}

{integer}						{
									addstring(true, yytext, yyleng);
									addchar(false, '\0');
									yylval->str = scanstring;
									return INT_P;
								}

{realfail}						{ yyerror(NULL, "invalid numeric literal"); }
{integer_junk}					{ yyerror(NULL, "trailing junk after numeric literal"); }
{decimal_junk}					{ yyerror(NULL, "trailing junk after numeric literal"); }
{real_junk}						{ yyerror(NULL, "trailing junk after numeric literal"); }

\"								{
									addchar(true, '\0');
									BEGIN xq;
								}

\\								{
									yyless(0);
									addchar(true, '\0');
									BEGIN xnq;
								}

{other}+						{
									addstring(true, yytext, yyleng);
									BEGIN xnq;
								}

<<EOF>>							{ yyterminate(); }

%%

/* LCOV_EXCL_STOP */

void
jsonpath_yyerror(JsonPathParseResult **result, const char *message)
{
	if (*yytext == YY_END_OF_BUFFER_CHAR)
	{
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 /* translator: %s is typically "syntax error" */
				 errmsg("%s at end of jsonpath input", _(message))));
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 /* translator: first %s is typically "syntax error" */
				 errmsg("%s at or near \"%s\" of jsonpath input",
						_(message), yytext)));
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
	{ 8, false,	DATETIME_P,	"datetime"},
	{ 8, false,	KEYVALUE_P,	"keyvalue"},
	{ 10,false, LIKE_REGEX_P, "like_regex"},
};

/* Check if current scanstring value is a keyword */
static enum yytokentype
checkKeyword()
{
	int			res = IDENT_P;
	int			diff;
	const JsonPathKeyword  *StopLow = keywords,
						   *StopHigh = keywords + lengthof(keywords),
						   *StopMiddle;

	if (scanstring.len > keywords[lengthof(keywords) - 1].len)
		return res;

	while (StopLow < StopHigh)
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

/*
 * Resize scanstring so that it can append string of given length.
 * Reinitialize if required.
 */
static void
resizeString(bool init, int appendLen)
{
	if (init)
	{
		scanstring.total = Max(32, appendLen);
		scanstring.val = (char *) palloc(scanstring.total);
		scanstring.len = 0;
	}
	else
	{
		if (scanstring.len + appendLen >= scanstring.total)
		{
			while (scanstring.len + appendLen >= scanstring.total)
				scanstring.total *= 2;
			scanstring.val = repalloc(scanstring.val, scanstring.total);
		}
	}
}

/* Add set of bytes at "s" of length "l" to scanstring */
static void
addstring(bool init, char *s, int l)
{
	resizeString(init, l + 1);
	memcpy(scanstring.val + scanstring.len, s, l);
	scanstring.len += l;
}

/* Add single byte "c" to scanstring */
static void
addchar(bool init, char c)
{
	resizeString(init, 1);
	scanstring.val[scanstring.len] = c;
	if (c != '\0')
		scanstring.len++;
}

/* Interface to jsonpath parser */
JsonPathParseResult *
parsejsonpath(const char *str, int len)
{
	JsonPathParseResult	*parseresult;

	jsonpath_scanner_init(str, len);

	if (jsonpath_yyparse((void *) &parseresult) != 0)
		jsonpath_yyerror(NULL, "bogus input"); /* shouldn't happen */

	jsonpath_scanner_finish();

	return parseresult;
}

/* Turn hex character into integer */
static int
hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 0xA;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 0xA;
	jsonpath_yyerror(NULL, "invalid hexadecimal digit");
	return 0; /* not reached */
}

/* Add given unicode character to scanstring */
static void
addUnicodeChar(int ch)
{
	if (ch == 0)
	{
		/* We can't allow this, since our TEXT type doesn't */
		ereport(ERROR,
				(errcode(ERRCODE_UNTRANSLATABLE_CHARACTER),
				 errmsg("unsupported Unicode escape sequence"),
				  errdetail("\\u0000 cannot be converted to text.")));
	}
	else
	{
		char		cbuf[MAX_UNICODE_EQUIVALENT_STRING + 1];

		pg_unicode_to_server(ch, (unsigned char *) cbuf);
		addstring(false, cbuf, strlen(cbuf));
	}
}

/* Add unicode character, processing any surrogate pairs */
static void
addUnicode(int ch, int *hi_surrogate)
{
	if (is_utf16_surrogate_first(ch))
	{
		if (*hi_surrogate != -1)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for type %s", "jsonpath"),
					 errdetail("Unicode high surrogate must not follow "
							   "a high surrogate.")));
		*hi_surrogate = ch;
		return;
	}
	else if (is_utf16_surrogate_second(ch))
	{
		if (*hi_surrogate == -1)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for type %s", "jsonpath"),
					 errdetail("Unicode low surrogate must follow a high "
							   "surrogate.")));
		ch = surrogate_pair_to_codepoint(*hi_surrogate, ch);
		*hi_surrogate = -1;
	}
	else if (*hi_surrogate != -1)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type %s", "jsonpath"),
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
	int			i = 2;
	int			hi_surrogate = -1;

	for (i = 2; i < l; i += 2)	/* skip '\u' */
	{
		int			ch = 0;
		int			j;

		if (s[i] == '{')	/* parse '\u{XX...}' */
		{
			while (s[++i] != '}' && i < l)
				ch = (ch << 4) | hexval(s[i]);
			i++;	/* skip '}' */
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
				 errmsg("invalid input syntax for type %s", "jsonpath"),
				 errdetail("Unicode low surrogate must follow a high "
						   "surrogate.")));
	}
}

/* Parse sequence of hex-encoded characters */
static void
parseHexChar(char *s)
{
	int			ch = (hexval(s[2]) << 4) |
					  hexval(s[3]);

	addUnicodeChar(ch);
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

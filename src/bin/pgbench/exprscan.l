%{
/*-------------------------------------------------------------------------
 *
 * exprscan.l
 *	  a lexical scanner for a simple expression syntax
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pgbench/exprscan.l
 *
 *-------------------------------------------------------------------------
 */

/* line and column number for error reporting */
static int	yyline = 0, yycol = 0;

/* Handles to the buffer that the lexer uses internally */
static YY_BUFFER_STATE scanbufhandle;
static char *scanbuf;

/* context information for error reporting */
static const char *expr_source = NULL;
static int expr_lineno = 0;
static const char *expr_full_line = NULL;
static const char *expr_command = NULL;
static int expr_col = 0;

/*
 * Work around a bug in flex 2.5.35: it emits a couple of functions that
 * it forgets to emit declarations for.  Since we use -Wmissing-prototypes,
 * this would cause warnings.  Providing our own declarations should be
 * harmless even when the bug gets fixed.
 */
extern int	expr_yyget_column(yyscan_t yyscanner);
extern void expr_yyset_column(int column_no, yyscan_t yyscanner);

%}

/* Except for the prefix, these options should match psqlscan.l */
%option reentrant
%option 8bit
%option never-interactive
%option nodefault
%option noinput
%option nounput
%option noyywrap
%option warn
%option prefix="expr_yy"

alpha			[a-zA-Z_]
digit			[0-9]
alnum			[a-zA-Z0-9_]
space			[ \t\r\f]

%%

%{
		/*
		 * Force flex into the appropriate start state ... which, for this
		 * case, is always INITIAL.  This ensures that we can transition
		 * between different lexers sharing the same yyscan_t.
		 */
		BEGIN(INITIAL);
%}

"+"				{ yycol += yyleng; return '+'; }
"-"				{ yycol += yyleng; return '-'; }
"*"				{ yycol += yyleng; return '*'; }
"/"				{ yycol += yyleng; return '/'; }
"%"				{ yycol += yyleng; return '%'; }
"("				{ yycol += yyleng; return '('; }
")"				{ yycol += yyleng; return ')'; }
","				{ yycol += yyleng; return ','; }

:{alnum}+		{
					yycol += yyleng;
					yylval.str = pg_strdup(yytext + 1);
					return VARIABLE;
				}
{digit}+		{
					yycol += yyleng;
					yylval.ival = strtoint64(yytext);
					return INTEGER;
				}
{alpha}{alnum}*	{
					yycol += yyleng;
					yylval.str = pg_strdup(yytext);
					return FUNCTION;
				}

[\n]			{ yycol = 0; yyline++; }

{space}+		{ yycol += yyleng; /* otherwise ignore */ }

.				{
					yycol += yyleng;
					syntax_error(expr_source, expr_lineno, expr_full_line, expr_command,
								 "unexpected character", yytext, expr_col + yycol);
					/* NOTREACHED, exit is called from syntax_error */
					return 0;
				}
%%

void
expr_yyerror_more(yyscan_t yyscanner, const char *message, const char *more)
{
	syntax_error(expr_source, expr_lineno, expr_full_line, expr_command,
				 message, more, expr_col + yycol);
}

void
yyerror(yyscan_t yyscanner, const char *message)
{
	expr_yyerror_more(yyscanner, message, NULL);
}

/*
 * Called before any actual parsing is done
 */
yyscan_t
expr_scanner_init(const char *str, const char *source,
				  int lineno, const char *line,
				  const char *cmd, int ecol)
{
	yyscan_t yyscanner;
	Size	slen = strlen(str);

	/* Set up yyscan_t */
	yylex_init(&yyscanner);

	/* save context information for error messages */
	expr_source = source;
	expr_lineno = lineno;
	expr_full_line = line;
	expr_command = cmd;
	expr_col = ecol;

	/* reset error pointers for this scan */
	yycol = yyline = 0;

	/*
	 * Make a scan buffer with special termination needed by flex.
	 */
	scanbuf = pg_malloc(slen + 2);
	memcpy(scanbuf, str, slen);
	scanbuf[slen] = scanbuf[slen + 1] = YY_END_OF_BUFFER_CHAR;
	scanbufhandle = yy_scan_buffer(scanbuf, slen + 2, yyscanner);

	return yyscanner;
}


/*
 * Called after parsing is done to clean up after expr_scanner_init()
 */
void
expr_scanner_finish(yyscan_t yyscanner)
{
	yy_delete_buffer(scanbufhandle, yyscanner);
	pg_free(scanbuf);
	yylex_destroy(yyscanner);

	expr_source = NULL;
	expr_lineno = 0;
	expr_full_line = NULL;
	expr_command = NULL;
	expr_col = 0;
}

%{
/*-------------------------------------------------------------------------
 *
 * exprscan.l
 *	  a lexical scanner for a simple expression syntax
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *-------------------------------------------------------------------------
 */

/* line and column number for error reporting */
static int	yyline = 0, yycol = 0;

/* Handles to the buffer that the lexer uses internally */
static YY_BUFFER_STATE scanbufhandle;
static char *scanbuf;
static int	scanbuflen;

/* context information for error reporting */
static char *expr_source = NULL;
static int expr_lineno = 0;
static char *expr_full_line = NULL;
static char *expr_command = NULL;
static int expr_col = 0;
%}

%option 8bit
%option never-interactive
%option nodefault
%option noinput
%option nounput
%option noyywrap
%option warn
%option prefix="expr_yy"

space			[ \t\r\f]

%%

"+"				{ yycol += yyleng; return '+'; }
"-"				{ yycol += yyleng; return '-'; }
"*"				{ yycol += yyleng; return '*'; }
"/"				{ yycol += yyleng; return '/'; }
"%"				{ yycol += yyleng; return '%'; }
"("				{ yycol += yyleng; return '('; }
")"				{ yycol += yyleng; return ')'; }
","				{ yycol += yyleng; return ','; }

:[a-zA-Z0-9_]+	{
					yycol += yyleng;
					yylval.str = pg_strdup(yytext + 1);
					return VARIABLE;
				}
[0-9]+			{
					yycol += yyleng;
					yylval.ival = strtoint64(yytext);
					return INTEGER;
				}
[a-zA-Z0-9_]+   {
					yycol += yyleng;
					yylval.str = pg_strdup(yytext);
					return FUNCTION;
				}

[\n]			{ yycol = 0; yyline++; }

{space}+		{ yycol += yyleng; /* ignore */ }

.				{
					yycol += yyleng;
					syntax_error(expr_source, expr_lineno, expr_full_line, expr_command,
								 "unexpected character", yytext, expr_col + yycol);
					/* dead code, exit is called from syntax_error */
					return CHAR_ERROR;
				}
%%

void
expr_yyerror_more(const char *message, const char *more)
{
	syntax_error(expr_source, expr_lineno, expr_full_line, expr_command,
				 message, more, expr_col + yycol);
}

void
yyerror(const char *message)
{
	expr_yyerror_more(message, NULL);
}

/*
 * Called before any actual parsing is done
 */
void
expr_scanner_init(const char *str, const char *source,
				  const int lineno, const char *line,
				  const char *cmd, const int ecol)
{
	Size	slen = strlen(str);

	/* save context informations for error messages */
	expr_source = (char *) source;
	expr_lineno = (int) lineno;
	expr_full_line = (char *) line;
	expr_command = (char *) cmd;
	expr_col = (int) ecol;

	/* reset column count for this scan */
	yycol = 0;

	/*
	 * Might be left over after error
	 */
	if (YY_CURRENT_BUFFER)
		yy_delete_buffer(YY_CURRENT_BUFFER);

	/*
	 * Make a scan buffer with special termination needed by flex.
	 */
	scanbuflen = slen;
	scanbuf = pg_malloc(slen + 2);
	memcpy(scanbuf, str, slen);
	scanbuf[slen] = scanbuf[slen + 1] = YY_END_OF_BUFFER_CHAR;
	scanbufhandle = yy_scan_buffer(scanbuf, slen + 2);

	BEGIN(INITIAL);
}


/*
 * Called after parsing is done to clean up after seg_scanner_init()
 */
void
expr_scanner_finish(void)
{
	yy_delete_buffer(scanbufhandle);
	pg_free(scanbuf);
	expr_source = NULL;
	expr_lineno = 0;
	expr_full_line = NULL;
	expr_command = NULL;
	expr_col = 0;
}

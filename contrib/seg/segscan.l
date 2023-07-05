%top{
/*
 * A scanner for EMP-style numeric ranges
 */
#include "postgres.h"

#include "nodes/miscnodes.h"

/*
 * NB: include segparse.h only AFTER including segdata.h, because segdata.h
 * contains the definition for SEG.
 */
#include "segdata.h"
#include "segparse.h"
}

%{
/* LCOV_EXCL_START */

/* No reason to constrain amount of data slurped */
#define YY_READ_BUF_SIZE 16777216

/* Avoid exit() on fatal scanner errors (a bit ugly -- see yy_fatal_error) */
#undef fprintf
#define fprintf(file, fmt, msg)  fprintf_to_ereport(fmt, msg)

static void
fprintf_to_ereport(const char *fmt, const char *msg)
{
	ereport(ERROR, (errmsg_internal("%s", msg)));
}

/* Handles to the buffer that the lexer uses internally */
static YY_BUFFER_STATE scanbufhandle;
static char *scanbuf;
%}

%option 8bit
%option never-interactive
%option nodefault
%option noinput
%option nounput
%option noyywrap
%option warn
%option prefix="seg_yy"


range        (\.\.)(\.)?
plumin       (\'\+\-\')|(\(\+\-)\)
integer      [+-]?[0-9]+
real         [+-]?[0-9]+\.[0-9]+
float        ({integer}|{real})([eE]{integer})?

%%

{range}      seg_yylval.text = yytext; return RANGE;
{plumin}     seg_yylval.text = yytext; return PLUMIN;
{float}      seg_yylval.text = yytext; return SEGFLOAT;
\<           seg_yylval.text = "<"; return EXTENSION;
\>           seg_yylval.text = ">"; return EXTENSION;
\~           seg_yylval.text = "~"; return EXTENSION;
[ \t\n\r\f\v]+ /* discard spaces */
.            return yytext[0]; /* alert parser of the garbage */

%%

/* LCOV_EXCL_STOP */

void
seg_yyerror(SEG *result, struct Node *escontext, const char *message)
{
	/* if we already reported an error, don't overwrite it */
	if (SOFT_ERROR_OCCURRED(escontext))
		return;

	if (*yytext == YY_END_OF_BUFFER_CHAR)
	{
		errsave(escontext,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("bad seg representation"),
				 /* translator: %s is typically "syntax error" */
				 errdetail("%s at end of input", message)));
	}
	else
	{
		errsave(escontext,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("bad seg representation"),
				 /* translator: first %s is typically "syntax error" */
				 errdetail("%s at or near \"%s\"", message, yytext)));
	}
}


/*
 * Called before any actual parsing is done
 */
void
seg_scanner_init(const char *str)
{
	Size	slen = strlen(str);

	/*
	 * Might be left over after ereport()
	 */
	if (YY_CURRENT_BUFFER)
		yy_delete_buffer(YY_CURRENT_BUFFER);

	/*
	 * Make a scan buffer with special termination needed by flex.
	 */
	scanbuf = palloc(slen + 2);
	memcpy(scanbuf, str, slen);
	scanbuf[slen] = scanbuf[slen + 1] = YY_END_OF_BUFFER_CHAR;
	scanbufhandle = yy_scan_buffer(scanbuf, slen + 2);

	BEGIN(INITIAL);
}


/*
 * Called after parsing is done to clean up after seg_scanner_init()
 */
void
seg_scanner_finish(void)
{
	yy_delete_buffer(scanbufhandle);
	pfree(scanbuf);
}

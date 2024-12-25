%top{
/*
 * A scanner for EMP-style numeric ranges
 */
#include "postgres.h"

#include "nodes/miscnodes.h"

#include "segdata.h"
#include "segparse.h"	/* must be after segdata.h for SEG */
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
%}

%option reentrant
%option bison-bridge
%option 8bit
%option never-interactive
%option nodefault
%option noinput
%option nounput
%option noyywrap
%option noyyalloc
%option noyyrealloc
%option noyyfree
%option warn
%option prefix="seg_yy"


range        (\.\.)(\.)?
plumin       (\'\+\-\')|(\(\+\-)\)
integer      [+-]?[0-9]+
real         [+-]?[0-9]+\.[0-9]+
float        ({integer}|{real})([eE]{integer})?

%%

{range}      yylval->text = yytext; return RANGE;
{plumin}     yylval->text = yytext; return PLUMIN;
{float}      yylval->text = yytext; return SEGFLOAT;
\<           yylval->text = "<"; return EXTENSION;
\>           yylval->text = ">"; return EXTENSION;
\~           yylval->text = "~"; return EXTENSION;
[ \t\n\r\f\v]+ /* discard spaces */
.            return yytext[0]; /* alert parser of the garbage */

%%

/* LCOV_EXCL_STOP */

void
seg_yyerror(SEG *result, struct Node *escontext, yyscan_t yyscanner, const char *message)
{
	struct yyguts_t *yyg = (struct yyguts_t *) yyscanner;	/* needed for yytext
															 * macro */

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
seg_scanner_init(const char *str, yyscan_t *yyscannerp)
{
	yyscan_t	yyscanner;

	if (yylex_init(yyscannerp) != 0)
		elog(ERROR, "yylex_init() failed: %m");

	yyscanner = *yyscannerp;

	yy_scan_string(str, yyscanner);
}


/*
 * Called after parsing is done to clean up after seg_scanner_init()
 */
void
seg_scanner_finish(yyscan_t yyscanner)
{
	yylex_destroy(yyscanner);
}

/*
 * Interface functions to make flex use palloc() instead of malloc().
 * It'd be better to make these static, but flex insists otherwise.
 */

void *
yyalloc(yy_size_t size, yyscan_t yyscanner)
{
	return palloc(size);
}

void *
yyrealloc(void *ptr, yy_size_t size, yyscan_t yyscanner)
{
	if (ptr)
		return repalloc(ptr, size);
	else
		return palloc(size);
}

void
yyfree(void *ptr, yyscan_t yyscanner)
{
	if (ptr)
		pfree(ptr);
}

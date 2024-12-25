%top{
/*
 * A scanner for EMP-style numeric ranges
 * contrib/cube/cubescan.l
 */

#include "postgres.h"

#include "cubedata.h"
#include "cubeparse.h"	/* must be after cubedata.h for YYSTYPE and NDBOX */
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
%option prefix="cube_yy"


n            [0-9]+
integer      [+-]?{n}
real         [+-]?({n}\.{n}?|\.{n})
float        ({integer}|{real})([eE]{integer})?
infinity     [+-]?[iI][nN][fF]([iI][nN][iI][tT][yY])?
NaN          [nN][aA][nN]

%%

{float}      *yylval = yytext; return CUBEFLOAT;
{infinity}   *yylval = yytext; return CUBEFLOAT;
{NaN}        *yylval = yytext; return CUBEFLOAT;
\[           *yylval = "("; return O_BRACKET;
\]           *yylval = ")"; return C_BRACKET;
\(           *yylval = "("; return O_PAREN;
\)           *yylval = ")"; return C_PAREN;
\,           *yylval = ","; return COMMA;
[ \t\n\r\f\v]+ /* discard spaces */
.            return yytext[0]; /* alert parser of the garbage */

%%

/* LCOV_EXCL_STOP */

/* result and scanbuflen are not used, but Bison expects this signature */
void
cube_yyerror(NDBOX **result, Size scanbuflen,
			 struct Node *escontext,
			 yyscan_t yyscanner,
			 const char *message)
{
	struct yyguts_t *yyg = (struct yyguts_t *) yyscanner;	/* needed for yytext
															 * macro */

	if (*yytext == YY_END_OF_BUFFER_CHAR)
	{
		errsave(escontext,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for cube"),
		/* translator: %s is typically "syntax error" */
				 errdetail("%s at end of input", message)));
	}
	else
	{
		errsave(escontext,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for cube"),
		/* translator: first %s is typically "syntax error" */
				 errdetail("%s at or near \"%s\"", message, yytext)));
	}
}


/*
 * Called before any actual parsing is done
 */
void
cube_scanner_init(const char *str, Size *scanbuflen, yyscan_t *yyscannerp)
{
	Size		slen = strlen(str);
	yyscan_t	yyscanner;

	if (yylex_init(yyscannerp) != 0)
		elog(ERROR, "yylex_init() failed: %m");

	yyscanner = *yyscannerp;

	yy_scan_bytes(str, slen, yyscanner);
	*scanbuflen = slen;
}


/*
 * Called after parsing is done to clean up after cube_scanner_init()
 */
void
cube_scanner_finish(yyscan_t yyscanner)
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

%top{
/*
 * A scanner for EMP-style numeric ranges
 * contrib/cube/cubescan.l
 */

#include "postgres.h"

/*
 * NB: include cubeparse.h only AFTER defining YYSTYPE (to match cubeparse.y)
 * and cubedata.h for NDBOX.
 */
#include "cubedata.h"
#define YYSTYPE char *
#include "cubeparse.h"
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
%option prefix="cube_yy"


n            [0-9]+
integer      [+-]?{n}
real         [+-]?({n}\.{n}?|\.{n})
float        ({integer}|{real})([eE]{integer})?
infinity     [+-]?[iI][nN][fF]([iI][nN][iI][tT][yY])?
NaN          [nN][aA][nN]

%%

{float}      cube_yylval = yytext; return CUBEFLOAT;
{infinity}   cube_yylval = yytext; return CUBEFLOAT;
{NaN}        cube_yylval = yytext; return CUBEFLOAT;
\[           cube_yylval = "("; return O_BRACKET;
\]           cube_yylval = ")"; return C_BRACKET;
\(           cube_yylval = "("; return O_PAREN;
\)           cube_yylval = ")"; return C_PAREN;
\,           cube_yylval = ","; return COMMA;
[ \t\n\r\f\v]+ /* discard spaces */
.            return yytext[0]; /* alert parser of the garbage */

%%

/* LCOV_EXCL_STOP */

/* result and scanbuflen are not used, but Bison expects this signature */
void
cube_yyerror(NDBOX **result, Size scanbuflen,
			 struct Node *escontext,
			 const char *message)
{
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
cube_scanner_init(const char *str, Size *scanbuflen)
{
	Size		slen = strlen(str);

	/*
	 * Might be left over after ereport()
	 */
	if (YY_CURRENT_BUFFER)
		yy_delete_buffer(YY_CURRENT_BUFFER);

	/*
	 * Make a scan buffer with special termination needed by flex.
	 */
	*scanbuflen = slen;
	scanbuf = palloc(slen + 2);
	memcpy(scanbuf, str, slen);
	scanbuf[slen] = scanbuf[slen + 1] = YY_END_OF_BUFFER_CHAR;
	scanbufhandle = yy_scan_buffer(scanbuf, slen + 2);

	BEGIN(INITIAL);
}


/*
 * Called after parsing is done to clean up after cube_scanner_init()
 */
void
cube_scanner_finish(void)
{
	yy_delete_buffer(scanbufhandle);
	pfree(scanbuf);
}

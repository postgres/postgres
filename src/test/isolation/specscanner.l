%top{
/*-------------------------------------------------------------------------
 *
 * specscanner.l
 *	  a lexical scanner for an isolation test specification
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

/*
 * NB: include specparse.h only AFTER including isolationtester.h, because
 * isolationtester.h includes node definitions needed for YYSTYPE.
 */
#include "isolationtester.h"
#include "specparse.h"
}

%{
static int	yyline = 1;			/* line number for error reporting */

#define LITBUF_INIT	1024		/* initial size of litbuf */
static char *litbuf = NULL;
static size_t litbufsize = 0;
static size_t litbufpos = 0;

static void addlitchar(char c);

/* LCOV_EXCL_START */

%}

%option 8bit
%option never-interactive
%option nodefault
%option noinput
%option nounput
%option noyywrap
%option warn
%option prefix="spec_yy"


%x sql
%x qident

non_newline		[^\n\r]
space			[ \t\r\f]

comment			("#"{non_newline}*)

digit			[0-9]
ident_start		[A-Za-z\200-\377_]
ident_cont		[A-Za-z\200-\377_0-9\$]

identifier		{ident_start}{ident_cont}*

self			[,()*]

%%

%{
	/* Allocate litbuf in first call of yylex() */
	if (litbuf == NULL)
	{
		litbuf = pg_malloc(LITBUF_INIT);
		litbufsize = LITBUF_INIT;
	}
%}

 /* Keywords (must appear before the {identifier} rule!) */
notices			{ return NOTICES; }
permutation		{ return PERMUTATION; }
session			{ return SESSION; }
setup			{ return SETUP; }
step			{ return STEP; }
teardown		{ return TEARDOWN; }

 /* Whitespace and comments */
[\n]			{ yyline++; }
{comment}		{ /* ignore */ }
{space}			{ /* ignore */ }

 /* Plain identifiers */
{identifier}	{
					spec_yylval.str = pg_strdup(yytext);
					return(identifier);
				}

 /* Quoted identifiers: "foo" */
\"				{
					litbufpos = 0;
					BEGIN(qident);
				}
<qident>\"\"	{ addlitchar(yytext[0]); }
<qident>\"		{
					litbuf[litbufpos] = '\0';
					spec_yylval.str = pg_strdup(litbuf);
					BEGIN(INITIAL);
					return(identifier);
				}
<qident>.		{ addlitchar(yytext[0]); }
<qident>\n		{ spec_yyerror("unexpected newline in quoted identifier"); }
<qident><<EOF>>	{ spec_yyerror("unterminated quoted identifier"); }

 /* SQL blocks: { UPDATE ... } */
 /* We trim leading/trailing whitespace, otherwise they're unprocessed */
"{"{space}*		{

					litbufpos = 0;
					BEGIN(sql);
				}
<sql>{space}*"}" {
					litbuf[litbufpos] = '\0';
					spec_yylval.str = pg_strdup(litbuf);
					BEGIN(INITIAL);
					return(sqlblock);
				}
<sql>.			{
					addlitchar(yytext[0]);
				}
<sql>\n			{
					yyline++;
					addlitchar(yytext[0]);
				}
<sql><<EOF>>	{
					spec_yyerror("unterminated sql block");
				}

 /* Numbers and punctuation */
{digit}+		{
					spec_yylval.integer = atoi(yytext);
					return INTEGER;
				}

{self}			{ return yytext[0]; }

 /* Anything else is an error */
.				{
					fprintf(stderr, "syntax error at line %d: unexpected character \"%s\"\n", yyline, yytext);
					exit(1);
				}
%%

/* LCOV_EXCL_STOP */

static void
addlitchar(char c)
{
	/* We must always leave room to add a trailing \0 */
	if (litbufpos >= litbufsize - 1)
	{
		/* Double the size of litbuf if it gets full */
		litbufsize += litbufsize;
		litbuf = pg_realloc(litbuf, litbufsize);
	}
	litbuf[litbufpos++] = c;
}

void
spec_yyerror(const char *message)
{
	fprintf(stderr, "%s at line %d\n", message, yyline);
	exit(1);
}

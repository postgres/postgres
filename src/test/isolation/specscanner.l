%{
/*-------------------------------------------------------------------------
 *
 * specscanner.l
 *	  a lexical scanner for an isolation test specification
 *
 * Portions Copyright (c) 1996-2011, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *-------------------------------------------------------------------------
 */

static int	yyline = 1;			/* line number for error reporting */

static char litbuf[1024];
static int litbufpos = 0;

static void addlitchar(const char c);

%}

%option 8bit
%option never-interactive
%option nodefault
%option noinput
%option nounput
%option noyywrap
%option prefix="spec_yy"


%x sql
%x qstr

non_newline		[^\n\r]
space			[ \t\n\r\f]

comment			("#"{non_newline}*)
whitespace		({space}+|{comment})

%%

permutation		{ return(PERMUTATION); }
session			{ return(SESSION); }
setup			{ return(SETUP); }
step			{ return(STEP); }
teardown		{ return(TEARDOWN); }

[\n]			{ yyline++; }
{whitespace}	{
					/* ignore */
				}

\"				{
					litbufpos = 0;
					BEGIN(qstr);
				}
<qstr>\"		{
					litbuf[litbufpos] = '\0';
					yylval.str = strdup(litbuf);
					BEGIN(INITIAL);
					return(string);
				}
<qstr>.			{ addlitchar(yytext[0]); }

"{"				{

					litbufpos = 0;
					BEGIN(sql);
				}

<sql>"}"		{
					litbuf[litbufpos] = '\0';
					yylval.str = strdup(litbuf);
					BEGIN(INITIAL);
					return(sqlblock);
				}
<sql>[^}]		{ addlitchar(yytext[0]);}


.				{
					fprintf(stderr, "syntax error at line %d: unexpected character \"%s\"\n", yyline, yytext);
					exit(1);
				}

%%

static void
addlitchar(const char c)
{
  if (litbufpos >= sizeof(litbuf) - 1)
  {
    fprintf(stderr, "SQL step too long\n");
    exit(1);
  }
  litbuf[litbufpos++] = c;
}

void
yyerror(const char *message)
{
	fprintf(stderr, "%s at line %d\n", message, yyline);
	exit(1);
}

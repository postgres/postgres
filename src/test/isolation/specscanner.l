%{
/*-------------------------------------------------------------------------
 *
 * specscanner.l
 *	  a lexical scanner for an isolation test specification
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *-------------------------------------------------------------------------
 */

static int	yyline = 1;			/* line number for error reporting */

static char litbuf[1024];
static int litbufpos = 0;

static void addlitchar(char c);

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
%x qstr

non_newline		[^\n\r]
space			[ \t\r\f]

comment			("#"{non_newline}*)

%%

permutation		{ return(PERMUTATION); }
session			{ return(SESSION); }
setup			{ return(SETUP); }
step			{ return(STEP); }
teardown		{ return(TEARDOWN); }

[\n]			{ yyline++; }
{comment}		{ /* ignore */ }
{space}			{ /* ignore */ }

 /* Quoted strings: "foo" */
\"				{
					litbufpos = 0;
					BEGIN(qstr);
				}
<qstr>\"		{
					litbuf[litbufpos] = '\0';
					yylval.str = strdup(litbuf);
					BEGIN(INITIAL);
					return(string_literal);
				}
<qstr>.			{ addlitchar(yytext[0]); }
<qstr>\n		{ yyerror("unexpected newline in quoted string"); }
<qstr><<EOF>>	{ yyerror("unterminated quoted string"); }

 /* SQL blocks: { UPDATE ... } */
"{"{space}*		{

					litbufpos = 0;
					BEGIN(sql);
				}
<sql>{space}*"}" {
					litbuf[litbufpos] = '\0';
					yylval.str = strdup(litbuf);
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
					yyerror("unterminated sql block");
				}

.				{
					fprintf(stderr, "syntax error at line %d: unexpected character \"%s\"\n", yyline, yytext);
					exit(1);
				}
%%

static void
addlitchar(char c)
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

%{
/*-------------------------------------------------------------------------
 *
 * pgc.l
 *	  lexical scanner for ecpg
 *
 * This is a modified version of src/backend/parser/scan.l
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/interfaces/ecpg/preproc/pgc.l,v 1.70 2000/12/15 20:01:55 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <ctype.h>
#include <sys/types.h>
#include <limits.h>
#include <errno.h>

#include "miscadmin.h"
#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"
#include "parser/scansup.h"
#include "extern.h"
#include "preproc.h"
#include "utils/builtins.h"

/* some versions of lex define this as a macro */
#if defined(yywrap)
#undef yywrap
#endif /* yywrap */

#define YY_NO_UNPUT

extern YYSTYPE yylval;

/*
 * literalbuf is used to accumulate literal values when multiple rules
 * are needed to parse a single literal.  Call startlit to reset buffer
 * to empty, addlit to add text.  Note that the buffer is permanently
 * malloc'd to the largest size needed so far in the current run.
 */
static char	   *literalbuf = NULL;		/* expandable buffer */
static int		literallen;		/* actual current length */
static int		literalalloc;	/* current allocated buffer size */

static int             xcdepth = 0; 

#define startlit()  (literalbuf[0] = '\0', literallen = 0)
static void addlit(char *ytext, int yleng);

int state_before;

struct _yy_buffer { YY_BUFFER_STATE 	buffer;
		    long		lineno;
		    char	      * filename;
		    struct _yy_buffer * next;
		  } *yy_buffer = NULL;

static char *old;

#define MAX_NESTED_IF 128
static short preproc_tos;
static short ifcond;
static struct _if_value {
    short condition;
    short else_branch;
} stacked_if_value[MAX_NESTED_IF];

%}

%option yylineno
%s C SQL incl def def_ident

/*
 * OK, here is a short description of lex/flex rules behavior.
 * The longest pattern which matches an input string is always chosen.
 * For equal-length patterns, the first occurring in the rules list is chosen.
 * INITIAL is the starting state, to which all non-conditional rules apply.
 * Exclusive states change parsing rules while the state is active.  When in
 * an exclusive state, only those rules defined for that state apply.
 *
 * We use exclusive states for quoted strings, extended comments,
 * and to eliminate parsing troubles for numeric strings.
 * Exclusive states:
 *  <xbit> bit string literal
 *  <xc> extended C-style comments - thomas 1997-07-12
 *  <xd> delimited identifiers (double-quoted identifiers) - thomas 1997-10-27
 *  <xh> hexadecimal numeric string - thomas 1997-11-16
 *  <xq> quoted strings - thomas 1997-07-30
 */

%x xbit
%x xc
%x xd
%x xdc
%x xh
%x xq
%x xpre
%x xcond
%x xskip

/* Bit string
 */
xbitstart		[bB]{quote}
xbitstop		{quote}
xbitinside		[^']*
xbitcat			{quote}{whitespace_with_newline}{quote}

/* Hexadecimal number
 */
xhstart			[xX]{quote}
xhstop			{quote}
xhinside		[^']+
xhcat			{quote}{whitespace_with_newline}{quote}

/* C version of hex number 
 */
xch			0[xX][0-9A-Fa-f]*

/* Extended quote
 * xqdouble implements SQL92 embedded quote
 * xqcat allows strings to cross input lines
 */
quote			'
xqstart			{quote}
xqstop			{quote}
xqdouble		{quote}{quote}
xqinside		[^\\']+
xqliteral		[\\](.|\n)
xqcat			{quote}{whitespace_with_newline}{quote}

/* Delimited quote
 * Allows embedded spaces and other special characters into identifiers.
 */
dquote			\"
xdstart			{dquote}
xdstop			{dquote}
xddouble               	{dquote}{dquote}
xdinside		[^"]+

/* special stuff for C strings */
xdcqq			\\\\
xdcqdq			\\\"
xdcother		[^"]
xdcinside		({xdcqq}|{xdcqdq}|{xdcother})

/* C-style comments
 *
 * The "extended comment" syntax closely resembles allowable operator syntax.
 * The tricky part here is to get lex to recognize a string starting with
 * slash-star as a comment, when interpreting it as an operator would produce
 * a longer match --- remember lex will prefer a longer match!  Also, if we
 * have something like plus-slash-star, lex will think this is a 3-character
 * operator whereas we want to see it as a + operator and a comment start.
 * The solution is two-fold:
 * 1. append {op_chars}* to xcstart so that it matches as much text as
 *    {operator} would. Then the tie-breaker (first matching rule of same
 *    length) ensures xcstart wins.  We put back the extra stuff with yyless()
 *    in case it contains a star-slash that should terminate the comment.
 * 2. In the operator rule, check for slash-star within the operator, and
 *    if found throw it back with yyless().  This handles the plus-slash-star
 *    problem.
 * SQL92-style comments, which start with dash-dash, have similar interactions
 * with the operator rule.
 */
xcstart			\/\*{op_chars}*
xcstop			\*+\/
xcinside 		[^*/]+

digit			[0-9]
letter			[\200-\377_A-Za-z]
letter_or_digit	[\200-\377_A-Za-z0-9]

identifier		{letter}{letter_or_digit}*

typecast		"::"

/*
 * "self" is the set of chars that should be returned as single-character
 * tokens.  "op_chars" is the set of chars that can make up "Op" tokens,
 * which can be one or more characters long (but if a single-char token
 * appears in the "self" set, it is not to be returned as an Op).  Note
 * that the sets overlap, but each has some chars that are not in the other.
 *
 * If you change either set, adjust the character lists appearing in the
 * rule for "operator"!
 */
self			[,()\[\].;$\:\+\-\*\/\%\^\<\>\=]
op_chars		[\~\!\@\#\^\&\|\`\?\$\+\-\*\/\%\<\>\=]
operator		{op_chars}+

/* we no longer allow unary minus in numbers. 
 * instead we pass it separately to parser. there it gets
 * coerced via doNegate() -- Leon aug 20 1999 
 */

integer			{digit}+
decimal			(({digit}*\.{digit}+)|({digit}+\.{digit}*))
real			((({digit}*\.{digit}+)|({digit}+\.{digit}*)|({digit}+))([Ee][-+]?{digit}+))

param			\${integer}

/*
 * In order to make the world safe for Windows and Mac clients as well as
 * Unix ones, we accept either \n or \r as a newline.  A DOS-style \r\n
 * sequence will be seen as two successive newlines, but that doesn't cause
 * any problems.  SQL92-style comments, which start with -- and extend to the
 * next newline, are treated as equivalent to a single whitespace character.
 *
 * NOTE a fine point: if there is no newline following --, we will absorb
 * everything to the end of the input as a comment.  This is correct.  Older
 * versions of Postgres failed to recognize -- as a comment if the input
 * did not end with a newline.
 *
 * XXX perhaps \f (formfeed) should be treated as a newline as well?
 */

ccomment		"//".*\n

space			[ \t\r\f]
space_or_nl		[ \t\r\f\n]
line_end                {space}*\n
horiz_space		[ \t\f]
newline                 [\n\r]
non_newline		[^\n\r]

comment         ("--"{non_newline}*)

whitespace 		({space}|{comment})

/*
 * SQL92 requires at least one newline in the whitespace separating
 * string literals that are to be concatenated.  Silly, but who are we
 * to argue?  Note that {whitespace_with_newline} should not have * after
 * it, whereas {whitespace} should generally have a * after it...
 */

horiz_whitespace	({horiz_space}|{comment})
whitespace_with_newline	({horiz_whitespace}*{newline}{whitespace}*)

other			.

/* some stuff needed for ecpg */
exec    [eE][xX][eE][cC]
sql     [sS][qQ][lL]
define	[dD][eE][fF][iI][nN][eE]
include [iI][nN][cC][lL][uU][dD][eE]

ifdef	[iI][fF][dD][eE][fF]
ifndef	[iI][fF][nN][dD][eE][fF]
else	[eE][lL][sS][eE]
elif	[eE][lL][iI][fF]
endif	[eE][nN][dD][iI][fF]

exec_sql		{exec}{space_or_nl}*{sql}{space_or_nl}*
ipdigit			({digit}|{digit}{digit}|{digit}{digit}{digit})
ip			{ipdigit}\.{ipdigit}\.{ipdigit}\.{ipdigit}

/* Take care of cpp continuation lines */
cppline			{space}*#(.*\\{line_end})*.*

/* DO NOT PUT ANY COMMENTS IN THE FOLLOWING SECTION.
 * AT&T lex does not properly handle C-style comments in this second lex block.
 * So, put comments here. thomas - 1997-09-08
 *
 * Quoted strings must allow some special characters such as single-quote
 *  and newline.
 * Embedded single-quotes are implemented both in the SQL92-standard
 *  style of two adjacent single quotes "''" and in the Postgres/Java style
 *  of escaped-quote "\'".
 * Other embedded escaped characters are matched explicitly and the leading
 *  backslash is dropped from the string. - thomas 1997-09-24
 * Note that xcstart must appear before operator, as explained above!
 *  Also whitespace (comment) must appear before operator.
 */

%%
<SQL>{whitespace}	{ /* ignore */ }

{xcstart}		{
				state_before = YYSTATE;
				xcdepth = 0;
	 			BEGIN(xc);
				/* Put back any characters past slash-star; see above */
				yyless(2);
				fputs("/*", yyout);
			}

<xc>{xcstart}   {
                        xcdepth++;
                        /* Put back any characters past slash-star; see above */
                        yyless(2);
			fputs("/*", yyout);
                }

<xc>{xcstop}    {
			ECHO;
                        if (xcdepth <= 0)
                        	BEGIN(state_before);
                        else
                                xcdepth--;
		}

<xc>{xcinside}	{ ECHO; }
<xc>{op_chars}  { ECHO; }

<xc><<EOF>>            { mmerror(ET_ERROR, "Unterminated /* comment"); }

<SQL>{xbitstart}		{
					BEGIN(xbit);
					startlit();
				}
<xbit>{xbitstop}		{
					BEGIN(SQL);
					if (literalbuf[strspn(literalbuf, "01") + 1] != '\0')
						mmerror(ET_ERROR, "invalid bit string input.");
					yylval.str = literalbuf;
			                return BITCONST;
				}
 
<xh>{xhinside}	|
<xbit>{xbitinside}	{
					addlit(yytext, yyleng);
				}
<xh>{xhcat}		|
<xbit>{xbitcat}		{
					/* ignore */
				}
<xbit><<EOF>>		{ mmerror(ET_ERROR, "Unterminated bit string"); }

<SQL>{xhstart}		{
					BEGIN(xh);
					startlit();
				}
<xh>{xhstop}			{
					char* endptr;

					BEGIN(SQL);
					errno = 0;
					yylval.ival = strtol(literalbuf, &endptr, 16);
					if (*endptr != '\0' || errno == ERANGE)
						mmerror(ET_ERROR, "Bad hexadecimal integer input");
					return ICONST;
				}

<xh><<EOF>>		{ mmerror(ET_ERROR, "Unterminated hexadecimal integer"); }

{xqstart}			{
					state_before = YYSTATE;
					BEGIN(xq);
					startlit();
				}
<xq>{xqstop}			{
					BEGIN(state_before);
					yylval.str = mm_strdup(literalbuf);
					return SCONST;
				}
<xq>{xqdouble}	|
<xq>{xqinside}	|
<xq>{xqliteral} 		{
					addlit(yytext, yyleng);
				}
<xq>{xqcat}			{
					/* ignore */
				}

<xq><<EOF>> 	 	        { mmerror(ET_ERROR, "Unterminated quoted string"); }

<SQL>{xdstart}			{
					state_before = YYSTATE;
					BEGIN(xd);
					startlit();
				}
<xd>{xdstop}		{
					BEGIN(state_before);
					if (strlen(literalbuf) >= NAMEDATALEN)
       			                {
#ifdef MULTIBYTE_NOTUSED
                                                int len;

	                                        len = pg_mbcliplen(literalbuf,strlen(literalbuf),NAMEDATALEN-1);
						sprintf(errortext, "identifier \"%s\" will be truncated to \"%.*s\"",
                                                        literalbuf, len, literalbuf);
	                                        literalbuf[len] = '\0';
#else
						sprintf(errortext, "identifier \"%s\" will be truncated to \"%.*s\"",
                                                        literalbuf, NAMEDATALEN-1, literalbuf);
                                                literalbuf[NAMEDATALEN-1] = '\0';
#endif
 						mmerror(ET_NOTICE, errortext);
                                        }

					yylval.str = mm_strdup(literalbuf);
					return CSTRING;
				}
<xdc>{xdstop}			{
					BEGIN(state_before);   
					yylval.str = mm_strdup(literalbuf);
					return CSTRING;
				}
<xd>{xddouble} 			{
					addlit(yytext, yyleng-1);
				}
<xd>{xdinside}			{
					addlit(yytext, yyleng);
				}
<xd,xdc><<EOF>> 	 	        { mmerror(ET_ERROR, "Unterminated quoted identifier"); }
{xdstart}			{
					state_before = YYSTATE;
					BEGIN(xdc);
					startlit();
				}
<xdc>{xdcinside}		{
 					addlit(yytext, yyleng);
				}
<SQL>{typecast}			{ 	return TYPECAST; }
<SQL>{self}			{ /* 
				   * We may find a ';' inside a structure
				   * definition in a TYPE or VAR statement.
				   * This is not an EOL marker.
				   */
				  if (yytext[0] == ';' && struct_level == 0)
					 BEGIN C;
				  return yytext[0];
				}
<SQL>{operator}			{
					/*
					 * Check for embedded slash-star or dash-dash; those
					 * are comment starts, so operator must stop there.
					 * Note that slash-star or dash-dash at the first
					 * character will match a prior rule, not this one.
					 */
					int		nchars = yyleng;
					char   *slashstar = strstr((char*)yytext, "/*");
					char   *dashdash = strstr((char*)yytext, "--");

					if (slashstar && dashdash)
					{
						/* if both appear, take the first one */
						if (slashstar > dashdash)
							slashstar = dashdash;
					}
					else if (!slashstar)
						slashstar = dashdash;
					if (slashstar)
						nchars = slashstar - ((char*)yytext);

					/*
					 * For SQL92 compatibility, '+' and '-' cannot be the
					 * last char of a multi-char operator unless the operator
					 * contains chars that are not in SQL92 operators.
					 * The idea is to lex '=-' as two operators, but not
					 * to forbid operator names like '?-' that could not be
					 * sequences of SQL92 operators.
					 */
					while (nchars > 1 &&
						   (yytext[nchars-1] == '+' ||
							yytext[nchars-1] == '-'))
					{
						int		ic;

						for (ic = nchars-2; ic >= 0; ic--)
						{
							if (strchr("~!@#^&|`?$%", yytext[ic]))
								break;
						}
						if (ic >= 0)
							break; /* found a char that makes it OK */
						nchars--; /* else remove the +/-, and check again */
					}

					if (nchars < yyleng)
					{
						/* Strip the unwanted chars from the token */
						yyless(nchars);
						/*
						 * If what we have left is only one char, and it's
						 * one of the characters matching "self", then
						 * return it as a character token the same way
						 * that the "self" rule would have.
						 */
						if (nchars == 1 &&
							strchr(",()[].;$:+-*/%^<>=", yytext[0]))
							return yytext[0];
					}

					/* Convert "!=" operator to "<>" for compatibility */
					if (strcmp((char*)yytext, "!=") == 0)
						yylval.str = mm_strdup("<>");
					else
						yylval.str = mm_strdup((char*)yytext);
					return Op;
				}
<SQL>{param}			{
					yylval.ival = atol((char*)&yytext[1]);
					return PARAM;
				}
<C,SQL>{integer}		{
					char* endptr;

					errno = 0;
					yylval.ival = strtol((char *)yytext, &endptr,10);
					if (*endptr != '\0' || errno == ERANGE)
					{
						errno = 0;
						yylval.str = mm_strdup((char*)yytext);
						return FCONST;
					}
					return ICONST;
				}
<SQL>{ip}			{
					yylval.str = mm_strdup((char*)yytext);
					return IP;
				}
{decimal}		{
					yylval.str = mm_strdup((char*)yytext);
					return FCONST;
				}
<C,SQL>{real}			{
					yylval.str = mm_strdup((char*)yytext);
					return FCONST;
				}
<SQL>:{identifier}(("->"|\.){identifier})*	{
					yylval.str = mm_strdup((char*)yytext+1);
					return(CVARIABLE);
			}
<SQL>{identifier}	{
					int i;
					ScanKeyword		*keyword;
					char lower_text[NAMEDATALEN];

					/* this should leave the last byte set to '\0' */
					strncpy(lower_text, yytext, NAMEDATALEN-1);
					for(i = 0; lower_text[i]; i++)
						if (isupper((unsigned char) lower_text[i]))
							lower_text[i] = tolower((unsigned char) lower_text[i]);

					if (i >= NAMEDATALEN)
					{
#ifdef MULTIBYTE_NOTUSED
                                                int len;

	                                        len = pg_mbcliplen(lower_text,strlen(lower_text),NAMEDATALEN-1);
						sprintf(errortext, "identifier \"%s\" will be truncated to \"%.*s\"",
                                                        lower_text, len, lower_text);
	                                        lower_text[len] = '\0';
#else
						sprintf(errortext, "identifier \"%s\" will be truncated to \"%.*s\"",
                                                        lower_text, NAMEDATALEN-1, lower_text);
                                                lower_text[NAMEDATALEN-1] = '\0';
#endif
 						mmerror(ET_NOTICE, errortext);
                                                yytext[NAMEDATALEN-1] = '\0';
					}

					keyword = ScanKeywordLookup((char*)lower_text);
					if (keyword != NULL) {
						return keyword->value;
					}
					else
					{
						keyword = ScanECPGKeywordLookup((char*)lower_text);
						if (keyword != NULL) {
							return keyword->value;
						}
						else
						{
							struct _defines *ptr;

							for (ptr = defines; ptr; ptr = ptr->next)
							{
								if (strcmp(yytext, ptr->old) == 0)
								{
									struct _yy_buffer *yb;

									yb = mm_alloc(sizeof(struct _yy_buffer));

						                        yb->buffer =  YY_CURRENT_BUFFER;
						                        yb->lineno = yylineno;
						                        yb->filename = mm_strdup(input_filename);
						                        yb->next = yy_buffer;

						                        yy_buffer = yb;

 									yy_scan_string(ptr->new);
									break;
								}
							}
							if (ptr == NULL) 
							{
								yylval.str = mm_strdup((char*)yytext);
								return IDENT;
							}
						}
					}
				}
<SQL>{other}			{ return yytext[0]; }
<C>{exec_sql}			{ BEGIN SQL; return SQL_START; }
<C>{ccomment}			{ /* ignore */ } 
<C>{xch}			{
					char* endptr;

					errno = 0;
					yylval.ival = strtol((char *)yytext,&endptr,16);
					if (*endptr != '\0' || errno == ERANGE)
					{
						errno = 0;
						yylval.str = mm_strdup((char*)yytext);
						return SCONST;
					}
					return ICONST;
				}
<C>{cppline}			{
					yylval.str = mm_strdup((char*)yytext);
					return(CPP_LINE);
				}
<C>{identifier}	{
					ScanKeyword		*keyword;

					keyword = ScanCKeywordLookup((char*)yytext);
					if (keyword != NULL) {
						return keyword->value;
					}
					else
					{
						struct _defines *ptr;

						for (ptr = defines; ptr; ptr = ptr->next)
						{
							if (strcmp(yytext, ptr->old) == 0)
							{
								struct _yy_buffer *yb;

								yb = mm_alloc(sizeof(struct _yy_buffer));

					                        yb->buffer =  YY_CURRENT_BUFFER;
					                        yb->lineno = yylineno;
					                        yb->filename = mm_strdup(input_filename);
					                        yb->next = yy_buffer;

					                        yy_buffer = yb;

								yy_scan_string(ptr->new);
								break;
							}
						}
						if (ptr == NULL) 
						{
							yylval.str = mm_strdup((char*)yytext);
							return IDENT;
						}
					}
				}
<C>";"	      	        { return(';'); }
<C>","	      	        { return(','); }
<C>"*"	      	        { return('*'); }
<C>"%"	      	        { return('%'); }
<C>"/"	      	        { return('/'); }
<C>"+"	      	        { return('+'); }
<C>"-"	      	        { return('-'); }
<C>"("	      	        { return('('); }
<C>")"	      	        { return(')'); }
<C>{space_or_nl}	{ ECHO; }
<C>\{			{ return('{'); }
<C>\}			{ return('}'); }
<C>\[			{ return('['); }
<C>\]			{ return(']'); }
<C>\=			{ return('='); }
<C>"->"			{ return(S_MEMBER); } 
<C>">>"			{ return(S_RSHIFT); } 
<C>"<<"			{ return(S_LSHIFT); } 
<C>"||"			{ return(S_OR); } 
<C>"&&"			{ return(S_AND); } 
<C>"++"			{ return(S_INC); } 
<C>"--"			{ return(S_DEC); } 
<C>"=="			{ return(S_EQUAL); } 
<C>"!="			{ return(S_NEQUAL); } 
<C>"+="			{ return(S_ADD); } 
<C>"-="			{ return(S_SUB); } 
<C>"*="			{ return(S_MUL); } 
<C>"/="			{ return(S_DIV); } 
<C>"%="			{ return(S_MOD); } 
<C>"->*"		{ return(S_MEMPOINT); } 
<C>".*"			{ return(S_DOTPOINT); } 
<C>{other}		{ return S_ANYTHING; }

<C>{exec_sql}{define}{space_or_nl}*	{ BEGIN(def_ident); }
<C>{exec_sql}{include}{space_or_nl}*	{ BEGIN(incl); }

<C,xskip>{exec_sql}{ifdef}{space_or_nl}* 	{ ifcond = TRUE; BEGIN(xcond); }
<C,xskip>{exec_sql}{ifndef}{space_or_nl}*	{ ifcond = FALSE; BEGIN(xcond); }

<C,xskip>{exec_sql}{elif}{space_or_nl}*	{	/* pop stack */
						if ( preproc_tos == 0 ) {
						    mmerror(ET_FATAL, "Missing matching 'EXEC SQL IFDEF / EXEC SQL IFNDEF'");
						}
						else if ( stacked_if_value[preproc_tos].else_branch ) {
						    mmerror(ET_FATAL, "Missing 'EXEC SQL ENDIF;'");
						}
						else {
						    preproc_tos--;
						}

						ifcond = TRUE; BEGIN(xcond);
					}

<C,xskip>{exec_sql}{else}{space_or_nl}*";" {	/* only exec sql endif pops the stack, so take care of duplicated 'else' */
						if ( stacked_if_value[preproc_tos].else_branch ) {
						    mmerror(ET_FATAL, "Duplicated 'EXEC SQL ELSE;'");
						}
						else {
						    stacked_if_value[preproc_tos].else_branch = TRUE;
						    stacked_if_value[preproc_tos].condition = 
							(stacked_if_value[preproc_tos-1].condition &&
							 ! stacked_if_value[preproc_tos].condition);

						    if ( stacked_if_value[preproc_tos].condition ) {
							BEGIN(C);
						    }
						    else {
							BEGIN(xskip);
						    }
						}
					}
<C,xskip>{exec_sql}{endif}{space_or_nl}*";" { 
						if ( preproc_tos == 0 ) {
						    mmerror(ET_FATAL, "Unmatched 'EXEC SQL ENDIF;'");
						}
						else {
						    preproc_tos--;
						}

						if ( stacked_if_value[preproc_tos].condition ) {
						   BEGIN(C);
						}
						else {
						   BEGIN(xskip);
						}
					}

<xskip>{other}			{ /* ignore */ }

<xcond>{identifier}{space_or_nl}*";" {
					if ( preproc_tos >= MAX_NESTED_IF-1 ) {
					    mmerror(ET_FATAL, "Too many nested 'EXEC SQL IFDEF' conditions");
					}
					else {
					    struct _defines *defptr;
					    unsigned int i;

					    /* skip the ";" and trailing whitespace. Note that yytext contains
					       at least one non-space character plus the ";" */
					    for ( i = strlen(yytext)-2;
							  i > 0 && isspace((unsigned char) yytext[i]);
							  i-- )
						{}
					    yytext[i+1] = '\0';

					    for ( defptr = defines; defptr != NULL &&
						      ( strcmp((char*)yytext, defptr->old) != 0 ); defptr = defptr->next );

					    preproc_tos++;
					    stacked_if_value[preproc_tos].else_branch = FALSE;
					    stacked_if_value[preproc_tos].condition = 
						( (defptr ? ifcond : !ifcond) && stacked_if_value[preproc_tos-1].condition );
					}

					if ( stacked_if_value[preproc_tos].condition ) {
					   BEGIN C;
					}
					else {
					   BEGIN(xskip);
					}
				}

<def_ident>{identifier}	{
				old = mm_strdup(yytext);
				BEGIN(def);
				startlit();
			}
<def>{space_or_nl}*";"	{
				struct _defines *ptr, *this;
        
                                for (ptr = defines; ptr != NULL; ptr = ptr->next)
                                {
                                     if (strcmp(old, ptr->old) == 0)
                                     {
					free(ptr->new);
					/* ptr->new = mm_strdup(scanstr(literalbuf));*/
					ptr->new = mm_strdup(literalbuf);
                                     }
                                }
				if (ptr == NULL)
				{                        
                                        this = (struct _defines *) mm_alloc(sizeof(struct _defines));

                                        /* initial definition */
                                        this->old = old;
                                        /* this->new = mm_strdup(scanstr(literalbuf));*/
                                        this->new = mm_strdup(literalbuf);
					this->next = defines;
					defines = this;
				}

				BEGIN(C);
			}
<def>[^";"]		{
				addlit(yytext, yyleng);
			}

<incl>[^";"]+";" 	{ /* got the include file name */
			  struct _yy_buffer *yb;
			  struct _include_path *ip;
			  char inc_file[MAXPGPATH];
			  unsigned int i;

			  yb = mm_alloc(sizeof(struct _yy_buffer));

			  yb->buffer =  YY_CURRENT_BUFFER;
			  yb->lineno = yylineno;
			  yb->filename = input_filename;
			  yb->next = yy_buffer;

			  yy_buffer = yb;

			  /* skip the ";" and trailing whitespace. Note that yytext contains
			     at least one non-space character plus the ";" */
			  for ( i = strlen(yytext)-2;
					i > 0 && isspace((unsigned char) yytext[i]);
					i-- )
			  {}
			  yytext[i+1] = '\0';

			  yyin = NULL;
			  for (ip = include_paths; yyin == NULL && ip != NULL; ip = ip->next)
			  {
				if (strlen(ip->path) + strlen(yytext) + 3 > MAXPGPATH)
				{
					fprintf(stderr, "Error: Path %s/%s is too long in line %d, skipping.\n", ip->path, yytext, yylineno);
					continue;
				}
			  	sprintf (inc_file, "%s/%s", ip->path, yytext);
		          	yyin = fopen( inc_file, "r" );
			  	if (!yyin)
				{
					if (strcmp(inc_file + strlen(inc_file) - 2, ".h"))
					{
						strcat(inc_file, ".h");
						yyin = fopen( inc_file, "r" );
					}

				}
			  }
			  if (!yyin)
			  {
				fprintf(stderr, "Error: Cannot open include file %s in line %d\n", yytext, yylineno);
				exit(NO_INCLUDE_FILE); 
			  }

			  input_filename = mm_strdup(inc_file);
			  yy_switch_to_buffer(yy_create_buffer(yyin,YY_BUF_SIZE ));
			  yylineno = 1;
			  output_line_number();

			  BEGIN C;
			}

<<EOF>>			{
			  if ( preproc_tos > 0 ) {
			      preproc_tos = 0;

			      mmerror(ET_FATAL, "Missing 'EXEC SQL ENDIF;'");
			  }

			  if (yy_buffer == NULL)
				yyterminate();
			  else
			  {
				struct _yy_buffer *yb = yy_buffer;

				if (yyin != NULL)
					fclose(yyin);

				yy_delete_buffer( YY_CURRENT_BUFFER );
				yy_switch_to_buffer(yy_buffer->buffer);

				yylineno = yy_buffer->lineno;

				free(input_filename);
				input_filename = yy_buffer->filename;

				yy_buffer = yy_buffer->next;
				free(yb);
				output_line_number();
			  }
			}
%%
void
lex_init(void)
{
	braces_open = 0;

	preproc_tos = 0;
	yylineno = 0;
	ifcond = TRUE;
	stacked_if_value[preproc_tos].condition = ifcond;
	stacked_if_value[preproc_tos].else_branch = FALSE;

	/* initialize literal buffer to a reasonable but expansible size */
	if (literalbuf == NULL)
	{
		literalalloc = 128;
		literalbuf = (char *) malloc(literalalloc);
	}
	startlit();

    BEGIN C;
}

static void
addlit(char *ytext, int yleng)
{
	/* enlarge buffer if needed */
	if ((literallen+yleng) >= literalalloc)
	{
		do {
			literalalloc *= 2;
		} while ((literallen+yleng) >= literalalloc);
		literalbuf = (char *) realloc(literalbuf, literalalloc);
	}
	/* append data --- note we assume ytext is null-terminated */
	memcpy(literalbuf+literallen, ytext, yleng+1);
	literallen += yleng;
}

int yywrap(void) 
{
    return 1;
}

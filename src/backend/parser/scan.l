%{
/*-------------------------------------------------------------------------
 *
 * scan.l--
 *    lexical scanner for POSTGRES
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/parser/scan.l,v 1.4 1996/11/11 04:54:45 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <ctype.h>
#ifndef WIN32
#include <unistd.h>
#endif /* WIN32 */
#ifndef __linux__
#include <math.h>
#else
#include <stdlib.h>
#endif /* __linux__ */
#include <string.h>

#include "postgres.h"
#include "miscadmin.h"
#include "nodes/pg_list.h"
#include "nodes/parsenodes.h"
#include "parser/gramparse.h"
#include "parser/keywords.h"
#include "parser/scansup.h"
#include "parser/sysfunc.h"
#include "parse.h"

extern char *parseString;
extern char *parseCh;

/* some versions of lex define this as a macro */
#if defined(yywrap)
#undef yywrap
#endif /* yywrap */

#if defined(FLEX_SCANNER)
/* MAX_PARSE_BUFFER is defined in miscadmin.h */
#define YYLMAX MAX_PARSE_BUFFER
extern int myinput(char* buf, int max);
#undef YY_INPUT
#define YY_INPUT(buf,result,max) {result = myinput(buf,max);}
#else
#undef input
int input();
#undef unput
void unput(char);
#endif /* FLEX_SCANNER */

extern YYSTYPE yylval;
%}

digit		[0-9]
letter		[_A-Za-z]
letter_or_digit	[_A-Za-z0-9]

sysfunc		SYS_{letter}{letter_or_digit}*

identifier	{letter}{letter_or_digit}*

self		[,()\[\].;$\:\+\-\*\/\<\>\=\|]
op_and_self	[\~\!\@\#\%\^\&\|\`\?\$\:\+\-\*\/\<\>\=]
op_only		[\~\!\@\#\%\^\&\`\?]

operator	({op_and_self}{op_and_self}+)|{op_only}+
    /* we used to allow double-quoted strings, but SQL doesn't */
    /* so we won't either*/
quote           '

integer		-?{digit}+
real		-?{digit}+\.{digit}+([Ee][-+]?{digit}+)?

param		\${integer}

comment		"--".*\n

space		[ \t\n\f]
other		.

%%
{sysfunc}	{
			yylval.str = pstrdup(SystemFunctionHandler((char *)yytext));
                	return (SCONST);
		}

{comment}	{ /* ignore */	}

"::"		{ return TYPECAST;	}

{self}		{ return (yytext[0]);	}

{operator}	{
			yylval.str = pstrdup((char*)yytext);
			return (Op);
		}
{param}	        {       yylval.ival = atoi((char*)&yytext[1]);		
	                return (PARAM);
                }
{integer}	{
			yylval.ival = atoi((char*)yytext);		
			return (ICONST);
		}
{real}		{
			yylval.dval = atof((char*)yytext);
			return (FCONST);
		}
{quote}	        {       
                        char literal[MAX_PARSE_BUFFER];
                        int i = 0;
                        int c = 0;
                        /* quote_seen can be either \ or ' because
                           we handle both cases of \' and '' for
                           quoting quotes*/
                        int quote_seen = 0; 
                                             
                        while (i < MAX_PARSE_BUFFER - 1) {
                            c = input();
                            if (quote_seen != 0) {
                                 if (quote_seen == '\'' &&
                                     c != '\'') {
                                    /* a non-quote follows a single quote */
                                    /* so we've hit the end of the literal */
                                    if (c != '\0' && c != EOF)
                                      unput(c); /* put back the extra char we read*/
                                    i = i - 1;
                                    break; /* break out of the while loop */
                                 }  
                                 /* if we reach here, we're still in */
                                 /* the string literal */
                                 literal[i++] = c;
                                 quote_seen = 0;
                                 continue;
                            }
                            if (c == '\0' || c == EOF) {
                               elog(WARN,"unterminated quoted string literal");
                               /* not reached */
                            }
                            literal[i++] = c;
                            if (c == '\'' || c == '\\')
                               quote_seen = c;
                        }
                        if ( i == MAX_PARSE_BUFFER - 1) {
                           elog (WARN, "unterminated quote string.  parse buffer of %d chars exceeded", MAX_PARSE_BUFFER);
                           /* not reached */
		       }
                        literal[i] = '\0';
			yylval.str = pstrdup(scanstr(literal));
	                return (SCONST); 
		    }
{identifier}	{
			ScanKeyword	*keyword;

			keyword = ScanKeywordLookup((char*)yytext);
			if (keyword != NULL) {
				return (keyword->value);
			} else {
				yylval.str = pstrdup((char*)yytext);
				return (IDENT);
			}
		}
{space}		{ /* ignore */		}

{other}		{ return (yytext[0]);	}

%%

void yyerror(char message[])
{
    elog(WARN, "parser: %s at or near \"%s\"\n", message, yytext);
}

int yywrap()
{
    return(1);
}

/*
 init_io:
    called by postgres before any actual parsing is done
*/
void
init_io()
{
    /* it's important to set this to NULL
       because input()/myinput() checks the non-nullness of parseCh
       to know when to pass the string to lex/flex */
    parseCh = NULL;
#if defined(FLEX_SCANNER)
    if (YY_CURRENT_BUFFER)
	yy_flush_buffer(YY_CURRENT_BUFFER);
#endif /* FLEX_SCANNER */
    BEGIN INITIAL;
}


#if !defined(FLEX_SCANNER)
/* get lex input from a string instead of from stdin */
int
input()
{
    if (parseCh == NULL) {
	parseCh = parseString;
	return(*parseCh++);
    } else if (*parseCh == '\0') {
	return(0);
    } else {
	return(*parseCh++);
    }
}

/* undo lex input from a string instead of from stdin */
void
unput(char c)
{
    if (parseCh == NULL) {
	elog(FATAL, "Unput() failed.\n");
    } else if (c != 0) {
	*--parseCh = c;
    }
}
#endif /* !defined(FLEX_SCANNER) */

#ifdef FLEX_SCANNER
/* input routine for flex to read input from a string instead of a file */
int 
myinput(char* buf, int max)
{
    int len, copylen;

    if (parseCh == NULL) {
	len = strlen(parseString);
	if (len >= max)
	    copylen = max - 1;
	else
	    copylen = len;
	if (copylen > 0)
	    memcpy(buf, parseString, copylen);
	buf[copylen] = '\0';
	parseCh = parseString;
	return copylen;
    } else {
	return 0; /* end of string */
    }
}

#ifdef NOT_USED
char*
CurScan(void)
{
/*
    return (InputFrag ? InputFrag : parseCh) +
	   (yy_c_buf_p - &yy_current_buffer->yy_ch_buf[yy_n_chars]);
*/
}
#endif

#endif /* FLEX_SCANNER */


%{
/*-------------------------------------------------------------------------
 *
 * pgc.l
 *	  lexical scanner for ecpg
 *
 * This is a modified version of src/backend/parser/scan.l
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/interfaces/ecpg/preproc/pgc.l,v 1.46 1999/10/25 03:07:59 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <ctype.h>
#include <sys/types.h>
#include <limits.h>
#include <errno.h>

#include "postgres.h"

#include "miscadmin.h"
#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"
#include "parser/gramparse.h"
#include "parser/scansup.h"
#include "extern.h"
#include "preproc.h"
#include "utils/builtins.h"

/* some versions of lex define this as a macro */
#if defined(yywrap)
#undef yywrap
#endif /* yywrap */

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

#define startlit()  (literalbuf[0] = '\0', literallen = 0)
static void addlit(char *ytext, int yleng);

int before_comment;

struct _yy_buffer { YY_BUFFER_STATE 	buffer;
		    long		lineno;
		    char	      * filename;
		    struct _yy_buffer * next;
		  } *yy_buffer = NULL;

struct _defines *defines = NULL;
static char *old;

%}
%option yylineno
%s C SQL incl def def_ident
/* OK, here is a short description of lex/flex rules behavior.
 * The longest pattern which matches an input string is always chosen.
 * For equal-length patterns, the first occurring in the rules list is chosen.
 * INITIAL is the starting condition, to which all non-conditional rules apply.
 * When in an exclusive condition, only those rules defined for that condition apply.
 *
 * Exclusive states change parsing rules while the state is active.
 * There are exclusive states for quoted strings, extended comments,
 *  and to eliminate parsing troubles for numeric strings.
 * Exclusive states:
 *  <xb> binary numeric string - thomas 1997-11-16
 *  <xc> extended C-style comments - tgl 1997-07-12
 *  <xd> delimited identifiers (double-quoted identifiers) - tgl 1997-10-27
 *  <xh> hexadecimal numeric string - thomas 1997-11-16
 *  <xq> quoted strings - tgl 1997-07-30
 *
 * The "extended comment" syntax closely resembles allowable operator syntax.
 * So, when in condition <xc>, only strings which would terminate the
 *  "extended comment" trigger any action other than "ignore".
 * Be sure to match _any_ candidate comment, including those with appended
 *	operator-like symbols. - thomas 1997-07-14
 */

%x xb
%x xc
%x xd
%x xdc
%x xh
%x xq

/* Binary number
 */
xbstart			[bB]{quote}
xbstop			{quote}
xbinside		[^']*
xbcat			{quote}{space}*\n{space}*{quote}

/* Hexadecimal number
 */
xhstart			[xX]{quote}
xhstop			{quote}
xhinside		[^']*
xhcat			{quote}{space}*\n{space}*{quote}

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
xqinside		[^\\']*
xqliteral		[\\](.|\n)
xqcat			{quote}{space}*\n{space}*{quote}

/* Delimited quote
 * Allows embedded spaces and other special characters into identifiers.
 */
dquote			\"
xdstart			{dquote}
xdstop			{dquote}
xdinside		[^"]*

/* special stuff for C strings */
xdcqq			\\\\
xdcqdq			\\\"
xdcother		[^"]
xdcinside		({xdcqq}|{xdcqdq}|{xdcother})

/* Comments
 * Ignored by the scanner and parser.
 */
xcline			[\/][\*].*[\*][\/]{space}*\n*
xcstart			[\/][\*]{op_and_self}*
xcstop			{op_and_self}*[\*][\/]({space}*|\n)
xcinside		[^*]*
xcstar			[^/]

digit			[0-9]
letter			[\200-\377_A-Za-z]
letter_or_digit	[\200-\377_A-Za-z0-9]

identifier		{letter}{letter_or_digit}*

typecast		"::"

self			[,()\[\].;$\:\+\-\*\/\%\^\<\>\=\|]
op_and_self		[\~\!\@\#\^\&\|\`\?\$\:\+\-\*\/\%\<\>\=]
operator		{op_and_self}+

/* we no longer allow unary minus in numbers. 
 * instead we pass it separately to parser. there it gets
 * coerced via doNegate() -- Leon aug 20 1999 
 */

integer			{digit}+
decimal			(({digit}*\.{digit}+)|({digit}+\.{digit}*))
real				((({digit}*\.{digit}+)|({digit}+\.{digit}*)|({digit}+))([Ee][-+]?{digit}+))

param			\${integer}

comment			("--"|"//").*
ccomment		"//".*\n

space			[ \t\n\r\f]
other			.

/* some stuff needed for ecpg */
exec    [eE][xX][eE][cC]
define	[dD][eE][fF][iI][nN][eE]
include [iI][nN][cC][lL][uU][dD][eE]
sql     [sS][qQ][lL]

cppline		{space}*#.*(\\{space}*\n)*\n*

/* DO NOT PUT ANY COMMENTS IN THE FOLLOWING SECTION.
 * AT&T lex does not properly handle C-style comments in this second lex block.
 * So, put comments here. tgl - 1997-09-08
 *
 * Quoted strings must allow some special characters such as single-quote
 *  and newline.
 * Embedded single-quotes are implemented both in the SQL/92-standard
 *  style of two adjacent single quotes "''" and in the Postgres/Java style
 *  of escaped-quote "\'".
 * Other embedded escaped characters are matched explicitly and the leading
 *  backslash is dropped from the string. - thomas 1997-09-24
 */

%%
<SQL>{comment}		{ /* ignore */ }

{xcline}		{ ECHO; }

<xc>{xcstar}		{ ECHO; }
{xcstart}		{
				before_comment = YYSTATE;
				ECHO;
	 			BEGIN(xc);
			}

<xc>{xcstop}	{ ECHO; BEGIN(before_comment); }

<xc>{xcinside}	{ ECHO; }

<SQL>{xbstart}		{
					BEGIN(xb);
					startlit();
				}
<xb>{xbstop}	{
					char* endptr;

					BEGIN(SQL);
					errno = 0;
					yylval.ival = strtol(literalbuf, &endptr, 2);
					if (*endptr != '\0' || errno == ERANGE)
						yyerror("ERROR: Bad binary integer input!");
					return ICONST;
				}
<xh>{xhinside}	|
<xb>{xbinside}	{
					addlit(yytext, yyleng);
				}
<xh>{xhcat}		|
<xb>{xbcat}		{
				}

<SQL>{xhstart}		{
					BEGIN(xh);
					startlit();
				}
<xh>{xhstop}	{
					char* endptr;

					BEGIN(SQL);
					errno = 0;
					yylval.ival = strtol(literalbuf, &endptr, 16);
					if (*endptr != '\0' || errno == ERANGE)
						yyerror("ERROR: Bad hexadecimal integer input");
					return ICONST;
				}

<SQL>{xqstart}		{
					BEGIN(xq);
					startlit();
				}
<xq>{xqstop}	{
					BEGIN(SQL);
					yylval.str = mm_strdup(literalbuf);
					return SCONST;
				}
<xq>{xqdouble}	|
<xq>{xqinside}	|
<xq>{xqliteral} {
					addlit(yytext, yyleng);
				}
<xq>{xqcat}		{
				}


<SQL>{xdstart}		{
					BEGIN(xd);
					startlit();
				}
<xd>{xdstop}	{
					BEGIN(SQL);
					yylval.str = mm_strdup(literalbuf);
					return CSTRING;
				}
<xd>{xdinside}	{
					addlit(yytext, yyleng);
				}
{xdstart}		{
					BEGIN(xdc);
					startlit();
				}
<xdc>{xdstop}	{
					BEGIN(C);
					yylval.str = mm_strdup(literalbuf);
					return CSTRING;
				}
<xdc>{xdcinside}	{
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
<SQL>{operator}		{
					if (strcmp((char*)yytext,"!=") == 0)
						yylval.str = mm_strdup("<>"); /* compatability */
					else
						yylval.str = mm_strdup((char*)yytext);
					return Op;
				}
<SQL>{param}			{
					yylval.ival = atoi((char*)&yytext[1]);
					return PARAM;
				}
<C,SQL>{integer}		{
					char* endptr;

					errno = 0;
					yylval.ival = strtol((char *)yytext,&endptr,10);
					if (*endptr != '\0' || errno == ERANGE)
					{
						errno = 0;
						yylval.str = mm_strdup((char*)yytext);
						return SCONST;
					}
					return ICONST;
				}
{decimal}		{
					char* endptr;

					if (strlen((char *)yytext) <= 17)
					{
						errno = 0;
						yylval.dval = strtod((char *)yytext,&endptr);
						if (*endptr != '\0' || errno == ERANGE)
							yyerror("ERROR: Bad float8 input");
						return FCONST;
					}
					yylval.str = mm_strdup((char*)yytext);
					return SCONST;
				}
<C,SQL>{real}			{
					char* endptr;

					errno = 0;
					yylval.dval = strtod((char *)yytext,&endptr);
					if (*endptr != '\0' || errno == ERANGE)
						yyerror("ERROR: Bad float input");
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
						if (isascii((unsigned char)lower_text[i]) && isupper(lower_text[i]))
							lower_text[i] = tolower(lower_text[i]);

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
<SQL>{space}			{ /* ignore */ }
<SQL>{other}			{ return yytext[0]; }
<C>{exec}{space}*{sql}		{ BEGIN SQL; return SQL_START; }
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
<C>{space}		{ ECHO; }
<C>\{			{ return('{'); }
<C>\}			{ return('}'); }
<C>\[			{ return('['); }
<C>\]			{ return(']'); }
<C>\=			{ return('='); }
<C>{other}		{ return S_ANYTHING; }
<C>{exec}{space}{sql}{space}{define}	{BEGIN(def_ident);}
<def_ident>{space}	{}
<def_ident>{identifier}	{
				old = mm_strdup(yytext);
				BEGIN(def);
				startlit();
			}
<def>{space}		/* eat the whitespace */
<def>";"		{
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
<C>{exec}{space}{sql}{space}{include}	{ BEGIN(incl); }
<incl>{space}		/* eat the whitespace */
<incl>[^ \t\n]+ 	{ /* got the include file name */
			  struct _yy_buffer *yb;
			  struct _include_path *ip;
			  char inc_file[MAXPGPATH];

			  yb = mm_alloc(sizeof(struct _yy_buffer));

			  yb->buffer =  YY_CURRENT_BUFFER;
			  yb->lineno = yylineno;
			  yb->filename = input_filename;
			  yb->next = yy_buffer;

			  yy_buffer = yb;

			  if (yytext[strlen(yytext) - 1] == ';')
				yytext[strlen(yytext) - 1] = '\0';

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
			  yylineno = 0;
			  output_line_number();

			  BEGIN C;
			}
<incl>";"		{ BEGIN C; }
<<EOF>>			{ if (yy_buffer == NULL)
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

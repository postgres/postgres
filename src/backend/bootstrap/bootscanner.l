%{
/*-------------------------------------------------------------------------
 *
 * bootscanner.lex--
 *    a lexical scanner for the bootstrap parser
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/bootstrap/bootscanner.l,v 1.2 1996/08/28 07:14:09 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "bootstrap/bootstrap.h"
#include "utils/portal.h" 
#include "access/xact.h"
#include "parser/scansup.h"

#include "bootstrap_tokens.h"

/* some versions of lex define this as a macro */
#if defined(yywrap)
#undef yywrap
#endif /* yywrap */

YYSTYPE	yylval;
int	yyline;  /* keep track of the line number for error reporting */

%}

D	[0-9]
oct     \\{D}{D}{D}
Exp	[Ee][-+]?{D}+
id      ([A-Za-z0-9_]|{oct}|\-)+
sid     \"([^\"])*\"
arrayid	[A-Za-z0-9_]+\[{D}*\]

%%

open       	{ return(OPEN); }

close		{ return(XCLOSE); }

create		{ return(XCREATE); }

OID             { return(OBJ_ID); }
bootstrap	{ return(XBOOTSTRAP); }
_null_		{ return(NULLVAL); }

insert		{ return(INSERT_TUPLE); }

","     	{ return(COMMA); }
"="		{ return(EQUALS); }
"("		{ return(LPAREN); }
")"		{ return(RPAREN); }

[\n]      	{ yyline++; }
[\t]		;
" "		; 

^\#[^\n]* ; /* drop everything after "#" for comments */


"declare"	{ return(XDECLARE); }
"build"		{ return(XBUILD); }
"indices"	{ return(INDICES); }
"index"		{ return(INDEX); }
"on"		{ return(ON); }
"using"		{ return(USING); }
{arrayid}	{
		    yylval.ival = EnterString(MapArrayTypeName((char*)yytext));
		    return(ID);
		}
{id}	 	{ 
		    yylval.ival = EnterString(scanstr((char*)yytext));
		    return(ID);
		}
{sid}		{
		    yylval.ival = EnterString(scanstr((char*)yytext));
		    return(ID);
		}

(-)?{D}+"."{D}*({Exp})?	|
(-)?{D}*"."{D}+({Exp})?	|
(-)?{D}+{Exp}		{
			    yylval.ival = EnterString((char*)yytext);
			    return(CONST);
			}

.		{
		    printf("syntax error %d : -> %s\n", yyline, yytext);
		}



%%

yywrap()
{
    return 1;
}

yyerror(str)
    char *str;
{
    fprintf(stderr,"\tsyntax error %d : %s",yyline, str);
}

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
 *    $Header: /cvsroot/pgsql/src/backend/bootstrap/bootscanner.l,v 1.4 1996/10/23 07:39:39 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <time.h>

#include "postgres.h"

#include "storage/block.h"
#include "storage/off.h"
#include "storage/itemptr.h"
#include "catalog/pg_attribute.h"
#include "access/attnum.h"
#include "nodes/pg_list.h"
#include "access/tupdesc.h"
#include "access/itup.h"
#include "access/funcindex.h"
#include "storage/fd.h"
#include "catalog/pg_am.h"
#include "catalog/pg_class.h"
#include "nodes/nodes.h"  
#include "rewrite/prs2lock.h"
#include "access/skey.h"
#include "access/strat.h"
#include "utils/rel.h"
#include "bootstrap/bootstrap.h"

#include "nodes/primnodes.h"
#include "utils/nabstime.h"
#include "access/htup.h"  
#include "utils/tqual.h"
#include "nodes/parsenodes.h"

#include "parser/scansup.h"

#include "bootstrap_tokens.h"

#define		YY_NO_UNPUT

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

int
yywrap(void)
{
    return 1;
}

void
yyerror(const char *str)
{
    fprintf(stderr,"\tsyntax error %d : %s",yyline, str);
}

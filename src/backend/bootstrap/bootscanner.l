%{
/*-------------------------------------------------------------------------
 *
 * bootscanner.lex
 *	  a lexical scanner for the bootstrap parser
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/bootstrap/bootscanner.l,v 1.16 1999/11/04 08:00:58 inoue Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <time.h>

#include "postgres.h"

#include "access/attnum.h"
#include "access/funcindex.h"
#include "access/htup.h"
#include "access/itup.h"
#include "access/skey.h"
#include "access/strat.h"
#include "access/tupdesc.h"
#include "bootstrap/bootstrap.h"
#include "catalog/pg_am.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_class.h"
#include "nodes/nodes.h"
#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"
#include "nodes/primnodes.h"
#include "parser/scansup.h"
#include "rewrite/prs2lock.h"
#include "storage/block.h"
#include "storage/fd.h"
#include "storage/itemptr.h"
#include "storage/off.h"
#include "utils/nabstime.h"
#include "utils/rel.h"

#include "bootstrap_tokens.h"

#define			YY_NO_UNPUT

/* some versions of lex define this as a macro */
#if defined(yywrap)
#undef yywrap
#endif /* yywrap */

YYSTYPE yylval;
int		yyline;  /* keep track of the line number for error reporting */

%}

D		[0-9]
oct		\\{D}{D}{D}
Exp		[Ee][-+]?{D}+
id		([A-Za-z0-9_]|{oct}|\-)+
sid		\"([^\"])*\"
arrayid [A-Za-z0-9_]+\[{D}*\]

%%

open			{ return(OPEN); }

close			{ return(XCLOSE); }

create			{ return(XCREATE); }

OID				{ return(OBJ_ID); }
bootstrap		{ return(XBOOTSTRAP); }
_null_			{ return(NULLVAL); }

insert			{ return(INSERT_TUPLE); }

","				{ return(COMMA); }
"="				{ return(EQUALS); }
"("				{ return(LPAREN); }
")"				{ return(RPAREN); }

[\n]			{ yyline++; }
[\t]			;
" "				;

^\#[^\n]* ; /* drop everything after "#" for comments */


"declare"		{ return(XDECLARE); }
"build"			{ return(XBUILD); }
"indices"		{ return(INDICES); }
"unique"		{ return(UNIQUE); }
"index"			{ return(INDEX); }
"on"			{ return(ON); }
"using"			{ return(USING); }
{arrayid}		{
					yylval.ival = EnterString(MapArrayTypeName((char*)yytext));
					return(ID);
				}
{id}			{
					char   *newid = scanstr((char*)yytext);
					yylval.ival = EnterString(newid);
					pfree(newid);
					return(ID);
				}
{sid}			{
					char   *newid;
					yytext[strlen(yytext)-1] = '\0'; /* strip off quotes */
					newid = scanstr((char*)yytext+1);
					yylval.ival = EnterString(newid);
					pfree(newid);
					yytext[strlen(yytext)] = '"'; /* restore quotes */
					return(ID);
				}

(-)?{D}+"."{D}*({Exp})? |
(-)?{D}*"."{D}+({Exp})? |
(-)?{D}+{Exp}			{
							yylval.ival = EnterString((char*)yytext);
							return(CONST);
						}

.				{
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

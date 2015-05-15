%{
/*-------------------------------------------------------------------------
 *
 * bootscanner.l
 *	  a lexical scanner for the bootstrap parser
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/bootstrap/bootscanner.l
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/attnum.h"
#include "access/htup.h"
#include "access/itup.h"
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
#include "utils/rel.h"

/* Not needed now that this file is compiled as part of bootparse. */
/* #include "bootparse.h" */


/* Avoid exit() on fatal scanner errors (a bit ugly -- see yy_fatal_error) */
#undef fprintf
#define fprintf(file, fmt, msg)  fprintf_to_ereport(fmt, msg)

static void
fprintf_to_ereport(const char *fmt, const char *msg)
{
	ereport(ERROR, (errmsg_internal("%s", msg)));
}


static int	yyline = 1;			/* line number for error reporting */

%}

%option 8bit
%option never-interactive
%option nodefault
%option noinput
%option nounput
%option noyywrap
%option warn
%option prefix="boot_yy"


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
"shared_relation"	{ return(XSHARED_RELATION); }
"without_oids"	{ return(XWITHOUT_OIDS); }
"rowtype_oid"	{ return(XROWTYPE_OID); }
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
"toast"			{ return(XTOAST); }
"FORCE"			{ return(XFORCE); }
"NOT"			{ return(XNOT); }
"NULL"			{ return(XNULL); }

{arrayid}		{
					yylval.str = MapArrayTypeName(yytext);
					return(ID);
				}
{id}			{
					yylval.str = scanstr(yytext);
					return(ID);
				}
{sid}			{
					yytext[strlen(yytext)-1] = '\0'; /* strip off quotes */
					yylval.str = scanstr(yytext+1);
					yytext[strlen(yytext)] = '"'; /* restore quotes */
					return(ID);
				}

(-)?{D}+"."{D}*({Exp})? |
(-)?{D}*"."{D}+({Exp})? |
(-)?{D}+{Exp}			{
							yylval.str = pstrdup(yytext);
							return(CONST_P);
						}

.				{
					elog(ERROR, "syntax error at line %d: unexpected character \"%s\"", yyline, yytext);
				}



%%

void
yyerror(const char *message)
{
	elog(ERROR, "%s at line %d", message, yyline);
}

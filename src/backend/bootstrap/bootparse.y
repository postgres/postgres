%{
/*-------------------------------------------------------------------------
 *
 * backendparse.y--
 *	  yacc parser grammer for the "backend" initialization program.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/bootstrap/bootparse.y,v 1.19 1998/08/06 05:12:16 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <stdio.h>
#include <time.h>

#include "postgres.h"

#include "miscadmin.h"

#include "access/attnum.h"
#include "access/funcindex.h"
#include "access/htup.h"
#include "access/itup.h"
#include "access/skey.h"
#include "access/strat.h"
#include "access/tupdesc.h"
#include "access/xact.h"
#include "bootstrap/bootstrap.h"
#include "catalog/heap.h"
#include "catalog/pg_am.h"
#ifdef MULTIBYTE
#include "catalog/pg_attribute_mb.h"
#include "catalog/pg_class_mb.h"
#else
#include "catalog/pg_attribute.h"
#include "catalog/pg_class.h"
#endif
#include "commands/defrem.h"
#include "nodes/nodes.h"
#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"
#include "nodes/primnodes.h"
#include "rewrite/prs2lock.h"
#include "storage/block.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/itemptr.h"
#include "storage/off.h"
#include "storage/smgr.h"
#include "storage/spin.h"
#include "tcop/dest.h"
#include "utils/nabstime.h"
#include "utils/rel.h"

#define DO_START { \
					StartTransactionCommand();\
				 }

#define DO_END	 { \
					CommitTransactionCommand();\
					if (!Quiet) { EMITPROMPT; }\
						fflush(stdout); \
				 }

int num_tuples_read = 0;
static Oid objectid;

%}

%union
{
	List		*list;
	IndexElem	*ielem;
	char		*str;
	int			ival;
}

%type <list>  boot_arg_list
%type <ielem> boot_index_params boot_index_on
%type <ival> boot_const boot_ident
%type <ival> optbootstrap optoideq boot_tuple boot_tuplelist

%token <ival> CONST ID
%token OPEN XCLOSE XCREATE INSERT_TUPLE
%token STRING XDEFINE
%token XDECLARE INDEX ON USING XBUILD INDICES
%token COMMA EQUALS LPAREN RPAREN
%token OBJ_ID XBOOTSTRAP NULLVAL
%start TopLevel

%nonassoc low
%nonassoc high

%%

TopLevel:
		  Boot_Queries
		|
		;

Boot_Queries:
		  Boot_Query
		| Boot_Queries Boot_Query
		;

Boot_Query :
		  Boot_OpenStmt
		| Boot_CloseStmt
		| Boot_CreateStmt
		| Boot_InsertStmt
		| Boot_DeclareIndexStmt
		| Boot_BuildIndsStmt
		;

Boot_OpenStmt:
		  OPEN boot_ident
				{
					DO_START;
					boot_openrel(LexIDStr($2));
					DO_END;
				}
		;

Boot_CloseStmt:
		  XCLOSE boot_ident %prec low
				{
					DO_START;
					closerel(LexIDStr($2));
					DO_END;
				}
		| XCLOSE %prec high
				{
					DO_START;
					closerel(NULL);
					DO_END;
				}
		;

Boot_CreateStmt:
		  XCREATE optbootstrap boot_ident LPAREN
				{
					DO_START;
					numattr=(int)0;
				}
		  boot_typelist
				{
					if (!Quiet)
						putchar('\n');
					DO_END;
				}
		  RPAREN
				{
					DO_START;

					if ($2)
					{
						extern Relation reldesc;
						TupleDesc tupdesc;

						if (reldesc)
						{
							puts("create bootstrap: Warning, open relation");
							puts("exists, closing first");
							closerel(NULL);
						}
						if (DebugMode)
							puts("creating bootstrap relation");
						tupdesc = CreateTupleDesc(numattr,attrtypes);
						reldesc = heap_create(LexIDStr($3), tupdesc);
						if (DebugMode)
							puts("bootstrap relation created ok");
					}
					else
					{
						Oid id;
						TupleDesc tupdesc;

						tupdesc = CreateTupleDesc(numattr,attrtypes);
						id = heap_create_with_catalog(LexIDStr($3),
												tupdesc, RELKIND_RELATION);
						if (!Quiet)
							printf("CREATED relation %s with OID %d\n",
								   LexIDStr($3), id);
					}
					DO_END;
					if (DebugMode)
						puts("Commit End");
				}
		;

Boot_InsertStmt:
		  INSERT_TUPLE optoideq
				{
					DO_START;
					if (DebugMode)
						printf("tuple %d<", $2);
					num_tuples_read = 0;
				}
		  LPAREN  boot_tuplelist RPAREN
				{
					if (num_tuples_read != numattr)
						elog(ERROR,"incorrect number of values for tuple");
					if (reldesc == (Relation)NULL)
					{
						elog(ERROR,"must OPEN RELATION before INSERT\n");
						err_out();
					}
					if (DebugMode)
						puts("Insert Begin");
					objectid = $2;
					InsertOneTuple(objectid);
					if (DebugMode)
						puts("Insert End");
					if (!Quiet)
						putchar('\n');
					DO_END;
					if (DebugMode)
						puts("Transaction End");
				}
		;

Boot_DeclareIndexStmt:
		  XDECLARE INDEX boot_ident ON boot_ident USING boot_ident LPAREN boot_index_params RPAREN
				{
					List *params;

					DO_START;

					params = lappend(NIL, (List*)$9);
					DefineIndex(LexIDStr($5),
								LexIDStr($3),
								LexIDStr($7),
								params, NIL, 0, 0, NIL);
					DO_END;
				}
		;

Boot_BuildIndsStmt:
		  XBUILD INDICES		{ build_indices(); }

boot_index_params:
		boot_index_on boot_ident
				{
					IndexElem *n = (IndexElem*)$1;
					n->class = LexIDStr($2);
					$$ = n;
				}

boot_index_on:
		  boot_ident
				{
					IndexElem *n = makeNode(IndexElem);
					n->name = LexIDStr($1);
					$$ = n;
				}
		| boot_ident LPAREN boot_arg_list RPAREN
				{
					IndexElem *n = makeNode(IndexElem);
					n->name = LexIDStr($1);
					n->args = (List*)$3;
					$$ = n;
				}

boot_arg_list:
		  boot_ident
				{
					$$ = lappend(NIL, makeString(LexIDStr($1)));
				}
		| boot_arg_list COMMA boot_ident
				{
					$$ = lappend((List*)$1, makeString(LexIDStr($3)));
				}

optbootstrap:
			XBOOTSTRAP	{ $$ = 1; }
		|				{ $$ = 0; }
		;

boot_typelist:
		  boot_type_thing
		| boot_typelist COMMA boot_type_thing
		;

boot_type_thing:
		  boot_ident EQUALS boot_ident
				{
				   if(++numattr > MAXATTR)
						elog(FATAL,"Too many attributes\n");
				   DefineAttr(LexIDStr($1),LexIDStr($3),numattr-1);
				   if (DebugMode)
					   printf("\n");
				}
		;

optoideq:
			OBJ_ID EQUALS boot_ident { $$ = atol(LexIDStr($3));				}
		|						{ extern Oid newoid(); $$ = newoid();	}
		;

boot_tuplelist:
		   boot_tuple
		|  boot_tuplelist boot_tuple
		|  boot_tuplelist COMMA boot_tuple
		;

boot_tuple:
		  boot_ident {InsertOneValue(objectid, LexIDStr($1), num_tuples_read++); }
		| boot_const {InsertOneValue(objectid, LexIDStr($1), num_tuples_read++); }
		| NULLVAL
			{ InsertOneNull(num_tuples_read++); }
		;

boot_const :
		  CONST { $$=yylval.ival; }
		;

boot_ident :
		  ID	{ $$=yylval.ival; }
		;
%%

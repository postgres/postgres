%{
/*-------------------------------------------------------------------------
 *
 * bootparse.y
 *	  yacc grammar for the "bootstrap" mode (BKI file format)
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/bootstrap/bootparse.y,v 1.84 2006/08/25 04:06:46 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <unistd.h>

#include "access/attnum.h"
#include "access/htup.h"
#include "access/itup.h"
#include "access/skey.h"
#include "access/tupdesc.h"
#include "access/xact.h"
#include "bootstrap/bootstrap.h"
#include "catalog/catalog.h"
#include "catalog/heap.h"
#include "catalog/pg_am.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_class.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_tablespace.h"
#include "catalog/toasting.h"
#include "commands/defrem.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
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
#include "tcop/dest.h"
#include "utils/rel.h"

#define atooid(x)	((Oid) strtoul((x), NULL, 10))


static void
do_start(void)
{
	StartTransactionCommand();
	elog(DEBUG4, "start transaction");
}


static void
do_end(void)
{
	CommitTransactionCommand();
	elog(DEBUG4, "commit transaction");
	CHECK_FOR_INTERRUPTS();		/* allow SIGINT to kill bootstrap run */
	if (isatty(0))
	{
		printf("bootstrap> ");
		fflush(stdout);
	}
}


int num_columns_read = 0;

%}

%name-prefix="boot_yy"

%union
{
	List		*list;
	IndexElem	*ielem;
	char		*str;
	int			ival;
	Oid			oidval;
}

%type <list>  boot_index_params
%type <ielem> boot_index_param
%type <ival>  boot_const boot_ident
%type <ival>  optbootstrap optsharedrelation optwithoutoids
%type <ival>  boot_tuple boot_tuplelist
%type <oidval> oidspec optoideq

%token <ival> CONST_P ID
%token OPEN XCLOSE XCREATE INSERT_TUPLE
%token XDECLARE INDEX ON USING XBUILD INDICES UNIQUE XTOAST
%token COMMA EQUALS LPAREN RPAREN
%token OBJ_ID XBOOTSTRAP XSHARED_RELATION XWITHOUT_OIDS NULLVAL
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
		| Boot_DeclareUniqueIndexStmt
		| Boot_DeclareToastStmt
		| Boot_BuildIndsStmt
		;

Boot_OpenStmt:
		  OPEN boot_ident
				{
					do_start();
					boot_openrel(LexIDStr($2));
					do_end();
				}
		;

Boot_CloseStmt:
		  XCLOSE boot_ident %prec low
				{
					do_start();
					closerel(LexIDStr($2));
					do_end();
				}
		| XCLOSE %prec high
				{
					do_start();
					closerel(NULL);
					do_end();
				}
		;

Boot_CreateStmt:
		  XCREATE optbootstrap optsharedrelation optwithoutoids boot_ident oidspec LPAREN
				{
					do_start();
					numattr = 0;
					elog(DEBUG4, "creating%s%s relation %s %u",
						 $2 ? " bootstrap" : "",
						 $3 ? " shared" : "",
						 LexIDStr($5),
						 $6);
				}
		  boot_typelist
				{
					do_end();
				}
		  RPAREN
				{
					TupleDesc tupdesc;

					do_start();

					tupdesc = CreateTupleDesc(numattr, !($4), attrtypes);

					if ($2)
					{
						if (boot_reldesc)
						{
							elog(DEBUG4, "create bootstrap: warning, open relation exists, closing first");
							closerel(NULL);
						}

						boot_reldesc = heap_create(LexIDStr($5),
												   PG_CATALOG_NAMESPACE,
												   $3 ? GLOBALTABLESPACE_OID : 0,
												   $6,
												   tupdesc,
												   RELKIND_RELATION,
												   $3,
												   true);
						elog(DEBUG4, "bootstrap relation created");
					}
					else
					{
						Oid id;

						id = heap_create_with_catalog(LexIDStr($5),
													  PG_CATALOG_NAMESPACE,
													  $3 ? GLOBALTABLESPACE_OID : 0,
													  $6,
													  BOOTSTRAP_SUPERUSERID,
													  tupdesc,
													  RELKIND_RELATION,
													  $3,
													  true,
													  0,
													  ONCOMMIT_NOOP,
													  (Datum) 0,
													  true);
						elog(DEBUG4, "relation created with oid %u", id);
					}
					do_end();
				}
		;

Boot_InsertStmt:
		  INSERT_TUPLE optoideq
				{
					do_start();
					if ($2)
						elog(DEBUG4, "inserting row with oid %u", $2);
					else
						elog(DEBUG4, "inserting row");
					num_columns_read = 0;
				}
		  LPAREN  boot_tuplelist RPAREN
				{
					if (num_columns_read != numattr)
						elog(ERROR, "incorrect number of columns in row (expected %d, got %d)",
							 numattr, num_columns_read);
					if (boot_reldesc == NULL)
					{
						elog(ERROR, "relation not open");
						err_out();
					}
					InsertOneTuple($2);
					do_end();
				}
		;

Boot_DeclareIndexStmt:
		  XDECLARE INDEX boot_ident oidspec ON boot_ident USING boot_ident LPAREN boot_index_params RPAREN
				{
					do_start();

					DefineIndex(makeRangeVar(NULL, LexIDStr($6)),
								LexIDStr($3),
								$4,
								LexIDStr($8),
								NULL,
								$10,
								NULL, NIL, NIL,
								false, false, false,
								false, false, true, false, false);
					do_end();
				}
		;

Boot_DeclareUniqueIndexStmt:
		  XDECLARE UNIQUE INDEX boot_ident oidspec ON boot_ident USING boot_ident LPAREN boot_index_params RPAREN
				{
					do_start();

					DefineIndex(makeRangeVar(NULL, LexIDStr($7)),
								LexIDStr($4),
								$5,
								LexIDStr($9),
								NULL,
								$11,
								NULL, NIL, NIL,
								true, false, false,
								false, false, true, false, false);
					do_end();
				}
		;

Boot_DeclareToastStmt:
		  XDECLARE XTOAST oidspec oidspec ON boot_ident
				{
					do_start();

					BootstrapToastTable(LexIDStr($6), $3, $4);
					do_end();
				}
		;

Boot_BuildIndsStmt:
		  XBUILD INDICES
				{
					do_start();
					build_indices();
					do_end();
				}
		;


boot_index_params:
		boot_index_params COMMA boot_index_param	{ $$ = lappend($1, $3); }
		| boot_index_param							{ $$ = list_make1($1); }
		;

boot_index_param:
		boot_ident boot_ident
				{
					IndexElem *n = makeNode(IndexElem);
					n->name = LexIDStr($1);
					n->expr = NULL;
					n->opclass = list_make1(makeString(LexIDStr($2)));
					$$ = n;
				}
		;

optbootstrap:
			XBOOTSTRAP	{ $$ = 1; }
		|				{ $$ = 0; }
		;

optsharedrelation:
			XSHARED_RELATION	{ $$ = 1; }
		|						{ $$ = 0; }
		;

optwithoutoids:
			XWITHOUT_OIDS	{ $$ = 1; }
		|					{ $$ = 0; }
		;

boot_typelist:
		  boot_type_thing
		| boot_typelist COMMA boot_type_thing
		;

boot_type_thing:
		  boot_ident EQUALS boot_ident
				{
				   if (++numattr > MAXATTR)
						elog(FATAL, "too many columns");
				   DefineAttr(LexIDStr($1),LexIDStr($3),numattr-1);
				}
		;

oidspec:
			boot_ident							{ $$ = atooid(LexIDStr($1)); }
		;

optoideq:
			OBJ_ID EQUALS oidspec				{ $$ = $3; }
		|										{ $$ = (Oid) 0; }
		;

boot_tuplelist:
		   boot_tuple
		|  boot_tuplelist boot_tuple
		|  boot_tuplelist COMMA boot_tuple
		;

boot_tuple:
		  boot_ident
			{ InsertOneValue(LexIDStr($1), num_columns_read++); }
		| boot_const
			{ InsertOneValue(LexIDStr($1), num_columns_read++); }
		| NULLVAL
			{ InsertOneNull(num_columns_read++); }
		;

boot_const :
		  CONST_P { $$=yylval.ival; }
		;

boot_ident :
		  ID	{ $$=yylval.ival; }
		;
%%

#include "bootscanner.c"

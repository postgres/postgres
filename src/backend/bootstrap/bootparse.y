%{
/*-------------------------------------------------------------------------
 *
 * bootparse.y
 *	  yacc grammar for the "bootstrap" mode (BKI file format)
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/bootstrap/bootparse.y
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
#include "catalog/namespace.h"
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


/*
 * Bison doesn't allocate anything that needs to live across parser calls,
 * so we can easily have it use palloc instead of malloc.  This prevents
 * memory leaks if we error out during parsing.  Note this only works with
 * bison >= 2.0.  However, in bison 1.875 the default is to use alloca()
 * if possible, so there's not really much problem anyhow, at least if
 * you're building with gcc.
 */
#define YYMALLOC palloc
#define YYFREE   pfree

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


static int num_columns_read = 0;

%}

%expect 0
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
%type <str>   boot_const boot_ident
%type <ival>  optbootstrap optsharedrelation optwithoutoids
%type <oidval> oidspec optoideq optrowtypeoid

%token <str> CONST_P ID
%token OPEN XCLOSE XCREATE INSERT_TUPLE
%token XDECLARE INDEX ON USING XBUILD INDICES UNIQUE XTOAST
%token COMMA EQUALS LPAREN RPAREN
%token OBJ_ID XBOOTSTRAP XSHARED_RELATION XWITHOUT_OIDS XROWTYPE_OID NULLVAL

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
					boot_openrel($2);
					do_end();
				}
		;

Boot_CloseStmt:
		  XCLOSE boot_ident %prec low
				{
					do_start();
					closerel($2);
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
		  XCREATE boot_ident oidspec optbootstrap optsharedrelation optwithoutoids optrowtypeoid LPAREN
				{
					do_start();
					numattr = 0;
					elog(DEBUG4, "creating%s%s relation %s %u",
						 $4 ? " bootstrap" : "",
						 $5 ? " shared" : "",
						 $2,
						 $3);
				}
		  boot_column_list
				{
					do_end();
				}
		  RPAREN
				{
					TupleDesc tupdesc;
					bool	shared_relation;
					bool	mapped_relation;

					do_start();

					tupdesc = CreateTupleDesc(numattr, !($6), attrtypes);

					shared_relation = $5;

					/*
					 * The catalogs that use the relation mapper are the
					 * bootstrap catalogs plus the shared catalogs.  If this
					 * ever gets more complicated, we should invent a BKI
					 * keyword to mark the mapped catalogs, but for now a
					 * quick hack seems the most appropriate thing.  Note in
					 * particular that all "nailed" heap rels (see formrdesc
					 * in relcache.c) must be mapped.
					 */
					mapped_relation = ($4 || shared_relation);

					if ($4)
					{
						if (boot_reldesc)
						{
							elog(DEBUG4, "create bootstrap: warning, open relation exists, closing first");
							closerel(NULL);
						}

						boot_reldesc = heap_create($2,
												   PG_CATALOG_NAMESPACE,
												   shared_relation ? GLOBALTABLESPACE_OID : 0,
												   $3,
												   InvalidOid,
												   tupdesc,
												   RELKIND_RELATION,
												   RELPERSISTENCE_PERMANENT,
												   shared_relation,
												   mapped_relation,
												   true);
						elog(DEBUG4, "bootstrap relation created");
					}
					else
					{
						Oid id;

						id = heap_create_with_catalog($2,
													  PG_CATALOG_NAMESPACE,
													  shared_relation ? GLOBALTABLESPACE_OID : 0,
													  $3,
													  $7,
													  InvalidOid,
													  BOOTSTRAP_SUPERUSERID,
													  tupdesc,
													  NIL,
													  RELKIND_RELATION,
													  RELPERSISTENCE_PERMANENT,
													  shared_relation,
													  mapped_relation,
													  true,
													  0,
													  ONCOMMIT_NOOP,
													  (Datum) 0,
													  false,
													  true,
													  false);
						elog(DEBUG4, "relation created with OID %u", id);
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
		  LPAREN boot_column_val_list RPAREN
				{
					if (num_columns_read != numattr)
						elog(ERROR, "incorrect number of columns in row (expected %d, got %d)",
							 numattr, num_columns_read);
					if (boot_reldesc == NULL)
						elog(FATAL, "relation not open");
					InsertOneTuple($2);
					do_end();
				}
		;

Boot_DeclareIndexStmt:
		  XDECLARE INDEX boot_ident oidspec ON boot_ident USING boot_ident LPAREN boot_index_params RPAREN
				{
					IndexStmt *stmt = makeNode(IndexStmt);
					Oid		relationId;

					do_start();

					stmt->idxname = $3;
					stmt->relation = makeRangeVar(NULL, $6, -1);
					stmt->accessMethod = $8;
					stmt->tableSpace = NULL;
					stmt->indexParams = $10;
					stmt->options = NIL;
					stmt->whereClause = NULL;
					stmt->excludeOpNames = NIL;
					stmt->idxcomment = NULL;
					stmt->indexOid = InvalidOid;
					stmt->oldNode = InvalidOid;
					stmt->unique = false;
					stmt->primary = false;
					stmt->isconstraint = false;
					stmt->deferrable = false;
					stmt->initdeferred = false;
					stmt->concurrent = false;

					/* locks and races need not concern us in bootstrap mode */
					relationId = RangeVarGetRelid(stmt->relation, NoLock,
												  false);

					DefineIndex(relationId,
								stmt,
								$4,
								false,
								false,
								true, /* skip_build */
								false);
					do_end();
				}
		;

Boot_DeclareUniqueIndexStmt:
		  XDECLARE UNIQUE INDEX boot_ident oidspec ON boot_ident USING boot_ident LPAREN boot_index_params RPAREN
				{
					IndexStmt *stmt = makeNode(IndexStmt);
					Oid		relationId;

					do_start();

					stmt->idxname = $4;
					stmt->relation = makeRangeVar(NULL, $7, -1);
					stmt->accessMethod = $9;
					stmt->tableSpace = NULL;
					stmt->indexParams = $11;
					stmt->options = NIL;
					stmt->whereClause = NULL;
					stmt->excludeOpNames = NIL;
					stmt->idxcomment = NULL;
					stmt->indexOid = InvalidOid;
					stmt->oldNode = InvalidOid;
					stmt->unique = true;
					stmt->primary = false;
					stmt->isconstraint = false;
					stmt->deferrable = false;
					stmt->initdeferred = false;
					stmt->concurrent = false;

					/* locks and races need not concern us in bootstrap mode */
					relationId = RangeVarGetRelid(stmt->relation, NoLock,
												  false);

					DefineIndex(relationId,
								stmt,
								$5,
								false,
								false,
								true, /* skip_build */
								false);
					do_end();
				}
		;

Boot_DeclareToastStmt:
		  XDECLARE XTOAST oidspec oidspec ON boot_ident
				{
					do_start();

					BootstrapToastTable($6, $3, $4);
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
					n->name = $1;
					n->expr = NULL;
					n->indexcolname = NULL;
					n->collation = NIL;
					n->opclass = list_make1(makeString($2));
					n->ordering = SORTBY_DEFAULT;
					n->nulls_ordering = SORTBY_NULLS_DEFAULT;
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

optrowtypeoid:
			XROWTYPE_OID oidspec	{ $$ = $2; }
		|							{ $$ = InvalidOid; }
		;

boot_column_list:
		  boot_column_def
		| boot_column_list COMMA boot_column_def
		;

boot_column_def:
		  boot_ident EQUALS boot_ident
				{
				   if (++numattr > MAXATTR)
						elog(FATAL, "too many columns");
				   DefineAttr($1, $3, numattr-1);
				}
		;

oidspec:
			boot_ident							{ $$ = atooid($1); }
		;

optoideq:
			OBJ_ID EQUALS oidspec				{ $$ = $3; }
		|										{ $$ = InvalidOid; }
		;

boot_column_val_list:
		   boot_column_val
		|  boot_column_val_list boot_column_val
		|  boot_column_val_list COMMA boot_column_val
		;

boot_column_val:
		  boot_ident
			{ InsertOneValue($1, num_columns_read++); }
		| boot_const
			{ InsertOneValue($1, num_columns_read++); }
		| NULLVAL
			{ InsertOneNull(num_columns_read++); }
		;

boot_const :
		  CONST_P { $$ = yylval.str; }
		;

boot_ident :
		  ID	{ $$ = yylval.str; }
		;
%%

#include "bootscanner.c"

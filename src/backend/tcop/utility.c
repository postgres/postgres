/*-------------------------------------------------------------------------
 *
 * utility.c
 *	  Contains functions which control the execution of the POSTGRES utility
 *	  commands.  At one time acted as an interface between the Lisp and C
 *	  systems.
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/tcop/utility.c,v 1.140 2002/03/26 19:16:03 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "catalog/catalog.h"
#include "catalog/namespace.h"
#include "catalog/pg_shadow.h"
#include "commands/async.h"
#include "commands/cluster.h"
#include "commands/command.h"
#include "commands/comment.h"
#include "commands/copy.h"
#include "commands/creatinh.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "commands/proclang.h"
#include "commands/rename.h"
#include "commands/sequence.h"
#include "commands/trigger.h"
#include "commands/user.h"
#include "commands/vacuum.h"
#include "commands/variable.h"
#include "commands/view.h"
#include "miscadmin.h"
#include "parser/parse.h"
#include "parser/parse_clause.h"
#include "parser/parse_expr.h"
#include "rewrite/rewriteDefine.h"
#include "rewrite/rewriteRemove.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/syscache.h"
#include "utils/temprel.h"
#include "access/xlog.h"

/*
 * Error-checking support for DROP commands
 */

struct kindstrings
{
	char		kind;
	char	   *indef_article;
	char	   *name;
	char	   *command;
};

static struct kindstrings kindstringarray[] = {
	{RELKIND_RELATION, "a", "table", "TABLE"},
	{RELKIND_SEQUENCE, "a", "sequence", "SEQUENCE"},
	{RELKIND_VIEW, "a", "view", "VIEW"},
	{RELKIND_INDEX, "an", "index", "INDEX"},
	{'\0', "a", "???", "???"}
};


static void
DropErrorMsg(char *relname, char wrongkind, char rightkind)
{
	struct kindstrings *rentry;
	struct kindstrings *wentry;

	for (rentry = kindstringarray; rentry->kind != '\0'; rentry++)
		if (rentry->kind == rightkind)
			break;
	Assert(rentry->kind != '\0');

	for (wentry = kindstringarray; wentry->kind != '\0'; wentry++)
		if (wentry->kind == wrongkind)
			break;
	/* wrongkind could be something we don't have in our table... */
	if (wentry->kind != '\0')
		elog(ERROR, "\"%s\" is not %s %s. Use DROP %s to remove %s %s",
			 relname, rentry->indef_article, rentry->name,
			 wentry->command, wentry->indef_article, wentry->name);
	else
		elog(ERROR, "\"%s\" is not %s %s",
			 relname, rentry->indef_article, rentry->name);
}

static void
CheckDropPermissions(RangeVar *rel, char rightkind)
{
	struct kindstrings *rentry;
	Oid			relOid;
	HeapTuple	tuple;
	Form_pg_class classform;

	for (rentry = kindstringarray; rentry->kind != '\0'; rentry++)
		if (rentry->kind == rightkind)
			break;
	Assert(rentry->kind != '\0');

	relOid = RangeVarGetRelid(rel, true);
	if (!OidIsValid(relOid))
		elog(ERROR, "%s \"%s\" does not exist", rentry->name, rel->relname);
	tuple = SearchSysCache(RELOID,
						   ObjectIdGetDatum(relOid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "%s \"%s\" does not exist", rentry->name, rel->relname);

	classform = (Form_pg_class) GETSTRUCT(tuple);

	if (classform->relkind != rightkind)
		DropErrorMsg(rel->relname, classform->relkind, rightkind);

	if (!pg_class_ownercheck(relOid, GetUserId()))
		elog(ERROR, "you do not own %s \"%s\"",
			 rentry->name, rel->relname);

	if (!allowSystemTableMods && IsSystemRelationName(rel->relname) &&
		!is_temp_relname(rel->relname))
		elog(ERROR, "%s \"%s\" is a system %s",
			 rentry->name, rel->relname, rentry->name);

	ReleaseSysCache(tuple);
}

static void
CheckOwnership(RangeVar *rel, bool noCatalogs)
{
	Oid			relOid;
	HeapTuple	tuple;

	relOid = RangeVarGetRelid(rel, false);
	tuple = SearchSysCache(RELOID,
						   ObjectIdGetDatum(relOid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "Relation \"%s\" does not exist", rel->relname);

	if (!pg_class_ownercheck(relOid, GetUserId()))
		elog(ERROR, "%s: %s", rel->relname,
			 aclcheck_error_strings[ACLCHECK_NOT_OWNER]);

	if (noCatalogs)
	{
		if (!allowSystemTableMods && IsSystemRelationName(rel->relname))
			elog(ERROR, "relation \"%s\" is a system catalog",
				 rel->relname);
	}

	ReleaseSysCache(tuple);
}


/*
 * ProcessUtility
 *		general utility function invoker
 *
 *	parsetree: the parse tree for the utility statement
 *	dest: where to send results
 *	completionTag: points to a buffer of size COMPLETION_TAG_BUFSIZE
 *		in which to store a command completion status string.
 *
 * completionTag is only set nonempty if we want to return a nondefault
 * status (currently, only used for MOVE/FETCH).
 *
 * completionTag may be NULL if caller doesn't want a status string.
 */
void
ProcessUtility(Node *parsetree,
			   CommandDest dest,
			   char *completionTag)
{
	char	   *relname;

	if (completionTag)
		completionTag[0] = '\0';

	switch (nodeTag(parsetree))
	{
			/*
			 * ******************************** transactions ********************************
			 *
			 */
		case T_TransactionStmt:
			{
				TransactionStmt *stmt = (TransactionStmt *) parsetree;

				switch (stmt->command)
				{
					case BEGIN_TRANS:
						BeginTransactionBlock();
						break;

					case COMMIT:
						EndTransactionBlock();
						break;

					case ROLLBACK:
						UserAbortTransactionBlock();
						break;
				}
			}
			break;

			/*
			 * ******************************** portal manipulation ********************************
			 *
			 */
		case T_ClosePortalStmt:
			{
				ClosePortalStmt *stmt = (ClosePortalStmt *) parsetree;

				PerformPortalClose(stmt->portalname, dest);
			}
			break;

		case T_FetchStmt:
			{
				FetchStmt  *stmt = (FetchStmt *) parsetree;
				char	   *portalName = stmt->portalname;
				bool		forward;
				int			count;

				SetQuerySnapshot();

				forward = (bool) (stmt->direction == FORWARD);

				/*
				 * parser ensures that count is >= 0 and 'fetch ALL' -> 0
				 */

				count = stmt->howMany;
				PerformPortalFetch(portalName, forward, count,
								   (stmt->ismove) ? None : dest,
								   completionTag);
			}
			break;

			/*
			 * relation and attribute manipulation
			 */
		case T_CreateSchemaStmt:
			{
				CreateSchemaStmt  *stmt = (CreateSchemaStmt *) parsetree;

				CreateSchemaCommand(stmt);
			}
			break;

		case T_CreateStmt:
			{
				Oid			relOid;

				relOid = DefineRelation((CreateStmt *) parsetree,
										RELKIND_RELATION);

				/*
				 * Let AlterTableCreateToastTable decide if this one needs a
				 * secondary relation too.
				 */
				CommandCounterIncrement();
				AlterTableCreateToastTable(relOid, true);
			}
			break;

		case T_DropStmt:
			{
				DropStmt   *stmt = (DropStmt *) parsetree;
				List	   *arg;

				foreach(arg, stmt->objects)
				{
					RangeVar   *rel = (RangeVar *) lfirst(arg);

					relname = rel->relname;

					switch (stmt->removeType)
					{
						case DROP_TABLE:
							CheckDropPermissions(rel, RELKIND_RELATION);
							RemoveRelation(relname);
							break;

						case DROP_SEQUENCE:
							CheckDropPermissions(rel, RELKIND_SEQUENCE);
							RemoveRelation(relname);
							break;

						case DROP_VIEW:
							CheckDropPermissions(rel, RELKIND_VIEW);
							RemoveView(relname);
							break;

						case DROP_INDEX:
							CheckDropPermissions(rel, RELKIND_INDEX);
							RemoveIndex(rel);
							break;

						case DROP_RULE:
							/* RemoveRewriteRule checks permissions */
							RemoveRewriteRule(relname);
							break;

						case DROP_TYPE:
							/* RemoveType does its own permissions checks */
							RemoveType(relname);
							break;

						case DROP_DOMAIN:
							/* RemoveDomain does its own permissions checks */
							RemoveDomain(relname, stmt->behavior);
							break;
					}

					/*
					 * Make sure subsequent loop iterations will see
					 * results of this one; needed if removing multiple
					 * rules for same table, for example.
					 */
					CommandCounterIncrement();
				}
			}
			break;

		case T_TruncateStmt:
			{
				relname = ((TruncateStmt *) parsetree)->relation->relname;
				TruncateRelation(relname);
			}
			break;

		case T_CommentStmt:
			{
				CommentStmt *stmt;

				stmt = ((CommentStmt *) parsetree);

				CommentObject(stmt->objtype, stmt->objschema, stmt->objname,
							  stmt->objproperty, stmt->objlist, stmt->comment);
			}
			break;

		case T_CopyStmt:
			{
				CopyStmt   *stmt = (CopyStmt *) parsetree;

				if (stmt->direction != FROM)
					SetQuerySnapshot();

				DoCopy(stmt->relation->relname,
					   stmt->binary,
					   stmt->oids,
					   (bool) (stmt->direction == FROM),
					   (bool) (stmt->filename == NULL),

				/*
				 * null filename means copy to/from stdout/stdin, rather
				 * than to/from a file.
				 */
					   stmt->filename,
					   stmt->delimiter,
					   stmt->null_print);
			}
			break;

			/*
			 * schema
			 */
		case T_RenameStmt:
			{
				RenameStmt *stmt = (RenameStmt *) parsetree;

				relname = stmt->relation->relname;
				CheckOwnership(stmt->relation, true);

				/* ----------------
				 *	XXX using len == 3 to tell the difference
				 *		between "rename rel to newrel" and
				 *		"rename att in rel to newatt" will not
				 *		work soon because "rename type/operator/rule"
				 *		stuff is being added. - cim 10/24/90
				 * ----------------
				 * [another piece of amuzing but useless anecdote -- ay]
				 */
				if (stmt->column == NULL)
				{
					/*
					 * rename relation
					 *
					 * Note: we also rename the "type" tuple corresponding to
					 * the relation.
					 */
					renamerel(stmt->relation,	/* old relation */
							  stmt->newname);	/* new name */
				}
				else
				{
					/*
					 * rename attribute
					 */
					renameatt(relname,	/* relname */
							  stmt->column,		/* old att name */
							  stmt->newname,	/* new att name */
							  interpretInhOption(stmt->relation->inhOpt));		/* recursive? */
				}
			}
			break;

			/* various Alter Table forms */

		case T_AlterTableStmt:
			{
				AlterTableStmt *stmt = (AlterTableStmt *) parsetree;

				/*
				 * Some or all of these functions are recursive to cover
				 * inherited things, so permission checks are done there.
				 */
				switch (stmt->subtype)
				{
					case 'A':	/* ADD COLUMN */
						AlterTableAddColumn(stmt->relation->relname,
										interpretInhOption((stmt->relation)->inhOpt),
											(ColumnDef *) stmt->def);
						break;
					case 'T':	/* ALTER COLUMN DEFAULT */
						AlterTableAlterColumnDefault(stmt->relation->relname,
										interpretInhOption((stmt->relation)->inhOpt),
													 stmt->name,
													 stmt->def);
						break;
					case 'S':	/* ALTER COLUMN STATISTICS */
					case 'M':   /* ALTER COLUMN STORAGE */
						AlterTableAlterColumnFlags(stmt->relation->relname,
										interpretInhOption(stmt->relation->inhOpt),
														stmt->name,
														stmt->def,
												        &(stmt->subtype));
						break;
					case 'D':	/* DROP COLUMN */
						AlterTableDropColumn(stmt->relation->relname,
										interpretInhOption(stmt->relation->inhOpt),
											 stmt->name,
											 stmt->behavior);
						break;
					case 'C':	/* ADD CONSTRAINT */
						AlterTableAddConstraint(stmt->relation->relname,
										interpretInhOption(stmt->relation->inhOpt),
												(List *) stmt->def);
						break;
					case 'X':	/* DROP CONSTRAINT */
						AlterTableDropConstraint(stmt->relation->relname,
										interpretInhOption(stmt->relation->inhOpt),
												 stmt->name,
												 stmt->behavior);
						break;
					case 'E':	/* CREATE TOAST TABLE */
						AlterTableCreateToastTable(RangeVarGetRelid(stmt->relation, false),
												   false);
						break;
					case 'U':	/* ALTER OWNER */
						AlterTableOwner(stmt->relation,
										stmt->name);
						break;
					default:	/* oops */
						elog(ERROR, "T_AlterTableStmt: unknown subtype");
						break;
				}
			}
			break;


		case T_GrantStmt:
			{
				GrantStmt  *stmt = (GrantStmt *) parsetree;

				ExecuteGrantStmt(stmt);
			}
			break;

			/*
			 * ******************************** object creation /
			 * destruction ********************************
			 *
			 */
		case T_DefineStmt:
			{
				DefineStmt *stmt = (DefineStmt *) parsetree;

				switch (stmt->defType)
				{
					case OPERATOR:
						DefineOperator(stmt->defname,	/* operator name */
									   stmt->definition);		/* rest */
						break;
					case TYPE_P:
						DefineType(stmt->defname, stmt->definition);
						break;
					case AGGREGATE:
						DefineAggregate(stmt->defname,	/* aggregate name */
										stmt->definition);		/* rest */
						break;
				}
			}
			break;

		case T_ViewStmt:		/* CREATE VIEW */
			{
				ViewStmt   *stmt = (ViewStmt *) parsetree;

				DefineView(stmt->view->relname, stmt->query);	/* retrieve parsetree */
			}
			break;

		case T_ProcedureStmt:	/* CREATE FUNCTION */
			CreateFunction((ProcedureStmt *) parsetree);
			break;

		case T_IndexStmt:		/* CREATE INDEX */
			{
				IndexStmt  *stmt = (IndexStmt *) parsetree;

				CheckOwnership(stmt->relation, true);

				DefineIndex(stmt->relation,				/* relation */
							stmt->idxname,				/* index name */
							stmt->accessMethod, 		/* am name */
							stmt->indexParams,			/* parameters */
							stmt->unique,
							stmt->primary,
							(Expr *) stmt->whereClause,
							stmt->rangetable);
			}
			break;

		case T_RuleStmt:		/* CREATE RULE */
			DefineQueryRewrite((RuleStmt *) parsetree);
			break;

		case T_CreateSeqStmt:
			DefineSequence((CreateSeqStmt *) parsetree);
			break;

		case T_RemoveAggrStmt:
			{
				RemoveAggrStmt *stmt = (RemoveAggrStmt *) parsetree;
				char	   *typename = (char *) NULL;

				if (stmt->aggtype != NULL)
					typename = TypeNameToInternalName((TypeName *) stmt->aggtype);

				RemoveAggregate(stmt->aggname, typename);
			}
			break;

		case T_RemoveFuncStmt:
			{
				RemoveFuncStmt *stmt = (RemoveFuncStmt *) parsetree;

				RemoveFunction(stmt->funcname, stmt->args);
			}
			break;

		case T_RemoveOperStmt:
			{
				RemoveOperStmt *stmt = (RemoveOperStmt *) parsetree;
				TypeName   *typenode1 = (TypeName *) lfirst(stmt->args);
				TypeName   *typenode2 = (TypeName *) lsecond(stmt->args);
				char	   *typename1 = (char *) NULL;
				char	   *typename2 = (char *) NULL;

				if (typenode1 != NULL)
					typename1 = TypeNameToInternalName(typenode1);
				if (typenode2 != NULL)
					typename2 = TypeNameToInternalName(typenode2);

				RemoveOperator(stmt->opname, typename1, typename2);
			}
			break;

		case T_CreatedbStmt:
			{
				CreatedbStmt *stmt = (CreatedbStmt *) parsetree;

				createdb(stmt->dbname, stmt->dbowner,
						 stmt->dbpath, stmt->dbtemplate,
						 stmt->encoding);
			}
			break;

		case T_AlterDatabaseSetStmt:
			AlterDatabaseSet((AlterDatabaseSetStmt *)parsetree);
			break;

		case T_DropdbStmt:
			{
				DropdbStmt *stmt = (DropdbStmt *) parsetree;

				dropdb(stmt->dbname);
			}
			break;

			/* Query-level asynchronous notification */
		case T_NotifyStmt:
			{
				NotifyStmt *stmt = (NotifyStmt *) parsetree;

				Async_Notify(stmt->relation->relname);
			}
			break;

		case T_ListenStmt:
			{
				ListenStmt *stmt = (ListenStmt *) parsetree;

				Async_Listen(stmt->relation->relname, MyProcPid);
			}
			break;

		case T_UnlistenStmt:
			{
				UnlistenStmt *stmt = (UnlistenStmt *) parsetree;

				Async_Unlisten(stmt->relation->relname, MyProcPid);
			}
			break;

		case T_LoadStmt:
			{
				LoadStmt   *stmt = (LoadStmt *) parsetree;

				closeAllVfds(); /* probably not necessary... */
				load_file(stmt->filename);
			}
			break;

		case T_ClusterStmt:
			{
				ClusterStmt *stmt = (ClusterStmt *) parsetree;

				CheckOwnership(stmt->relation, true);

				cluster(stmt->relation, stmt->indexname);
			}
			break;

		case T_VacuumStmt:
			vacuum((VacuumStmt *) parsetree);
			break;

		case T_ExplainStmt:
			ExplainQuery((ExplainStmt *) parsetree, dest);
			break;

#ifdef NOT_USED

		case T_RecipeStmt:
			{
				RecipeStmt *stmt = (RecipeStmt *) parsetree;

				beginRecipe(stmt);
			}
			break;
#endif

		case T_VariableSetStmt:
			{
				VariableSetStmt *n = (VariableSetStmt *) parsetree;

				SetPGVariable(n->name, n->args);
			}
			break;

		case T_VariableShowStmt:
			{
				VariableShowStmt *n = (VariableShowStmt *) parsetree;

				GetPGVariable(n->name);
			}
			break;

		case T_VariableResetStmt:
			{
				VariableResetStmt *n = (VariableResetStmt *) parsetree;

				ResetPGVariable(n->name);
			}
			break;

		case T_CreateTrigStmt:
			CreateTrigger((CreateTrigStmt *) parsetree);
			break;

		case T_DropTrigStmt:
			DropTrigger((DropTrigStmt *) parsetree);
			break;

		case T_CreatePLangStmt:
			CreateProceduralLanguage((CreatePLangStmt *) parsetree);
			break;

		case T_DropPLangStmt:
			DropProceduralLanguage((DropPLangStmt *) parsetree);
			break;

			/*
			 * ******************************** DOMAIN statements ****
			 */
		case T_CreateDomainStmt:
			DefineDomain((CreateDomainStmt *) parsetree);
			break;

			/*
			 * ******************************** USER statements ****
			 */
		case T_CreateUserStmt:
			CreateUser((CreateUserStmt *) parsetree);
			break;

		case T_AlterUserStmt:
			AlterUser((AlterUserStmt *) parsetree);
			break;

		case T_AlterUserSetStmt:
			AlterUserSet((AlterUserSetStmt *) parsetree);
			break;

		case T_DropUserStmt:
			DropUser((DropUserStmt *) parsetree);
			break;

		case T_LockStmt:
			LockTableCommand((LockStmt *) parsetree);
			break;

		case T_ConstraintsSetStmt:
			DeferredTriggerSetState((ConstraintsSetStmt *) parsetree);
			break;

		case T_CreateGroupStmt:
			CreateGroup((CreateGroupStmt *) parsetree);
			break;

		case T_AlterGroupStmt:
			AlterGroup((AlterGroupStmt *) parsetree, "ALTER GROUP");
			break;

		case T_DropGroupStmt:
			DropGroup((DropGroupStmt *) parsetree);
			break;

		case T_CheckPointStmt:
			{
				if (!superuser())
					elog(ERROR, "permission denied");
				CreateCheckPoint(false);
			}
			break;

		case T_ReindexStmt:
			{
				ReindexStmt *stmt = (ReindexStmt *) parsetree;

				switch (stmt->reindexType)
				{
					case INDEX:
						relname = (char *) stmt->relation->relname;
						if (IsSystemRelationName(relname))
						{
							if (!allowSystemTableMods)
								elog(ERROR, "\"%s\" is a system index. call REINDEX under standalone postgres with -O -P options",
									 relname);
							if (!IsIgnoringSystemIndexes())
								elog(ERROR, "\"%s\" is a system index. call REINDEX under standalone postgres with -P -O options",
									 relname);
						}
						CheckOwnership(stmt->relation, false);
						ReindexIndex(stmt->relation, stmt->force);
						break;
					case TABLE:
						CheckOwnership(stmt->relation, false);
						ReindexTable(stmt->relation, stmt->force);
						break;
					case DATABASE:
						relname = (char *) stmt->name;
						if (!allowSystemTableMods)
							elog(ERROR, "must be called under standalone postgres with -O -P options");
						if (!IsIgnoringSystemIndexes())
							elog(ERROR, "must be called under standalone postgres with -P -O options");
						ReindexDatabase(relname, stmt->force, false);
						break;
				}
				break;
			}
			break;

		default:
			elog(ERROR, "ProcessUtility: command #%d unsupported",
				 nodeTag(parsetree));
			break;
	}
}

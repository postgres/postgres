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
 *	  $Header: /cvsroot/pgsql/src/backend/tcop/utility.c,v 1.108 2001/03/22 03:59:48 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "catalog/catalog.h"
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
#include "utils/ps_status.h"
#include "utils/syscache.h"
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
CheckDropPermissions(char *name, char rightkind)
{
	struct kindstrings *rentry;
	HeapTuple	tuple;
	Form_pg_class classform;

	for (rentry = kindstringarray; rentry->kind != '\0'; rentry++)
		if (rentry->kind == rightkind)
			break;
	Assert(rentry->kind != '\0');

	tuple = SearchSysCache(RELNAME,
						   PointerGetDatum(name),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "%s \"%s\" does not exist", rentry->name, name);

	classform = (Form_pg_class) GETSTRUCT(tuple);

	if (classform->relkind != rightkind)
		DropErrorMsg(name, classform->relkind, rightkind);

	if (!pg_ownercheck(GetUserId(), name, RELNAME))
		elog(ERROR, "you do not own %s \"%s\"",
			 rentry->name, name);

	if (!allowSystemTableMods && IsSystemRelationName(name))
		elog(ERROR, "%s \"%s\" is a system %s",
			 rentry->name, name, rentry->name);

	ReleaseSysCache(tuple);
}


/* ----------------
 *		general utility function invoker
 * ----------------
 */
void
ProcessUtility(Node *parsetree,
			   CommandDest dest)
{
	char	   *commandTag = NULL;
	char	   *relname;
	char	   *relationName;

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
						set_ps_display(commandTag = "BEGIN");
						BeginTransactionBlock();
						break;

					case COMMIT:
						set_ps_display(commandTag = "COMMIT");
						EndTransactionBlock();
						break;

					case ROLLBACK:
						set_ps_display(commandTag = "ROLLBACK");
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

				set_ps_display(commandTag = "CLOSE");

				PerformPortalClose(stmt->portalname, dest);
			}
			break;

		case T_FetchStmt:
			{
				FetchStmt  *stmt = (FetchStmt *) parsetree;
				char	   *portalName = stmt->portalname;
				bool		forward;
				int			count;

				set_ps_display(commandTag = (stmt->ismove) ? "MOVE" : "FETCH");

				SetQuerySnapshot();

				forward = (bool) (stmt->direction == FORWARD);

				/*
				 * parser ensures that count is >= 0 and 'fetch ALL' -> 0
				 */

				count = stmt->howMany;
				PerformPortalFetch(portalName, forward, count, commandTag,
								   (stmt->ismove) ? None : dest);		/* /dev/null for MOVE */
			}
			break;

			/*
			 * ******************************** relation and attribute
			 * manipulation ********************************
			 *
			 */
		case T_CreateStmt:
			set_ps_display(commandTag = "CREATE");

			DefineRelation((CreateStmt *) parsetree, RELKIND_RELATION);

			/*
			 * Let AlterTableCreateToastTable decide if this one needs a
			 * secondary relation too.
			 */
			CommandCounterIncrement();
			AlterTableCreateToastTable(((CreateStmt *) parsetree)->relname,
									   true);
			break;

		case T_DropStmt:
			{
				DropStmt   *stmt = (DropStmt *) parsetree;
				List	   *args = stmt->names;
				List	   *arg;

				set_ps_display(commandTag = "DROP");

				foreach(arg, args)
				{
					relname = strVal(lfirst(arg));

					switch (stmt->removeType)
					{
						case DROP_TABLE:
							CheckDropPermissions(relname, RELKIND_RELATION);
							RemoveRelation(relname);
							break;

						case DROP_SEQUENCE:
							CheckDropPermissions(relname, RELKIND_SEQUENCE);
							RemoveRelation(relname);
							break;

						case DROP_VIEW:
							CheckDropPermissions(relname, RELKIND_VIEW);
							RemoveView(relname);
							break;

						case DROP_INDEX:
							CheckDropPermissions(relname, RELKIND_INDEX);
							RemoveIndex(relname);
							break;

						case DROP_RULE:
							{
								char	   *rulename = relname;
								int			aclcheck_result;

								relationName = RewriteGetRuleEventRel(rulename);
								aclcheck_result = pg_aclcheck(relationName, GetUserId(), ACL_RU);
								if (aclcheck_result != ACLCHECK_OK)
									elog(ERROR, "%s: %s", relationName,
										 aclcheck_error_strings[aclcheck_result]);
								RemoveRewriteRule(rulename);
							}
							break;

						case DROP_TYPE_P:
							/* RemoveType does its own permissions checks */
							RemoveType(relname);
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
				Relation	rel;

				set_ps_display(commandTag = "TRUNCATE");

				relname = ((TruncateStmt *) parsetree)->relName;
				if (!allowSystemTableMods && IsSystemRelationName(relname))
					elog(ERROR, "TRUNCATE cannot be used on system tables. '%s' is a system table",
						 relname);

				/* Grab exclusive lock in preparation for truncate... */
				rel = heap_openr(relname, AccessExclusiveLock);
				if (rel->rd_rel->relkind == RELKIND_SEQUENCE)
					elog(ERROR, "TRUNCATE cannot be used on sequences. '%s' is a sequence",
						 relname);
				if (rel->rd_rel->relkind == RELKIND_VIEW)
					elog(ERROR, "TRUNCATE cannot be used on views. '%s' is a sequence",
						 relname);
				heap_close(rel, NoLock);

				if (!pg_ownercheck(GetUserId(), relname, RELNAME))
					elog(ERROR, "you do not own class \"%s\"", relname);
				TruncateRelation(relname);
			}
			break;

		case T_CommentStmt:
			{
				CommentStmt *statement;

				statement = ((CommentStmt *) parsetree);

				set_ps_display(commandTag = "COMMENT");

				CommentObject(statement->objtype, statement->objname,
							  statement->objproperty, statement->objlist,
							  statement->comment);
			}
			break;

		case T_CopyStmt:
			{
				CopyStmt   *stmt = (CopyStmt *) parsetree;

				set_ps_display(commandTag = "COPY");

				if (stmt->direction != FROM)
					SetQuerySnapshot();

				DoCopy(stmt->relname,
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

				set_ps_display(commandTag = "ALTER");

				relname = stmt->relname;
				if (!allowSystemTableMods && IsSystemRelationName(relname))
					elog(ERROR, "ALTER TABLE: relation \"%s\" is a system catalog",
						 relname);
				if (!pg_ownercheck(GetUserId(), relname, RELNAME))
					elog(ERROR, "permission denied");

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
					/* ----------------
					 *		rename relation
					 *
					 *		Note: we also rename the "type" tuple
					 *		corresponding to the relation.
					 * ----------------
					 */
					renamerel(relname,	/* old name */
							  stmt->newname);	/* new name */
				}
				else
				{
					/* ----------------
					 *		rename attribute
					 * ----------------
					 */
					renameatt(relname,	/* relname */
							  stmt->column,		/* old att name */
							  stmt->newname,	/* new att name */
							  interpretInhOption(stmt->inhOpt));		/* recursive? */
				}
			}
			break;

			/* various Alter Table forms */

		case T_AlterTableStmt:
			{
				AlterTableStmt *stmt = (AlterTableStmt *) parsetree;

				set_ps_display(commandTag = "ALTER");

				/*
				 * Some or all of these functions are recursive to cover
				 * inherited things, so permission checks are done there.
				 */
				switch (stmt->subtype)
				{
					case 'A':	/* ADD COLUMN */
						AlterTableAddColumn(stmt->relname,
										interpretInhOption(stmt->inhOpt),
											(ColumnDef *) stmt->def);
						break;
					case 'T':	/* ALTER COLUMN */
						AlterTableAlterColumn(stmt->relname,
										interpretInhOption(stmt->inhOpt),
											  stmt->name,
											  stmt->def);
						break;
					case 'D':	/* ALTER DROP */
						AlterTableDropColumn(stmt->relname,
										interpretInhOption(stmt->inhOpt),
											 stmt->name,
											 stmt->behavior);
						break;
					case 'C':	/* ADD CONSTRAINT */
						AlterTableAddConstraint(stmt->relname,
										interpretInhOption(stmt->inhOpt),
												stmt->def);
						break;
					case 'X':	/* DROP CONSTRAINT */
						AlterTableDropConstraint(stmt->relname,
										interpretInhOption(stmt->inhOpt),
												 stmt->name,
												 stmt->behavior);
						break;
					case 'E':	/* CREATE TOAST TABLE */
						AlterTableCreateToastTable(stmt->relname,
												   false);
						break;
					case 'U':	/* ALTER OWNER */
						AlterTableOwner(stmt->relname,
										stmt->name);
						break;
					default:	/* oops */
						elog(ERROR, "T_AlterTableStmt: unknown subtype");
						break;
				}
			}
			break;


		case T_ChangeACLStmt:
			{
				ChangeACLStmt *stmt = (ChangeACLStmt *) parsetree;

				set_ps_display(commandTag = "CHANGE");

				ExecuteChangeACLStmt(stmt);
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

				set_ps_display(commandTag = "CREATE");

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

				set_ps_display(commandTag = "CREATE");

				DefineView(stmt->viewname, stmt->query);		/* retrieve parsetree */
			}
			break;

		case T_ProcedureStmt:	/* CREATE FUNCTION */
			set_ps_display(commandTag = "CREATE");

			CreateFunction((ProcedureStmt *) parsetree, dest);	/* everything */
			break;

		case T_IndexStmt:		/* CREATE INDEX */
			{
				IndexStmt  *stmt = (IndexStmt *) parsetree;

				set_ps_display(commandTag = "CREATE");

				DefineIndex(stmt->relname,		/* relation name */
							stmt->idxname,		/* index name */
							stmt->accessMethod, /* am name */
							stmt->indexParams,	/* parameters */
							stmt->withClause,
							stmt->unique,
							stmt->primary,
							(Expr *) stmt->whereClause,
							stmt->rangetable);
			}
			break;

		case T_RuleStmt:		/* CREATE RULE */
			{
				RuleStmt   *stmt = (RuleStmt *) parsetree;
				int			aclcheck_result;

				relname = stmt->object->relname;
				aclcheck_result = pg_aclcheck(relname, GetUserId(), ACL_RU);
				if (aclcheck_result != ACLCHECK_OK)
					elog(ERROR, "%s: %s", relname, aclcheck_error_strings[aclcheck_result]);
				set_ps_display(commandTag = "CREATE");

				DefineQueryRewrite(stmt);
			}
			break;

		case T_CreateSeqStmt:
			set_ps_display(commandTag = "CREATE");

			DefineSequence((CreateSeqStmt *) parsetree);
			break;

		case T_ExtendStmt:
			{
				ExtendStmt *stmt = (ExtendStmt *) parsetree;

				set_ps_display(commandTag = "EXTEND");

				ExtendIndex(stmt->idxname,		/* index name */
							(Expr *) stmt->whereClause, /* where */
							stmt->rangetable);
			}
			break;

		case T_RemoveAggrStmt:
			{
				RemoveAggrStmt *stmt = (RemoveAggrStmt *) parsetree;
				char	   *typename = (char *) NULL;

				set_ps_display(commandTag = "DROP");

				if (stmt->aggtype != NULL)
					typename = TypeNameToInternalName((TypeName *) stmt->aggtype);

				RemoveAggregate(stmt->aggname, typename);
			}
			break;

		case T_RemoveFuncStmt:
			{
				RemoveFuncStmt *stmt = (RemoveFuncStmt *) parsetree;

				set_ps_display(commandTag = "DROP");

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

				set_ps_display(commandTag = "DROP");

				if (typenode1 != NULL)
					typename1 = TypeNameToInternalName(typenode1);
				if (typenode2 != NULL)
					typename2 = TypeNameToInternalName(typenode2);

				RemoveOperator(stmt->opname, typename1, typename2);
			}
			break;

		case T_VersionStmt:
			elog(ERROR, "CREATE VERSION is not currently implemented");
			break;

		case T_CreatedbStmt:
			{
				CreatedbStmt *stmt = (CreatedbStmt *) parsetree;

				set_ps_display(commandTag = "CREATE DATABASE");

				createdb(stmt->dbname, stmt->dbpath,
						 stmt->dbtemplate, stmt->encoding);
			}
			break;

		case T_DropdbStmt:
			{
				DropdbStmt *stmt = (DropdbStmt *) parsetree;

				set_ps_display(commandTag = "DROP DATABASE");

				dropdb(stmt->dbname);
			}
			break;

			/* Query-level asynchronous notification */
		case T_NotifyStmt:
			{
				NotifyStmt *stmt = (NotifyStmt *) parsetree;

				set_ps_display(commandTag = "NOTIFY");

				Async_Notify(stmt->relname);
			}
			break;

		case T_ListenStmt:
			{
				ListenStmt *stmt = (ListenStmt *) parsetree;

				set_ps_display(commandTag = "LISTEN");

				Async_Listen(stmt->relname, MyProcPid);
			}
			break;

		case T_UnlistenStmt:
			{
				UnlistenStmt *stmt = (UnlistenStmt *) parsetree;

				set_ps_display(commandTag = "UNLISTEN");

				Async_Unlisten(stmt->relname, MyProcPid);
			}
			break;

			/*
			 * ******************************** dynamic loader ********************************
			 *
			 */
		case T_LoadStmt:
			{
				LoadStmt   *stmt = (LoadStmt *) parsetree;

				set_ps_display(commandTag = "LOAD");

				closeAllVfds(); /* probably not necessary... */
				load_file(stmt->filename);
			}
			break;

		case T_ClusterStmt:
			{
				ClusterStmt *stmt = (ClusterStmt *) parsetree;

				set_ps_display(commandTag = "CLUSTER");

				relname = stmt->relname;
				if (IsSystemRelationName(relname))
					elog(ERROR, "CLUSTER: relation \"%s\" is a system catalog",
						 relname);
				if (!pg_ownercheck(GetUserId(), relname, RELNAME))
					elog(ERROR, "permission denied");

				cluster(relname, stmt->indexname);
			}
			break;

		case T_VacuumStmt:
			set_ps_display(commandTag = "VACUUM");

			vacuum(((VacuumStmt *) parsetree)->vacrel,
				   ((VacuumStmt *) parsetree)->verbose,
				   ((VacuumStmt *) parsetree)->analyze,
				   ((VacuumStmt *) parsetree)->va_spec);
			break;

		case T_ExplainStmt:
			{
				ExplainStmt *stmt = (ExplainStmt *) parsetree;

				set_ps_display(commandTag = "EXPLAIN");

				ExplainQuery(stmt->query, stmt->verbose, dest);
			}
			break;

#ifdef NOT_USED

			/*
			 * ******************************** Tioga-related statements *******************************
			 */
		case T_RecipeStmt:
			{
				RecipeStmt *stmt = (RecipeStmt *) parsetree;

				set_ps_display(commandTag = "EXECUTE RECIPE");

				beginRecipe(stmt);
			}
			break;
#endif

			/*
			 * ******************************** set variable statements *******************************
			 */
		case T_VariableSetStmt:
			{
				VariableSetStmt *n = (VariableSetStmt *) parsetree;

				SetPGVariable(n->name, n->value);
				set_ps_display(commandTag = "SET VARIABLE");
			}
			break;

		case T_VariableShowStmt:
			{
				VariableShowStmt *n = (VariableShowStmt *) parsetree;

				GetPGVariable(n->name);
				set_ps_display(commandTag = "SHOW VARIABLE");
			}
			break;

		case T_VariableResetStmt:
			{
				VariableResetStmt *n = (VariableResetStmt *) parsetree;

				ResetPGVariable(n->name);
				set_ps_display(commandTag = "RESET VARIABLE");
			}
			break;

			/*
			 * ******************************** TRIGGER statements *******************************
			 */
		case T_CreateTrigStmt:
			set_ps_display(commandTag = "CREATE");

			CreateTrigger((CreateTrigStmt *) parsetree);
			break;

		case T_DropTrigStmt:
			set_ps_display(commandTag = "DROP");

			DropTrigger((DropTrigStmt *) parsetree);
			break;

			/*
			 * ************* PROCEDURAL LANGUAGE statements *****************
			 */
		case T_CreatePLangStmt:
			set_ps_display(commandTag = "CREATE");

			CreateProceduralLanguage((CreatePLangStmt *) parsetree);
			break;

		case T_DropPLangStmt:
			set_ps_display(commandTag = "DROP");

			DropProceduralLanguage((DropPLangStmt *) parsetree);
			break;

			/*
			 * ******************************** USER statements ****
			 *
			 */
		case T_CreateUserStmt:
			set_ps_display(commandTag = "CREATE USER");

			CreateUser((CreateUserStmt *) parsetree);
			break;

		case T_AlterUserStmt:
			set_ps_display(commandTag = "ALTER USER");

			AlterUser((AlterUserStmt *) parsetree);
			break;

		case T_DropUserStmt:
			set_ps_display(commandTag = "DROP USER");

			DropUser((DropUserStmt *) parsetree);
			break;

		case T_LockStmt:
			set_ps_display(commandTag = "LOCK TABLE");

			LockTableCommand((LockStmt *) parsetree);
			break;

		case T_ConstraintsSetStmt:
			set_ps_display(commandTag = "SET CONSTRAINTS");

			DeferredTriggerSetState((ConstraintsSetStmt *) parsetree);
			break;

		case T_CreateGroupStmt:
			set_ps_display(commandTag = "CREATE GROUP");

			CreateGroup((CreateGroupStmt *) parsetree);
			break;

		case T_AlterGroupStmt:
			set_ps_display(commandTag = "ALTER GROUP");

			AlterGroup((AlterGroupStmt *) parsetree, "ALTER GROUP");
			break;

		case T_DropGroupStmt:
			set_ps_display(commandTag = "DROP GROUP");

			DropGroup((DropGroupStmt *) parsetree);
			break;

		case T_CheckPointStmt:
			{
				set_ps_display(commandTag = "CHECKPOINT");

				if (!superuser())
					elog(ERROR, "permission denied");
				CreateCheckPoint(false);
			}
			break;

		case T_ReindexStmt:
			{
				ReindexStmt *stmt = (ReindexStmt *) parsetree;

				set_ps_display(commandTag = "REINDEX");

				switch (stmt->reindexType)
				{
					case INDEX:
						relname = (char *) stmt->name;
						if (IsSystemRelationName(relname))
						{
							if (!allowSystemTableMods && IsSystemRelationName(relname))
								elog(ERROR, "\"%s\" is a system index. call REINDEX under standalone postgres with -O -P options",
									 relname);
							if (!IsIgnoringSystemIndexes())
								elog(ERROR, "\"%s\" is a system index. call REINDEX under standalone postgres with -P -O options",
									 relname);
						}
						if (!pg_ownercheck(GetUserId(), relname, RELNAME))
							elog(ERROR, "%s: %s", relname, aclcheck_error_strings[ACLCHECK_NOT_OWNER]);
						ReindexIndex(relname, stmt->force);
						break;
					case TABLE:
						relname = (char *) stmt->name;
						if (IsSystemRelationName(relname))
						{
#ifdef	OLD_FILE_NAMING
							if (!allowSystemTableMods && IsSystemRelationName(relname))
								elog(ERROR, "\"%s\" is a system table. call REINDEX under standalone postgres with -O -P options",
									 relname);
							if (!IsIgnoringSystemIndexes())
								elog(ERROR, "\"%s\" is a system table. call REINDEX under standalone postgres with -P -O options",

									 relname);
#endif	 /* OLD_FILE_NAMING */
						}
						if (!pg_ownercheck(GetUserId(), relname, RELNAME))
							elog(ERROR, "%s: %s", relname, aclcheck_error_strings[ACLCHECK_NOT_OWNER]);
						ReindexTable(relname, stmt->force);
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

			/*
			 * ******************************** default ********************************
			 *
			 */
		default:
			elog(ERROR, "ProcessUtility: command #%d unsupported",
				 nodeTag(parsetree));
			break;
	}

	/* ----------------
	 *	tell fe/be or whatever that we're done.
	 * ----------------
	 */
	EndCommand(commandTag, dest);
}

/*-------------------------------------------------------------------------
 *
 * utility.c--
 *	  Contains functions which control the execution of the POSTGRES utility
 *	  commands.  At one time acted as an interface between the Lisp and C
 *	  systems.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/tcop/utility.c,v 1.31 1997/11/24 05:32:40 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "access/xact.h"
#include "access/heapam.h"
#include "catalog/catalog.h"
#include "catalog/pg_type.h"

#include "commands/async.h"
#include "commands/cluster.h"
#include "commands/command.h"
#include "commands/copy.h"
#include "commands/creatinh.h"
#include "commands/dbcommands.h"
#include "commands/sequence.h"
#include "commands/defrem.h"
#include "commands/rename.h"
#include "commands/view.h"
#include "commands/version.h"
#include "commands/vacuum.h"
#include "commands/recipe.h"
#include "commands/explain.h"
#include "commands/trigger.h"
#include "commands/proclang.h"

#include "nodes/parsenodes.h"
#include "../backend/parser/parse.h"
#include "utils/builtins.h"
#include "utils/acl.h"
#include "utils/palloc.h"
#include "rewrite/rewriteRemove.h"
#include "rewrite/rewriteDefine.h"
#include "tcop/tcopdebug.h"
#include "tcop/dest.h"
#include "tcop/variable.h"
#include "tcop/utility.h"
#include "fmgr.h"				/* For load_file() */
#include "storage/fd.h"

#ifndef NO_SECURITY
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/syscache.h"
#endif


/* ----------------
 *		CHECK_IF_ABORTED() is used to avoid doing unnecessary
 *		processing within an aborted transaction block.
 * ----------------
 */
#define CHECK_IF_ABORTED() \
	if (IsAbortedTransactionBlockState()) { \
		elog(NOTICE, "(transaction aborted): %s", \
			 "queries ignored until END"); \
		commandTag = "*ABORT STATE*"; \
		break; \
	} \

/* ----------------
 *		general utility function invoker
 * ----------------
 */
void
ProcessUtility(Node * parsetree,
			   CommandDest dest)
{
	char	   *commandTag = NULL;
	char	   *relname;
	char	   *relationName;
	char	   *userName;

	userName = GetPgUserName();

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
						commandTag = "BEGIN";
						CHECK_IF_ABORTED();
						BeginTransactionBlock();
						break;

					case END_TRANS:
						commandTag = "END";
						EndTransactionBlock();
						break;

					case ABORT_TRANS:
						commandTag = "ABORT";
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

				commandTag = "CLOSE";
				CHECK_IF_ABORTED();

				PerformPortalClose(stmt->portalname, dest);
			}
			break;

		case T_FetchStmt:
			{
				FetchStmt  *stmt = (FetchStmt *) parsetree;
				char	   *portalName = stmt->portalname;
				bool		forward;
				int			count;

				commandTag = (stmt->ismove) ? "MOVE" : "FETCH";
				CHECK_IF_ABORTED();

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
			commandTag = "CREATE";
			CHECK_IF_ABORTED();

			DefineRelation((CreateStmt *) parsetree);
			break;

		case T_DestroyStmt:
			{
				DestroyStmt *stmt = (DestroyStmt *) parsetree;
				List	   *arg;
				List	   *args = stmt->relNames;
				Relation	rel;

				commandTag = "DROP";
				CHECK_IF_ABORTED();

				foreach(arg, args)
				{
					relname = strVal(lfirst(arg));
					if (IsSystemRelationName(relname))
						elog(WARN, "class \"%s\" is a system catalog",
							 relname);
					rel = heap_openr(relname);
					if (RelationIsValid(rel))
					{
						if (stmt->sequence &&
							rel->rd_rel->relkind != RELKIND_SEQUENCE)
							elog(WARN, "Use DROP TABLE to drop table '%s'",
								 relname);
						if (!(stmt->sequence) &&
							rel->rd_rel->relkind == RELKIND_SEQUENCE)
							elog(WARN, "Use DROP SEQUENCE to drop sequence '%s'",
								 relname);
						heap_close(rel);
					}
#ifndef NO_SECURITY
					if (!pg_ownercheck(userName, relname, RELNAME))
						elog(WARN, "you do not own class \"%s\"",
							 relname);
#endif
				}
				foreach(arg, args)
				{
					relname = strVal(lfirst(arg));
					RemoveRelation(relname);
				}
			}
			break;

		case T_CopyStmt:
			{
				CopyStmt   *stmt = (CopyStmt *) parsetree;

				commandTag = "COPY";
				CHECK_IF_ABORTED();

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
					   stmt->delimiter);
			}
			break;

		case T_AddAttrStmt:
			{
				AddAttrStmt *stmt = (AddAttrStmt *) parsetree;

				commandTag = "ADD";
				CHECK_IF_ABORTED();

				/*
				 * owner checking done in PerformAddAttribute (now
				 * recursive)
				 */
				PerformAddAttribute(stmt->relname,
									userName,
									stmt->inh,
									stmt->colDef);
			}
			break;

			/*
			 * schema
			 */
		case T_RenameStmt:
			{
				RenameStmt *stmt = (RenameStmt *) parsetree;

				commandTag = "RENAME";
				CHECK_IF_ABORTED();

				relname = stmt->relname;
				if (IsSystemRelationName(relname))
					elog(WARN, "class \"%s\" is a system catalog",
						 relname);
#ifndef NO_SECURITY
				if (!pg_ownercheck(userName, relname, RELNAME))
					elog(WARN, "you do not own class \"%s\"",
						 relname);
#endif

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
					TypeRename(relname, /* old name */
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
							  userName,
							  stmt->inh);		/* recursive? */
				}
			}
			break;

		case T_ChangeACLStmt:
			{
				ChangeACLStmt *stmt = (ChangeACLStmt *) parsetree;
				List	   *i;
				AclItem    *aip;
				unsigned	modechg;

				commandTag = "CHANGE";
				CHECK_IF_ABORTED();

				aip = stmt->aclitem;

				modechg = stmt->modechg;
#ifndef NO_SECURITY
				foreach(i, stmt->relNames)
				{
					relname = strVal(lfirst(i));
					if (!pg_ownercheck(userName, relname, RELNAME))
						elog(WARN, "you do not own class \"%s\"",
							 relname);
				}
#endif
				foreach(i, stmt->relNames)
				{
					relname = strVal(lfirst(i));
					ChangeAcl(relname, aip, modechg);
				}

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

				commandTag = "CREATE";
				CHECK_IF_ABORTED();

				switch (stmt->defType)
				{
					case OPERATOR:
						DefineOperator(stmt->defname,	/* operator name */
									   stmt->definition);		/* rest */
						break;
					case TYPE_P:
						{
							DefineType(stmt->defname, stmt->definition);
						}
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

				commandTag = "CREATE";
				CHECK_IF_ABORTED();
				DefineView(stmt->viewname, stmt->query);		/* retrieve parsetree */
			}
			break;

		case T_ProcedureStmt:	/* CREATE FUNCTION */
			commandTag = "CREATE";
			CHECK_IF_ABORTED();
			CreateFunction((ProcedureStmt *) parsetree, dest);	/* everything */
			break;

		case T_IndexStmt:		/* CREATE INDEX */
			{
				IndexStmt  *stmt = (IndexStmt *) parsetree;

				commandTag = "CREATE";
				CHECK_IF_ABORTED();
				DefineIndex(stmt->relname,		/* relation name */
							stmt->idxname,		/* index name */
							stmt->accessMethod, /* am name */
							stmt->indexParams,	/* parameters */
							stmt->withClause,
							stmt->unique,
							(Expr *) stmt->whereClause,
							stmt->rangetable);
			}
			break;

		case T_RuleStmt:		/* CREATE RULE */
			{
				RuleStmt   *stmt = (RuleStmt *) parsetree;
				int			aclcheck_result;

#ifndef NO_SECURITY
				relname = stmt->object->relname;
				aclcheck_result = pg_aclcheck(relname, userName, ACL_RU);
				if (aclcheck_result != ACLCHECK_OK)
					elog(WARN, "%s: %s", relname, aclcheck_error_strings[aclcheck_result]);
#endif
				commandTag = "CREATE";
				CHECK_IF_ABORTED();
				DefineQueryRewrite(stmt);
			}
			break;

		case T_CreateSeqStmt:
			commandTag = "CREATE";
			CHECK_IF_ABORTED();

			DefineSequence((CreateSeqStmt *) parsetree);
			break;

		case T_ExtendStmt:
			{
				ExtendStmt *stmt = (ExtendStmt *) parsetree;

				commandTag = "EXTEND";
				CHECK_IF_ABORTED();

				ExtendIndex(stmt->idxname,		/* index name */
							(Expr *) stmt->whereClause, /* where */
							stmt->rangetable);
			}
			break;

		case T_RemoveStmt:
			{
				RemoveStmt *stmt = (RemoveStmt *) parsetree;

				commandTag = "DROP";
				CHECK_IF_ABORTED();

				switch (stmt->removeType)
				{
					case INDEX:
						relname = stmt->name;
						if (IsSystemRelationName(relname))
							elog(WARN, "class \"%s\" is a system catalog index",
								 relname);
#ifndef NO_SECURITY
						if (!pg_ownercheck(userName, relname, RELNAME))
							elog(WARN, "%s: %s", relname, aclcheck_error_strings[ACLCHECK_NOT_OWNER]);
#endif
						RemoveIndex(relname);
						break;
					case RULE:
						{
							char	   *rulename = stmt->name;
							int			aclcheck_result;

#ifndef NO_SECURITY

							relationName = RewriteGetRuleEventRel(rulename);
							aclcheck_result = pg_aclcheck(relationName, userName, ACL_RU);
							if (aclcheck_result != ACLCHECK_OK)
							{
								elog(WARN, "%s: %s", relationName, aclcheck_error_strings[aclcheck_result]);
							}
#endif
							RemoveRewriteRule(rulename);
						}
						break;
					case TYPE_P:
#ifndef NO_SECURITY
						/* XXX moved to remove.c */
#endif
						RemoveType(stmt->name);
						break;
					case VIEW:
						{
							char	   *viewName = stmt->name;
							char	   *ruleName;

#ifndef NO_SECURITY

							ruleName = MakeRetrieveViewRuleName(viewName);
							relationName = RewriteGetRuleEventRel(ruleName);
							if (!pg_ownercheck(userName, relationName, RELNAME))
								elog(WARN, "%s: %s", relationName, aclcheck_error_strings[ACLCHECK_NOT_OWNER]);
							pfree(ruleName);
#endif
							RemoveView(viewName);
						}
						break;
				}
				break;
			}
			break;

		case T_RemoveAggrStmt:
			{
				RemoveAggrStmt *stmt = (RemoveAggrStmt *) parsetree;

				commandTag = "DROP";
				CHECK_IF_ABORTED();
				RemoveAggregate(stmt->aggname, stmt->aggtype);
			}
			break;

		case T_RemoveFuncStmt:
			{
				RemoveFuncStmt *stmt = (RemoveFuncStmt *) parsetree;

				commandTag = "DROP";
				CHECK_IF_ABORTED();
				RemoveFunction(stmt->funcname,
							   length(stmt->args),
							   stmt->args);
			}
			break;

		case T_RemoveOperStmt:
			{
				RemoveOperStmt *stmt = (RemoveOperStmt *) parsetree;
				char	   *type1 = (char *) NULL;
				char	   *type2 = (char *) NULL;

				commandTag = "DROP";
				CHECK_IF_ABORTED();

				if (lfirst(stmt->args) != NULL)
					type1 = strVal(lfirst(stmt->args));
				if (lsecond(stmt->args) != NULL)
					type2 = strVal(lsecond(stmt->args));
				RemoveOperator(stmt->opname, type1, type2);
			}
			break;

		case T_VersionStmt:
			{
				elog(WARN, "CREATE VERSION is not currently implemented");
			}
			break;

		case T_CreatedbStmt:
			{
				CreatedbStmt *stmt = (CreatedbStmt *) parsetree;

				commandTag = "CREATEDB";
				CHECK_IF_ABORTED();
				createdb(stmt->dbname, stmt->dbpath);
			}
			break;

		case T_DestroydbStmt:
			{
				DestroydbStmt *stmt = (DestroydbStmt *) parsetree;

				commandTag = "DESTROYDB";
				CHECK_IF_ABORTED();
				destroydb(stmt->dbname);
			}
			break;

			/* Query-level asynchronous notification */
		case T_NotifyStmt:
			{
				NotifyStmt *stmt = (NotifyStmt *) parsetree;

				commandTag = "NOTIFY";
				CHECK_IF_ABORTED();

				Async_Notify(stmt->relname);
			}
			break;

		case T_ListenStmt:
			{
				ListenStmt *stmt = (ListenStmt *) parsetree;

				commandTag = "LISTEN";
				CHECK_IF_ABORTED();

				Async_Listen(stmt->relname, MasterPid);
			}
			break;

			/*
			 * ******************************** dynamic loader ********************************
			 *
			 */
		case T_LoadStmt:
			{
				LoadStmt   *stmt = (LoadStmt *) parsetree;
				FILE	   *fp;
				char	   *filename;

				commandTag = "LOAD";
				CHECK_IF_ABORTED();

				filename = stmt->filename;
				closeAllVfds();
				if ((fp = AllocateFile(filename, "r")) == NULL)
					elog(WARN, "LOAD: could not open file %s", filename);
				FreeFile(fp);
				load_file(filename);
			}
			break;

		case T_ClusterStmt:
			{
				ClusterStmt *stmt = (ClusterStmt *) parsetree;

				commandTag = "CLUSTER";
				CHECK_IF_ABORTED();

				cluster(stmt->relname, stmt->indexname);
			}
			break;

		case T_VacuumStmt:
			commandTag = "VACUUM";
			CHECK_IF_ABORTED();
			vacuum(((VacuumStmt *) parsetree)->vacrel,
				   ((VacuumStmt *) parsetree)->verbose,
				   ((VacuumStmt *) parsetree)->analyze,
				   ((VacuumStmt *) parsetree)->va_spec);
			break;

		case T_ExplainStmt:
			{
				ExplainStmt *stmt = (ExplainStmt *) parsetree;

				commandTag = "EXPLAIN";
				CHECK_IF_ABORTED();

				ExplainQuery(stmt->query, stmt->verbose, dest);
			}
			break;

			/*
			 * ******************************** Tioga-related statements *******************************
			 */
		case T_RecipeStmt:
			{
				RecipeStmt *stmt = (RecipeStmt *) parsetree;

				commandTag = "EXECUTE RECIPE";
				CHECK_IF_ABORTED();
				beginRecipe(stmt);
			}
			break;

			/*
			 * ******************************** set variable statements *******************************
			 */
		case T_VariableSetStmt:
			{
				VariableSetStmt *n = (VariableSetStmt *) parsetree;

				SetPGVariable(n->name, n->value);
				commandTag = "SET VARIABLE";
			}
			break;

		case T_VariableShowStmt:
			{
				VariableShowStmt *n = (VariableShowStmt *) parsetree;

				GetPGVariable(n->name);
				commandTag = "SHOW VARIABLE";
			}
			break;

		case T_VariableResetStmt:
			{
				VariableResetStmt *n = (VariableResetStmt *) parsetree;

				ResetPGVariable(n->name);
				commandTag = "RESET VARIABLE";
			}
			break;

			/*
			 * ******************************** TRIGGER statements *******************************
			 */
		case T_CreateTrigStmt:
			commandTag = "CREATE";
			CHECK_IF_ABORTED();

			CreateTrigger((CreateTrigStmt *) parsetree);
			break;

		case T_DropTrigStmt:
			commandTag = "DROP";
			CHECK_IF_ABORTED();

			DropTrigger((DropTrigStmt *) parsetree);
			break;

			/*
			 * ************* PROCEDURAL LANGUAGE statements *****************
			 */
		case T_CreatePLangStmt:
			commandTag = "CREATE";
			CHECK_IF_ABORTED();

			CreateProceduralLanguage((CreatePLangStmt *) parsetree);
			break;

		case T_DropPLangStmt:
			commandTag = "DROP";
			CHECK_IF_ABORTED();

			DropProceduralLanguage((DropPLangStmt *) parsetree);
			break;

			/*
			 * ******************************** default ********************************
			 *
			 */
		default:
			elog(WARN, "ProcessUtility: command #%d unsupported",
				 nodeTag(parsetree));
			break;
	}

	/* ----------------
	 *	tell fe/be or whatever that we're done.
	 * ----------------
	 */
	EndCommand(commandTag, dest);
}

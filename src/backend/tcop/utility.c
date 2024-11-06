/*-------------------------------------------------------------------------
 *
 * utility.c
 *	  Contains functions which control the execution of the POSTGRES utility
 *	  commands.  At one time acted as an interface between the Lisp and C
 *	  systems.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/tcop/utility.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/reloptions.h"
#include "access/twophase.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/namespace.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_inherits.h"
#include "catalog/toasting.h"
#include "commands/alter.h"
#include "commands/async.h"
#include "commands/cluster.h"
#include "commands/collationcmds.h"
#include "commands/comment.h"
#include "commands/conversioncmds.h"
#include "commands/copy.h"
#include "commands/createas.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "commands/discard.h"
#include "commands/event_trigger.h"
#include "commands/explain.h"
#include "commands/extension.h"
#include "commands/lockcmds.h"
#include "commands/matview.h"
#include "commands/policy.h"
#include "commands/portalcmds.h"
#include "commands/prepare.h"
#include "commands/proclang.h"
#include "commands/publicationcmds.h"
#include "commands/schemacmds.h"
#include "commands/seclabel.h"
#include "commands/sequence.h"
#include "commands/subscriptioncmds.h"
#include "commands/tablecmds.h"
#include "commands/tablespace.h"
#include "commands/trigger.h"
#include "commands/typecmds.h"
#include "commands/user.h"
#include "commands/vacuum.h"
#include "commands/view.h"
#include "miscadmin.h"
#include "parser/parse_utilcmd.h"
#include "postmaster/bgwriter.h"
#include "rewrite/rewriteDefine.h"
#include "storage/fd.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"

/* Hook for plugins to get control in ProcessUtility() */
ProcessUtility_hook_type ProcessUtility_hook = NULL;

/* local function declarations */
static int	ClassifyUtilityCommandAsReadOnly(Node *parsetree);
static void ProcessUtilitySlow(ParseState *pstate,
							   PlannedStmt *pstmt,
							   const char *queryString,
							   ProcessUtilityContext context,
							   ParamListInfo params,
							   QueryEnvironment *queryEnv,
							   DestReceiver *dest,
							   QueryCompletion *qc);
static void ExecDropStmt(DropStmt *stmt, bool isTopLevel);

/*
 * CommandIsReadOnly: is an executable query read-only?
 *
 * This is a much stricter test than we apply for XactReadOnly mode;
 * the query must be *in truth* read-only, because the caller wishes
 * not to do CommandCounterIncrement for it.
 *
 * Note: currently no need to support raw or analyzed queries here
 */
bool
CommandIsReadOnly(PlannedStmt *pstmt)
{
	Assert(IsA(pstmt, PlannedStmt));
	switch (pstmt->commandType)
	{
		case CMD_SELECT:
			if (pstmt->rowMarks != NIL)
				return false;	/* SELECT FOR [KEY] UPDATE/SHARE */
			else if (pstmt->hasModifyingCTE)
				return false;	/* data-modifying CTE */
			else
				return true;
		case CMD_UPDATE:
		case CMD_INSERT:
		case CMD_DELETE:
		case CMD_MERGE:
			return false;
		case CMD_UTILITY:
			/* For now, treat all utility commands as read/write */
			return false;
		default:
			elog(WARNING, "unrecognized commandType: %d",
				 (int) pstmt->commandType);
			break;
	}
	return false;
}

/*
 * Determine the degree to which a utility command is read only.
 *
 * Note the definitions of the relevant flags in src/include/utility/tcop.h.
 */
static int
ClassifyUtilityCommandAsReadOnly(Node *parsetree)
{
	switch (nodeTag(parsetree))
	{
		case T_AlterCollationStmt:
		case T_AlterDatabaseRefreshCollStmt:
		case T_AlterDatabaseSetStmt:
		case T_AlterDatabaseStmt:
		case T_AlterDefaultPrivilegesStmt:
		case T_AlterDomainStmt:
		case T_AlterEnumStmt:
		case T_AlterEventTrigStmt:
		case T_AlterExtensionContentsStmt:
		case T_AlterExtensionStmt:
		case T_AlterFdwStmt:
		case T_AlterForeignServerStmt:
		case T_AlterFunctionStmt:
		case T_AlterObjectDependsStmt:
		case T_AlterObjectSchemaStmt:
		case T_AlterOpFamilyStmt:
		case T_AlterOperatorStmt:
		case T_AlterOwnerStmt:
		case T_AlterPolicyStmt:
		case T_AlterPublicationStmt:
		case T_AlterRoleSetStmt:
		case T_AlterRoleStmt:
		case T_AlterSeqStmt:
		case T_AlterStatsStmt:
		case T_AlterSubscriptionStmt:
		case T_AlterTSConfigurationStmt:
		case T_AlterTSDictionaryStmt:
		case T_AlterTableMoveAllStmt:
		case T_AlterTableSpaceOptionsStmt:
		case T_AlterTableStmt:
		case T_AlterTypeStmt:
		case T_AlterUserMappingStmt:
		case T_CommentStmt:
		case T_CompositeTypeStmt:
		case T_CreateAmStmt:
		case T_CreateCastStmt:
		case T_CreateConversionStmt:
		case T_CreateDomainStmt:
		case T_CreateEnumStmt:
		case T_CreateEventTrigStmt:
		case T_CreateExtensionStmt:
		case T_CreateFdwStmt:
		case T_CreateForeignServerStmt:
		case T_CreateForeignTableStmt:
		case T_CreateFunctionStmt:
		case T_CreateOpClassStmt:
		case T_CreateOpFamilyStmt:
		case T_CreatePLangStmt:
		case T_CreatePolicyStmt:
		case T_CreatePublicationStmt:
		case T_CreateRangeStmt:
		case T_CreateRoleStmt:
		case T_CreateSchemaStmt:
		case T_CreateSeqStmt:
		case T_CreateStatsStmt:
		case T_CreateStmt:
		case T_CreateSubscriptionStmt:
		case T_CreateTableAsStmt:
		case T_CreateTableSpaceStmt:
		case T_CreateTransformStmt:
		case T_CreateTrigStmt:
		case T_CreateUserMappingStmt:
		case T_CreatedbStmt:
		case T_DefineStmt:
		case T_DropOwnedStmt:
		case T_DropRoleStmt:
		case T_DropStmt:
		case T_DropSubscriptionStmt:
		case T_DropTableSpaceStmt:
		case T_DropUserMappingStmt:
		case T_DropdbStmt:
		case T_GrantRoleStmt:
		case T_GrantStmt:
		case T_ImportForeignSchemaStmt:
		case T_IndexStmt:
		case T_ReassignOwnedStmt:
		case T_RefreshMatViewStmt:
		case T_RenameStmt:
		case T_RuleStmt:
		case T_SecLabelStmt:
		case T_TruncateStmt:
		case T_ViewStmt:
			{
				/* DDL is not read-only, and neither is TRUNCATE. */
				return COMMAND_IS_NOT_READ_ONLY;
			}

		case T_AlterSystemStmt:
			{
				/*
				 * Surprisingly, ALTER SYSTEM meets all our definitions of
				 * read-only: it changes nothing that affects the output of
				 * pg_dump, it doesn't write WAL or imperil the application of
				 * future WAL, and it doesn't depend on any state that needs
				 * to be synchronized with parallel workers.
				 *
				 * So, despite the fact that it writes to a file, it's read
				 * only!
				 */
				return COMMAND_IS_STRICTLY_READ_ONLY;
			}

		case T_CallStmt:
		case T_DoStmt:
			{
				/*
				 * Commands inside the DO block or the called procedure might
				 * not be read only, but they'll be checked separately when we
				 * try to execute them.  Here we only need to worry about the
				 * DO or CALL command itself.
				 */
				return COMMAND_IS_STRICTLY_READ_ONLY;
			}

		case T_CheckPointStmt:
			{
				/*
				 * You might think that this should not be permitted in
				 * recovery, but we interpret a CHECKPOINT command during
				 * recovery as a request for a restartpoint instead. We allow
				 * this since it can be a useful way of reducing switchover
				 * time when using various forms of replication.
				 */
				return COMMAND_IS_STRICTLY_READ_ONLY;
			}

		case T_ClosePortalStmt:
		case T_ConstraintsSetStmt:
		case T_DeallocateStmt:
		case T_DeclareCursorStmt:
		case T_DiscardStmt:
		case T_ExecuteStmt:
		case T_FetchStmt:
		case T_LoadStmt:
		case T_PrepareStmt:
		case T_UnlistenStmt:
		case T_VariableSetStmt:
			{
				/*
				 * These modify only backend-local state, so they're OK to run
				 * in a read-only transaction or on a standby. However, they
				 * are disallowed in parallel mode, because they either rely
				 * upon or modify backend-local state that might not be
				 * synchronized among cooperating backends.
				 */
				return COMMAND_OK_IN_RECOVERY | COMMAND_OK_IN_READ_ONLY_TXN;
			}

		case T_ClusterStmt:
		case T_ReindexStmt:
		case T_VacuumStmt:
			{
				/*
				 * These commands write WAL, so they're not strictly
				 * read-only, and running them in parallel workers isn't
				 * supported.
				 *
				 * However, they don't change the database state in a way that
				 * would affect pg_dump output, so it's fine to run them in a
				 * read-only transaction. (CLUSTER might change the order of
				 * rows on disk, which could affect the ordering of pg_dump
				 * output, but that's not semantically significant.)
				 */
				return COMMAND_OK_IN_READ_ONLY_TXN;
			}

		case T_CopyStmt:
			{
				CopyStmt   *stmt = (CopyStmt *) parsetree;

				/*
				 * You might think that COPY FROM is not at all read only, but
				 * it's OK to copy into a temporary table, because that
				 * wouldn't change the output of pg_dump.  If the target table
				 * turns out to be non-temporary, DoCopy itself will call
				 * PreventCommandIfReadOnly.
				 */
				if (stmt->is_from)
					return COMMAND_OK_IN_READ_ONLY_TXN;
				else
					return COMMAND_IS_STRICTLY_READ_ONLY;
			}

		case T_ExplainStmt:
		case T_VariableShowStmt:
			{
				/*
				 * These commands don't modify any data and are safe to run in
				 * a parallel worker.
				 */
				return COMMAND_IS_STRICTLY_READ_ONLY;
			}

		case T_ListenStmt:
		case T_NotifyStmt:
			{
				/*
				 * NOTIFY requires an XID assignment, so it can't be permitted
				 * on a standby. Perhaps LISTEN could, since without NOTIFY it
				 * would be OK to just do nothing, at least until promotion,
				 * but we currently prohibit it lest the user get the wrong
				 * idea.
				 *
				 * (We do allow T_UnlistenStmt on a standby, though, because
				 * it's a no-op.)
				 */
				return COMMAND_OK_IN_READ_ONLY_TXN;
			}

		case T_LockStmt:
			{
				LockStmt   *stmt = (LockStmt *) parsetree;

				/*
				 * Only weaker locker modes are allowed during recovery. The
				 * restrictions here must match those in
				 * LockAcquireExtended().
				 */
				if (stmt->mode > RowExclusiveLock)
					return COMMAND_OK_IN_READ_ONLY_TXN;
				else
					return COMMAND_IS_STRICTLY_READ_ONLY;
			}

		case T_TransactionStmt:
			{
				TransactionStmt *stmt = (TransactionStmt *) parsetree;

				/*
				 * PREPARE, COMMIT PREPARED, and ROLLBACK PREPARED all write
				 * WAL, so they're not read-only in the strict sense; but the
				 * first and third do not change pg_dump output, so they're OK
				 * in a read-only transactions.
				 *
				 * We also consider COMMIT PREPARED to be OK in a read-only
				 * transaction environment, by way of exception.
				 */
				switch (stmt->kind)
				{
					case TRANS_STMT_BEGIN:
					case TRANS_STMT_START:
					case TRANS_STMT_COMMIT:
					case TRANS_STMT_ROLLBACK:
					case TRANS_STMT_SAVEPOINT:
					case TRANS_STMT_RELEASE:
					case TRANS_STMT_ROLLBACK_TO:
						return COMMAND_IS_STRICTLY_READ_ONLY;

					case TRANS_STMT_PREPARE:
					case TRANS_STMT_COMMIT_PREPARED:
					case TRANS_STMT_ROLLBACK_PREPARED:
						return COMMAND_OK_IN_READ_ONLY_TXN;
				}
				elog(ERROR, "unrecognized TransactionStmtKind: %d",
					 (int) stmt->kind);
				return 0;		/* silence stupider compilers */
			}

		default:
			elog(ERROR, "unrecognized node type: %d",
				 (int) nodeTag(parsetree));
			return 0;			/* silence stupider compilers */
	}
}

/*
 * PreventCommandIfReadOnly: throw error if XactReadOnly
 *
 * This is useful partly to ensure consistency of the error message wording;
 * some callers have checked XactReadOnly for themselves.
 */
void
PreventCommandIfReadOnly(const char *cmdname)
{
	if (XactReadOnly)
		ereport(ERROR,
				(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
		/* translator: %s is name of a SQL command, eg CREATE */
				 errmsg("cannot execute %s in a read-only transaction",
						cmdname)));
}

/*
 * PreventCommandIfParallelMode: throw error if current (sub)transaction is
 * in parallel mode.
 *
 * This is useful partly to ensure consistency of the error message wording;
 * some callers have checked IsInParallelMode() for themselves.
 */
void
PreventCommandIfParallelMode(const char *cmdname)
{
	if (IsInParallelMode())
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
		/* translator: %s is name of a SQL command, eg CREATE */
				 errmsg("cannot execute %s during a parallel operation",
						cmdname)));
}

/*
 * PreventCommandDuringRecovery: throw error if RecoveryInProgress
 *
 * The majority of operations that are unsafe in a Hot Standby
 * will be rejected by XactReadOnly tests.  However there are a few
 * commands that are allowed in "read-only" xacts but cannot be allowed
 * in Hot Standby mode.  Those commands should call this function.
 */
void
PreventCommandDuringRecovery(const char *cmdname)
{
	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
		/* translator: %s is name of a SQL command, eg CREATE */
				 errmsg("cannot execute %s during recovery",
						cmdname)));
}

/*
 * CheckRestrictedOperation: throw error for hazardous command if we're
 * inside a security restriction context.
 *
 * This is needed to protect session-local state for which there is not any
 * better-defined protection mechanism, such as ownership.
 */
static void
CheckRestrictedOperation(const char *cmdname)
{
	if (InSecurityRestrictedOperation())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
		/* translator: %s is name of a SQL command, eg PREPARE */
				 errmsg("cannot execute %s within security-restricted operation",
						cmdname)));
}

/*
 * ProcessUtility
 *		general utility function invoker
 *
 *	pstmt: PlannedStmt wrapper for the utility statement
 *	queryString: original source text of command
 *	readOnlyTree: if true, pstmt's node tree must not be modified
 *	context: identifies source of statement (toplevel client command,
 *		non-toplevel client command, subcommand of a larger utility command)
 *	params: parameters to use during execution
 *	queryEnv: environment for parse through execution (e.g., ephemeral named
 *		tables like trigger transition tables).  May be NULL.
 *	dest: where to send results
 *	qc: where to store command completion status data.  May be NULL,
 *		but if not, then caller must have initialized it.
 *
 * Caller MUST supply a queryString; it is not allowed (anymore) to pass NULL.
 * If you really don't have source text, you can pass a constant string,
 * perhaps "(query not available)".
 *
 * Note for users of ProcessUtility_hook: the same queryString may be passed
 * to multiple invocations of ProcessUtility when processing a query string
 * containing multiple semicolon-separated statements.  One should use
 * pstmt->stmt_location and pstmt->stmt_len to identify the substring
 * containing the current statement.  Keep in mind also that some utility
 * statements (e.g., CREATE SCHEMA) will recurse to ProcessUtility to process
 * sub-statements, often passing down the same queryString, stmt_location,
 * and stmt_len that were given for the whole statement.
 */
void
ProcessUtility(PlannedStmt *pstmt,
			   const char *queryString,
			   bool readOnlyTree,
			   ProcessUtilityContext context,
			   ParamListInfo params,
			   QueryEnvironment *queryEnv,
			   DestReceiver *dest,
			   QueryCompletion *qc)
{
	Assert(IsA(pstmt, PlannedStmt));
	Assert(pstmt->commandType == CMD_UTILITY);
	Assert(queryString != NULL);	/* required as of 8.4 */
	Assert(qc == NULL || qc->commandTag == CMDTAG_UNKNOWN);

	/*
	 * We provide a function hook variable that lets loadable plugins get
	 * control when ProcessUtility is called.  Such a plugin would normally
	 * call standard_ProcessUtility().
	 */
	if (ProcessUtility_hook)
		(*ProcessUtility_hook) (pstmt, queryString, readOnlyTree,
								context, params, queryEnv,
								dest, qc);
	else
		standard_ProcessUtility(pstmt, queryString, readOnlyTree,
								context, params, queryEnv,
								dest, qc);
}

/*
 * standard_ProcessUtility itself deals only with utility commands for
 * which we do not provide event trigger support.  Commands that do have
 * such support are passed down to ProcessUtilitySlow, which contains the
 * necessary infrastructure for such triggers.
 *
 * This division is not just for performance: it's critical that the
 * event trigger code not be invoked when doing START TRANSACTION for
 * example, because we might need to refresh the event trigger cache,
 * which requires being in a valid transaction.
 *
 * When adding or moving utility commands, check that the documentation in
 * event-trigger.sgml is kept up to date.
 */
void
standard_ProcessUtility(PlannedStmt *pstmt,
						const char *queryString,
						bool readOnlyTree,
						ProcessUtilityContext context,
						ParamListInfo params,
						QueryEnvironment *queryEnv,
						DestReceiver *dest,
						QueryCompletion *qc)
{
	Node	   *parsetree;
	bool		isTopLevel = (context == PROCESS_UTILITY_TOPLEVEL);
	bool		isAtomicContext = (!(context == PROCESS_UTILITY_TOPLEVEL || context == PROCESS_UTILITY_QUERY_NONATOMIC) || IsTransactionBlock());
	ParseState *pstate;
	int			readonly_flags;

	/* This can recurse, so check for excessive recursion */
	check_stack_depth();

	/*
	 * If the given node tree is read-only, make a copy to ensure that parse
	 * transformations don't damage the original tree.  This could be
	 * refactored to avoid making unnecessary copies in more cases, but it's
	 * not clear that it's worth a great deal of trouble over.  Statements
	 * that are complex enough to be expensive to copy are exactly the ones
	 * we'd need to copy, so that only marginal savings seem possible.
	 */
	if (readOnlyTree)
		pstmt = copyObject(pstmt);
	parsetree = pstmt->utilityStmt;

	/* Prohibit read/write commands in read-only states. */
	readonly_flags = ClassifyUtilityCommandAsReadOnly(parsetree);
	if (readonly_flags != COMMAND_IS_STRICTLY_READ_ONLY &&
		(XactReadOnly || IsInParallelMode()))
	{
		CommandTag	commandtag = CreateCommandTag(parsetree);

		if ((readonly_flags & COMMAND_OK_IN_READ_ONLY_TXN) == 0)
			PreventCommandIfReadOnly(GetCommandTagName(commandtag));
		if ((readonly_flags & COMMAND_OK_IN_PARALLEL_MODE) == 0)
			PreventCommandIfParallelMode(GetCommandTagName(commandtag));
		if ((readonly_flags & COMMAND_OK_IN_RECOVERY) == 0)
			PreventCommandDuringRecovery(GetCommandTagName(commandtag));
	}

	pstate = make_parsestate(NULL);
	pstate->p_sourcetext = queryString;
	pstate->p_queryEnv = queryEnv;

	switch (nodeTag(parsetree))
	{
			/*
			 * ******************** transactions ********************
			 */
		case T_TransactionStmt:
			{
				TransactionStmt *stmt = (TransactionStmt *) parsetree;

				switch (stmt->kind)
				{
						/*
						 * START TRANSACTION, as defined by SQL99: Identical
						 * to BEGIN.  Same code for both.
						 */
					case TRANS_STMT_BEGIN:
					case TRANS_STMT_START:
						{
							ListCell   *lc;

							BeginTransactionBlock();
							foreach(lc, stmt->options)
							{
								DefElem    *item = (DefElem *) lfirst(lc);

								if (strcmp(item->defname, "transaction_isolation") == 0)
									SetPGVariable("transaction_isolation",
												  list_make1(item->arg),
												  true);
								else if (strcmp(item->defname, "transaction_read_only") == 0)
									SetPGVariable("transaction_read_only",
												  list_make1(item->arg),
												  true);
								else if (strcmp(item->defname, "transaction_deferrable") == 0)
									SetPGVariable("transaction_deferrable",
												  list_make1(item->arg),
												  true);
							}
						}
						break;

					case TRANS_STMT_COMMIT:
						if (!EndTransactionBlock(stmt->chain))
						{
							/* report unsuccessful commit in qc */
							if (qc)
								SetQueryCompletion(qc, CMDTAG_ROLLBACK, 0);
						}
						break;

					case TRANS_STMT_PREPARE:
						if (!PrepareTransactionBlock(stmt->gid))
						{
							/* report unsuccessful commit in qc */
							if (qc)
								SetQueryCompletion(qc, CMDTAG_ROLLBACK, 0);
						}
						break;

					case TRANS_STMT_COMMIT_PREPARED:
						PreventInTransactionBlock(isTopLevel, "COMMIT PREPARED");
						FinishPreparedTransaction(stmt->gid, true);
						break;

					case TRANS_STMT_ROLLBACK_PREPARED:
						PreventInTransactionBlock(isTopLevel, "ROLLBACK PREPARED");
						FinishPreparedTransaction(stmt->gid, false);
						break;

					case TRANS_STMT_ROLLBACK:
						UserAbortTransactionBlock(stmt->chain);
						break;

					case TRANS_STMT_SAVEPOINT:
						RequireTransactionBlock(isTopLevel, "SAVEPOINT");
						DefineSavepoint(stmt->savepoint_name);
						break;

					case TRANS_STMT_RELEASE:
						RequireTransactionBlock(isTopLevel, "RELEASE SAVEPOINT");
						ReleaseSavepoint(stmt->savepoint_name);
						break;

					case TRANS_STMT_ROLLBACK_TO:
						RequireTransactionBlock(isTopLevel, "ROLLBACK TO SAVEPOINT");
						RollbackToSavepoint(stmt->savepoint_name);

						/*
						 * CommitTransactionCommand is in charge of
						 * re-defining the savepoint again
						 */
						break;
				}
			}
			break;

			/*
			 * Portal (cursor) manipulation
			 */
		case T_DeclareCursorStmt:
			PerformCursorOpen(pstate, (DeclareCursorStmt *) parsetree, params,
							  isTopLevel);
			break;

		case T_ClosePortalStmt:
			{
				ClosePortalStmt *stmt = (ClosePortalStmt *) parsetree;

				CheckRestrictedOperation("CLOSE");
				PerformPortalClose(stmt->portalname);
			}
			break;

		case T_FetchStmt:
			PerformPortalFetch((FetchStmt *) parsetree, dest, qc);
			break;

		case T_DoStmt:
			ExecuteDoStmt(pstate, (DoStmt *) parsetree, isAtomicContext);
			break;

		case T_CreateTableSpaceStmt:
			/* no event triggers for global objects */
			PreventInTransactionBlock(isTopLevel, "CREATE TABLESPACE");
			CreateTableSpace((CreateTableSpaceStmt *) parsetree);
			break;

		case T_DropTableSpaceStmt:
			/* no event triggers for global objects */
			PreventInTransactionBlock(isTopLevel, "DROP TABLESPACE");
			DropTableSpace((DropTableSpaceStmt *) parsetree);
			break;

		case T_AlterTableSpaceOptionsStmt:
			/* no event triggers for global objects */
			AlterTableSpaceOptions((AlterTableSpaceOptionsStmt *) parsetree);
			break;

		case T_TruncateStmt:
			ExecuteTruncate((TruncateStmt *) parsetree);
			break;

		case T_CopyStmt:
			{
				uint64		processed;

				DoCopy(pstate, (CopyStmt *) parsetree,
					   pstmt->stmt_location, pstmt->stmt_len,
					   &processed);
				if (qc)
					SetQueryCompletion(qc, CMDTAG_COPY, processed);
			}
			break;

		case T_PrepareStmt:
			CheckRestrictedOperation("PREPARE");
			PrepareQuery(pstate, (PrepareStmt *) parsetree,
						 pstmt->stmt_location, pstmt->stmt_len);
			break;

		case T_ExecuteStmt:
			ExecuteQuery(pstate,
						 (ExecuteStmt *) parsetree, NULL,
						 params,
						 dest, qc);
			break;

		case T_DeallocateStmt:
			CheckRestrictedOperation("DEALLOCATE");
			DeallocateQuery((DeallocateStmt *) parsetree);
			break;

		case T_GrantRoleStmt:
			/* no event triggers for global objects */
			GrantRole(pstate, (GrantRoleStmt *) parsetree);
			break;

		case T_CreatedbStmt:
			/* no event triggers for global objects */
			PreventInTransactionBlock(isTopLevel, "CREATE DATABASE");
			createdb(pstate, (CreatedbStmt *) parsetree);
			break;

		case T_AlterDatabaseStmt:
			/* no event triggers for global objects */
			AlterDatabase(pstate, (AlterDatabaseStmt *) parsetree, isTopLevel);
			break;

		case T_AlterDatabaseRefreshCollStmt:
			/* no event triggers for global objects */
			AlterDatabaseRefreshColl((AlterDatabaseRefreshCollStmt *) parsetree);
			break;

		case T_AlterDatabaseSetStmt:
			/* no event triggers for global objects */
			AlterDatabaseSet((AlterDatabaseSetStmt *) parsetree);
			break;

		case T_DropdbStmt:
			/* no event triggers for global objects */
			PreventInTransactionBlock(isTopLevel, "DROP DATABASE");
			DropDatabase(pstate, (DropdbStmt *) parsetree);
			break;

			/* Query-level asynchronous notification */
		case T_NotifyStmt:
			{
				NotifyStmt *stmt = (NotifyStmt *) parsetree;

				Async_Notify(stmt->conditionname, stmt->payload);
			}
			break;

		case T_ListenStmt:
			{
				ListenStmt *stmt = (ListenStmt *) parsetree;

				CheckRestrictedOperation("LISTEN");

				/*
				 * We don't allow LISTEN in background processes, as there is
				 * no mechanism for them to collect NOTIFY messages, so they'd
				 * just block cleanout of the async SLRU indefinitely.
				 * (Authors of custom background workers could bypass this
				 * restriction by calling Async_Listen directly, but then it's
				 * on them to provide some mechanism to process the message
				 * queue.)  Note there seems no reason to forbid UNLISTEN.
				 */
				if (MyBackendType != B_BACKEND)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					/* translator: %s is name of a SQL command, eg LISTEN */
							 errmsg("cannot execute %s within a background process",
									"LISTEN")));

				Async_Listen(stmt->conditionname);
			}
			break;

		case T_UnlistenStmt:
			{
				UnlistenStmt *stmt = (UnlistenStmt *) parsetree;

				CheckRestrictedOperation("UNLISTEN");
				if (stmt->conditionname)
					Async_Unlisten(stmt->conditionname);
				else
					Async_UnlistenAll();
			}
			break;

		case T_LoadStmt:
			{
				LoadStmt   *stmt = (LoadStmt *) parsetree;

				closeAllVfds(); /* probably not necessary... */
				/* Allowed names are restricted if you're not superuser */
				load_file(stmt->filename, !superuser());
			}
			break;

		case T_CallStmt:
			ExecuteCallStmt(castNode(CallStmt, parsetree), params, isAtomicContext, dest);
			break;

		case T_ClusterStmt:
			cluster(pstate, (ClusterStmt *) parsetree, isTopLevel);
			break;

		case T_VacuumStmt:
			ExecVacuum(pstate, (VacuumStmt *) parsetree, isTopLevel);
			break;

		case T_ExplainStmt:
			ExplainQuery(pstate, (ExplainStmt *) parsetree, params, dest);
			break;

		case T_AlterSystemStmt:
			PreventInTransactionBlock(isTopLevel, "ALTER SYSTEM");
			AlterSystemSetConfigFile((AlterSystemStmt *) parsetree);
			break;

		case T_VariableSetStmt:
			ExecSetVariableStmt((VariableSetStmt *) parsetree, isTopLevel);
			break;

		case T_VariableShowStmt:
			{
				VariableShowStmt *n = (VariableShowStmt *) parsetree;

				GetPGVariable(n->name, dest);
			}
			break;

		case T_DiscardStmt:
			/* should we allow DISCARD PLANS? */
			CheckRestrictedOperation("DISCARD");
			DiscardCommand((DiscardStmt *) parsetree, isTopLevel);
			break;

		case T_CreateEventTrigStmt:
			/* no event triggers on event triggers */
			CreateEventTrigger((CreateEventTrigStmt *) parsetree);
			break;

		case T_AlterEventTrigStmt:
			/* no event triggers on event triggers */
			AlterEventTrigger((AlterEventTrigStmt *) parsetree);
			break;

			/*
			 * ******************************** ROLE statements ****
			 */
		case T_CreateRoleStmt:
			/* no event triggers for global objects */
			CreateRole(pstate, (CreateRoleStmt *) parsetree);
			break;

		case T_AlterRoleStmt:
			/* no event triggers for global objects */
			AlterRole(pstate, (AlterRoleStmt *) parsetree);
			break;

		case T_AlterRoleSetStmt:
			/* no event triggers for global objects */
			AlterRoleSet((AlterRoleSetStmt *) parsetree);
			break;

		case T_DropRoleStmt:
			/* no event triggers for global objects */
			DropRole((DropRoleStmt *) parsetree);
			break;

		case T_ReassignOwnedStmt:
			/* no event triggers for global objects */
			ReassignOwnedObjects((ReassignOwnedStmt *) parsetree);
			break;

		case T_LockStmt:

			/*
			 * Since the lock would just get dropped immediately, LOCK TABLE
			 * outside a transaction block is presumed to be user error.
			 */
			RequireTransactionBlock(isTopLevel, "LOCK TABLE");
			LockTableCommand((LockStmt *) parsetree);
			break;

		case T_ConstraintsSetStmt:
			WarnNoTransactionBlock(isTopLevel, "SET CONSTRAINTS");
			AfterTriggerSetState((ConstraintsSetStmt *) parsetree);
			break;

		case T_CheckPointStmt:
			if (!has_privs_of_role(GetUserId(), ROLE_PG_CHECKPOINT))
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				/* translator: %s is name of a SQL command, eg CHECKPOINT */
						 errmsg("permission denied to execute %s command",
								"CHECKPOINT"),
						 errdetail("Only roles with privileges of the \"%s\" role may execute this command.",
								   "pg_checkpoint")));

			RequestCheckpoint(CHECKPOINT_IMMEDIATE | CHECKPOINT_WAIT |
							  (RecoveryInProgress() ? 0 : CHECKPOINT_FORCE));
			break;

			/*
			 * The following statements are supported by Event Triggers only
			 * in some cases, so we "fast path" them in the other cases.
			 */

		case T_GrantStmt:
			{
				GrantStmt  *stmt = (GrantStmt *) parsetree;

				if (EventTriggerSupportsObjectType(stmt->objtype))
					ProcessUtilitySlow(pstate, pstmt, queryString,
									   context, params, queryEnv,
									   dest, qc);
				else
					ExecuteGrantStmt(stmt);
			}
			break;

		case T_DropStmt:
			{
				DropStmt   *stmt = (DropStmt *) parsetree;

				if (EventTriggerSupportsObjectType(stmt->removeType))
					ProcessUtilitySlow(pstate, pstmt, queryString,
									   context, params, queryEnv,
									   dest, qc);
				else
					ExecDropStmt(stmt, isTopLevel);
			}
			break;

		case T_RenameStmt:
			{
				RenameStmt *stmt = (RenameStmt *) parsetree;

				if (EventTriggerSupportsObjectType(stmt->renameType))
					ProcessUtilitySlow(pstate, pstmt, queryString,
									   context, params, queryEnv,
									   dest, qc);
				else
					ExecRenameStmt(stmt);
			}
			break;

		case T_AlterObjectDependsStmt:
			{
				AlterObjectDependsStmt *stmt = (AlterObjectDependsStmt *) parsetree;

				if (EventTriggerSupportsObjectType(stmt->objectType))
					ProcessUtilitySlow(pstate, pstmt, queryString,
									   context, params, queryEnv,
									   dest, qc);
				else
					ExecAlterObjectDependsStmt(stmt, NULL);
			}
			break;

		case T_AlterObjectSchemaStmt:
			{
				AlterObjectSchemaStmt *stmt = (AlterObjectSchemaStmt *) parsetree;

				if (EventTriggerSupportsObjectType(stmt->objectType))
					ProcessUtilitySlow(pstate, pstmt, queryString,
									   context, params, queryEnv,
									   dest, qc);
				else
					ExecAlterObjectSchemaStmt(stmt, NULL);
			}
			break;

		case T_AlterOwnerStmt:
			{
				AlterOwnerStmt *stmt = (AlterOwnerStmt *) parsetree;

				if (EventTriggerSupportsObjectType(stmt->objectType))
					ProcessUtilitySlow(pstate, pstmt, queryString,
									   context, params, queryEnv,
									   dest, qc);
				else
					ExecAlterOwnerStmt(stmt);
			}
			break;

		case T_CommentStmt:
			{
				CommentStmt *stmt = (CommentStmt *) parsetree;

				if (EventTriggerSupportsObjectType(stmt->objtype))
					ProcessUtilitySlow(pstate, pstmt, queryString,
									   context, params, queryEnv,
									   dest, qc);
				else
					CommentObject(stmt);
				break;
			}

		case T_SecLabelStmt:
			{
				SecLabelStmt *stmt = (SecLabelStmt *) parsetree;

				if (EventTriggerSupportsObjectType(stmt->objtype))
					ProcessUtilitySlow(pstate, pstmt, queryString,
									   context, params, queryEnv,
									   dest, qc);
				else
					ExecSecLabelStmt(stmt);
				break;
			}

		default:
			/* All other statement types have event trigger support */
			ProcessUtilitySlow(pstate, pstmt, queryString,
							   context, params, queryEnv,
							   dest, qc);
			break;
	}

	free_parsestate(pstate);

	/*
	 * Make effects of commands visible, for instance so that
	 * PreCommit_on_commit_actions() can see them (see for example bug
	 * #15631).
	 */
	CommandCounterIncrement();
}

/*
 * The "Slow" variant of ProcessUtility should only receive statements
 * supported by the event triggers facility.  Therefore, we always
 * perform the trigger support calls if the context allows it.
 */
static void
ProcessUtilitySlow(ParseState *pstate,
				   PlannedStmt *pstmt,
				   const char *queryString,
				   ProcessUtilityContext context,
				   ParamListInfo params,
				   QueryEnvironment *queryEnv,
				   DestReceiver *dest,
				   QueryCompletion *qc)
{
	Node	   *parsetree = pstmt->utilityStmt;
	bool		isTopLevel = (context == PROCESS_UTILITY_TOPLEVEL);
	bool		isCompleteQuery = (context != PROCESS_UTILITY_SUBCOMMAND);
	bool		needCleanup;
	bool		commandCollected = false;
	ObjectAddress address;
	ObjectAddress secondaryObject = InvalidObjectAddress;

	/* All event trigger calls are done only when isCompleteQuery is true */
	needCleanup = isCompleteQuery && EventTriggerBeginCompleteQuery();

	/* PG_TRY block is to ensure we call EventTriggerEndCompleteQuery */
	PG_TRY();
	{
		if (isCompleteQuery)
			EventTriggerDDLCommandStart(parsetree);

		switch (nodeTag(parsetree))
		{
				/*
				 * relation and attribute manipulation
				 */
			case T_CreateSchemaStmt:
				CreateSchemaCommand((CreateSchemaStmt *) parsetree,
									queryString,
									pstmt->stmt_location,
									pstmt->stmt_len);

				/*
				 * EventTriggerCollectSimpleCommand called by
				 * CreateSchemaCommand
				 */
				commandCollected = true;
				break;

			case T_CreateStmt:
			case T_CreateForeignTableStmt:
				{
					List	   *stmts;
					RangeVar   *table_rv = NULL;

					/* Run parse analysis ... */
					stmts = transformCreateStmt((CreateStmt *) parsetree,
												queryString);

					/*
					 * ... and do it.  We can't use foreach() because we may
					 * modify the list midway through, so pick off the
					 * elements one at a time, the hard way.
					 */
					while (stmts != NIL)
					{
						Node	   *stmt = (Node *) linitial(stmts);

						stmts = list_delete_first(stmts);

						if (IsA(stmt, CreateStmt))
						{
							CreateStmt *cstmt = (CreateStmt *) stmt;
							Datum		toast_options;
							const char *const validnsps[] = HEAP_RELOPT_NAMESPACES;

							/* Remember transformed RangeVar for LIKE */
							table_rv = cstmt->relation;

							/* Create the table itself */
							address = DefineRelation(cstmt,
													 RELKIND_RELATION,
													 InvalidOid, NULL,
													 queryString);
							EventTriggerCollectSimpleCommand(address,
															 secondaryObject,
															 stmt);

							/*
							 * Let NewRelationCreateToastTable decide if this
							 * one needs a secondary relation too.
							 */
							CommandCounterIncrement();

							/*
							 * parse and validate reloptions for the toast
							 * table
							 */
							toast_options = transformRelOptions((Datum) 0,
																cstmt->options,
																"toast",
																validnsps,
																true,
																false);
							(void) heap_reloptions(RELKIND_TOASTVALUE,
												   toast_options,
												   true);

							NewRelationCreateToastTable(address.objectId,
														toast_options);
						}
						else if (IsA(stmt, CreateForeignTableStmt))
						{
							CreateForeignTableStmt *cstmt = (CreateForeignTableStmt *) stmt;

							/* Remember transformed RangeVar for LIKE */
							table_rv = cstmt->base.relation;

							/* Create the table itself */
							address = DefineRelation(&cstmt->base,
													 RELKIND_FOREIGN_TABLE,
													 InvalidOid, NULL,
													 queryString);
							CreateForeignTable(cstmt,
											   address.objectId);
							EventTriggerCollectSimpleCommand(address,
															 secondaryObject,
															 stmt);
						}
						else if (IsA(stmt, TableLikeClause))
						{
							/*
							 * Do delayed processing of LIKE options.  This
							 * will result in additional sub-statements for us
							 * to process.  Those should get done before any
							 * remaining actions, so prepend them to "stmts".
							 */
							TableLikeClause *like = (TableLikeClause *) stmt;
							List	   *morestmts;

							Assert(table_rv != NULL);

							morestmts = expandTableLikeClause(table_rv, like);
							stmts = list_concat(morestmts, stmts);
						}
						else
						{
							/*
							 * Recurse for anything else.  Note the recursive
							 * call will stash the objects so created into our
							 * event trigger context.
							 */
							PlannedStmt *wrapper;

							wrapper = makeNode(PlannedStmt);
							wrapper->commandType = CMD_UTILITY;
							wrapper->canSetTag = false;
							wrapper->utilityStmt = stmt;
							wrapper->stmt_location = pstmt->stmt_location;
							wrapper->stmt_len = pstmt->stmt_len;

							ProcessUtility(wrapper,
										   queryString,
										   false,
										   PROCESS_UTILITY_SUBCOMMAND,
										   params,
										   NULL,
										   None_Receiver,
										   NULL);
						}

						/* Need CCI between commands */
						if (stmts != NIL)
							CommandCounterIncrement();
					}

					/*
					 * The multiple commands generated here are stashed
					 * individually, so disable collection below.
					 */
					commandCollected = true;
				}
				break;

			case T_AlterTableStmt:
				{
					AlterTableStmt *atstmt = (AlterTableStmt *) parsetree;
					Oid			relid;
					LOCKMODE	lockmode;
					ListCell   *cell;

					/*
					 * Disallow ALTER TABLE .. DETACH CONCURRENTLY in a
					 * transaction block or function.  (Perhaps it could be
					 * allowed in a procedure, but don't hold your breath.)
					 */
					foreach(cell, atstmt->cmds)
					{
						AlterTableCmd *cmd = (AlterTableCmd *) lfirst(cell);

						/* Disallow DETACH CONCURRENTLY in a transaction block */
						if (cmd->subtype == AT_DetachPartition)
						{
							if (((PartitionCmd *) cmd->def)->concurrent)
								PreventInTransactionBlock(isTopLevel,
														  "ALTER TABLE ... DETACH CONCURRENTLY");
						}
					}

					/*
					 * Figure out lock mode, and acquire lock.  This also does
					 * basic permissions checks, so that we won't wait for a
					 * lock on (for example) a relation on which we have no
					 * permissions.
					 */
					lockmode = AlterTableGetLockLevel(atstmt->cmds);
					relid = AlterTableLookupRelation(atstmt, lockmode);

					if (OidIsValid(relid))
					{
						AlterTableUtilityContext atcontext;

						/* Set up info needed for recursive callbacks ... */
						atcontext.pstmt = pstmt;
						atcontext.queryString = queryString;
						atcontext.relid = relid;
						atcontext.params = params;
						atcontext.queryEnv = queryEnv;

						/* ... ensure we have an event trigger context ... */
						EventTriggerAlterTableStart(parsetree);
						EventTriggerAlterTableRelid(relid);

						/* ... and do it */
						AlterTable(atstmt, lockmode, &atcontext);

						/* done */
						EventTriggerAlterTableEnd();
					}
					else
						ereport(NOTICE,
								(errmsg("relation \"%s\" does not exist, skipping",
										atstmt->relation->relname)));
				}

				/* ALTER TABLE stashes commands internally */
				commandCollected = true;
				break;

			case T_AlterDomainStmt:
				{
					AlterDomainStmt *stmt = (AlterDomainStmt *) parsetree;

					/*
					 * Some or all of these functions are recursive to cover
					 * inherited things, so permission checks are done there.
					 */
					switch (stmt->subtype)
					{
						case 'T':	/* ALTER DOMAIN DEFAULT */

							/*
							 * Recursively alter column default for table and,
							 * if requested, for descendants
							 */
							address =
								AlterDomainDefault(stmt->typeName,
												   stmt->def);
							break;
						case 'N':	/* ALTER DOMAIN DROP NOT NULL */
							address =
								AlterDomainNotNull(stmt->typeName,
												   false);
							break;
						case 'O':	/* ALTER DOMAIN SET NOT NULL */
							address =
								AlterDomainNotNull(stmt->typeName,
												   true);
							break;
						case 'C':	/* ADD CONSTRAINT */
							address =
								AlterDomainAddConstraint(stmt->typeName,
														 stmt->def,
														 &secondaryObject);
							break;
						case 'X':	/* DROP CONSTRAINT */
							address =
								AlterDomainDropConstraint(stmt->typeName,
														  stmt->name,
														  stmt->behavior,
														  stmt->missing_ok);
							break;
						case 'V':	/* VALIDATE CONSTRAINT */
							address =
								AlterDomainValidateConstraint(stmt->typeName,
															  stmt->name);
							break;
						default:	/* oops */
							elog(ERROR, "unrecognized alter domain type: %d",
								 (int) stmt->subtype);
							break;
					}
				}
				break;

				/*
				 * ************* object creation / destruction **************
				 */
			case T_DefineStmt:
				{
					DefineStmt *stmt = (DefineStmt *) parsetree;

					switch (stmt->kind)
					{
						case OBJECT_AGGREGATE:
							address =
								DefineAggregate(pstate, stmt->defnames, stmt->args,
												stmt->oldstyle,
												stmt->definition,
												stmt->replace);
							break;
						case OBJECT_OPERATOR:
							Assert(stmt->args == NIL);
							address = DefineOperator(stmt->defnames,
													 stmt->definition);
							break;
						case OBJECT_TYPE:
							Assert(stmt->args == NIL);
							address = DefineType(pstate,
												 stmt->defnames,
												 stmt->definition);
							break;
						case OBJECT_TSPARSER:
							Assert(stmt->args == NIL);
							address = DefineTSParser(stmt->defnames,
													 stmt->definition);
							break;
						case OBJECT_TSDICTIONARY:
							Assert(stmt->args == NIL);
							address = DefineTSDictionary(stmt->defnames,
														 stmt->definition);
							break;
						case OBJECT_TSTEMPLATE:
							Assert(stmt->args == NIL);
							address = DefineTSTemplate(stmt->defnames,
													   stmt->definition);
							break;
						case OBJECT_TSCONFIGURATION:
							Assert(stmt->args == NIL);
							address = DefineTSConfiguration(stmt->defnames,
															stmt->definition,
															&secondaryObject);
							break;
						case OBJECT_COLLATION:
							Assert(stmt->args == NIL);
							address = DefineCollation(pstate,
													  stmt->defnames,
													  stmt->definition,
													  stmt->if_not_exists);
							break;
						default:
							elog(ERROR, "unrecognized define stmt type: %d",
								 (int) stmt->kind);
							break;
					}
				}
				break;

			case T_IndexStmt:	/* CREATE INDEX */
				{
					IndexStmt  *stmt = (IndexStmt *) parsetree;
					Oid			relid;
					LOCKMODE	lockmode;
					int			nparts = -1;
					bool		is_alter_table;

					if (stmt->concurrent)
						PreventInTransactionBlock(isTopLevel,
												  "CREATE INDEX CONCURRENTLY");

					/*
					 * Look up the relation OID just once, right here at the
					 * beginning, so that we don't end up repeating the name
					 * lookup later and latching onto a different relation
					 * partway through.  To avoid lock upgrade hazards, it's
					 * important that we take the strongest lock that will
					 * eventually be needed here, so the lockmode calculation
					 * needs to match what DefineIndex() does.
					 */
					lockmode = stmt->concurrent ? ShareUpdateExclusiveLock
						: ShareLock;
					relid =
						RangeVarGetRelidExtended(stmt->relation, lockmode,
												 0,
												 RangeVarCallbackOwnsRelation,
												 NULL);

					/*
					 * CREATE INDEX on partitioned tables (but not regular
					 * inherited tables) recurses to partitions, so we must
					 * acquire locks early to avoid deadlocks.
					 *
					 * We also take the opportunity to verify that all
					 * partitions are something we can put an index on, to
					 * avoid building some indexes only to fail later.  While
					 * at it, also count the partitions, so that DefineIndex
					 * needn't do a duplicative find_all_inheritors search.
					 */
					if (stmt->relation->inh &&
						get_rel_relkind(relid) == RELKIND_PARTITIONED_TABLE)
					{
						ListCell   *lc;
						List	   *inheritors = NIL;

						inheritors = find_all_inheritors(relid, lockmode, NULL);
						foreach(lc, inheritors)
						{
							Oid			partrelid = lfirst_oid(lc);
							char		relkind = get_rel_relkind(partrelid);

							if (relkind != RELKIND_RELATION &&
								relkind != RELKIND_MATVIEW &&
								relkind != RELKIND_PARTITIONED_TABLE &&
								relkind != RELKIND_FOREIGN_TABLE)
								elog(ERROR, "unexpected relkind \"%c\" on partition \"%s\"",
									 relkind, stmt->relation->relname);

							if (relkind == RELKIND_FOREIGN_TABLE &&
								(stmt->unique || stmt->primary))
								ereport(ERROR,
										(errcode(ERRCODE_WRONG_OBJECT_TYPE),
										 errmsg("cannot create unique index on partitioned table \"%s\"",
												stmt->relation->relname),
										 errdetail("Table \"%s\" contains partitions that are foreign tables.",
												   stmt->relation->relname)));
						}
						/* count direct and indirect children, but not rel */
						nparts = list_length(inheritors) - 1;
						list_free(inheritors);
					}

					/*
					 * If the IndexStmt is already transformed, it must have
					 * come from generateClonedIndexStmt, which in current
					 * usage means it came from expandTableLikeClause rather
					 * than from original parse analysis.  And that means we
					 * must treat it like ALTER TABLE ADD INDEX, not CREATE.
					 * (This is a bit grotty, but currently it doesn't seem
					 * worth adding a separate bool field for the purpose.)
					 */
					is_alter_table = stmt->transformed;

					/* Run parse analysis ... */
					stmt = transformIndexStmt(relid, stmt, queryString);

					/* ... and do it */
					EventTriggerAlterTableStart(parsetree);
					address =
						DefineIndex(relid,	/* OID of heap relation */
									stmt,
									InvalidOid, /* no predefined OID */
									InvalidOid, /* no parent index */
									InvalidOid, /* no parent constraint */
									nparts, /* # of partitions, or -1 */
									is_alter_table,
									true,	/* check_rights */
									true,	/* check_not_in_use */
									false,	/* skip_build */
									false); /* quiet */

					/*
					 * Add the CREATE INDEX node itself to stash right away;
					 * if there were any commands stashed in the ALTER TABLE
					 * code, we need them to appear after this one.
					 */
					EventTriggerCollectSimpleCommand(address, secondaryObject,
													 parsetree);
					commandCollected = true;
					EventTriggerAlterTableEnd();
				}
				break;

			case T_ReindexStmt:
				ExecReindex(pstate, (ReindexStmt *) parsetree, isTopLevel);

				/* EventTriggerCollectSimpleCommand is called directly */
				commandCollected = true;
				break;

			case T_CreateExtensionStmt:
				address = CreateExtension(pstate, (CreateExtensionStmt *) parsetree);
				break;

			case T_AlterExtensionStmt:
				address = ExecAlterExtensionStmt(pstate, (AlterExtensionStmt *) parsetree);
				break;

			case T_AlterExtensionContentsStmt:
				address = ExecAlterExtensionContentsStmt((AlterExtensionContentsStmt *) parsetree,
														 &secondaryObject);
				break;

			case T_CreateFdwStmt:
				address = CreateForeignDataWrapper(pstate, (CreateFdwStmt *) parsetree);
				break;

			case T_AlterFdwStmt:
				address = AlterForeignDataWrapper(pstate, (AlterFdwStmt *) parsetree);
				break;

			case T_CreateForeignServerStmt:
				address = CreateForeignServer((CreateForeignServerStmt *) parsetree);
				break;

			case T_AlterForeignServerStmt:
				address = AlterForeignServer((AlterForeignServerStmt *) parsetree);
				break;

			case T_CreateUserMappingStmt:
				address = CreateUserMapping((CreateUserMappingStmt *) parsetree);
				break;

			case T_AlterUserMappingStmt:
				address = AlterUserMapping((AlterUserMappingStmt *) parsetree);
				break;

			case T_DropUserMappingStmt:
				RemoveUserMapping((DropUserMappingStmt *) parsetree);
				/* no commands stashed for DROP */
				commandCollected = true;
				break;

			case T_ImportForeignSchemaStmt:
				ImportForeignSchema((ImportForeignSchemaStmt *) parsetree);
				/* commands are stashed inside ImportForeignSchema */
				commandCollected = true;
				break;

			case T_CompositeTypeStmt:	/* CREATE TYPE (composite) */
				{
					CompositeTypeStmt *stmt = (CompositeTypeStmt *) parsetree;

					address = DefineCompositeType(stmt->typevar,
												  stmt->coldeflist);
				}
				break;

			case T_CreateEnumStmt:	/* CREATE TYPE AS ENUM */
				address = DefineEnum((CreateEnumStmt *) parsetree);
				break;

			case T_CreateRangeStmt: /* CREATE TYPE AS RANGE */
				address = DefineRange(pstate, (CreateRangeStmt *) parsetree);
				break;

			case T_AlterEnumStmt:	/* ALTER TYPE (enum) */
				address = AlterEnum((AlterEnumStmt *) parsetree);
				break;

			case T_ViewStmt:	/* CREATE VIEW */
				EventTriggerAlterTableStart(parsetree);
				address = DefineView((ViewStmt *) parsetree, queryString,
									 pstmt->stmt_location, pstmt->stmt_len);
				EventTriggerCollectSimpleCommand(address, secondaryObject,
												 parsetree);
				/* stashed internally */
				commandCollected = true;
				EventTriggerAlterTableEnd();
				break;

			case T_CreateFunctionStmt:	/* CREATE FUNCTION */
				address = CreateFunction(pstate, (CreateFunctionStmt *) parsetree);
				break;

			case T_AlterFunctionStmt:	/* ALTER FUNCTION */
				address = AlterFunction(pstate, (AlterFunctionStmt *) parsetree);
				break;

			case T_RuleStmt:	/* CREATE RULE */
				address = DefineRule((RuleStmt *) parsetree, queryString);
				break;

			case T_CreateSeqStmt:
				address = DefineSequence(pstate, (CreateSeqStmt *) parsetree);
				break;

			case T_AlterSeqStmt:
				address = AlterSequence(pstate, (AlterSeqStmt *) parsetree);
				break;

			case T_CreateTableAsStmt:
				address = ExecCreateTableAs(pstate, (CreateTableAsStmt *) parsetree,
											params, queryEnv, qc);
				break;

			case T_RefreshMatViewStmt:

				/*
				 * REFRESH CONCURRENTLY executes some DDL commands internally.
				 * Inhibit DDL command collection here to avoid those commands
				 * from showing up in the deparsed command queue.  The refresh
				 * command itself is queued, which is enough.
				 */
				EventTriggerInhibitCommandCollection();
				PG_TRY(2);
				{
					address = ExecRefreshMatView((RefreshMatViewStmt *) parsetree,
												 queryString, qc);
				}
				PG_FINALLY(2);
				{
					EventTriggerUndoInhibitCommandCollection();
				}
				PG_END_TRY(2);
				break;

			case T_CreateTrigStmt:
				address = CreateTrigger((CreateTrigStmt *) parsetree,
										queryString, InvalidOid, InvalidOid,
										InvalidOid, InvalidOid, InvalidOid,
										InvalidOid, NULL, false, false);
				break;

			case T_CreatePLangStmt:
				address = CreateProceduralLanguage((CreatePLangStmt *) parsetree);
				break;

			case T_CreateDomainStmt:
				address = DefineDomain((CreateDomainStmt *) parsetree);
				break;

			case T_CreateConversionStmt:
				address = CreateConversionCommand((CreateConversionStmt *) parsetree);
				break;

			case T_CreateCastStmt:
				address = CreateCast((CreateCastStmt *) parsetree);
				break;

			case T_CreateOpClassStmt:
				DefineOpClass((CreateOpClassStmt *) parsetree);
				/* command is stashed in DefineOpClass */
				commandCollected = true;
				break;

			case T_CreateOpFamilyStmt:
				address = DefineOpFamily((CreateOpFamilyStmt *) parsetree);

				/*
				 * DefineOpFamily calls EventTriggerCollectSimpleCommand
				 * directly.
				 */
				commandCollected = true;
				break;

			case T_CreateTransformStmt:
				address = CreateTransform((CreateTransformStmt *) parsetree);
				break;

			case T_AlterOpFamilyStmt:
				AlterOpFamily((AlterOpFamilyStmt *) parsetree);
				/* commands are stashed in AlterOpFamily */
				commandCollected = true;
				break;

			case T_AlterTSDictionaryStmt:
				address = AlterTSDictionary((AlterTSDictionaryStmt *) parsetree);
				break;

			case T_AlterTSConfigurationStmt:
				AlterTSConfiguration((AlterTSConfigurationStmt *) parsetree);

				/*
				 * Commands are stashed in MakeConfigurationMapping and
				 * DropConfigurationMapping, which are called from
				 * AlterTSConfiguration
				 */
				commandCollected = true;
				break;

			case T_AlterTableMoveAllStmt:
				AlterTableMoveAll((AlterTableMoveAllStmt *) parsetree);
				/* commands are stashed in AlterTableMoveAll */
				commandCollected = true;
				break;

			case T_DropStmt:
				ExecDropStmt((DropStmt *) parsetree, isTopLevel);
				/* no commands stashed for DROP */
				commandCollected = true;
				break;

			case T_RenameStmt:
				address = ExecRenameStmt((RenameStmt *) parsetree);
				break;

			case T_AlterObjectDependsStmt:
				address =
					ExecAlterObjectDependsStmt((AlterObjectDependsStmt *) parsetree,
											   &secondaryObject);
				break;

			case T_AlterObjectSchemaStmt:
				address =
					ExecAlterObjectSchemaStmt((AlterObjectSchemaStmt *) parsetree,
											  &secondaryObject);
				break;

			case T_AlterOwnerStmt:
				address = ExecAlterOwnerStmt((AlterOwnerStmt *) parsetree);
				break;

			case T_AlterOperatorStmt:
				address = AlterOperator((AlterOperatorStmt *) parsetree);
				break;

			case T_AlterTypeStmt:
				address = AlterType((AlterTypeStmt *) parsetree);
				break;

			case T_CommentStmt:
				address = CommentObject((CommentStmt *) parsetree);
				break;

			case T_GrantStmt:
				ExecuteGrantStmt((GrantStmt *) parsetree);
				/* commands are stashed in ExecGrantStmt_oids */
				commandCollected = true;
				break;

			case T_DropOwnedStmt:
				DropOwnedObjects((DropOwnedStmt *) parsetree);
				/* no commands stashed for DROP */
				commandCollected = true;
				break;

			case T_AlterDefaultPrivilegesStmt:
				ExecAlterDefaultPrivilegesStmt(pstate, (AlterDefaultPrivilegesStmt *) parsetree);
				EventTriggerCollectAlterDefPrivs((AlterDefaultPrivilegesStmt *) parsetree);
				commandCollected = true;
				break;

			case T_CreatePolicyStmt:	/* CREATE POLICY */
				address = CreatePolicy((CreatePolicyStmt *) parsetree);
				break;

			case T_AlterPolicyStmt: /* ALTER POLICY */
				address = AlterPolicy((AlterPolicyStmt *) parsetree);
				break;

			case T_SecLabelStmt:
				address = ExecSecLabelStmt((SecLabelStmt *) parsetree);
				break;

			case T_CreateAmStmt:
				address = CreateAccessMethod((CreateAmStmt *) parsetree);
				break;

			case T_CreatePublicationStmt:
				address = CreatePublication(pstate, (CreatePublicationStmt *) parsetree);
				break;

			case T_AlterPublicationStmt:
				AlterPublication(pstate, (AlterPublicationStmt *) parsetree);

				/*
				 * AlterPublication calls EventTriggerCollectSimpleCommand
				 * directly
				 */
				commandCollected = true;
				break;

			case T_CreateSubscriptionStmt:
				address = CreateSubscription(pstate,
											 (CreateSubscriptionStmt *) parsetree,
											 isTopLevel);
				break;

			case T_AlterSubscriptionStmt:
				address = AlterSubscription(pstate,
											(AlterSubscriptionStmt *) parsetree,
											isTopLevel);
				break;

			case T_DropSubscriptionStmt:
				DropSubscription((DropSubscriptionStmt *) parsetree, isTopLevel);
				/* no commands stashed for DROP */
				commandCollected = true;
				break;

			case T_CreateStatsStmt:
				{
					Oid			relid;
					CreateStatsStmt *stmt = (CreateStatsStmt *) parsetree;
					RangeVar   *rel = (RangeVar *) linitial(stmt->relations);

					if (!IsA(rel, RangeVar))
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("only a single relation is allowed in CREATE STATISTICS")));

					/*
					 * CREATE STATISTICS will influence future execution plans
					 * but does not interfere with currently executing plans.
					 * So it should be enough to take ShareUpdateExclusiveLock
					 * on relation, conflicting with ANALYZE and other DDL
					 * that sets statistical information, but not with normal
					 * queries.
					 *
					 * XXX RangeVarCallbackOwnsRelation not needed here, to
					 * keep the same behavior as before.
					 */
					relid = RangeVarGetRelid(rel, ShareUpdateExclusiveLock, false);

					/* Run parse analysis ... */
					stmt = transformStatsStmt(relid, stmt, queryString);

					address = CreateStatistics(stmt);
				}
				break;

			case T_AlterStatsStmt:
				address = AlterStatistics((AlterStatsStmt *) parsetree);
				break;

			case T_AlterCollationStmt:
				address = AlterCollation((AlterCollationStmt *) parsetree);
				break;

			default:
				elog(ERROR, "unrecognized node type: %d",
					 (int) nodeTag(parsetree));
				break;
		}

		/*
		 * Remember the object so that ddl_command_end event triggers have
		 * access to it.
		 */
		if (!commandCollected)
			EventTriggerCollectSimpleCommand(address, secondaryObject,
											 parsetree);

		if (isCompleteQuery)
		{
			EventTriggerSQLDrop(parsetree);
			EventTriggerDDLCommandEnd(parsetree);
		}
	}
	PG_FINALLY();
	{
		if (needCleanup)
			EventTriggerEndCompleteQuery();
	}
	PG_END_TRY();
}

/*
 * ProcessUtilityForAlterTable
 *		Recursive entry from ALTER TABLE
 *
 * ALTER TABLE sometimes generates subcommands such as CREATE INDEX.
 * It calls this, not the main entry point ProcessUtility, to execute
 * such subcommands.
 *
 * stmt: the utility command to execute
 * context: opaque passthrough struct with the info we need
 *
 * It's caller's responsibility to do CommandCounterIncrement after
 * calling this, if needed.
 */
void
ProcessUtilityForAlterTable(Node *stmt, AlterTableUtilityContext *context)
{
	PlannedStmt *wrapper;

	/*
	 * For event triggers, we must "close" the current complex-command set,
	 * and start a new one afterwards; this is needed to ensure the ordering
	 * of command events is consistent with the way they were executed.
	 */
	EventTriggerAlterTableEnd();

	/* Create a suitable wrapper */
	wrapper = makeNode(PlannedStmt);
	wrapper->commandType = CMD_UTILITY;
	wrapper->canSetTag = false;
	wrapper->utilityStmt = stmt;
	wrapper->stmt_location = context->pstmt->stmt_location;
	wrapper->stmt_len = context->pstmt->stmt_len;

	ProcessUtility(wrapper,
				   context->queryString,
				   false,
				   PROCESS_UTILITY_SUBCOMMAND,
				   context->params,
				   context->queryEnv,
				   None_Receiver,
				   NULL);

	EventTriggerAlterTableStart(context->pstmt->utilityStmt);
	EventTriggerAlterTableRelid(context->relid);
}

/*
 * Dispatch function for DropStmt
 */
static void
ExecDropStmt(DropStmt *stmt, bool isTopLevel)
{
	switch (stmt->removeType)
	{
		case OBJECT_INDEX:
			if (stmt->concurrent)
				PreventInTransactionBlock(isTopLevel,
										  "DROP INDEX CONCURRENTLY");
			/* fall through */

		case OBJECT_TABLE:
		case OBJECT_SEQUENCE:
		case OBJECT_VIEW:
		case OBJECT_MATVIEW:
		case OBJECT_FOREIGN_TABLE:
			RemoveRelations(stmt);
			break;
		default:
			RemoveObjects(stmt);
			break;
	}
}


/*
 * UtilityReturnsTuples
 *		Return "true" if this utility statement will send output to the
 *		destination.
 *
 * Generally, there should be a case here for each case in ProcessUtility
 * where "dest" is passed on.
 */
bool
UtilityReturnsTuples(Node *parsetree)
{
	switch (nodeTag(parsetree))
	{
		case T_CallStmt:
			{
				CallStmt   *stmt = (CallStmt *) parsetree;

				return (stmt->funcexpr->funcresulttype == RECORDOID);
			}
		case T_FetchStmt:
			{
				FetchStmt  *stmt = (FetchStmt *) parsetree;
				Portal		portal;

				if (stmt->ismove)
					return false;
				portal = GetPortalByName(stmt->portalname);
				if (!PortalIsValid(portal))
					return false;	/* not our business to raise error */
				return portal->tupDesc ? true : false;
			}

		case T_ExecuteStmt:
			{
				ExecuteStmt *stmt = (ExecuteStmt *) parsetree;
				PreparedStatement *entry;

				entry = FetchPreparedStatement(stmt->name, false);
				if (!entry)
					return false;	/* not our business to raise error */
				if (entry->plansource->resultDesc)
					return true;
				return false;
			}

		case T_ExplainStmt:
			return true;

		case T_VariableShowStmt:
			return true;

		default:
			return false;
	}
}

/*
 * UtilityTupleDescriptor
 *		Fetch the actual output tuple descriptor for a utility statement
 *		for which UtilityReturnsTuples() previously returned "true".
 *
 * The returned descriptor is created in (or copied into) the current memory
 * context.
 */
TupleDesc
UtilityTupleDescriptor(Node *parsetree)
{
	switch (nodeTag(parsetree))
	{
		case T_CallStmt:
			return CallStmtResultDesc((CallStmt *) parsetree);

		case T_FetchStmt:
			{
				FetchStmt  *stmt = (FetchStmt *) parsetree;
				Portal		portal;

				if (stmt->ismove)
					return NULL;
				portal = GetPortalByName(stmt->portalname);
				if (!PortalIsValid(portal))
					return NULL;	/* not our business to raise error */
				return CreateTupleDescCopy(portal->tupDesc);
			}

		case T_ExecuteStmt:
			{
				ExecuteStmt *stmt = (ExecuteStmt *) parsetree;
				PreparedStatement *entry;

				entry = FetchPreparedStatement(stmt->name, false);
				if (!entry)
					return NULL;	/* not our business to raise error */
				return FetchPreparedStatementResultDesc(entry);
			}

		case T_ExplainStmt:
			return ExplainResultDesc((ExplainStmt *) parsetree);

		case T_VariableShowStmt:
			{
				VariableShowStmt *n = (VariableShowStmt *) parsetree;

				return GetPGVariableResultDesc(n->name);
			}

		default:
			return NULL;
	}
}


/*
 * QueryReturnsTuples
 *		Return "true" if this Query will send output to the destination.
 */
#ifdef NOT_USED
bool
QueryReturnsTuples(Query *parsetree)
{
	switch (parsetree->commandType)
	{
		case CMD_SELECT:
			/* returns tuples */
			return true;
		case CMD_INSERT:
		case CMD_UPDATE:
		case CMD_DELETE:
		case CMD_MERGE:
			/* the forms with RETURNING return tuples */
			if (parsetree->returningList)
				return true;
			break;
		case CMD_UTILITY:
			return UtilityReturnsTuples(parsetree->utilityStmt);
		case CMD_UNKNOWN:
		case CMD_NOTHING:
			/* probably shouldn't get here */
			break;
	}
	return false;				/* default */
}
#endif


/*
 * UtilityContainsQuery
 *		Return the contained Query, or NULL if there is none
 *
 * Certain utility statements, such as EXPLAIN, contain a plannable Query.
 * This function encapsulates knowledge of exactly which ones do.
 * We assume it is invoked only on already-parse-analyzed statements
 * (else the contained parsetree isn't a Query yet).
 *
 * In some cases (currently, only EXPLAIN of CREATE TABLE AS/SELECT INTO and
 * CREATE MATERIALIZED VIEW), potentially Query-containing utility statements
 * can be nested.  This function will drill down to a non-utility Query, or
 * return NULL if none.
 */
Query *
UtilityContainsQuery(Node *parsetree)
{
	Query	   *qry;

	switch (nodeTag(parsetree))
	{
		case T_DeclareCursorStmt:
			qry = castNode(Query, ((DeclareCursorStmt *) parsetree)->query);
			if (qry->commandType == CMD_UTILITY)
				return UtilityContainsQuery(qry->utilityStmt);
			return qry;

		case T_ExplainStmt:
			qry = castNode(Query, ((ExplainStmt *) parsetree)->query);
			if (qry->commandType == CMD_UTILITY)
				return UtilityContainsQuery(qry->utilityStmt);
			return qry;

		case T_CreateTableAsStmt:
			qry = castNode(Query, ((CreateTableAsStmt *) parsetree)->query);
			if (qry->commandType == CMD_UTILITY)
				return UtilityContainsQuery(qry->utilityStmt);
			return qry;

		default:
			return NULL;
	}
}


/*
 * AlterObjectTypeCommandTag
 *		helper function for CreateCommandTag
 *
 * This covers most cases where ALTER is used with an ObjectType enum.
 */
static CommandTag
AlterObjectTypeCommandTag(ObjectType objtype)
{
	CommandTag	tag;

	switch (objtype)
	{
		case OBJECT_AGGREGATE:
			tag = CMDTAG_ALTER_AGGREGATE;
			break;
		case OBJECT_ATTRIBUTE:
			tag = CMDTAG_ALTER_TYPE;
			break;
		case OBJECT_CAST:
			tag = CMDTAG_ALTER_CAST;
			break;
		case OBJECT_COLLATION:
			tag = CMDTAG_ALTER_COLLATION;
			break;
		case OBJECT_COLUMN:
			tag = CMDTAG_ALTER_TABLE;
			break;
		case OBJECT_CONVERSION:
			tag = CMDTAG_ALTER_CONVERSION;
			break;
		case OBJECT_DATABASE:
			tag = CMDTAG_ALTER_DATABASE;
			break;
		case OBJECT_DOMAIN:
		case OBJECT_DOMCONSTRAINT:
			tag = CMDTAG_ALTER_DOMAIN;
			break;
		case OBJECT_EXTENSION:
			tag = CMDTAG_ALTER_EXTENSION;
			break;
		case OBJECT_FDW:
			tag = CMDTAG_ALTER_FOREIGN_DATA_WRAPPER;
			break;
		case OBJECT_FOREIGN_SERVER:
			tag = CMDTAG_ALTER_SERVER;
			break;
		case OBJECT_FOREIGN_TABLE:
			tag = CMDTAG_ALTER_FOREIGN_TABLE;
			break;
		case OBJECT_FUNCTION:
			tag = CMDTAG_ALTER_FUNCTION;
			break;
		case OBJECT_INDEX:
			tag = CMDTAG_ALTER_INDEX;
			break;
		case OBJECT_LANGUAGE:
			tag = CMDTAG_ALTER_LANGUAGE;
			break;
		case OBJECT_LARGEOBJECT:
			tag = CMDTAG_ALTER_LARGE_OBJECT;
			break;
		case OBJECT_OPCLASS:
			tag = CMDTAG_ALTER_OPERATOR_CLASS;
			break;
		case OBJECT_OPERATOR:
			tag = CMDTAG_ALTER_OPERATOR;
			break;
		case OBJECT_OPFAMILY:
			tag = CMDTAG_ALTER_OPERATOR_FAMILY;
			break;
		case OBJECT_POLICY:
			tag = CMDTAG_ALTER_POLICY;
			break;
		case OBJECT_PROCEDURE:
			tag = CMDTAG_ALTER_PROCEDURE;
			break;
		case OBJECT_ROLE:
			tag = CMDTAG_ALTER_ROLE;
			break;
		case OBJECT_ROUTINE:
			tag = CMDTAG_ALTER_ROUTINE;
			break;
		case OBJECT_RULE:
			tag = CMDTAG_ALTER_RULE;
			break;
		case OBJECT_SCHEMA:
			tag = CMDTAG_ALTER_SCHEMA;
			break;
		case OBJECT_SEQUENCE:
			tag = CMDTAG_ALTER_SEQUENCE;
			break;
		case OBJECT_TABLE:
		case OBJECT_TABCONSTRAINT:
			tag = CMDTAG_ALTER_TABLE;
			break;
		case OBJECT_TABLESPACE:
			tag = CMDTAG_ALTER_TABLESPACE;
			break;
		case OBJECT_TRIGGER:
			tag = CMDTAG_ALTER_TRIGGER;
			break;
		case OBJECT_EVENT_TRIGGER:
			tag = CMDTAG_ALTER_EVENT_TRIGGER;
			break;
		case OBJECT_TSCONFIGURATION:
			tag = CMDTAG_ALTER_TEXT_SEARCH_CONFIGURATION;
			break;
		case OBJECT_TSDICTIONARY:
			tag = CMDTAG_ALTER_TEXT_SEARCH_DICTIONARY;
			break;
		case OBJECT_TSPARSER:
			tag = CMDTAG_ALTER_TEXT_SEARCH_PARSER;
			break;
		case OBJECT_TSTEMPLATE:
			tag = CMDTAG_ALTER_TEXT_SEARCH_TEMPLATE;
			break;
		case OBJECT_TYPE:
			tag = CMDTAG_ALTER_TYPE;
			break;
		case OBJECT_VIEW:
			tag = CMDTAG_ALTER_VIEW;
			break;
		case OBJECT_MATVIEW:
			tag = CMDTAG_ALTER_MATERIALIZED_VIEW;
			break;
		case OBJECT_PUBLICATION:
			tag = CMDTAG_ALTER_PUBLICATION;
			break;
		case OBJECT_SUBSCRIPTION:
			tag = CMDTAG_ALTER_SUBSCRIPTION;
			break;
		case OBJECT_STATISTIC_EXT:
			tag = CMDTAG_ALTER_STATISTICS;
			break;
		default:
			tag = CMDTAG_UNKNOWN;
			break;
	}

	return tag;
}

/*
 * CreateCommandTag
 *		utility to get a CommandTag for the command operation,
 *		given either a raw (un-analyzed) parsetree, an analyzed Query,
 *		or a PlannedStmt.
 *
 * This must handle all command types, but since the vast majority
 * of 'em are utility commands, it seems sensible to keep it here.
 */
CommandTag
CreateCommandTag(Node *parsetree)
{
	CommandTag	tag;

	switch (nodeTag(parsetree))
	{
			/* recurse if we're given a RawStmt */
		case T_RawStmt:
			tag = CreateCommandTag(((RawStmt *) parsetree)->stmt);
			break;

			/* raw plannable queries */
		case T_InsertStmt:
			tag = CMDTAG_INSERT;
			break;

		case T_DeleteStmt:
			tag = CMDTAG_DELETE;
			break;

		case T_UpdateStmt:
			tag = CMDTAG_UPDATE;
			break;

		case T_MergeStmt:
			tag = CMDTAG_MERGE;
			break;

		case T_SelectStmt:
			tag = CMDTAG_SELECT;
			break;

		case T_PLAssignStmt:
			tag = CMDTAG_SELECT;
			break;

			/* utility statements --- same whether raw or cooked */
		case T_TransactionStmt:
			{
				TransactionStmt *stmt = (TransactionStmt *) parsetree;

				switch (stmt->kind)
				{
					case TRANS_STMT_BEGIN:
						tag = CMDTAG_BEGIN;
						break;

					case TRANS_STMT_START:
						tag = CMDTAG_START_TRANSACTION;
						break;

					case TRANS_STMT_COMMIT:
						tag = CMDTAG_COMMIT;
						break;

					case TRANS_STMT_ROLLBACK:
					case TRANS_STMT_ROLLBACK_TO:
						tag = CMDTAG_ROLLBACK;
						break;

					case TRANS_STMT_SAVEPOINT:
						tag = CMDTAG_SAVEPOINT;
						break;

					case TRANS_STMT_RELEASE:
						tag = CMDTAG_RELEASE;
						break;

					case TRANS_STMT_PREPARE:
						tag = CMDTAG_PREPARE_TRANSACTION;
						break;

					case TRANS_STMT_COMMIT_PREPARED:
						tag = CMDTAG_COMMIT_PREPARED;
						break;

					case TRANS_STMT_ROLLBACK_PREPARED:
						tag = CMDTAG_ROLLBACK_PREPARED;
						break;

					default:
						tag = CMDTAG_UNKNOWN;
						break;
				}
			}
			break;

		case T_DeclareCursorStmt:
			tag = CMDTAG_DECLARE_CURSOR;
			break;

		case T_ClosePortalStmt:
			{
				ClosePortalStmt *stmt = (ClosePortalStmt *) parsetree;

				if (stmt->portalname == NULL)
					tag = CMDTAG_CLOSE_CURSOR_ALL;
				else
					tag = CMDTAG_CLOSE_CURSOR;
			}
			break;

		case T_FetchStmt:
			{
				FetchStmt  *stmt = (FetchStmt *) parsetree;

				tag = (stmt->ismove) ? CMDTAG_MOVE : CMDTAG_FETCH;
			}
			break;

		case T_CreateDomainStmt:
			tag = CMDTAG_CREATE_DOMAIN;
			break;

		case T_CreateSchemaStmt:
			tag = CMDTAG_CREATE_SCHEMA;
			break;

		case T_CreateStmt:
			tag = CMDTAG_CREATE_TABLE;
			break;

		case T_CreateTableSpaceStmt:
			tag = CMDTAG_CREATE_TABLESPACE;
			break;

		case T_DropTableSpaceStmt:
			tag = CMDTAG_DROP_TABLESPACE;
			break;

		case T_AlterTableSpaceOptionsStmt:
			tag = CMDTAG_ALTER_TABLESPACE;
			break;

		case T_CreateExtensionStmt:
			tag = CMDTAG_CREATE_EXTENSION;
			break;

		case T_AlterExtensionStmt:
			tag = CMDTAG_ALTER_EXTENSION;
			break;

		case T_AlterExtensionContentsStmt:
			tag = CMDTAG_ALTER_EXTENSION;
			break;

		case T_CreateFdwStmt:
			tag = CMDTAG_CREATE_FOREIGN_DATA_WRAPPER;
			break;

		case T_AlterFdwStmt:
			tag = CMDTAG_ALTER_FOREIGN_DATA_WRAPPER;
			break;

		case T_CreateForeignServerStmt:
			tag = CMDTAG_CREATE_SERVER;
			break;

		case T_AlterForeignServerStmt:
			tag = CMDTAG_ALTER_SERVER;
			break;

		case T_CreateUserMappingStmt:
			tag = CMDTAG_CREATE_USER_MAPPING;
			break;

		case T_AlterUserMappingStmt:
			tag = CMDTAG_ALTER_USER_MAPPING;
			break;

		case T_DropUserMappingStmt:
			tag = CMDTAG_DROP_USER_MAPPING;
			break;

		case T_CreateForeignTableStmt:
			tag = CMDTAG_CREATE_FOREIGN_TABLE;
			break;

		case T_ImportForeignSchemaStmt:
			tag = CMDTAG_IMPORT_FOREIGN_SCHEMA;
			break;

		case T_DropStmt:
			switch (((DropStmt *) parsetree)->removeType)
			{
				case OBJECT_TABLE:
					tag = CMDTAG_DROP_TABLE;
					break;
				case OBJECT_SEQUENCE:
					tag = CMDTAG_DROP_SEQUENCE;
					break;
				case OBJECT_VIEW:
					tag = CMDTAG_DROP_VIEW;
					break;
				case OBJECT_MATVIEW:
					tag = CMDTAG_DROP_MATERIALIZED_VIEW;
					break;
				case OBJECT_INDEX:
					tag = CMDTAG_DROP_INDEX;
					break;
				case OBJECT_TYPE:
					tag = CMDTAG_DROP_TYPE;
					break;
				case OBJECT_DOMAIN:
					tag = CMDTAG_DROP_DOMAIN;
					break;
				case OBJECT_COLLATION:
					tag = CMDTAG_DROP_COLLATION;
					break;
				case OBJECT_CONVERSION:
					tag = CMDTAG_DROP_CONVERSION;
					break;
				case OBJECT_SCHEMA:
					tag = CMDTAG_DROP_SCHEMA;
					break;
				case OBJECT_TSPARSER:
					tag = CMDTAG_DROP_TEXT_SEARCH_PARSER;
					break;
				case OBJECT_TSDICTIONARY:
					tag = CMDTAG_DROP_TEXT_SEARCH_DICTIONARY;
					break;
				case OBJECT_TSTEMPLATE:
					tag = CMDTAG_DROP_TEXT_SEARCH_TEMPLATE;
					break;
				case OBJECT_TSCONFIGURATION:
					tag = CMDTAG_DROP_TEXT_SEARCH_CONFIGURATION;
					break;
				case OBJECT_FOREIGN_TABLE:
					tag = CMDTAG_DROP_FOREIGN_TABLE;
					break;
				case OBJECT_EXTENSION:
					tag = CMDTAG_DROP_EXTENSION;
					break;
				case OBJECT_FUNCTION:
					tag = CMDTAG_DROP_FUNCTION;
					break;
				case OBJECT_PROCEDURE:
					tag = CMDTAG_DROP_PROCEDURE;
					break;
				case OBJECT_ROUTINE:
					tag = CMDTAG_DROP_ROUTINE;
					break;
				case OBJECT_AGGREGATE:
					tag = CMDTAG_DROP_AGGREGATE;
					break;
				case OBJECT_OPERATOR:
					tag = CMDTAG_DROP_OPERATOR;
					break;
				case OBJECT_LANGUAGE:
					tag = CMDTAG_DROP_LANGUAGE;
					break;
				case OBJECT_CAST:
					tag = CMDTAG_DROP_CAST;
					break;
				case OBJECT_TRIGGER:
					tag = CMDTAG_DROP_TRIGGER;
					break;
				case OBJECT_EVENT_TRIGGER:
					tag = CMDTAG_DROP_EVENT_TRIGGER;
					break;
				case OBJECT_RULE:
					tag = CMDTAG_DROP_RULE;
					break;
				case OBJECT_FDW:
					tag = CMDTAG_DROP_FOREIGN_DATA_WRAPPER;
					break;
				case OBJECT_FOREIGN_SERVER:
					tag = CMDTAG_DROP_SERVER;
					break;
				case OBJECT_OPCLASS:
					tag = CMDTAG_DROP_OPERATOR_CLASS;
					break;
				case OBJECT_OPFAMILY:
					tag = CMDTAG_DROP_OPERATOR_FAMILY;
					break;
				case OBJECT_POLICY:
					tag = CMDTAG_DROP_POLICY;
					break;
				case OBJECT_TRANSFORM:
					tag = CMDTAG_DROP_TRANSFORM;
					break;
				case OBJECT_ACCESS_METHOD:
					tag = CMDTAG_DROP_ACCESS_METHOD;
					break;
				case OBJECT_PUBLICATION:
					tag = CMDTAG_DROP_PUBLICATION;
					break;
				case OBJECT_STATISTIC_EXT:
					tag = CMDTAG_DROP_STATISTICS;
					break;
				default:
					tag = CMDTAG_UNKNOWN;
			}
			break;

		case T_TruncateStmt:
			tag = CMDTAG_TRUNCATE_TABLE;
			break;

		case T_CommentStmt:
			tag = CMDTAG_COMMENT;
			break;

		case T_SecLabelStmt:
			tag = CMDTAG_SECURITY_LABEL;
			break;

		case T_CopyStmt:
			tag = CMDTAG_COPY;
			break;

		case T_RenameStmt:

			/*
			 * When the column is renamed, the command tag is created from its
			 * relation type
			 */
			tag = AlterObjectTypeCommandTag(((RenameStmt *) parsetree)->renameType == OBJECT_COLUMN ?
											((RenameStmt *) parsetree)->relationType :
											((RenameStmt *) parsetree)->renameType);
			break;

		case T_AlterObjectDependsStmt:
			tag = AlterObjectTypeCommandTag(((AlterObjectDependsStmt *) parsetree)->objectType);
			break;

		case T_AlterObjectSchemaStmt:
			tag = AlterObjectTypeCommandTag(((AlterObjectSchemaStmt *) parsetree)->objectType);
			break;

		case T_AlterOwnerStmt:
			tag = AlterObjectTypeCommandTag(((AlterOwnerStmt *) parsetree)->objectType);
			break;

		case T_AlterTableMoveAllStmt:
			tag = AlterObjectTypeCommandTag(((AlterTableMoveAllStmt *) parsetree)->objtype);
			break;

		case T_AlterTableStmt:
			tag = AlterObjectTypeCommandTag(((AlterTableStmt *) parsetree)->objtype);
			break;

		case T_AlterDomainStmt:
			tag = CMDTAG_ALTER_DOMAIN;
			break;

		case T_AlterFunctionStmt:
			switch (((AlterFunctionStmt *) parsetree)->objtype)
			{
				case OBJECT_FUNCTION:
					tag = CMDTAG_ALTER_FUNCTION;
					break;
				case OBJECT_PROCEDURE:
					tag = CMDTAG_ALTER_PROCEDURE;
					break;
				case OBJECT_ROUTINE:
					tag = CMDTAG_ALTER_ROUTINE;
					break;
				default:
					tag = CMDTAG_UNKNOWN;
			}
			break;

		case T_GrantStmt:
			{
				GrantStmt  *stmt = (GrantStmt *) parsetree;

				tag = (stmt->is_grant) ? CMDTAG_GRANT : CMDTAG_REVOKE;
			}
			break;

		case T_GrantRoleStmt:
			{
				GrantRoleStmt *stmt = (GrantRoleStmt *) parsetree;

				tag = (stmt->is_grant) ? CMDTAG_GRANT_ROLE : CMDTAG_REVOKE_ROLE;
			}
			break;

		case T_AlterDefaultPrivilegesStmt:
			tag = CMDTAG_ALTER_DEFAULT_PRIVILEGES;
			break;

		case T_DefineStmt:
			switch (((DefineStmt *) parsetree)->kind)
			{
				case OBJECT_AGGREGATE:
					tag = CMDTAG_CREATE_AGGREGATE;
					break;
				case OBJECT_OPERATOR:
					tag = CMDTAG_CREATE_OPERATOR;
					break;
				case OBJECT_TYPE:
					tag = CMDTAG_CREATE_TYPE;
					break;
				case OBJECT_TSPARSER:
					tag = CMDTAG_CREATE_TEXT_SEARCH_PARSER;
					break;
				case OBJECT_TSDICTIONARY:
					tag = CMDTAG_CREATE_TEXT_SEARCH_DICTIONARY;
					break;
				case OBJECT_TSTEMPLATE:
					tag = CMDTAG_CREATE_TEXT_SEARCH_TEMPLATE;
					break;
				case OBJECT_TSCONFIGURATION:
					tag = CMDTAG_CREATE_TEXT_SEARCH_CONFIGURATION;
					break;
				case OBJECT_COLLATION:
					tag = CMDTAG_CREATE_COLLATION;
					break;
				case OBJECT_ACCESS_METHOD:
					tag = CMDTAG_CREATE_ACCESS_METHOD;
					break;
				default:
					tag = CMDTAG_UNKNOWN;
			}
			break;

		case T_CompositeTypeStmt:
			tag = CMDTAG_CREATE_TYPE;
			break;

		case T_CreateEnumStmt:
			tag = CMDTAG_CREATE_TYPE;
			break;

		case T_CreateRangeStmt:
			tag = CMDTAG_CREATE_TYPE;
			break;

		case T_AlterEnumStmt:
			tag = CMDTAG_ALTER_TYPE;
			break;

		case T_ViewStmt:
			tag = CMDTAG_CREATE_VIEW;
			break;

		case T_CreateFunctionStmt:
			if (((CreateFunctionStmt *) parsetree)->is_procedure)
				tag = CMDTAG_CREATE_PROCEDURE;
			else
				tag = CMDTAG_CREATE_FUNCTION;
			break;

		case T_IndexStmt:
			tag = CMDTAG_CREATE_INDEX;
			break;

		case T_RuleStmt:
			tag = CMDTAG_CREATE_RULE;
			break;

		case T_CreateSeqStmt:
			tag = CMDTAG_CREATE_SEQUENCE;
			break;

		case T_AlterSeqStmt:
			tag = CMDTAG_ALTER_SEQUENCE;
			break;

		case T_DoStmt:
			tag = CMDTAG_DO;
			break;

		case T_CreatedbStmt:
			tag = CMDTAG_CREATE_DATABASE;
			break;

		case T_AlterDatabaseStmt:
		case T_AlterDatabaseRefreshCollStmt:
		case T_AlterDatabaseSetStmt:
			tag = CMDTAG_ALTER_DATABASE;
			break;

		case T_DropdbStmt:
			tag = CMDTAG_DROP_DATABASE;
			break;

		case T_NotifyStmt:
			tag = CMDTAG_NOTIFY;
			break;

		case T_ListenStmt:
			tag = CMDTAG_LISTEN;
			break;

		case T_UnlistenStmt:
			tag = CMDTAG_UNLISTEN;
			break;

		case T_LoadStmt:
			tag = CMDTAG_LOAD;
			break;

		case T_CallStmt:
			tag = CMDTAG_CALL;
			break;

		case T_ClusterStmt:
			tag = CMDTAG_CLUSTER;
			break;

		case T_VacuumStmt:
			if (((VacuumStmt *) parsetree)->is_vacuumcmd)
				tag = CMDTAG_VACUUM;
			else
				tag = CMDTAG_ANALYZE;
			break;

		case T_ExplainStmt:
			tag = CMDTAG_EXPLAIN;
			break;

		case T_CreateTableAsStmt:
			switch (((CreateTableAsStmt *) parsetree)->objtype)
			{
				case OBJECT_TABLE:
					if (((CreateTableAsStmt *) parsetree)->is_select_into)
						tag = CMDTAG_SELECT_INTO;
					else
						tag = CMDTAG_CREATE_TABLE_AS;
					break;
				case OBJECT_MATVIEW:
					tag = CMDTAG_CREATE_MATERIALIZED_VIEW;
					break;
				default:
					tag = CMDTAG_UNKNOWN;
			}
			break;

		case T_RefreshMatViewStmt:
			tag = CMDTAG_REFRESH_MATERIALIZED_VIEW;
			break;

		case T_AlterSystemStmt:
			tag = CMDTAG_ALTER_SYSTEM;
			break;

		case T_VariableSetStmt:
			switch (((VariableSetStmt *) parsetree)->kind)
			{
				case VAR_SET_VALUE:
				case VAR_SET_CURRENT:
				case VAR_SET_DEFAULT:
				case VAR_SET_MULTI:
					tag = CMDTAG_SET;
					break;
				case VAR_RESET:
				case VAR_RESET_ALL:
					tag = CMDTAG_RESET;
					break;
				default:
					tag = CMDTAG_UNKNOWN;
			}
			break;

		case T_VariableShowStmt:
			tag = CMDTAG_SHOW;
			break;

		case T_DiscardStmt:
			switch (((DiscardStmt *) parsetree)->target)
			{
				case DISCARD_ALL:
					tag = CMDTAG_DISCARD_ALL;
					break;
				case DISCARD_PLANS:
					tag = CMDTAG_DISCARD_PLANS;
					break;
				case DISCARD_TEMP:
					tag = CMDTAG_DISCARD_TEMP;
					break;
				case DISCARD_SEQUENCES:
					tag = CMDTAG_DISCARD_SEQUENCES;
					break;
				default:
					tag = CMDTAG_UNKNOWN;
			}
			break;

		case T_CreateTransformStmt:
			tag = CMDTAG_CREATE_TRANSFORM;
			break;

		case T_CreateTrigStmt:
			tag = CMDTAG_CREATE_TRIGGER;
			break;

		case T_CreateEventTrigStmt:
			tag = CMDTAG_CREATE_EVENT_TRIGGER;
			break;

		case T_AlterEventTrigStmt:
			tag = CMDTAG_ALTER_EVENT_TRIGGER;
			break;

		case T_CreatePLangStmt:
			tag = CMDTAG_CREATE_LANGUAGE;
			break;

		case T_CreateRoleStmt:
			tag = CMDTAG_CREATE_ROLE;
			break;

		case T_AlterRoleStmt:
			tag = CMDTAG_ALTER_ROLE;
			break;

		case T_AlterRoleSetStmt:
			tag = CMDTAG_ALTER_ROLE;
			break;

		case T_DropRoleStmt:
			tag = CMDTAG_DROP_ROLE;
			break;

		case T_DropOwnedStmt:
			tag = CMDTAG_DROP_OWNED;
			break;

		case T_ReassignOwnedStmt:
			tag = CMDTAG_REASSIGN_OWNED;
			break;

		case T_LockStmt:
			tag = CMDTAG_LOCK_TABLE;
			break;

		case T_ConstraintsSetStmt:
			tag = CMDTAG_SET_CONSTRAINTS;
			break;

		case T_CheckPointStmt:
			tag = CMDTAG_CHECKPOINT;
			break;

		case T_ReindexStmt:
			tag = CMDTAG_REINDEX;
			break;

		case T_CreateConversionStmt:
			tag = CMDTAG_CREATE_CONVERSION;
			break;

		case T_CreateCastStmt:
			tag = CMDTAG_CREATE_CAST;
			break;

		case T_CreateOpClassStmt:
			tag = CMDTAG_CREATE_OPERATOR_CLASS;
			break;

		case T_CreateOpFamilyStmt:
			tag = CMDTAG_CREATE_OPERATOR_FAMILY;
			break;

		case T_AlterOpFamilyStmt:
			tag = CMDTAG_ALTER_OPERATOR_FAMILY;
			break;

		case T_AlterOperatorStmt:
			tag = CMDTAG_ALTER_OPERATOR;
			break;

		case T_AlterTypeStmt:
			tag = CMDTAG_ALTER_TYPE;
			break;

		case T_AlterTSDictionaryStmt:
			tag = CMDTAG_ALTER_TEXT_SEARCH_DICTIONARY;
			break;

		case T_AlterTSConfigurationStmt:
			tag = CMDTAG_ALTER_TEXT_SEARCH_CONFIGURATION;
			break;

		case T_CreatePolicyStmt:
			tag = CMDTAG_CREATE_POLICY;
			break;

		case T_AlterPolicyStmt:
			tag = CMDTAG_ALTER_POLICY;
			break;

		case T_CreateAmStmt:
			tag = CMDTAG_CREATE_ACCESS_METHOD;
			break;

		case T_CreatePublicationStmt:
			tag = CMDTAG_CREATE_PUBLICATION;
			break;

		case T_AlterPublicationStmt:
			tag = CMDTAG_ALTER_PUBLICATION;
			break;

		case T_CreateSubscriptionStmt:
			tag = CMDTAG_CREATE_SUBSCRIPTION;
			break;

		case T_AlterSubscriptionStmt:
			tag = CMDTAG_ALTER_SUBSCRIPTION;
			break;

		case T_DropSubscriptionStmt:
			tag = CMDTAG_DROP_SUBSCRIPTION;
			break;

		case T_AlterCollationStmt:
			tag = CMDTAG_ALTER_COLLATION;
			break;

		case T_PrepareStmt:
			tag = CMDTAG_PREPARE;
			break;

		case T_ExecuteStmt:
			tag = CMDTAG_EXECUTE;
			break;

		case T_CreateStatsStmt:
			tag = CMDTAG_CREATE_STATISTICS;
			break;

		case T_AlterStatsStmt:
			tag = CMDTAG_ALTER_STATISTICS;
			break;

		case T_DeallocateStmt:
			{
				DeallocateStmt *stmt = (DeallocateStmt *) parsetree;

				if (stmt->name == NULL)
					tag = CMDTAG_DEALLOCATE_ALL;
				else
					tag = CMDTAG_DEALLOCATE;
			}
			break;

			/* already-planned queries */
		case T_PlannedStmt:
			{
				PlannedStmt *stmt = (PlannedStmt *) parsetree;

				switch (stmt->commandType)
				{
					case CMD_SELECT:

						/*
						 * We take a little extra care here so that the result
						 * will be useful for complaints about read-only
						 * statements
						 */
						if (stmt->rowMarks != NIL)
						{
							/* not 100% but probably close enough */
							switch (((PlanRowMark *) linitial(stmt->rowMarks))->strength)
							{
								case LCS_FORKEYSHARE:
									tag = CMDTAG_SELECT_FOR_KEY_SHARE;
									break;
								case LCS_FORSHARE:
									tag = CMDTAG_SELECT_FOR_SHARE;
									break;
								case LCS_FORNOKEYUPDATE:
									tag = CMDTAG_SELECT_FOR_NO_KEY_UPDATE;
									break;
								case LCS_FORUPDATE:
									tag = CMDTAG_SELECT_FOR_UPDATE;
									break;
								default:
									tag = CMDTAG_SELECT;
									break;
							}
						}
						else
							tag = CMDTAG_SELECT;
						break;
					case CMD_UPDATE:
						tag = CMDTAG_UPDATE;
						break;
					case CMD_INSERT:
						tag = CMDTAG_INSERT;
						break;
					case CMD_DELETE:
						tag = CMDTAG_DELETE;
						break;
					case CMD_MERGE:
						tag = CMDTAG_MERGE;
						break;
					case CMD_UTILITY:
						tag = CreateCommandTag(stmt->utilityStmt);
						break;
					default:
						elog(WARNING, "unrecognized commandType: %d",
							 (int) stmt->commandType);
						tag = CMDTAG_UNKNOWN;
						break;
				}
			}
			break;

			/* parsed-and-rewritten-but-not-planned queries */
		case T_Query:
			{
				Query	   *stmt = (Query *) parsetree;

				switch (stmt->commandType)
				{
					case CMD_SELECT:

						/*
						 * We take a little extra care here so that the result
						 * will be useful for complaints about read-only
						 * statements
						 */
						if (stmt->rowMarks != NIL)
						{
							/* not 100% but probably close enough */
							switch (((RowMarkClause *) linitial(stmt->rowMarks))->strength)
							{
								case LCS_FORKEYSHARE:
									tag = CMDTAG_SELECT_FOR_KEY_SHARE;
									break;
								case LCS_FORSHARE:
									tag = CMDTAG_SELECT_FOR_SHARE;
									break;
								case LCS_FORNOKEYUPDATE:
									tag = CMDTAG_SELECT_FOR_NO_KEY_UPDATE;
									break;
								case LCS_FORUPDATE:
									tag = CMDTAG_SELECT_FOR_UPDATE;
									break;
								default:
									tag = CMDTAG_UNKNOWN;
									break;
							}
						}
						else
							tag = CMDTAG_SELECT;
						break;
					case CMD_UPDATE:
						tag = CMDTAG_UPDATE;
						break;
					case CMD_INSERT:
						tag = CMDTAG_INSERT;
						break;
					case CMD_DELETE:
						tag = CMDTAG_DELETE;
						break;
					case CMD_MERGE:
						tag = CMDTAG_MERGE;
						break;
					case CMD_UTILITY:
						tag = CreateCommandTag(stmt->utilityStmt);
						break;
					default:
						elog(WARNING, "unrecognized commandType: %d",
							 (int) stmt->commandType);
						tag = CMDTAG_UNKNOWN;
						break;
				}
			}
			break;

		default:
			elog(WARNING, "unrecognized node type: %d",
				 (int) nodeTag(parsetree));
			tag = CMDTAG_UNKNOWN;
			break;
	}

	return tag;
}


/*
 * GetCommandLogLevel
 *		utility to get the minimum log_statement level for a command,
 *		given either a raw (un-analyzed) parsetree, an analyzed Query,
 *		or a PlannedStmt.
 *
 * This must handle all command types, but since the vast majority
 * of 'em are utility commands, it seems sensible to keep it here.
 */
LogStmtLevel
GetCommandLogLevel(Node *parsetree)
{
	LogStmtLevel lev;

	switch (nodeTag(parsetree))
	{
			/* recurse if we're given a RawStmt */
		case T_RawStmt:
			lev = GetCommandLogLevel(((RawStmt *) parsetree)->stmt);
			break;

			/* raw plannable queries */
		case T_InsertStmt:
		case T_DeleteStmt:
		case T_UpdateStmt:
		case T_MergeStmt:
			lev = LOGSTMT_MOD;
			break;

		case T_SelectStmt:
			if (((SelectStmt *) parsetree)->intoClause)
				lev = LOGSTMT_DDL;	/* SELECT INTO */
			else
				lev = LOGSTMT_ALL;
			break;

		case T_PLAssignStmt:
			lev = LOGSTMT_ALL;
			break;

			/* utility statements --- same whether raw or cooked */
		case T_TransactionStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_DeclareCursorStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_ClosePortalStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_FetchStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_CreateSchemaStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateStmt:
		case T_CreateForeignTableStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateTableSpaceStmt:
		case T_DropTableSpaceStmt:
		case T_AlterTableSpaceOptionsStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateExtensionStmt:
		case T_AlterExtensionStmt:
		case T_AlterExtensionContentsStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateFdwStmt:
		case T_AlterFdwStmt:
		case T_CreateForeignServerStmt:
		case T_AlterForeignServerStmt:
		case T_CreateUserMappingStmt:
		case T_AlterUserMappingStmt:
		case T_DropUserMappingStmt:
		case T_ImportForeignSchemaStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_DropStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_TruncateStmt:
			lev = LOGSTMT_MOD;
			break;

		case T_CommentStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_SecLabelStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CopyStmt:
			if (((CopyStmt *) parsetree)->is_from)
				lev = LOGSTMT_MOD;
			else
				lev = LOGSTMT_ALL;
			break;

		case T_PrepareStmt:
			{
				PrepareStmt *stmt = (PrepareStmt *) parsetree;

				/* Look through a PREPARE to the contained stmt */
				lev = GetCommandLogLevel(stmt->query);
			}
			break;

		case T_ExecuteStmt:
			{
				ExecuteStmt *stmt = (ExecuteStmt *) parsetree;
				PreparedStatement *ps;

				/* Look through an EXECUTE to the referenced stmt */
				ps = FetchPreparedStatement(stmt->name, false);
				if (ps && ps->plansource->raw_parse_tree)
					lev = GetCommandLogLevel(ps->plansource->raw_parse_tree->stmt);
				else
					lev = LOGSTMT_ALL;
			}
			break;

		case T_DeallocateStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_RenameStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterObjectDependsStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterObjectSchemaStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterOwnerStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterOperatorStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterTypeStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterTableMoveAllStmt:
		case T_AlterTableStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterDomainStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_GrantStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_GrantRoleStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterDefaultPrivilegesStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_DefineStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CompositeTypeStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateEnumStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateRangeStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterEnumStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_ViewStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateFunctionStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterFunctionStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_IndexStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_RuleStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateSeqStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterSeqStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_DoStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_CreatedbStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterDatabaseStmt:
		case T_AlterDatabaseRefreshCollStmt:
		case T_AlterDatabaseSetStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_DropdbStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_NotifyStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_ListenStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_UnlistenStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_LoadStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_CallStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_ClusterStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_VacuumStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_ExplainStmt:
			{
				ExplainStmt *stmt = (ExplainStmt *) parsetree;
				bool		analyze = false;
				ListCell   *lc;

				/* Look through an EXPLAIN ANALYZE to the contained stmt */
				foreach(lc, stmt->options)
				{
					DefElem    *opt = (DefElem *) lfirst(lc);

					if (strcmp(opt->defname, "analyze") == 0)
						analyze = defGetBoolean(opt);
					/* don't "break", as explain.c will use the last value */
				}
				if (analyze)
					return GetCommandLogLevel(stmt->query);

				/* Plain EXPLAIN isn't so interesting */
				lev = LOGSTMT_ALL;
			}
			break;

		case T_CreateTableAsStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_RefreshMatViewStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterSystemStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_VariableSetStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_VariableShowStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_DiscardStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_CreateTrigStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateEventTrigStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterEventTrigStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreatePLangStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateDomainStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateRoleStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterRoleStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterRoleSetStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_DropRoleStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_DropOwnedStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_ReassignOwnedStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_LockStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_ConstraintsSetStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_CheckPointStmt:
			lev = LOGSTMT_ALL;
			break;

		case T_ReindexStmt:
			lev = LOGSTMT_ALL;	/* should this be DDL? */
			break;

		case T_CreateConversionStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateCastStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateOpClassStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateOpFamilyStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateTransformStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterOpFamilyStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreatePolicyStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterPolicyStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterTSDictionaryStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterTSConfigurationStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateAmStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreatePublicationStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterPublicationStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateSubscriptionStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterSubscriptionStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_DropSubscriptionStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_CreateStatsStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterStatsStmt:
			lev = LOGSTMT_DDL;
			break;

		case T_AlterCollationStmt:
			lev = LOGSTMT_DDL;
			break;

			/* already-planned queries */
		case T_PlannedStmt:
			{
				PlannedStmt *stmt = (PlannedStmt *) parsetree;

				switch (stmt->commandType)
				{
					case CMD_SELECT:
						lev = LOGSTMT_ALL;
						break;

					case CMD_UPDATE:
					case CMD_INSERT:
					case CMD_DELETE:
					case CMD_MERGE:
						lev = LOGSTMT_MOD;
						break;

					case CMD_UTILITY:
						lev = GetCommandLogLevel(stmt->utilityStmt);
						break;

					default:
						elog(WARNING, "unrecognized commandType: %d",
							 (int) stmt->commandType);
						lev = LOGSTMT_ALL;
						break;
				}
			}
			break;

			/* parsed-and-rewritten-but-not-planned queries */
		case T_Query:
			{
				Query	   *stmt = (Query *) parsetree;

				switch (stmt->commandType)
				{
					case CMD_SELECT:
						lev = LOGSTMT_ALL;
						break;

					case CMD_UPDATE:
					case CMD_INSERT:
					case CMD_DELETE:
					case CMD_MERGE:
						lev = LOGSTMT_MOD;
						break;

					case CMD_UTILITY:
						lev = GetCommandLogLevel(stmt->utilityStmt);
						break;

					default:
						elog(WARNING, "unrecognized commandType: %d",
							 (int) stmt->commandType);
						lev = LOGSTMT_ALL;
						break;
				}
			}
			break;

		default:
			elog(WARNING, "unrecognized node type: %d",
				 (int) nodeTag(parsetree));
			lev = LOGSTMT_ALL;
			break;
	}

	return lev;
}

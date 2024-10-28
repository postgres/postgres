/*--------------------------------------------------------------------------
 *
 * test_oat_hooks.c
 *		Code for testing mandatory access control (MAC) using object access hooks.
 *
 * Copyright (c) 2015-2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_oat_hooks/test_oat_hooks.c
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/parallel.h"
#include "catalog/dependency.h"
#include "catalog/objectaccess.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "tcop/utility.h"

PG_MODULE_MAGIC;

/*
 * GUCs controlling which operations to deny
 */
static bool REGRESS_deny_set_variable = false;
static bool REGRESS_deny_alter_system = false;
static bool REGRESS_deny_object_access = false;
static bool REGRESS_deny_exec_perms = false;
static bool REGRESS_deny_utility_commands = false;
static bool REGRESS_audit = false;

/*
 * GUCs for testing privileges on USERSET and SUSET variables,
 * with and without privileges granted prior to module load.
 */
static bool REGRESS_userset_variable1 = false;
static bool REGRESS_userset_variable2 = false;
static bool REGRESS_suset_variable1 = false;
static bool REGRESS_suset_variable2 = false;

/* Saved hook values */
static object_access_hook_type next_object_access_hook = NULL;
static object_access_hook_type_str next_object_access_hook_str = NULL;
static ExecutorCheckPerms_hook_type next_exec_check_perms_hook = NULL;
static ProcessUtility_hook_type next_ProcessUtility_hook = NULL;

/* Test Object Access Type Hook hooks */
static void REGRESS_object_access_hook_str(ObjectAccessType access,
										   Oid classId, const char *objName,
										   int subId, void *arg);
static void REGRESS_object_access_hook(ObjectAccessType access, Oid classId,
									   Oid objectId, int subId, void *arg);
static bool REGRESS_exec_check_perms(List *rangeTabls, List *rteperminfos, bool do_abort);
static void REGRESS_utility_command(PlannedStmt *pstmt,
									const char *queryString, bool readOnlyTree,
									ProcessUtilityContext context,
									ParamListInfo params,
									QueryEnvironment *queryEnv,
									DestReceiver *dest, QueryCompletion *qc);

/* Helper functions */
static char *accesstype_to_string(ObjectAccessType access, int subId);
static char *accesstype_arg_to_string(ObjectAccessType access, void *arg);


/*
 * Module load callback
 */
void
_PG_init(void)
{
	/*
	 * test_oat_hooks.deny_set_variable = (on|off)
	 */
	DefineCustomBoolVariable("test_oat_hooks.deny_set_variable",
							 "Deny non-superuser set permissions",
							 NULL,
							 &REGRESS_deny_set_variable,
							 false,
							 PGC_SUSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL,
							 NULL,
							 NULL);

	/*
	 * test_oat_hooks.deny_alter_system = (on|off)
	 */
	DefineCustomBoolVariable("test_oat_hooks.deny_alter_system",
							 "Deny non-superuser alter system set permissions",
							 NULL,
							 &REGRESS_deny_alter_system,
							 false,
							 PGC_SUSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL,
							 NULL,
							 NULL);

	/*
	 * test_oat_hooks.deny_object_access = (on|off)
	 */
	DefineCustomBoolVariable("test_oat_hooks.deny_object_access",
							 "Deny non-superuser object access permissions",
							 NULL,
							 &REGRESS_deny_object_access,
							 false,
							 PGC_SUSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL,
							 NULL,
							 NULL);

	/*
	 * test_oat_hooks.deny_exec_perms = (on|off)
	 */
	DefineCustomBoolVariable("test_oat_hooks.deny_exec_perms",
							 "Deny non-superuser exec permissions",
							 NULL,
							 &REGRESS_deny_exec_perms,
							 false,
							 PGC_SUSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL,
							 NULL,
							 NULL);

	/*
	 * test_oat_hooks.deny_utility_commands = (on|off)
	 */
	DefineCustomBoolVariable("test_oat_hooks.deny_utility_commands",
							 "Deny non-superuser utility commands",
							 NULL,
							 &REGRESS_deny_utility_commands,
							 false,
							 PGC_SUSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL,
							 NULL,
							 NULL);

	/*
	 * test_oat_hooks.audit = (on|off)
	 */
	DefineCustomBoolVariable("test_oat_hooks.audit",
							 "Turn on/off debug audit messages",
							 NULL,
							 &REGRESS_audit,
							 false,
							 PGC_SUSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL,
							 NULL,
							 NULL);

	/*
	 * test_oat_hooks.user_var{1,2} = (on|off)
	 */
	DefineCustomBoolVariable("test_oat_hooks.user_var1",
							 "Dummy parameter settable by public",
							 NULL,
							 &REGRESS_userset_variable1,
							 false,
							 PGC_USERSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("test_oat_hooks.user_var2",
							 "Dummy parameter settable by public",
							 NULL,
							 &REGRESS_userset_variable2,
							 false,
							 PGC_USERSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL,
							 NULL,
							 NULL);

	/*
	 * test_oat_hooks.super_var{1,2} = (on|off)
	 */
	DefineCustomBoolVariable("test_oat_hooks.super_var1",
							 "Dummy parameter settable by superuser",
							 NULL,
							 &REGRESS_suset_variable1,
							 false,
							 PGC_SUSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("test_oat_hooks.super_var2",
							 "Dummy parameter settable by superuser",
							 NULL,
							 &REGRESS_suset_variable2,
							 false,
							 PGC_SUSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL,
							 NULL,
							 NULL);

	MarkGUCPrefixReserved("test_oat_hooks");

	/* Object access hook */
	next_object_access_hook = object_access_hook;
	object_access_hook = REGRESS_object_access_hook;

	/* Object access hook str */
	next_object_access_hook_str = object_access_hook_str;
	object_access_hook_str = REGRESS_object_access_hook_str;

	/* DML permission check */
	next_exec_check_perms_hook = ExecutorCheckPerms_hook;
	ExecutorCheckPerms_hook = REGRESS_exec_check_perms;

	/* ProcessUtility hook */
	next_ProcessUtility_hook = ProcessUtility_hook;
	ProcessUtility_hook = REGRESS_utility_command;
}

static void
emit_audit_message(const char *type, const char *hook, char *action, char *objName)
{
	/*
	 * Ensure that audit messages are not duplicated by only emitting them
	 * from a leader process, not a worker process. This makes the test
	 * results deterministic even if run with debug_parallel_query = regress.
	 */
	if (REGRESS_audit && !IsParallelWorker())
	{
		const char *who = superuser_arg(GetUserId()) ? "superuser" : "non-superuser";

		if (objName)
			ereport(NOTICE,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("in %s: %s %s %s [%s]", hook, who, type, action, objName)));
		else
			ereport(NOTICE,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("in %s: %s %s %s", hook, who, type, action)));
	}

	if (action)
		pfree(action);
	if (objName)
		pfree(objName);
}

static void
audit_attempt(const char *hook, char *action, char *objName)
{
	emit_audit_message("attempting", hook, action, objName);
}

static void
audit_success(const char *hook, char *action, char *objName)
{
	emit_audit_message("finished", hook, action, objName);
}

static void
audit_failure(const char *hook, char *action, char *objName)
{
	emit_audit_message("denied", hook, action, objName);
}

static void
REGRESS_object_access_hook_str(ObjectAccessType access, Oid classId, const char *objName, int subId, void *arg)
{
	audit_attempt("object_access_hook_str",
				  accesstype_to_string(access, subId),
				  pstrdup(objName));

	if (next_object_access_hook_str)
	{
		(*next_object_access_hook_str) (access, classId, objName, subId, arg);
	}

	switch (access)
	{
		case OAT_POST_ALTER:
			if ((subId & ACL_SET) && (subId & ACL_ALTER_SYSTEM))
			{
				if (REGRESS_deny_set_variable && !superuser_arg(GetUserId()))
					ereport(ERROR,
							(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
							 errmsg("permission denied: all privileges %s", objName)));
			}
			else if (subId & ACL_SET)
			{
				if (REGRESS_deny_set_variable && !superuser_arg(GetUserId()))
					ereport(ERROR,
							(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
							 errmsg("permission denied: set %s", objName)));
			}
			else if (subId & ACL_ALTER_SYSTEM)
			{
				if (REGRESS_deny_alter_system && !superuser_arg(GetUserId()))
					ereport(ERROR,
							(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
							 errmsg("permission denied: alter system set %s", objName)));
			}
			else
				elog(ERROR, "Unknown ParameterAclRelationId subId: %d", subId);
			break;
		default:
			break;
	}

	audit_success("object_access_hook_str",
				  accesstype_to_string(access, subId),
				  pstrdup(objName));
}

static void
REGRESS_object_access_hook(ObjectAccessType access, Oid classId, Oid objectId, int subId, void *arg)
{
	audit_attempt("object access",
				  accesstype_to_string(access, 0),
				  accesstype_arg_to_string(access, arg));

	if (REGRESS_deny_object_access && !superuser_arg(GetUserId()))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: %s [%s]",
						accesstype_to_string(access, 0),
						accesstype_arg_to_string(access, arg))));

	/* Forward to next hook in the chain */
	if (next_object_access_hook)
		(*next_object_access_hook) (access, classId, objectId, subId, arg);

	audit_success("object access",
				  accesstype_to_string(access, 0),
				  accesstype_arg_to_string(access, arg));
}

static bool
REGRESS_exec_check_perms(List *rangeTabls, List *rteperminfos, bool do_abort)
{
	bool		am_super = superuser_arg(GetUserId());
	bool		allow = true;

	audit_attempt("executor check perms", pstrdup("execute"), NULL);

	/* Perform our check */
	allow = !REGRESS_deny_exec_perms || am_super;
	if (do_abort && !allow)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: %s", "execute")));

	/* Forward to next hook in the chain */
	if (next_exec_check_perms_hook &&
		!(*next_exec_check_perms_hook) (rangeTabls, rteperminfos, do_abort))
		allow = false;

	if (allow)
		audit_success("executor check perms",
					  pstrdup("execute"),
					  NULL);
	else
		audit_failure("executor check perms",
					  pstrdup("execute"),
					  NULL);

	return allow;
}

static void
REGRESS_utility_command(PlannedStmt *pstmt,
						const char *queryString,
						bool readOnlyTree,
						ProcessUtilityContext context,
						ParamListInfo params,
						QueryEnvironment *queryEnv,
						DestReceiver *dest,
						QueryCompletion *qc)
{
	Node	   *parsetree = pstmt->utilityStmt;
	const char *action = GetCommandTagName(CreateCommandTag(parsetree));

	audit_attempt("process utility",
				  pstrdup(action),
				  NULL);

	/* Check permissions */
	if (REGRESS_deny_utility_commands && !superuser_arg(GetUserId()))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: %s", action)));

	/* Forward to next hook in the chain */
	if (next_ProcessUtility_hook)
		(*next_ProcessUtility_hook) (pstmt, queryString, readOnlyTree,
									 context, params, queryEnv,
									 dest, qc);
	else
		standard_ProcessUtility(pstmt, queryString, readOnlyTree,
								context, params, queryEnv,
								dest, qc);

	/* We're done */
	audit_success("process utility",
				  pstrdup(action),
				  NULL);
}

static char *
accesstype_to_string(ObjectAccessType access, int subId)
{
	const char *type;

	switch (access)
	{
		case OAT_POST_CREATE:
			type = "create";
			break;
		case OAT_DROP:
			type = "drop";
			break;
		case OAT_POST_ALTER:
			type = "alter";
			break;
		case OAT_NAMESPACE_SEARCH:
			type = "namespace search";
			break;
		case OAT_FUNCTION_EXECUTE:
			type = "execute";
			break;
		case OAT_TRUNCATE:
			type = "truncate";
			break;
		default:
			type = "UNRECOGNIZED ObjectAccessType";
	}

	if ((subId & ACL_SET) && (subId & ACL_ALTER_SYSTEM))
		return psprintf("%s (subId=0x%x, all privileges)", type, subId);
	if (subId & ACL_SET)
		return psprintf("%s (subId=0x%x, set)", type, subId);
	if (subId & ACL_ALTER_SYSTEM)
		return psprintf("%s (subId=0x%x, alter system)", type, subId);

	return psprintf("%s (subId=0x%x)", type, subId);
}

static char *
accesstype_arg_to_string(ObjectAccessType access, void *arg)
{
	if (arg == NULL)
		return pstrdup("extra info null");

	switch (access)
	{
		case OAT_POST_CREATE:
			{
				ObjectAccessPostCreate *pc_arg = (ObjectAccessPostCreate *) arg;

				return pstrdup(pc_arg->is_internal ? "internal" : "explicit");
			}
			break;
		case OAT_DROP:
			{
				ObjectAccessDrop *drop_arg = (ObjectAccessDrop *) arg;

				return psprintf("%s%s%s%s%s%s",
								((drop_arg->dropflags & PERFORM_DELETION_INTERNAL)
								 ? "internal action," : ""),
								((drop_arg->dropflags & PERFORM_DELETION_CONCURRENTLY)
								 ? "concurrent drop," : ""),
								((drop_arg->dropflags & PERFORM_DELETION_QUIETLY)
								 ? "suppress notices," : ""),
								((drop_arg->dropflags & PERFORM_DELETION_SKIP_ORIGINAL)
								 ? "keep original object," : ""),
								((drop_arg->dropflags & PERFORM_DELETION_SKIP_EXTENSIONS)
								 ? "keep extensions," : ""),
								((drop_arg->dropflags & PERFORM_DELETION_CONCURRENT_LOCK)
								 ? "normal concurrent drop," : ""));
			}
			break;
		case OAT_POST_ALTER:
			{
				ObjectAccessPostAlter *pa_arg = (ObjectAccessPostAlter *) arg;

				return psprintf("%s %s auxiliary object",
								(pa_arg->is_internal ? "internal" : "explicit"),
								(OidIsValid(pa_arg->auxiliary_id) ? "with" : "without"));
			}
			break;
		case OAT_NAMESPACE_SEARCH:
			{
				ObjectAccessNamespaceSearch *ns_arg = (ObjectAccessNamespaceSearch *) arg;

				return psprintf("%s, %s",
								(ns_arg->ereport_on_violation ? "report on violation" : "no report on violation"),
								(ns_arg->result ? "allowed" : "denied"));
			}
			break;
		case OAT_TRUNCATE:
		case OAT_FUNCTION_EXECUTE:
			/* hook takes no arg. */
			return pstrdup("unexpected extra info pointer received");
		default:
			return pstrdup("cannot parse extra info for unrecognized access type");
	}

	return pstrdup("unknown");
}

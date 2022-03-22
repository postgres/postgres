/*--------------------------------------------------------------------------
 *
 * test_oat_hooks.c
 *		Code for testing mandatory access control (MAC) using object access hooks.
 *
 * Copyright (c) 2015-2022, PostgreSQL Global Development Group
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
#include "catalog/pg_proc.h"
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

/* Saved hook values in case of unload */
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
static bool REGRESS_exec_check_perms(List *rangeTabls, bool do_abort);
static void REGRESS_utility_command(PlannedStmt *pstmt,
									const char *queryString, bool readOnlyTree,
									ProcessUtilityContext context,
									ParamListInfo params,
									QueryEnvironment *queryEnv,
									DestReceiver *dest, QueryCompletion *qc);

/* Helper functions */
static const char *nodetag_to_string(NodeTag tag);
static char *accesstype_to_string(ObjectAccessType access, int subId);
static char *accesstype_arg_to_string(ObjectAccessType access, void *arg);


void		_PG_init(void);
void		_PG_fini(void);

/*
 * Module load/unload callback
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

void
_PG_fini(void)
{
	/* Unload hooks */
	if (object_access_hook == REGRESS_object_access_hook)
		object_access_hook = next_object_access_hook;

	if (object_access_hook_str == REGRESS_object_access_hook_str)
		object_access_hook_str = next_object_access_hook_str;

	if (ExecutorCheckPerms_hook == REGRESS_exec_check_perms)
		ExecutorCheckPerms_hook = next_exec_check_perms_hook;

	if (ProcessUtility_hook == REGRESS_utility_command)
		ProcessUtility_hook = next_ProcessUtility_hook;
}

static void
emit_audit_message(const char *type, const char *hook, char *action, char *objName)
{
	/*
	 * Ensure that audit messages are not duplicated by only emitting them from
	 * a leader process, not a worker process. This makes the test results
	 * deterministic even if run with force_parallel_mode = regress.
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
		(*next_object_access_hook_str)(access, classId, objName, subId, arg);
	}

	switch (access)
	{
		case OAT_POST_ALTER:
			if (subId & ACL_SET_VALUE)
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
				elog(ERROR, "Unknown SettingAclRelationId subId: %d", subId);
			break;
		default:
			break;
	}

	audit_success("object_access_hook_str",
				  accesstype_to_string(access, subId),
				  pstrdup(objName));
}

static void
REGRESS_object_access_hook (ObjectAccessType access, Oid classId, Oid objectId, int subId, void *arg)
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
		(*next_object_access_hook)(access, classId, objectId, subId, arg);

	audit_success("object access",
				  accesstype_to_string(access, 0),
				  accesstype_arg_to_string(access, arg));
}

static bool
REGRESS_exec_check_perms(List *rangeTabls, bool do_abort)
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
		!(*next_exec_check_perms_hook) (rangeTabls, do_abort))
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

	const char *action;
	NodeTag tag = nodeTag(parsetree);

	switch (tag)
	{
		case T_VariableSetStmt:
			action = "set";
			break;
		case T_AlterSystemStmt:
			action = "alter system";
			break;
		case T_LoadStmt:
			action = "load";
			break;
		default:
			action = nodetag_to_string(tag);
			break;
	}

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

static const char *
nodetag_to_string(NodeTag tag)
{
	switch (tag)
	{
		case T_Invalid: return "Invalid"; break;
		case T_IndexInfo: return "IndexInfo"; break;
		case T_ExprContext: return "ExprContext"; break;
		case T_ProjectionInfo: return "ProjectionInfo"; break;
		case T_JunkFilter: return "JunkFilter"; break;
		case T_OnConflictSetState: return "OnConflictSetState"; break;
		case T_ResultRelInfo: return "ResultRelInfo"; break;
		case T_EState: return "EState"; break;
		case T_TupleTableSlot: return "TupleTableSlot"; break;
		case T_Plan: return "Plan"; break;
		case T_Result: return "Result"; break;
		case T_ProjectSet: return "ProjectSet"; break;
		case T_ModifyTable: return "ModifyTable"; break;
		case T_Append: return "Append"; break;
		case T_MergeAppend: return "MergeAppend"; break;
		case T_RecursiveUnion: return "RecursiveUnion"; break;
		case T_BitmapAnd: return "BitmapAnd"; break;
		case T_BitmapOr: return "BitmapOr"; break;
		case T_Scan: return "Scan"; break;
		case T_SeqScan: return "SeqScan"; break;
		case T_SampleScan: return "SampleScan"; break;
		case T_IndexScan: return "IndexScan"; break;
		case T_IndexOnlyScan: return "IndexOnlyScan"; break;
		case T_BitmapIndexScan: return "BitmapIndexScan"; break;
		case T_BitmapHeapScan: return "BitmapHeapScan"; break;
		case T_TidScan: return "TidScan"; break;
		case T_TidRangeScan: return "TidRangeScan"; break;
		case T_SubqueryScan: return "SubqueryScan"; break;
		case T_FunctionScan: return "FunctionScan"; break;
		case T_ValuesScan: return "ValuesScan"; break;
		case T_TableFuncScan: return "TableFuncScan"; break;
		case T_CteScan: return "CteScan"; break;
		case T_NamedTuplestoreScan: return "NamedTuplestoreScan"; break;
		case T_WorkTableScan: return "WorkTableScan"; break;
		case T_ForeignScan: return "ForeignScan"; break;
		case T_CustomScan: return "CustomScan"; break;
		case T_Join: return "Join"; break;
		case T_NestLoop: return "NestLoop"; break;
		case T_MergeJoin: return "MergeJoin"; break;
		case T_HashJoin: return "HashJoin"; break;
		case T_Material: return "Material"; break;
		case T_Memoize: return "Memoize"; break;
		case T_Sort: return "Sort"; break;
		case T_IncrementalSort: return "IncrementalSort"; break;
		case T_Group: return "Group"; break;
		case T_Agg: return "Agg"; break;
		case T_WindowAgg: return "WindowAgg"; break;
		case T_Unique: return "Unique"; break;
		case T_Gather: return "Gather"; break;
		case T_GatherMerge: return "GatherMerge"; break;
		case T_Hash: return "Hash"; break;
		case T_SetOp: return "SetOp"; break;
		case T_LockRows: return "LockRows"; break;
		case T_Limit: return "Limit"; break;
		case T_NestLoopParam: return "NestLoopParam"; break;
		case T_PlanRowMark: return "PlanRowMark"; break;
		case T_PartitionPruneInfo: return "PartitionPruneInfo"; break;
		case T_PartitionedRelPruneInfo: return "PartitionedRelPruneInfo"; break;
		case T_PartitionPruneStepOp: return "PartitionPruneStepOp"; break;
		case T_PartitionPruneStepCombine: return "PartitionPruneStepCombine"; break;
		case T_PlanInvalItem: return "PlanInvalItem"; break;
		case T_PlanState: return "PlanState"; break;
		case T_ResultState: return "ResultState"; break;
		case T_ProjectSetState: return "ProjectSetState"; break;
		case T_ModifyTableState: return "ModifyTableState"; break;
		case T_AppendState: return "AppendState"; break;
		case T_MergeAppendState: return "MergeAppendState"; break;
		case T_RecursiveUnionState: return "RecursiveUnionState"; break;
		case T_BitmapAndState: return "BitmapAndState"; break;
		case T_BitmapOrState: return "BitmapOrState"; break;
		case T_ScanState: return "ScanState"; break;
		case T_SeqScanState: return "SeqScanState"; break;
		case T_SampleScanState: return "SampleScanState"; break;
		case T_IndexScanState: return "IndexScanState"; break;
		case T_IndexOnlyScanState: return "IndexOnlyScanState"; break;
		case T_BitmapIndexScanState: return "BitmapIndexScanState"; break;
		case T_BitmapHeapScanState: return "BitmapHeapScanState"; break;
		case T_TidScanState: return "TidScanState"; break;
		case T_TidRangeScanState: return "TidRangeScanState"; break;
		case T_SubqueryScanState: return "SubqueryScanState"; break;
		case T_FunctionScanState: return "FunctionScanState"; break;
		case T_TableFuncScanState: return "TableFuncScanState"; break;
		case T_ValuesScanState: return "ValuesScanState"; break;
		case T_CteScanState: return "CteScanState"; break;
		case T_NamedTuplestoreScanState: return "NamedTuplestoreScanState"; break;
		case T_WorkTableScanState: return "WorkTableScanState"; break;
		case T_ForeignScanState: return "ForeignScanState"; break;
		case T_CustomScanState: return "CustomScanState"; break;
		case T_JoinState: return "JoinState"; break;
		case T_NestLoopState: return "NestLoopState"; break;
		case T_MergeJoinState: return "MergeJoinState"; break;
		case T_HashJoinState: return "HashJoinState"; break;
		case T_MaterialState: return "MaterialState"; break;
		case T_MemoizeState: return "MemoizeState"; break;
		case T_SortState: return "SortState"; break;
		case T_IncrementalSortState: return "IncrementalSortState"; break;
		case T_GroupState: return "GroupState"; break;
		case T_AggState: return "AggState"; break;
		case T_WindowAggState: return "WindowAggState"; break;
		case T_UniqueState: return "UniqueState"; break;
		case T_GatherState: return "GatherState"; break;
		case T_GatherMergeState: return "GatherMergeState"; break;
		case T_HashState: return "HashState"; break;
		case T_SetOpState: return "SetOpState"; break;
		case T_LockRowsState: return "LockRowsState"; break;
		case T_LimitState: return "LimitState"; break;
		case T_Alias: return "Alias"; break;
		case T_RangeVar: return "RangeVar"; break;
		case T_TableFunc: return "TableFunc"; break;
		case T_Var: return "Var"; break;
		case T_Const: return "Const"; break;
		case T_Param: return "Param"; break;
		case T_Aggref: return "Aggref"; break;
		case T_GroupingFunc: return "GroupingFunc"; break;
		case T_WindowFunc: return "WindowFunc"; break;
		case T_SubscriptingRef: return "SubscriptingRef"; break;
		case T_FuncExpr: return "FuncExpr"; break;
		case T_NamedArgExpr: return "NamedArgExpr"; break;
		case T_OpExpr: return "OpExpr"; break;
		case T_DistinctExpr: return "DistinctExpr"; break;
		case T_NullIfExpr: return "NullIfExpr"; break;
		case T_ScalarArrayOpExpr: return "ScalarArrayOpExpr"; break;
		case T_BoolExpr: return "BoolExpr"; break;
		case T_SubLink: return "SubLink"; break;
		case T_SubPlan: return "SubPlan"; break;
		case T_AlternativeSubPlan: return "AlternativeSubPlan"; break;
		case T_FieldSelect: return "FieldSelect"; break;
		case T_FieldStore: return "FieldStore"; break;
		case T_RelabelType: return "RelabelType"; break;
		case T_CoerceViaIO: return "CoerceViaIO"; break;
		case T_ArrayCoerceExpr: return "ArrayCoerceExpr"; break;
		case T_ConvertRowtypeExpr: return "ConvertRowtypeExpr"; break;
		case T_CollateExpr: return "CollateExpr"; break;
		case T_CaseExpr: return "CaseExpr"; break;
		case T_CaseWhen: return "CaseWhen"; break;
		case T_CaseTestExpr: return "CaseTestExpr"; break;
		case T_ArrayExpr: return "ArrayExpr"; break;
		case T_RowExpr: return "RowExpr"; break;
		case T_RowCompareExpr: return "RowCompareExpr"; break;
		case T_CoalesceExpr: return "CoalesceExpr"; break;
		case T_MinMaxExpr: return "MinMaxExpr"; break;
		case T_SQLValueFunction: return "SQLValueFunction"; break;
		case T_XmlExpr: return "XmlExpr"; break;
		case T_NullTest: return "NullTest"; break;
		case T_BooleanTest: return "BooleanTest"; break;
		case T_CoerceToDomain: return "CoerceToDomain"; break;
		case T_CoerceToDomainValue: return "CoerceToDomainValue"; break;
		case T_SetToDefault: return "SetToDefault"; break;
		case T_CurrentOfExpr: return "CurrentOfExpr"; break;
		case T_NextValueExpr: return "NextValueExpr"; break;
		case T_InferenceElem: return "InferenceElem"; break;
		case T_TargetEntry: return "TargetEntry"; break;
		case T_RangeTblRef: return "RangeTblRef"; break;
		case T_JoinExpr: return "JoinExpr"; break;
		case T_FromExpr: return "FromExpr"; break;
		case T_OnConflictExpr: return "OnConflictExpr"; break;
		case T_IntoClause: return "IntoClause"; break;
		case T_ExprState: return "ExprState"; break;
		case T_WindowFuncExprState: return "WindowFuncExprState"; break;
		case T_SetExprState: return "SetExprState"; break;
		case T_SubPlanState: return "SubPlanState"; break;
		case T_DomainConstraintState: return "DomainConstraintState"; break;
		case T_PlannerInfo: return "PlannerInfo"; break;
		case T_PlannerGlobal: return "PlannerGlobal"; break;
		case T_RelOptInfo: return "RelOptInfo"; break;
		case T_IndexOptInfo: return "IndexOptInfo"; break;
		case T_ForeignKeyOptInfo: return "ForeignKeyOptInfo"; break;
		case T_ParamPathInfo: return "ParamPathInfo"; break;
		case T_Path: return "Path"; break;
		case T_IndexPath: return "IndexPath"; break;
		case T_BitmapHeapPath: return "BitmapHeapPath"; break;
		case T_BitmapAndPath: return "BitmapAndPath"; break;
		case T_BitmapOrPath: return "BitmapOrPath"; break;
		case T_TidPath: return "TidPath"; break;
		case T_TidRangePath: return "TidRangePath"; break;
		case T_SubqueryScanPath: return "SubqueryScanPath"; break;
		case T_ForeignPath: return "ForeignPath"; break;
		case T_CustomPath: return "CustomPath"; break;
		case T_NestPath: return "NestPath"; break;
		case T_MergePath: return "MergePath"; break;
		case T_HashPath: return "HashPath"; break;
		case T_AppendPath: return "AppendPath"; break;
		case T_MergeAppendPath: return "MergeAppendPath"; break;
		case T_GroupResultPath: return "GroupResultPath"; break;
		case T_MaterialPath: return "MaterialPath"; break;
		case T_MemoizePath: return "MemoizePath"; break;
		case T_UniquePath: return "UniquePath"; break;
		case T_GatherPath: return "GatherPath"; break;
		case T_GatherMergePath: return "GatherMergePath"; break;
		case T_ProjectionPath: return "ProjectionPath"; break;
		case T_ProjectSetPath: return "ProjectSetPath"; break;
		case T_SortPath: return "SortPath"; break;
		case T_IncrementalSortPath: return "IncrementalSortPath"; break;
		case T_GroupPath: return "GroupPath"; break;
		case T_UpperUniquePath: return "UpperUniquePath"; break;
		case T_AggPath: return "AggPath"; break;
		case T_GroupingSetsPath: return "GroupingSetsPath"; break;
		case T_MinMaxAggPath: return "MinMaxAggPath"; break;
		case T_WindowAggPath: return "WindowAggPath"; break;
		case T_SetOpPath: return "SetOpPath"; break;
		case T_RecursiveUnionPath: return "RecursiveUnionPath"; break;
		case T_LockRowsPath: return "LockRowsPath"; break;
		case T_ModifyTablePath: return "ModifyTablePath"; break;
		case T_LimitPath: return "LimitPath"; break;
		case T_EquivalenceClass: return "EquivalenceClass"; break;
		case T_EquivalenceMember: return "EquivalenceMember"; break;
		case T_PathKey: return "PathKey"; break;
		case T_PathTarget: return "PathTarget"; break;
		case T_RestrictInfo: return "RestrictInfo"; break;
		case T_IndexClause: return "IndexClause"; break;
		case T_PlaceHolderVar: return "PlaceHolderVar"; break;
		case T_SpecialJoinInfo: return "SpecialJoinInfo"; break;
		case T_AppendRelInfo: return "AppendRelInfo"; break;
		case T_RowIdentityVarInfo: return "RowIdentityVarInfo"; break;
		case T_PlaceHolderInfo: return "PlaceHolderInfo"; break;
		case T_MinMaxAggInfo: return "MinMaxAggInfo"; break;
		case T_PlannerParamItem: return "PlannerParamItem"; break;
		case T_RollupData: return "RollupData"; break;
		case T_GroupingSetData: return "GroupingSetData"; break;
		case T_StatisticExtInfo: return "StatisticExtInfo"; break;
		case T_AllocSetContext: return "AllocSetContext"; break;
		case T_SlabContext: return "SlabContext"; break;
		case T_GenerationContext: return "GenerationContext"; break;
		case T_Integer: return "Integer"; break;
		case T_Float: return "Float"; break;
		case T_Boolean: return "Boolean"; break;
		case T_String: return "String"; break;
		case T_BitString: return "BitString"; break;
		case T_List: return "List"; break;
		case T_IntList: return "IntList"; break;
		case T_OidList: return "OidList"; break;
		case T_ExtensibleNode: return "ExtensibleNode"; break;
		case T_RawStmt: return "RawStmt"; break;
		case T_Query: return "Query"; break;
		case T_PlannedStmt: return "PlannedStmt"; break;
		case T_InsertStmt: return "InsertStmt"; break;
		case T_DeleteStmt: return "DeleteStmt"; break;
		case T_UpdateStmt: return "UpdateStmt"; break;
		case T_SelectStmt: return "SelectStmt"; break;
		case T_ReturnStmt: return "ReturnStmt"; break;
		case T_PLAssignStmt: return "PLAssignStmt"; break;
		case T_AlterTableStmt: return "AlterTableStmt"; break;
		case T_AlterTableCmd: return "AlterTableCmd"; break;
		case T_AlterDomainStmt: return "AlterDomainStmt"; break;
		case T_SetOperationStmt: return "SetOperationStmt"; break;
		case T_GrantStmt: return "GrantStmt"; break;
		case T_GrantRoleStmt: return "GrantRoleStmt"; break;
		case T_AlterDefaultPrivilegesStmt: return "AlterDefaultPrivilegesStmt"; break;
		case T_ClosePortalStmt: return "ClosePortalStmt"; break;
		case T_ClusterStmt: return "ClusterStmt"; break;
		case T_CopyStmt: return "CopyStmt"; break;
		case T_CreateStmt: return "CreateStmt"; break;
		case T_DefineStmt: return "DefineStmt"; break;
		case T_DropStmt: return "DropStmt"; break;
		case T_TruncateStmt: return "TruncateStmt"; break;
		case T_CommentStmt: return "CommentStmt"; break;
		case T_FetchStmt: return "FetchStmt"; break;
		case T_IndexStmt: return "IndexStmt"; break;
		case T_CreateFunctionStmt: return "CreateFunctionStmt"; break;
		case T_AlterFunctionStmt: return "AlterFunctionStmt"; break;
		case T_DoStmt: return "DoStmt"; break;
		case T_RenameStmt: return "RenameStmt"; break;
		case T_RuleStmt: return "RuleStmt"; break;
		case T_NotifyStmt: return "NotifyStmt"; break;
		case T_ListenStmt: return "ListenStmt"; break;
		case T_UnlistenStmt: return "UnlistenStmt"; break;
		case T_TransactionStmt: return "TransactionStmt"; break;
		case T_ViewStmt: return "ViewStmt"; break;
		case T_LoadStmt: return "LoadStmt"; break;
		case T_CreateDomainStmt: return "CreateDomainStmt"; break;
		case T_CreatedbStmt: return "CreatedbStmt"; break;
		case T_DropdbStmt: return "DropdbStmt"; break;
		case T_VacuumStmt: return "VacuumStmt"; break;
		case T_ExplainStmt: return "ExplainStmt"; break;
		case T_CreateTableAsStmt: return "CreateTableAsStmt"; break;
		case T_CreateSeqStmt: return "CreateSeqStmt"; break;
		case T_AlterSeqStmt: return "AlterSeqStmt"; break;
		case T_VariableSetStmt: return "VariableSetStmt"; break;
		case T_VariableShowStmt: return "VariableShowStmt"; break;
		case T_DiscardStmt: return "DiscardStmt"; break;
		case T_CreateTrigStmt: return "CreateTrigStmt"; break;
		case T_CreatePLangStmt: return "CreatePLangStmt"; break;
		case T_CreateRoleStmt: return "CreateRoleStmt"; break;
		case T_AlterRoleStmt: return "AlterRoleStmt"; break;
		case T_DropRoleStmt: return "DropRoleStmt"; break;
		case T_LockStmt: return "LockStmt"; break;
		case T_ConstraintsSetStmt: return "ConstraintsSetStmt"; break;
		case T_ReindexStmt: return "ReindexStmt"; break;
		case T_CheckPointStmt: return "CheckPointStmt"; break;
		case T_CreateSchemaStmt: return "CreateSchemaStmt"; break;
		case T_AlterDatabaseStmt: return "AlterDatabaseStmt"; break;
		case T_AlterDatabaseRefreshCollStmt: return "AlterDatabaseRefreshCollStmt"; break;
		case T_AlterDatabaseSetStmt: return "AlterDatabaseSetStmt"; break;
		case T_AlterRoleSetStmt: return "AlterRoleSetStmt"; break;
		case T_CreateConversionStmt: return "CreateConversionStmt"; break;
		case T_CreateCastStmt: return "CreateCastStmt"; break;
		case T_CreateOpClassStmt: return "CreateOpClassStmt"; break;
		case T_CreateOpFamilyStmt: return "CreateOpFamilyStmt"; break;
		case T_AlterOpFamilyStmt: return "AlterOpFamilyStmt"; break;
		case T_PrepareStmt: return "PrepareStmt"; break;
		case T_ExecuteStmt: return "ExecuteStmt"; break;
		case T_DeallocateStmt: return "DeallocateStmt"; break;
		case T_DeclareCursorStmt: return "DeclareCursorStmt"; break;
		case T_CreateTableSpaceStmt: return "CreateTableSpaceStmt"; break;
		case T_DropTableSpaceStmt: return "DropTableSpaceStmt"; break;
		case T_AlterObjectDependsStmt: return "AlterObjectDependsStmt"; break;
		case T_AlterObjectSchemaStmt: return "AlterObjectSchemaStmt"; break;
		case T_AlterOwnerStmt: return "AlterOwnerStmt"; break;
		case T_AlterOperatorStmt: return "AlterOperatorStmt"; break;
		case T_AlterTypeStmt: return "AlterTypeStmt"; break;
		case T_DropOwnedStmt: return "DropOwnedStmt"; break;
		case T_ReassignOwnedStmt: return "ReassignOwnedStmt"; break;
		case T_CompositeTypeStmt: return "CompositeTypeStmt"; break;
		case T_CreateEnumStmt: return "CreateEnumStmt"; break;
		case T_CreateRangeStmt: return "CreateRangeStmt"; break;
		case T_AlterEnumStmt: return "AlterEnumStmt"; break;
		case T_AlterTSDictionaryStmt: return "AlterTSDictionaryStmt"; break;
		case T_AlterTSConfigurationStmt: return "AlterTSConfigurationStmt"; break;
		case T_CreateFdwStmt: return "CreateFdwStmt"; break;
		case T_AlterFdwStmt: return "AlterFdwStmt"; break;
		case T_CreateForeignServerStmt: return "CreateForeignServerStmt"; break;
		case T_AlterForeignServerStmt: return "AlterForeignServerStmt"; break;
		case T_CreateUserMappingStmt: return "CreateUserMappingStmt"; break;
		case T_AlterUserMappingStmt: return "AlterUserMappingStmt"; break;
		case T_DropUserMappingStmt: return "DropUserMappingStmt"; break;
		case T_AlterTableSpaceOptionsStmt: return "AlterTableSpaceOptionsStmt"; break;
		case T_AlterTableMoveAllStmt: return "AlterTableMoveAllStmt"; break;
		case T_SecLabelStmt: return "SecLabelStmt"; break;
		case T_CreateForeignTableStmt: return "CreateForeignTableStmt"; break;
		case T_ImportForeignSchemaStmt: return "ImportForeignSchemaStmt"; break;
		case T_CreateExtensionStmt: return "CreateExtensionStmt"; break;
		case T_AlterExtensionStmt: return "AlterExtensionStmt"; break;
		case T_AlterExtensionContentsStmt: return "AlterExtensionContentsStmt"; break;
		case T_CreateEventTrigStmt: return "CreateEventTrigStmt"; break;
		case T_AlterEventTrigStmt: return "AlterEventTrigStmt"; break;
		case T_RefreshMatViewStmt: return "RefreshMatViewStmt"; break;
		case T_ReplicaIdentityStmt: return "ReplicaIdentityStmt"; break;
		case T_AlterSystemStmt: return "AlterSystemStmt"; break;
		case T_CreatePolicyStmt: return "CreatePolicyStmt"; break;
		case T_AlterPolicyStmt: return "AlterPolicyStmt"; break;
		case T_CreateTransformStmt: return "CreateTransformStmt"; break;
		case T_CreateAmStmt: return "CreateAmStmt"; break;
		case T_CreatePublicationStmt: return "CreatePublicationStmt"; break;
		case T_AlterPublicationStmt: return "AlterPublicationStmt"; break;
		case T_CreateSubscriptionStmt: return "CreateSubscriptionStmt"; break;
		case T_AlterSubscriptionStmt: return "AlterSubscriptionStmt"; break;
		case T_DropSubscriptionStmt: return "DropSubscriptionStmt"; break;
		case T_CreateStatsStmt: return "CreateStatsStmt"; break;
		case T_AlterCollationStmt: return "AlterCollationStmt"; break;
		case T_CallStmt: return "CallStmt"; break;
		case T_AlterStatsStmt: return "AlterStatsStmt"; break;
		case T_A_Expr: return "A_Expr"; break;
		case T_ColumnRef: return "ColumnRef"; break;
		case T_ParamRef: return "ParamRef"; break;
		case T_A_Const: return "A_Const"; break;
		case T_FuncCall: return "FuncCall"; break;
		case T_A_Star: return "A_Star"; break;
		case T_A_Indices: return "A_Indices"; break;
		case T_A_Indirection: return "A_Indirection"; break;
		case T_A_ArrayExpr: return "A_ArrayExpr"; break;
		case T_ResTarget: return "ResTarget"; break;
		case T_MultiAssignRef: return "MultiAssignRef"; break;
		case T_TypeCast: return "TypeCast"; break;
		case T_CollateClause: return "CollateClause"; break;
		case T_SortBy: return "SortBy"; break;
		case T_WindowDef: return "WindowDef"; break;
		case T_RangeSubselect: return "RangeSubselect"; break;
		case T_RangeFunction: return "RangeFunction"; break;
		case T_RangeTableSample: return "RangeTableSample"; break;
		case T_RangeTableFunc: return "RangeTableFunc"; break;
		case T_RangeTableFuncCol: return "RangeTableFuncCol"; break;
		case T_TypeName: return "TypeName"; break;
		case T_ColumnDef: return "ColumnDef"; break;
		case T_IndexElem: return "IndexElem"; break;
		case T_StatsElem: return "StatsElem"; break;
		case T_Constraint: return "Constraint"; break;
		case T_DefElem: return "DefElem"; break;
		case T_RangeTblEntry: return "RangeTblEntry"; break;
		case T_RangeTblFunction: return "RangeTblFunction"; break;
		case T_TableSampleClause: return "TableSampleClause"; break;
		case T_WithCheckOption: return "WithCheckOption"; break;
		case T_SortGroupClause: return "SortGroupClause"; break;
		case T_GroupingSet: return "GroupingSet"; break;
		case T_WindowClause: return "WindowClause"; break;
		case T_ObjectWithArgs: return "ObjectWithArgs"; break;
		case T_AccessPriv: return "AccessPriv"; break;
		case T_CreateOpClassItem: return "CreateOpClassItem"; break;
		case T_TableLikeClause: return "TableLikeClause"; break;
		case T_FunctionParameter: return "FunctionParameter"; break;
		case T_LockingClause: return "LockingClause"; break;
		case T_RowMarkClause: return "RowMarkClause"; break;
		case T_XmlSerialize: return "XmlSerialize"; break;
		case T_WithClause: return "WithClause"; break;
		case T_InferClause: return "InferClause"; break;
		case T_OnConflictClause: return "OnConflictClause"; break;
		case T_CTESearchClause: return "CTESearchClause"; break;
		case T_CTECycleClause: return "CTECycleClause"; break;
		case T_CommonTableExpr: return "CommonTableExpr"; break;
		case T_RoleSpec: return "RoleSpec"; break;
		case T_TriggerTransition: return "TriggerTransition"; break;
		case T_PartitionElem: return "PartitionElem"; break;
		case T_PartitionSpec: return "PartitionSpec"; break;
		case T_PartitionBoundSpec: return "PartitionBoundSpec"; break;
		case T_PartitionRangeDatum: return "PartitionRangeDatum"; break;
		case T_PartitionCmd: return "PartitionCmd"; break;
		case T_VacuumRelation: return "VacuumRelation"; break;
		case T_PublicationObjSpec: return "PublicationObjSpec"; break;
		case T_PublicationTable: return "PublicationTable"; break;
		case T_IdentifySystemCmd: return "IdentifySystemCmd"; break;
		case T_BaseBackupCmd: return "BaseBackupCmd"; break;
		case T_CreateReplicationSlotCmd: return "CreateReplicationSlotCmd"; break;
		case T_DropReplicationSlotCmd: return "DropReplicationSlotCmd"; break;
		case T_ReadReplicationSlotCmd: return "ReadReplicationSlotCmd"; break;
		case T_StartReplicationCmd: return "StartReplicationCmd"; break;
		case T_TimeLineHistoryCmd: return "TimeLineHistoryCmd"; break;
		case T_TriggerData: return "TriggerData"; break;
		case T_EventTriggerData: return "EventTriggerData"; break;
		case T_ReturnSetInfo: return "ReturnSetInfo"; break;
		case T_WindowObjectData: return "WindowObjectData"; break;
		case T_TIDBitmap: return "TIDBitmap"; break;
		case T_InlineCodeBlock: return "InlineCodeBlock"; break;
		case T_FdwRoutine: return "FdwRoutine"; break;
		case T_IndexAmRoutine: return "IndexAmRoutine"; break;
		case T_TableAmRoutine: return "TableAmRoutine"; break;
		case T_TsmRoutine: return "TsmRoutine"; break;
		case T_ForeignKeyCacheInfo: return "ForeignKeyCacheInfo"; break;
		case T_CallContext: return "CallContext"; break;
		case T_SupportRequestSimplify: return "SupportRequestSimplify"; break;
		case T_SupportRequestSelectivity: return "SupportRequestSelectivity"; break;
		case T_SupportRequestCost: return "SupportRequestCost"; break;
		case T_SupportRequestRows: return "SupportRequestRows"; break;
		case T_SupportRequestIndexCondition: return "SupportRequestIndexCondition"; break;
		default:
			break;
	}
	return "UNRECOGNIZED NodeTag";
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

	if (subId & ACL_SET_VALUE)
		return psprintf("%s (set)", type);
	if (subId & ACL_ALTER_SYSTEM)
		return psprintf("%s (alter system set)", type);

	return  psprintf("%s (subId=%d)", type, subId);
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
				ObjectAccessPostCreate *pc_arg = (ObjectAccessPostCreate *)arg;
				return pstrdup(pc_arg->is_internal ? "internal" : "explicit");
			}
			break;
		case OAT_DROP:
			{
				ObjectAccessDrop *drop_arg = (ObjectAccessDrop *)arg;

				return psprintf("%s%s%s%s%s%s",
					((drop_arg->dropflags & PERFORM_DELETION_INTERNAL)
						? "internal action," : ""),
					((drop_arg->dropflags & PERFORM_DELETION_INTERNAL)
						? "concurrent drop," : ""),
					((drop_arg->dropflags & PERFORM_DELETION_INTERNAL)
						? "suppress notices," : ""),
					((drop_arg->dropflags & PERFORM_DELETION_INTERNAL)
						? "keep original object," : ""),
					((drop_arg->dropflags & PERFORM_DELETION_INTERNAL)
						? "keep extensions," : ""),
					((drop_arg->dropflags & PERFORM_DELETION_INTERNAL)
						? "normal concurrent drop," : ""));
			}
			break;
		case OAT_POST_ALTER:
			{
				ObjectAccessPostAlter *pa_arg = (ObjectAccessPostAlter*)arg;

				return psprintf("%s %s auxiliary object",
					(pa_arg->is_internal ? "internal" : "explicit"),
					(OidIsValid(pa_arg->auxiliary_id) ? "with" : "without"));
			}
			break;
		case OAT_NAMESPACE_SEARCH:
			{
				ObjectAccessNamespaceSearch *ns_arg = (ObjectAccessNamespaceSearch *)arg;

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

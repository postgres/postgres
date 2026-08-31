/*-------------------------------------------------------------------------
 *
 * test_extensible.c
 *		Tests for extensible nodes and custom scans
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_extensible/test_extensible.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/table.h"
#include "access/tableam.h"
#include "catalog/namespace.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/nodes.h"
#include "nodes/plannodes.h"
#include "nodes/readfuncs.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/restrictinfo.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"

PG_MODULE_MAGIC;

/* Name of the test table that triggers our CustomScan injection */
#define TEST_TABLE_NAME		"test_extensible_tbl"

/*
 * TestExtNode - an ExtensibleNode subtype carrying our planning data.
 */
typedef struct TestExtNode
{
	ExtensibleNode base;		/* must be first */
	Oid			relid;			/* OID of the relation being scanned */
	int			repeat_count;	/* how many times to return each scanned row */
} TestExtNode;

#define TEST_EXT_NODE_NAME	"TestExtNode"
#define TEST_CUSTOM_SCAN_NAME	"TestCustomScan"

/* GUC: how many times the custom scan returns each row */
static int	test_repeat_count = 2;

static TestExtNode *text_to_test_ext_node(text *txt);

/* Encode a TestExtNode into its serialized representation */
#define TEST_EXT_NODE_TO_TEXT(node)	cstring_to_text(nodeToString(node))

/* Decode a function argument back into a TestExtNode */
#define PG_GETARG_TEST_EXT_NODE(n)	text_to_test_ext_node(PG_GETARG_TEXT_PP(n))

PG_FUNCTION_INFO_V1(test_get_extensible_node_methods);
PG_FUNCTION_INFO_V1(test_get_custom_scan_methods);
PG_FUNCTION_INFO_V1(test_ext_node_make);
PG_FUNCTION_INFO_V1(test_ext_node_copy);
PG_FUNCTION_INFO_V1(test_ext_node_equal);
PG_FUNCTION_INFO_V1(test_ext_node_get_relid);
PG_FUNCTION_INFO_V1(test_ext_node_get_repeat_count);

/*
 * ExtensibleNodeMethods callbacks.
 *
 * Note that nodeOut() and nodeRead() must agree on the set and order of
 * serialized fields.
 */
static void
test_ext_node_copy_cb(ExtensibleNode *newnode, const ExtensibleNode *oldnode)
{
	((TestExtNode *) newnode)->relid = ((const TestExtNode *) oldnode)->relid;
	((TestExtNode *) newnode)->repeat_count =
		((const TestExtNode *) oldnode)->repeat_count;
}

static bool
test_ext_node_equal_cb(const ExtensibleNode *a, const ExtensibleNode *b)
{
	return ((const TestExtNode *) a)->relid ==
		((const TestExtNode *) b)->relid &&
		((const TestExtNode *) a)->repeat_count ==
		((const TestExtNode *) b)->repeat_count;
}

static void
test_ext_node_out_cb(StringInfo str, const ExtensibleNode *node)
{
	appendStringInfo(str, " :relid %u", ((const TestExtNode *) node)->relid);
	appendStringInfo(str, " :repeat_count %d",
					 ((const TestExtNode *) node)->repeat_count);
}

/*
 * Fetch the next token, erroring out instead of returning NULL.
 *
 * Unlike anything in readfuncs.c, this callback is reachable with arbitrary
 * strings through SQL function calls, so we need this check.
 */
static const char *
test_ext_node_next_token(ReadNodeContext *ctx)
{
	int			length;
	const char *token = pg_strtok(ctx, &length);

	if (token == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("unexpected end of \"%s\"", TEST_EXT_NODE_NAME)));

	return token;
}

static void
test_ext_node_read_cb(ReadNodeContext *ctx, ExtensibleNode *node)
{
	TestExtNode *tnode = (TestExtNode *) node;

	(void) test_ext_node_next_token(ctx);	/* skip :relid */
	tnode->relid = atooid(test_ext_node_next_token(ctx));

	(void) test_ext_node_next_token(ctx);	/* skip :repeat_count */
	tnode->repeat_count = atoi(test_ext_node_next_token(ctx));
}

static const ExtensibleNodeMethods test_ext_node_methods =
{
	.extnodename = TEST_EXT_NODE_NAME,
	.node_size = sizeof(TestExtNode),
	.nodeCopy = test_ext_node_copy_cb,
	.nodeEqual = test_ext_node_equal_cb,
	.nodeOut = test_ext_node_out_cb,
	.nodeRead = test_ext_node_read_cb,
};

/*
 * TestCustomScanState - execution state for the custom scan
 */
typedef struct TestCustomScanState
{
	CustomScanState css;		/* must be first */
	TableScanDesc scandesc;
	int			repeat_count;	/* repeat_count from TestExtNode */
	int			repeats_left;	/* how many more times to return current row */
} TestCustomScanState;

/*
 * Executor callbacks
 */

/*
 * Retrieve our private planning data from a CustomScan node.
 */
static TestExtNode *
test_get_ext_node(CustomScan *cscan)
{
	TestExtNode *tnode;

	Assert(list_length(cscan->custom_private) == 1);
	tnode = (TestExtNode *) linitial(cscan->custom_private);

	Assert(IsA(tnode, ExtensibleNode));
	Assert(strcmp(tnode->base.extnodename, TEST_EXT_NODE_NAME) == 0);

	return tnode;
}

/*
 * BeginCustomScan.
 */
static void
test_begin_custom_scan(CustomScanState *node, EState *estate, int eflags)
{
	TestCustomScanState *tstate = (TestCustomScanState *) node;
	TestExtNode *tnode = test_get_ext_node((CustomScan *) node->ss.ps.plan);
	Relation	rel = node->ss.ss_currentRelation;

	Assert(tnode->repeat_count > 0);
	tstate->repeat_count = tnode->repeat_count;
	tstate->repeats_left = 0;
	tstate->scandesc = NULL;

	/* A plain EXPLAIN never executes the plan, so skip */
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	tstate->scandesc = table_beginscan(rel, estate->es_snapshot, 0, NULL,
									   SO_NONE);
}

/*
 * Access method for ExecScan(): return the next tuple to be considered, or
 * NULL when the scan is done.
 */
static TupleTableSlot *
test_scan_next(ScanState *node)
{
	TestCustomScanState *tstate = (TestCustomScanState *) node;
	TupleTableSlot *slot = node->ss_ScanTupleSlot;

	/* Return the current tuple again if it still has repeats left */
	if (tstate->repeats_left > 0)
	{
		tstate->repeats_left--;
		return slot;
	}

	if (!table_scan_getnextslot(tstate->scandesc, ForwardScanDirection, slot))
		return NULL;

	tstate->repeats_left = tstate->repeat_count - 1;
	return slot;
}

/*
 * Recheck method for ExecScan(), used only during EvalPlanQual rechecks.
 * We evaluate no quals of our own, so there is nothing to recheck.
 */
static bool
test_scan_recheck(ScanState *node, TupleTableSlot *slot)
{
	return true;
}

static TupleTableSlot *
test_exec_custom_scan(CustomScanState *node)
{
	return ExecScan(&node->ss, test_scan_next, test_scan_recheck);
}

static void
test_end_custom_scan(CustomScanState *node)
{
	TestCustomScanState *tstate = (TestCustomScanState *) node;

	/* No scan started under EXEC_FLAG_EXPLAIN_ONLY */
	if (tstate->scandesc != NULL)
		table_endscan(tstate->scandesc);
}

/*
 * ReScanCustomScan: reset our own state as well as ExecScan()'s.
 */
static void
test_rescan_custom_scan(CustomScanState *node)
{
	TestCustomScanState *tstate = (TestCustomScanState *) node;

	tstate->repeats_left = 0;
	table_rescan(tstate->scandesc, NULL);
	ExecScanReScan(&node->ss);
}

static const CustomExecMethods test_custom_exec_methods =
{
	.CustomName = TEST_CUSTOM_SCAN_NAME,
	.BeginCustomScan = test_begin_custom_scan,
	.ExecCustomScan = test_exec_custom_scan,
	.EndCustomScan = test_end_custom_scan,
	.ReScanCustomScan = test_rescan_custom_scan,
};

/*
 * CreateCustomScanState() allocates the CustomScanState and fills in its node
 * tag and its methods; everything else is left to ExecInitCustomScan().
 *
 * slotOps is set here because ExecInitCustomScan() reads it before creating
 * the scan tuple slot, and before it opens the scan relation itself.  So we
 * open the relation ourselves (its OID travels via our ExtensibleNode) just
 * to learn its slot type from table_slot_callbacks().
 *
 * A lock is taken rather than passing NoLock, since in a parallel worker
 * nothing has locked the relation yet; the worker takes its own lock later
 * in ExecGetRangeTableRelation().  AccessShareLock matches what a plain scan
 * uses.
 */
static Node *
test_create_custom_scan_state(CustomScan *cscan)
{
	TestCustomScanState *tstate;
	TestExtNode *tnode = test_get_ext_node(cscan);
	Relation	rel;

	tstate = (TestCustomScanState *)
		newNode(sizeof(TestCustomScanState), T_CustomScanState);
	tstate->css.methods = &test_custom_exec_methods;

	rel = table_open(tnode->relid, AccessShareLock);
	tstate->css.slotOps = table_slot_callbacks(rel);
	table_close(rel, NoLock);

	return (Node *) tstate;
}

static const CustomScanMethods test_custom_scan_methods =
{
	.CustomName = TEST_CUSTOM_SCAN_NAME,
	.CreateCustomScanState = test_create_custom_scan_state,
};

/*
 * Planner callbacks
 */

/*
 * PlanCustomPath turns our CustomPath into the CustomScan plan node that the
 * executor runs.
 */
static Plan *
test_plan_custom_path(PlannerInfo *root,
					  RelOptInfo *rel,
					  struct CustomPath *best_path,
					  List *tlist,
					  List *clauses,
					  List *custom_plans)
{
	CustomScan *cscan = makeNode(CustomScan);

	cscan->scan.plan.targetlist = tlist;

	/*
	 * Restriction clauses arrive as RestrictInfos; reduce them to bare
	 * expressions for the plan's  qual. Pseudoconstants are dropped since the
	 * core evaluates them in a gating Result node above us.
	 */
	cscan->scan.plan.qual = extract_actual_clauses(clauses, false);
	cscan->scan.scanrelid = rel->relid;
	cscan->flags = best_path->flags;

	/* Our CustomPath has no child paths */
	cscan->custom_plans = custom_plans;
	cscan->custom_exprs = NIL;

	/* Pass the ExtensibleNode from the path to the plan */
	cscan->custom_private = best_path->custom_private;
	cscan->custom_scan_tlist = NIL;
	cscan->custom_relids = NULL;
	cscan->methods = &test_custom_scan_methods;

	return (Plan *) cscan;
}

static const CustomPathMethods test_custom_path_methods =
{
	.CustomName = TEST_CUSTOM_SCAN_NAME,
	.PlanCustomPath = test_plan_custom_path,
};

static set_rel_pathlist_hook_type prev_set_rel_pathlist_hook = NULL;

static void
test_set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel,
					  Index rti, RangeTblEntry *rte)
{
	CustomPath *cpath;
	TestExtNode *tnode;
	char	   *relname;

	if (prev_set_rel_pathlist_hook)
		prev_set_rel_pathlist_hook(root, rel, rti, rte);

	/*
	 * Only consider plain base relations with a table AM, no inheritance and
	 * no tablesample, keeping the logic simple.
	 */
	if (rel->reloptkind != RELOPT_BASEREL || rte->rtekind != RTE_RELATION)
		return;
	if (rte->relkind != RELKIND_RELATION && rte->relkind != RELKIND_MATVIEW)
		return;
	if (rte->tablesample != NULL || rte->inh)
		return;

	/*
	 * Only inject our CustomPath for the specific marker table, free of
	 * namespace.
	 */
	relname = get_rel_name(rte->relid);
	if (relname == NULL || strcmp(relname, TEST_TABLE_NAME) != 0)
		return;

	/* TestExtNode for the executor callbacks */
	tnode = (TestExtNode *) newNode(sizeof(TestExtNode), T_ExtensibleNode);
	tnode->base.extnodename = TEST_EXT_NODE_NAME;
	tnode->relid = rte->relid;

	/*
	 * Read once.  Changes to the GUC do not affect already-planned queries.
	 * Each row is returned repeat_count times.
	 */
	tnode->repeat_count = test_repeat_count;

	/*
	 * Use a cost of zero to force our custom path; a real provider should
	 * estimate the cost honestly, but it does not matter for this module.
	 */
	cpath = makeNode(CustomPath);
	cpath->path.pathtype = T_CustomScan;
	cpath->path.parent = rel;
	cpath->path.pathtarget = rel->reltarget;
	cpath->path.rows = rel->rows * tnode->repeat_count;
	cpath->path.startup_cost = 0;
	cpath->path.total_cost = 0;

	/*
	 * Consider it as parallel safe, since our scan touches nothing but its
	 * own relation and keeps no state outside the CustomScanState.
	 */
	cpath->path.parallel_safe = rel->consider_parallel;

	cpath->flags = 0;
	cpath->custom_paths = NIL;
	cpath->custom_private = list_make1(tnode);
	cpath->methods = &test_custom_path_methods;

	add_path(rel, (Path *) cpath);
}

/*
 * test_get_extensible_node_methods
 *
 * Thin wrapper around GetExtensibleNodeMethods().  Returns the registered
 * extnodename, or NULL when missing_ok = true and the name is not found.
 */
Datum
test_get_extensible_node_methods(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	bool		missing_ok = PG_GETARG_BOOL(1);
	const ExtensibleNodeMethods *methods;

	methods = GetExtensibleNodeMethods(name, missing_ok);
	if (methods == NULL)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(cstring_to_text(methods->extnodename));
}

/*
 * test_get_custom_scan_methods
 *
 * Thin wrapper around GetCustomScanMethods().  Returns the registered
 * CustomName, or NULL when missing_ok = true and the name is not found.
 */
Datum
test_get_custom_scan_methods(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	bool		missing_ok = PG_GETARG_BOOL(1);
	const CustomScanMethods *methods;

	methods = GetCustomScanMethods(name, missing_ok);
	if (methods == NULL)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(cstring_to_text(methods->CustomName));
}

/*
 * Decode a TestExtNode via stringToNode(), rejecting a string describing
 * some other kind of node instead of misinterpreting it as one of ours.
 */
static TestExtNode *
text_to_test_ext_node(text *txt)
{
	Node	   *node = stringToNode(text_to_cstring(txt));

	if (node == NULL || !IsA(node, ExtensibleNode) ||
		strcmp(((ExtensibleNode *) node)->extnodename, TEST_EXT_NODE_NAME) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("argument is not a serialized \"%s\"",
						TEST_EXT_NODE_NAME)));

	return (TestExtNode *) node;
}

/*
 * test_ext_node_make
 *
 * Builds a TestExtNode and returns it.
 */
Datum
test_ext_node_make(PG_FUNCTION_ARGS)
{
	TestExtNode *tnode;

	tnode = (TestExtNode *) newNode(sizeof(TestExtNode), T_ExtensibleNode);
	tnode->base.extnodename = TEST_EXT_NODE_NAME;
	tnode->relid = PG_GETARG_OID(0);
	tnode->repeat_count = PG_GETARG_INT32(1);

	PG_RETURN_TEXT_P(TEST_EXT_NODE_TO_TEXT(tnode));
}

/*
 * test_ext_node_copy
 */
Datum
test_ext_node_copy(PG_FUNCTION_ARGS)
{
	TestExtNode *tnode = PG_GETARG_TEST_EXT_NODE(0);

	PG_RETURN_TEXT_P(TEST_EXT_NODE_TO_TEXT(copyObject(tnode)));
}

/*
 * test_ext_node_equal
 */
Datum
test_ext_node_equal(PG_FUNCTION_ARGS)
{
	TestExtNode *a = PG_GETARG_TEST_EXT_NODE(0);
	TestExtNode *b = PG_GETARG_TEST_EXT_NODE(1);

	PG_RETURN_BOOL(equal(a, b));
}

/*
 * test_ext_node_get_relid
 * test_ext_node_get_repeat_count
 *
 * Field accessors for the custom node contents.
 */
Datum
test_ext_node_get_relid(PG_FUNCTION_ARGS)
{
	PG_RETURN_OID(PG_GETARG_TEST_EXT_NODE(0)->relid);
}

Datum
test_ext_node_get_repeat_count(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(PG_GETARG_TEST_EXT_NODE(0)->repeat_count);
}

/*
 * Module initialization
 */
void
_PG_init(void)
{
	RegisterCustomScanMethods(&test_custom_scan_methods);
	RegisterExtensibleNodeMethods(&test_ext_node_methods);

	DefineCustomIntVariable("test_extensible.repeat_count",
							"Number of times the custom scan returns each row.",
							NULL,
							&test_repeat_count,
							2,
							1,
							100,
							PGC_USERSET,
							0,
							NULL, NULL, NULL);

	MarkGUCPrefixReserved("test_extensible");

	/* Install the path-list hook to inject CustomPaths for the test table */
	prev_set_rel_pathlist_hook = set_rel_pathlist_hook;
	set_rel_pathlist_hook = test_set_rel_pathlist;
}

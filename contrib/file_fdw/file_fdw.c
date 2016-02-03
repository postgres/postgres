/*-------------------------------------------------------------------------
 *
 * file_fdw.c
 *		  foreign-data wrapper for server-side flat files.
 *
 * Copyright (c) 2010-2016, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  contrib/file_fdw/file_fdw.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>
#include <unistd.h>

#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/sysattr.h"
#include "catalog/pg_foreign_table.h"
#include "commands/copy.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "commands/vacuum.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/var.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/sampling.h"

PG_MODULE_MAGIC;

/*
 * Describes the valid options for objects that use this wrapper.
 */
struct FileFdwOption
{
	const char *optname;
	Oid			optcontext;		/* Oid of catalog in which option may appear */
};

/*
 * Valid options for file_fdw.
 * These options are based on the options for the COPY FROM command.
 * But note that force_not_null and force_null are handled as boolean options
 * attached to a column, not as table options.
 *
 * Note: If you are adding new option for user mapping, you need to modify
 * fileGetOptions(), which currently doesn't bother to look at user mappings.
 */
static const struct FileFdwOption valid_options[] = {
	/* File options */
	{"filename", ForeignTableRelationId},

	/* Format options */
	/* oids option is not supported */
	{"format", ForeignTableRelationId},
	{"header", ForeignTableRelationId},
	{"delimiter", ForeignTableRelationId},
	{"quote", ForeignTableRelationId},
	{"escape", ForeignTableRelationId},
	{"null", ForeignTableRelationId},
	{"encoding", ForeignTableRelationId},
	{"force_not_null", AttributeRelationId},
	{"force_null", AttributeRelationId},

	/*
	 * force_quote is not supported by file_fdw because it's for COPY TO.
	 */

	/* Sentinel */
	{NULL, InvalidOid}
};

/*
 * FDW-specific information for RelOptInfo.fdw_private.
 */
typedef struct FileFdwPlanState
{
	char	   *filename;		/* file to read */
	List	   *options;		/* merged COPY options, excluding filename */
	BlockNumber pages;			/* estimate of file's physical size */
	double		ntuples;		/* estimate of number of rows in file */
} FileFdwPlanState;

/*
 * FDW-specific information for ForeignScanState.fdw_state.
 */
typedef struct FileFdwExecutionState
{
	char	   *filename;		/* file to read */
	List	   *options;		/* merged COPY options, excluding filename */
	CopyState	cstate;			/* state of reading file */
} FileFdwExecutionState;

/*
 * SQL functions
 */
PG_FUNCTION_INFO_V1(file_fdw_handler);
PG_FUNCTION_INFO_V1(file_fdw_validator);

/*
 * FDW callback routines
 */
static void fileGetForeignRelSize(PlannerInfo *root,
					  RelOptInfo *baserel,
					  Oid foreigntableid);
static void fileGetForeignPaths(PlannerInfo *root,
					RelOptInfo *baserel,
					Oid foreigntableid);
static ForeignScan *fileGetForeignPlan(PlannerInfo *root,
				   RelOptInfo *baserel,
				   Oid foreigntableid,
				   ForeignPath *best_path,
				   List *tlist,
				   List *scan_clauses,
				   Plan *outer_plan);
static void fileExplainForeignScan(ForeignScanState *node, ExplainState *es);
static void fileBeginForeignScan(ForeignScanState *node, int eflags);
static TupleTableSlot *fileIterateForeignScan(ForeignScanState *node);
static void fileReScanForeignScan(ForeignScanState *node);
static void fileEndForeignScan(ForeignScanState *node);
static bool fileAnalyzeForeignTable(Relation relation,
						AcquireSampleRowsFunc *func,
						BlockNumber *totalpages);

/*
 * Helper functions
 */
static bool is_valid_option(const char *option, Oid context);
static void fileGetOptions(Oid foreigntableid,
			   char **filename, List **other_options);
static List *get_file_fdw_attribute_options(Oid relid);
static bool check_selective_binary_conversion(RelOptInfo *baserel,
								  Oid foreigntableid,
								  List **columns);
static void estimate_size(PlannerInfo *root, RelOptInfo *baserel,
			  FileFdwPlanState *fdw_private);
static void estimate_costs(PlannerInfo *root, RelOptInfo *baserel,
			   FileFdwPlanState *fdw_private,
			   Cost *startup_cost, Cost *total_cost);
static int file_acquire_sample_rows(Relation onerel, int elevel,
						 HeapTuple *rows, int targrows,
						 double *totalrows, double *totaldeadrows);


/*
 * Foreign-data wrapper handler function: return a struct with pointers
 * to my callback routines.
 */
Datum
file_fdw_handler(PG_FUNCTION_ARGS)
{
	FdwRoutine *fdwroutine = makeNode(FdwRoutine);

	fdwroutine->GetForeignRelSize = fileGetForeignRelSize;
	fdwroutine->GetForeignPaths = fileGetForeignPaths;
	fdwroutine->GetForeignPlan = fileGetForeignPlan;
	fdwroutine->ExplainForeignScan = fileExplainForeignScan;
	fdwroutine->BeginForeignScan = fileBeginForeignScan;
	fdwroutine->IterateForeignScan = fileIterateForeignScan;
	fdwroutine->ReScanForeignScan = fileReScanForeignScan;
	fdwroutine->EndForeignScan = fileEndForeignScan;
	fdwroutine->AnalyzeForeignTable = fileAnalyzeForeignTable;

	PG_RETURN_POINTER(fdwroutine);
}

/*
 * Validate the generic options given to a FOREIGN DATA WRAPPER, SERVER,
 * USER MAPPING or FOREIGN TABLE that uses file_fdw.
 *
 * Raise an ERROR if the option or its value is considered invalid.
 */
Datum
file_fdw_validator(PG_FUNCTION_ARGS)
{
	List	   *options_list = untransformRelOptions(PG_GETARG_DATUM(0));
	Oid			catalog = PG_GETARG_OID(1);
	char	   *filename = NULL;
	DefElem    *force_not_null = NULL;
	DefElem    *force_null = NULL;
	List	   *other_options = NIL;
	ListCell   *cell;

	/*
	 * Only superusers are allowed to set options of a file_fdw foreign table.
	 * This is because the filename is one of those options, and we don't want
	 * non-superusers to be able to determine which file gets read.
	 *
	 * Putting this sort of permissions check in a validator is a bit of a
	 * crock, but there doesn't seem to be any other place that can enforce
	 * the check more cleanly.
	 *
	 * Note that the valid_options[] array disallows setting filename at any
	 * options level other than foreign table --- otherwise there'd still be a
	 * security hole.
	 */
	if (catalog == ForeignTableRelationId && !superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("only superuser can change options of a file_fdw foreign table")));

	/*
	 * Check that only options supported by file_fdw, and allowed for the
	 * current object type, are given.
	 */
	foreach(cell, options_list)
	{
		DefElem    *def = (DefElem *) lfirst(cell);

		if (!is_valid_option(def->defname, catalog))
		{
			const struct FileFdwOption *opt;
			StringInfoData buf;

			/*
			 * Unknown option specified, complain about it. Provide a hint
			 * with list of valid options for the object.
			 */
			initStringInfo(&buf);
			for (opt = valid_options; opt->optname; opt++)
			{
				if (catalog == opt->optcontext)
					appendStringInfo(&buf, "%s%s", (buf.len > 0) ? ", " : "",
									 opt->optname);
			}

			ereport(ERROR,
					(errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
					 errmsg("invalid option \"%s\"", def->defname),
					 buf.len > 0
					 ? errhint("Valid options in this context are: %s",
							   buf.data)
				  : errhint("There are no valid options in this context.")));
		}

		/*
		 * Separate out filename and column-specific options, since
		 * ProcessCopyOptions won't accept them.
		 */

		if (strcmp(def->defname, "filename") == 0)
		{
			if (filename)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			filename = defGetString(def);
		}

		/*
		 * force_not_null is a boolean option; after validation we can discard
		 * it - it will be retrieved later in get_file_fdw_attribute_options()
		 */
		else if (strcmp(def->defname, "force_not_null") == 0)
		{
			if (force_not_null)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options"),
						 errhint("option \"force_not_null\" supplied more than once for a column")));
			force_not_null = def;
			/* Don't care what the value is, as long as it's a legal boolean */
			(void) defGetBoolean(def);
		}
		/* See comments for force_not_null above */
		else if (strcmp(def->defname, "force_null") == 0)
		{
			if (force_null)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options"),
						 errhint("option \"force_null\" supplied more than once for a column")));
			force_null = def;
			(void) defGetBoolean(def);
		}
		else
			other_options = lappend(other_options, def);
	}

	/*
	 * Now apply the core COPY code's validation logic for more checks.
	 */
	ProcessCopyOptions(NULL, true, other_options);

	/*
	 * Filename option is required for file_fdw foreign tables.
	 */
	if (catalog == ForeignTableRelationId && filename == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_DYNAMIC_PARAMETER_VALUE_NEEDED),
				 errmsg("filename is required for file_fdw foreign tables")));

	PG_RETURN_VOID();
}

/*
 * Check if the provided option is one of the valid options.
 * context is the Oid of the catalog holding the object the option is for.
 */
static bool
is_valid_option(const char *option, Oid context)
{
	const struct FileFdwOption *opt;

	for (opt = valid_options; opt->optname; opt++)
	{
		if (context == opt->optcontext && strcmp(opt->optname, option) == 0)
			return true;
	}
	return false;
}

/*
 * Fetch the options for a file_fdw foreign table.
 *
 * We have to separate out "filename" from the other options because
 * it must not appear in the options list passed to the core COPY code.
 */
static void
fileGetOptions(Oid foreigntableid,
			   char **filename, List **other_options)
{
	ForeignTable *table;
	ForeignServer *server;
	ForeignDataWrapper *wrapper;
	List	   *options;
	ListCell   *lc,
			   *prev;

	/*
	 * Extract options from FDW objects.  We ignore user mappings because
	 * file_fdw doesn't have any options that can be specified there.
	 *
	 * (XXX Actually, given the current contents of valid_options[], there's
	 * no point in examining anything except the foreign table's own options.
	 * Simplify?)
	 */
	table = GetForeignTable(foreigntableid);
	server = GetForeignServer(table->serverid);
	wrapper = GetForeignDataWrapper(server->fdwid);

	options = NIL;
	options = list_concat(options, wrapper->options);
	options = list_concat(options, server->options);
	options = list_concat(options, table->options);
	options = list_concat(options, get_file_fdw_attribute_options(foreigntableid));

	/*
	 * Separate out the filename.
	 */
	*filename = NULL;
	prev = NULL;
	foreach(lc, options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "filename") == 0)
		{
			*filename = defGetString(def);
			options = list_delete_cell(options, lc, prev);
			break;
		}
		prev = lc;
	}

	/*
	 * The validator should have checked that a filename was included in the
	 * options, but check again, just in case.
	 */
	if (*filename == NULL)
		elog(ERROR, "filename is required for file_fdw foreign tables");

	*other_options = options;
}

/*
 * Retrieve per-column generic options from pg_attribute and construct a list
 * of DefElems representing them.
 *
 * At the moment we only have "force_not_null", and "force_null",
 * which should each be combined into a single DefElem listing all such
 * columns, since that's what COPY expects.
 */
static List *
get_file_fdw_attribute_options(Oid relid)
{
	Relation	rel;
	TupleDesc	tupleDesc;
	AttrNumber	natts;
	AttrNumber	attnum;
	List	   *fnncolumns = NIL;
	List	   *fncolumns = NIL;

	List	   *options = NIL;

	rel = heap_open(relid, AccessShareLock);
	tupleDesc = RelationGetDescr(rel);
	natts = tupleDesc->natts;

	/* Retrieve FDW options for all user-defined attributes. */
	for (attnum = 1; attnum <= natts; attnum++)
	{
		Form_pg_attribute attr = tupleDesc->attrs[attnum - 1];
		List	   *options;
		ListCell   *lc;

		/* Skip dropped attributes. */
		if (attr->attisdropped)
			continue;

		options = GetForeignColumnOptions(relid, attnum);
		foreach(lc, options)
		{
			DefElem    *def = (DefElem *) lfirst(lc);

			if (strcmp(def->defname, "force_not_null") == 0)
			{
				if (defGetBoolean(def))
				{
					char	   *attname = pstrdup(NameStr(attr->attname));

					fnncolumns = lappend(fnncolumns, makeString(attname));
				}
			}
			else if (strcmp(def->defname, "force_null") == 0)
			{
				if (defGetBoolean(def))
				{
					char	   *attname = pstrdup(NameStr(attr->attname));

					fncolumns = lappend(fncolumns, makeString(attname));
				}
			}
			/* maybe in future handle other options here */
		}
	}

	heap_close(rel, AccessShareLock);

	/*
	 * Return DefElem only when some column(s) have force_not_null /
	 * force_null options set
	 */
	if (fnncolumns != NIL)
		options = lappend(options, makeDefElem("force_not_null", (Node *) fnncolumns));

	if (fncolumns != NIL)
		options = lappend(options, makeDefElem("force_null", (Node *) fncolumns));

	return options;
}

/*
 * fileGetForeignRelSize
 *		Obtain relation size estimates for a foreign table
 */
static void
fileGetForeignRelSize(PlannerInfo *root,
					  RelOptInfo *baserel,
					  Oid foreigntableid)
{
	FileFdwPlanState *fdw_private;

	/*
	 * Fetch options.  We only need filename at this point, but we might as
	 * well get everything and not need to re-fetch it later in planning.
	 */
	fdw_private = (FileFdwPlanState *) palloc(sizeof(FileFdwPlanState));
	fileGetOptions(foreigntableid,
				   &fdw_private->filename, &fdw_private->options);
	baserel->fdw_private = (void *) fdw_private;

	/* Estimate relation size */
	estimate_size(root, baserel, fdw_private);
}

/*
 * fileGetForeignPaths
 *		Create possible access paths for a scan on the foreign table
 *
 *		Currently we don't support any push-down feature, so there is only one
 *		possible access path, which simply returns all records in the order in
 *		the data file.
 */
static void
fileGetForeignPaths(PlannerInfo *root,
					RelOptInfo *baserel,
					Oid foreigntableid)
{
	FileFdwPlanState *fdw_private = (FileFdwPlanState *) baserel->fdw_private;
	Cost		startup_cost;
	Cost		total_cost;
	List	   *columns;
	List	   *coptions = NIL;

	/* Decide whether to selectively perform binary conversion */
	if (check_selective_binary_conversion(baserel,
										  foreigntableid,
										  &columns))
		coptions = list_make1(makeDefElem("convert_selectively",
										  (Node *) columns));

	/* Estimate costs */
	estimate_costs(root, baserel, fdw_private,
				   &startup_cost, &total_cost);

	/*
	 * Create a ForeignPath node and add it as only possible path.  We use the
	 * fdw_private list of the path to carry the convert_selectively option;
	 * it will be propagated into the fdw_private list of the Plan node.
	 */
	add_path(baserel, (Path *)
			 create_foreignscan_path(root, baserel,
									 baserel->rows,
									 startup_cost,
									 total_cost,
									 NIL,		/* no pathkeys */
									 NULL,		/* no outer rel either */
									 NULL,		/* no extra plan */
									 coptions));

	/*
	 * If data file was sorted, and we knew it somehow, we could insert
	 * appropriate pathkeys into the ForeignPath node to tell the planner
	 * that.
	 */
}

/*
 * fileGetForeignPlan
 *		Create a ForeignScan plan node for scanning the foreign table
 */
static ForeignScan *
fileGetForeignPlan(PlannerInfo *root,
				   RelOptInfo *baserel,
				   Oid foreigntableid,
				   ForeignPath *best_path,
				   List *tlist,
				   List *scan_clauses,
				   Plan *outer_plan)
{
	Index		scan_relid = baserel->relid;

	/*
	 * We have no native ability to evaluate restriction clauses, so we just
	 * put all the scan_clauses into the plan node's qual list for the
	 * executor to check.  So all we have to do here is strip RestrictInfo
	 * nodes from the clauses and ignore pseudoconstants (which will be
	 * handled elsewhere).
	 */
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	/* Create the ForeignScan node */
	return make_foreignscan(tlist,
							scan_clauses,
							scan_relid,
							NIL,	/* no expressions to evaluate */
							best_path->fdw_private,
							NIL,	/* no custom tlist */
							NIL,	/* no remote quals */
							outer_plan);
}

/*
 * fileExplainForeignScan
 *		Produce extra output for EXPLAIN
 */
static void
fileExplainForeignScan(ForeignScanState *node, ExplainState *es)
{
	char	   *filename;
	List	   *options;

	/* Fetch options --- we only need filename at this point */
	fileGetOptions(RelationGetRelid(node->ss.ss_currentRelation),
				   &filename, &options);

	ExplainPropertyText("Foreign File", filename, es);

	/* Suppress file size if we're not showing cost details */
	if (es->costs)
	{
		struct stat stat_buf;

		if (stat(filename, &stat_buf) == 0)
			ExplainPropertyLong("Foreign File Size", (long) stat_buf.st_size,
								es);
	}
}

/*
 * fileBeginForeignScan
 *		Initiate access to the file by creating CopyState
 */
static void
fileBeginForeignScan(ForeignScanState *node, int eflags)
{
	ForeignScan *plan = (ForeignScan *) node->ss.ps.plan;
	char	   *filename;
	List	   *options;
	CopyState	cstate;
	FileFdwExecutionState *festate;

	/*
	 * Do nothing in EXPLAIN (no ANALYZE) case.  node->fdw_state stays NULL.
	 */
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	/* Fetch options of foreign table */
	fileGetOptions(RelationGetRelid(node->ss.ss_currentRelation),
				   &filename, &options);

	/* Add any options from the plan (currently only convert_selectively) */
	options = list_concat(options, plan->fdw_private);

	/*
	 * Create CopyState from FDW options.  We always acquire all columns, so
	 * as to match the expected ScanTupleSlot signature.
	 */
	cstate = BeginCopyFrom(node->ss.ss_currentRelation,
						   filename,
						   false,
						   NIL,
						   options);

	/*
	 * Save state in node->fdw_state.  We must save enough information to call
	 * BeginCopyFrom() again.
	 */
	festate = (FileFdwExecutionState *) palloc(sizeof(FileFdwExecutionState));
	festate->filename = filename;
	festate->options = options;
	festate->cstate = cstate;

	node->fdw_state = (void *) festate;
}

/*
 * fileIterateForeignScan
 *		Read next record from the data file and store it into the
 *		ScanTupleSlot as a virtual tuple
 */
static TupleTableSlot *
fileIterateForeignScan(ForeignScanState *node)
{
	FileFdwExecutionState *festate = (FileFdwExecutionState *) node->fdw_state;
	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
	bool		found;
	ErrorContextCallback errcallback;

	/* Set up callback to identify error line number. */
	errcallback.callback = CopyFromErrorCallback;
	errcallback.arg = (void *) festate->cstate;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/*
	 * The protocol for loading a virtual tuple into a slot is first
	 * ExecClearTuple, then fill the values/isnull arrays, then
	 * ExecStoreVirtualTuple.  If we don't find another row in the file, we
	 * just skip the last step, leaving the slot empty as required.
	 *
	 * We can pass ExprContext = NULL because we read all columns from the
	 * file, so no need to evaluate default expressions.
	 *
	 * We can also pass tupleOid = NULL because we don't allow oids for
	 * foreign tables.
	 */
	ExecClearTuple(slot);
	found = NextCopyFrom(festate->cstate, NULL,
						 slot->tts_values, slot->tts_isnull,
						 NULL);
	if (found)
		ExecStoreVirtualTuple(slot);

	/* Remove error callback. */
	error_context_stack = errcallback.previous;

	return slot;
}

/*
 * fileReScanForeignScan
 *		Rescan table, possibly with new parameters
 */
static void
fileReScanForeignScan(ForeignScanState *node)
{
	FileFdwExecutionState *festate = (FileFdwExecutionState *) node->fdw_state;

	EndCopyFrom(festate->cstate);

	festate->cstate = BeginCopyFrom(node->ss.ss_currentRelation,
									festate->filename,
									false,
									NIL,
									festate->options);
}

/*
 * fileEndForeignScan
 *		Finish scanning foreign table and dispose objects used for this scan
 */
static void
fileEndForeignScan(ForeignScanState *node)
{
	FileFdwExecutionState *festate = (FileFdwExecutionState *) node->fdw_state;

	/* if festate is NULL, we are in EXPLAIN; nothing to do */
	if (festate)
		EndCopyFrom(festate->cstate);
}

/*
 * fileAnalyzeForeignTable
 *		Test whether analyzing this foreign table is supported
 */
static bool
fileAnalyzeForeignTable(Relation relation,
						AcquireSampleRowsFunc *func,
						BlockNumber *totalpages)
{
	char	   *filename;
	List	   *options;
	struct stat stat_buf;

	/* Fetch options of foreign table */
	fileGetOptions(RelationGetRelid(relation), &filename, &options);

	/*
	 * Get size of the file.  (XXX if we fail here, would it be better to just
	 * return false to skip analyzing the table?)
	 */
	if (stat(filename, &stat_buf) < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not stat file \"%s\": %m",
						filename)));

	/*
	 * Convert size to pages.  Must return at least 1 so that we can tell
	 * later on that pg_class.relpages is not default.
	 */
	*totalpages = (stat_buf.st_size + (BLCKSZ - 1)) / BLCKSZ;
	if (*totalpages < 1)
		*totalpages = 1;

	*func = file_acquire_sample_rows;

	return true;
}

/*
 * check_selective_binary_conversion
 *
 * Check to see if it's useful to convert only a subset of the file's columns
 * to binary.  If so, construct a list of the column names to be converted,
 * return that at *columns, and return TRUE.  (Note that it's possible to
 * determine that no columns need be converted, for instance with a COUNT(*)
 * query.  So we can't use returning a NIL list to indicate failure.)
 */
static bool
check_selective_binary_conversion(RelOptInfo *baserel,
								  Oid foreigntableid,
								  List **columns)
{
	ForeignTable *table;
	ListCell   *lc;
	Relation	rel;
	TupleDesc	tupleDesc;
	AttrNumber	attnum;
	Bitmapset  *attrs_used = NULL;
	bool		has_wholerow = false;
	int			numattrs;
	int			i;

	*columns = NIL;				/* default result */

	/*
	 * Check format of the file.  If binary format, this is irrelevant.
	 */
	table = GetForeignTable(foreigntableid);
	foreach(lc, table->options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "format") == 0)
		{
			char	   *format = defGetString(def);

			if (strcmp(format, "binary") == 0)
				return false;
			break;
		}
	}

	/* Collect all the attributes needed for joins or final output. */
	pull_varattnos((Node *) baserel->reltargetlist, baserel->relid,
				   &attrs_used);

	/* Add all the attributes used by restriction clauses. */
	foreach(lc, baserel->baserestrictinfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		pull_varattnos((Node *) rinfo->clause, baserel->relid,
					   &attrs_used);
	}

	/* Convert attribute numbers to column names. */
	rel = heap_open(foreigntableid, AccessShareLock);
	tupleDesc = RelationGetDescr(rel);

	while ((attnum = bms_first_member(attrs_used)) >= 0)
	{
		/* Adjust for system attributes. */
		attnum += FirstLowInvalidHeapAttributeNumber;

		if (attnum == 0)
		{
			has_wholerow = true;
			break;
		}

		/* Ignore system attributes. */
		if (attnum < 0)
			continue;

		/* Get user attributes. */
		if (attnum > 0)
		{
			Form_pg_attribute attr = tupleDesc->attrs[attnum - 1];
			char	   *attname = NameStr(attr->attname);

			/* Skip dropped attributes (probably shouldn't see any here). */
			if (attr->attisdropped)
				continue;
			*columns = lappend(*columns, makeString(pstrdup(attname)));
		}
	}

	/* Count non-dropped user attributes while we have the tupdesc. */
	numattrs = 0;
	for (i = 0; i < tupleDesc->natts; i++)
	{
		Form_pg_attribute attr = tupleDesc->attrs[i];

		if (attr->attisdropped)
			continue;
		numattrs++;
	}

	heap_close(rel, AccessShareLock);

	/* If there's a whole-row reference, fail: we need all the columns. */
	if (has_wholerow)
	{
		*columns = NIL;
		return false;
	}

	/* If all the user attributes are needed, fail. */
	if (numattrs == list_length(*columns))
	{
		*columns = NIL;
		return false;
	}

	return true;
}

/*
 * Estimate size of a foreign table.
 *
 * The main result is returned in baserel->rows.  We also set
 * fdw_private->pages and fdw_private->ntuples for later use in the cost
 * calculation.
 */
static void
estimate_size(PlannerInfo *root, RelOptInfo *baserel,
			  FileFdwPlanState *fdw_private)
{
	struct stat stat_buf;
	BlockNumber pages;
	double		ntuples;
	double		nrows;

	/*
	 * Get size of the file.  It might not be there at plan time, though, in
	 * which case we have to use a default estimate.
	 */
	if (stat(fdw_private->filename, &stat_buf) < 0)
		stat_buf.st_size = 10 * BLCKSZ;

	/*
	 * Convert size to pages for use in I/O cost estimate later.
	 */
	pages = (stat_buf.st_size + (BLCKSZ - 1)) / BLCKSZ;
	if (pages < 1)
		pages = 1;
	fdw_private->pages = pages;

	/*
	 * Estimate the number of tuples in the file.
	 */
	if (baserel->pages > 0)
	{
		/*
		 * We have # of pages and # of tuples from pg_class (that is, from a
		 * previous ANALYZE), so compute a tuples-per-page estimate and scale
		 * that by the current file size.
		 */
		double		density;

		density = baserel->tuples / (double) baserel->pages;
		ntuples = clamp_row_est(density * (double) pages);
	}
	else
	{
		/*
		 * Otherwise we have to fake it.  We back into this estimate using the
		 * planner's idea of the relation width; which is bogus if not all
		 * columns are being read, not to mention that the text representation
		 * of a row probably isn't the same size as its internal
		 * representation.  Possibly we could do something better, but the
		 * real answer to anyone who complains is "ANALYZE" ...
		 */
		int			tuple_width;

		tuple_width = MAXALIGN(baserel->width) +
			MAXALIGN(SizeofHeapTupleHeader);
		ntuples = clamp_row_est((double) stat_buf.st_size /
								(double) tuple_width);
	}
	fdw_private->ntuples = ntuples;

	/*
	 * Now estimate the number of rows returned by the scan after applying the
	 * baserestrictinfo quals.
	 */
	nrows = ntuples *
		clauselist_selectivity(root,
							   baserel->baserestrictinfo,
							   0,
							   JOIN_INNER,
							   NULL);

	nrows = clamp_row_est(nrows);

	/* Save the output-rows estimate for the planner */
	baserel->rows = nrows;
}

/*
 * Estimate costs of scanning a foreign table.
 *
 * Results are returned in *startup_cost and *total_cost.
 */
static void
estimate_costs(PlannerInfo *root, RelOptInfo *baserel,
			   FileFdwPlanState *fdw_private,
			   Cost *startup_cost, Cost *total_cost)
{
	BlockNumber pages = fdw_private->pages;
	double		ntuples = fdw_private->ntuples;
	Cost		run_cost = 0;
	Cost		cpu_per_tuple;

	/*
	 * We estimate costs almost the same way as cost_seqscan(), thus assuming
	 * that I/O costs are equivalent to a regular table file of the same size.
	 * However, we take per-tuple CPU costs as 10x of a seqscan, to account
	 * for the cost of parsing records.
	 */
	run_cost += seq_page_cost * pages;

	*startup_cost = baserel->baserestrictcost.startup;
	cpu_per_tuple = cpu_tuple_cost * 10 + baserel->baserestrictcost.per_tuple;
	run_cost += cpu_per_tuple * ntuples;
	*total_cost = *startup_cost + run_cost;
}

/*
 * file_acquire_sample_rows -- acquire a random sample of rows from the table
 *
 * Selected rows are returned in the caller-allocated array rows[],
 * which must have at least targrows entries.
 * The actual number of rows selected is returned as the function result.
 * We also count the total number of rows in the file and return it into
 * *totalrows.  Note that *totaldeadrows is always set to 0.
 *
 * Note that the returned list of rows is not always in order by physical
 * position in the file.  Therefore, correlation estimates derived later
 * may be meaningless, but it's OK because we don't use the estimates
 * currently (the planner only pays attention to correlation for indexscans).
 */
static int
file_acquire_sample_rows(Relation onerel, int elevel,
						 HeapTuple *rows, int targrows,
						 double *totalrows, double *totaldeadrows)
{
	int			numrows = 0;
	double		rowstoskip = -1;	/* -1 means not set yet */
	ReservoirStateData rstate;
	TupleDesc	tupDesc;
	Datum	   *values;
	bool	   *nulls;
	bool		found;
	char	   *filename;
	List	   *options;
	CopyState	cstate;
	ErrorContextCallback errcallback;
	MemoryContext oldcontext = CurrentMemoryContext;
	MemoryContext tupcontext;

	Assert(onerel);
	Assert(targrows > 0);

	tupDesc = RelationGetDescr(onerel);
	values = (Datum *) palloc(tupDesc->natts * sizeof(Datum));
	nulls = (bool *) palloc(tupDesc->natts * sizeof(bool));

	/* Fetch options of foreign table */
	fileGetOptions(RelationGetRelid(onerel), &filename, &options);

	/*
	 * Create CopyState from FDW options.
	 */
	cstate = BeginCopyFrom(onerel, filename, false, NIL, options);

	/*
	 * Use per-tuple memory context to prevent leak of memory used to read
	 * rows from the file with Copy routines.
	 */
	tupcontext = AllocSetContextCreate(CurrentMemoryContext,
									   "file_fdw temporary context",
									   ALLOCSET_DEFAULT_MINSIZE,
									   ALLOCSET_DEFAULT_INITSIZE,
									   ALLOCSET_DEFAULT_MAXSIZE);

	/* Prepare for sampling rows */
	reservoir_init_selection_state(&rstate, targrows);

	/* Set up callback to identify error line number. */
	errcallback.callback = CopyFromErrorCallback;
	errcallback.arg = (void *) cstate;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	*totalrows = 0;
	*totaldeadrows = 0;
	for (;;)
	{
		/* Check for user-requested abort or sleep */
		vacuum_delay_point();

		/* Fetch next row */
		MemoryContextReset(tupcontext);
		MemoryContextSwitchTo(tupcontext);

		found = NextCopyFrom(cstate, NULL, values, nulls, NULL);

		MemoryContextSwitchTo(oldcontext);

		if (!found)
			break;

		/*
		 * The first targrows sample rows are simply copied into the
		 * reservoir.  Then we start replacing tuples in the sample until we
		 * reach the end of the relation. This algorithm is from Jeff Vitter's
		 * paper (see more info in commands/analyze.c).
		 */
		if (numrows < targrows)
		{
			rows[numrows++] = heap_form_tuple(tupDesc, values, nulls);
		}
		else
		{
			/*
			 * t in Vitter's paper is the number of records already processed.
			 * If we need to compute a new S value, we must use the
			 * not-yet-incremented value of totalrows as t.
			 */
			if (rowstoskip < 0)
				rowstoskip = reservoir_get_next_S(&rstate, *totalrows, targrows);

			if (rowstoskip <= 0)
			{
				/*
				 * Found a suitable tuple, so save it, replacing one old tuple
				 * at random
				 */
				int			k = (int) (targrows * sampler_random_fract(rstate.randstate));

				Assert(k >= 0 && k < targrows);
				heap_freetuple(rows[k]);
				rows[k] = heap_form_tuple(tupDesc, values, nulls);
			}

			rowstoskip -= 1;
		}

		*totalrows += 1;
	}

	/* Remove error callback. */
	error_context_stack = errcallback.previous;

	/* Clean up. */
	MemoryContextDelete(tupcontext);

	EndCopyFrom(cstate);

	pfree(values);
	pfree(nulls);

	/*
	 * Emit some interesting relation info
	 */
	ereport(elevel,
			(errmsg("\"%s\": file contains %.0f rows; "
					"%d rows in sample",
					RelationGetRelationName(onerel),
					*totalrows, numrows)));

	return numrows;
}

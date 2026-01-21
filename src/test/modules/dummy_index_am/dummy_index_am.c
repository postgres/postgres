/*-------------------------------------------------------------------------
 *
 * dummy_index_am.c
 *		Index AM template main file.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/test/modules/dummy_index_am/dummy_index_am.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/amapi.h"
#include "access/reloptions.h"
#include "catalog/index.h"
#include "commands/vacuum.h"
#include "nodes/pathnodes.h"

PG_MODULE_MAGIC;

/* parse table for fillRelOptions */
static relopt_parse_elt di_relopt_tab[8];

/* Kind of relation options for dummy index */
static relopt_kind di_relopt_kind;

typedef enum DummyAmEnum
{
	DUMMY_AM_ENUM_ONE,
	DUMMY_AM_ENUM_TWO,
}			DummyAmEnum;

/* Dummy index options */
typedef struct DummyIndexOptions
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	int			option_int;
	double		option_real;
	bool		option_bool;
	pg_ternary	option_ternary_1;
	DummyAmEnum option_enum;
	int			option_string_val_offset;
	int			option_string_null_offset;
}			DummyIndexOptions;

static relopt_enum_elt_def dummyAmEnumValues[] =
{
	{"one", DUMMY_AM_ENUM_ONE},
	{"two", DUMMY_AM_ENUM_TWO},
	{(const char *) NULL}		/* list terminator */
};

/* Handler for index AM */
PG_FUNCTION_INFO_V1(dihandler);

/*
 * Validation function for string relation options.
 */
static void
validate_string_option(const char *value)
{
	ereport(NOTICE,
			(errmsg("new option value for string parameter %s",
					value ? value : "NULL")));
}

/*
 * This function creates a full set of relation option types,
 * with various patterns.
 */
static void
create_reloptions_table(void)
{
	int			i = 0;

	di_relopt_kind = add_reloption_kind();

	add_int_reloption(di_relopt_kind, "option_int",
					  "Integer option for dummy_index_am",
					  10, -10, 100, AccessExclusiveLock);
	di_relopt_tab[i].optname = "option_int";
	di_relopt_tab[i].opttype = RELOPT_TYPE_INT;
	di_relopt_tab[i].offset = offsetof(DummyIndexOptions, option_int);
	i++;

	add_real_reloption(di_relopt_kind, "option_real",
					   "Real option for dummy_index_am",
					   3.1415, -10, 100, AccessExclusiveLock);
	di_relopt_tab[i].optname = "option_real";
	di_relopt_tab[i].opttype = RELOPT_TYPE_REAL;
	di_relopt_tab[i].offset = offsetof(DummyIndexOptions, option_real);
	i++;

	add_bool_reloption(di_relopt_kind, "option_bool",
					   "Boolean option for dummy_index_am",
					   true, AccessExclusiveLock);
	di_relopt_tab[i].optname = "option_bool";
	di_relopt_tab[i].opttype = RELOPT_TYPE_BOOL;
	di_relopt_tab[i].offset = offsetof(DummyIndexOptions, option_bool);
	i++;

	add_ternary_reloption(di_relopt_kind, "option_ternary_1",
						  "One ternary option for dummy_index_am",
						  AccessExclusiveLock);
	di_relopt_tab[i].optname = "option_ternary_1";
	di_relopt_tab[i].opttype = RELOPT_TYPE_TERNARY;
	di_relopt_tab[i].offset = offsetof(DummyIndexOptions, option_ternary_1);
	i++;

	add_enum_reloption(di_relopt_kind, "option_enum",
					   "Enum option for dummy_index_am",
					   dummyAmEnumValues,
					   DUMMY_AM_ENUM_ONE,
					   "Valid values are \"one\" and \"two\".",
					   AccessExclusiveLock);
	di_relopt_tab[i].optname = "option_enum";
	di_relopt_tab[i].opttype = RELOPT_TYPE_ENUM;
	di_relopt_tab[i].offset = offsetof(DummyIndexOptions, option_enum);
	i++;

	add_string_reloption(di_relopt_kind, "option_string_val",
						 "String option for dummy_index_am with non-NULL default",
						 "DefaultValue", &validate_string_option,
						 AccessExclusiveLock);
	di_relopt_tab[i].optname = "option_string_val";
	di_relopt_tab[i].opttype = RELOPT_TYPE_STRING;
	di_relopt_tab[i].offset = offsetof(DummyIndexOptions,
									   option_string_val_offset);
	i++;

	/*
	 * String option for dummy_index_am with NULL default, and without
	 * description.
	 */
	add_string_reloption(di_relopt_kind, "option_string_null",
						 NULL,	/* description */
						 NULL, &validate_string_option,
						 AccessExclusiveLock);
	di_relopt_tab[i].optname = "option_string_null";
	di_relopt_tab[i].opttype = RELOPT_TYPE_STRING;
	di_relopt_tab[i].offset = offsetof(DummyIndexOptions,
									   option_string_null_offset);
	i++;
}


/*
 * Build a new index.
 */
static IndexBuildResult *
dibuild(Relation heap, Relation index, IndexInfo *indexInfo)
{
	IndexBuildResult *result;

	result = palloc_object(IndexBuildResult);

	/* let's pretend that no tuples were scanned */
	result->heap_tuples = 0;
	/* and no index tuples were created (that is true) */
	result->index_tuples = 0;

	return result;
}

/*
 * Build an empty index for the initialization fork.
 */
static void
dibuildempty(Relation index)
{
	/* No need to build an init fork for a dummy index */
}

/*
 * Insert new tuple to index AM.
 */
static bool
diinsert(Relation index, Datum *values, bool *isnull,
		 ItemPointer ht_ctid, Relation heapRel,
		 IndexUniqueCheck checkUnique,
		 bool indexUnchanged,
		 IndexInfo *indexInfo)
{
	/* nothing to do */
	return false;
}

/*
 * Bulk deletion of all index entries pointing to a set of table tuples.
 */
static IndexBulkDeleteResult *
dibulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
			 IndexBulkDeleteCallback callback, void *callback_state)
{
	/*
	 * There is nothing to delete.  Return NULL as there is nothing to pass to
	 * amvacuumcleanup.
	 */
	return NULL;
}

/*
 * Post-VACUUM cleanup for index AM.
 */
static IndexBulkDeleteResult *
divacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	/* Index has not been modified, so returning NULL is fine */
	return NULL;
}

/*
 * Estimate cost of index AM.
 */
static void
dicostestimate(PlannerInfo *root, IndexPath *path, double loop_count,
			   Cost *indexStartupCost, Cost *indexTotalCost,
			   Selectivity *indexSelectivity, double *indexCorrelation,
			   double *indexPages)
{
	/* Tell planner to never use this index! */
	*indexStartupCost = 1.0e10;
	*indexTotalCost = 1.0e10;

	/* Do not care about the rest */
	*indexSelectivity = 1;
	*indexCorrelation = 0;
	*indexPages = 1;
}

/*
 * Parse relation options for index AM, returning a DummyIndexOptions
 * structure filled with option values.
 */
static bytea *
dioptions(Datum reloptions, bool validate)
{
	return (bytea *) build_reloptions(reloptions, validate,
									  di_relopt_kind,
									  sizeof(DummyIndexOptions),
									  di_relopt_tab, lengthof(di_relopt_tab));
}

/*
 * Validator for index AM.
 */
static bool
divalidate(Oid opclassoid)
{
	/* Index is dummy so we are happy with any opclass */
	return true;
}

/*
 * Begin scan of index AM.
 */
static IndexScanDesc
dibeginscan(Relation r, int nkeys, int norderbys)
{
	IndexScanDesc scan;

	/* Let's pretend we are doing something */
	scan = RelationGetIndexScan(r, nkeys, norderbys);
	return scan;
}

/*
 * Rescan of index AM.
 */
static void
direscan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
		 ScanKey orderbys, int norderbys)
{
	/* nothing to do */
}

/*
 * End scan of index AM.
 */
static void
diendscan(IndexScanDesc scan)
{
	/* nothing to do */
}

/*
 * Index AM handler function: returns IndexAmRoutine with access method
 * parameters and callbacks.
 */
Datum
dihandler(PG_FUNCTION_ARGS)
{
	static const IndexAmRoutine amroutine = {
		.type = T_IndexAmRoutine,
		.amstrategies = 0,
		.amsupport = 1,
		.amcanorder = false,
		.amcanorderbyop = false,
		.amcanhash = false,
		.amconsistentequality = false,
		.amconsistentordering = false,
		.amcanbackward = false,
		.amcanunique = false,
		.amcanmulticol = false,
		.amoptionalkey = false,
		.amsearcharray = false,
		.amsearchnulls = false,
		.amstorage = false,
		.amclusterable = false,
		.ampredlocks = false,
		.amcanparallel = false,
		.amcanbuildparallel = false,
		.amcaninclude = false,
		.amusemaintenanceworkmem = false,
		.amsummarizing = false,
		.amparallelvacuumoptions = VACUUM_OPTION_NO_PARALLEL,
		.amkeytype = InvalidOid,

		.ambuild = dibuild,
		.ambuildempty = dibuildempty,
		.aminsert = diinsert,
		.ambulkdelete = dibulkdelete,
		.amvacuumcleanup = divacuumcleanup,
		.amcanreturn = NULL,
		.amcostestimate = dicostestimate,
		.amgettreeheight = NULL,
		.amoptions = dioptions,
		.amproperty = NULL,
		.ambuildphasename = NULL,
		.amvalidate = divalidate,
		.ambeginscan = dibeginscan,
		.amrescan = direscan,
		.amgettuple = NULL,
		.amgetbitmap = NULL,
		.amendscan = diendscan,
		.ammarkpos = NULL,
		.amrestrpos = NULL,
		.amestimateparallelscan = NULL,
		.aminitparallelscan = NULL,
		.amparallelrescan = NULL,
	};

	PG_RETURN_POINTER(&amroutine);
}

void
_PG_init(void)
{
	create_reloptions_table();
}

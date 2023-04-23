/*-------------------------------------------------------------------------
 *
 * reloptions.c
 *	  Core support for relation options (pg_class.reloptions)
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/common/reloptions.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <float.h>

#include "access/gist_private.h"
#include "access/hash.h"
#include "access/heaptoast.h"
#include "access/htup_details.h"
#include "access/nbtree.h"
#include "access/reloptions.h"
#include "access/spgist_private.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/tablespace.h"
#include "commands/view.h"
#include "nodes/makefuncs.h"
#include "postmaster/postmaster.h"
#include "utils/array.h"
#include "utils/attoptcache.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/rel.h"

/*
 * Contents of pg_class.reloptions
 *
 * To add an option:
 *
 * (i) decide on a type (integer, real, bool, string), name, default value,
 * upper and lower bounds (if applicable); for strings, consider a validation
 * routine.
 * (ii) add a record below (or use add_<type>_reloption).
 * (iii) add it to the appropriate options struct (perhaps StdRdOptions)
 * (iv) add it to the appropriate handling routine (perhaps
 * default_reloptions)
 * (v) make sure the lock level is set correctly for that operation
 * (vi) don't forget to document the option
 *
 * The default choice for any new option should be AccessExclusiveLock.
 * In some cases the lock level can be reduced from there, but the lock
 * level chosen should always conflict with itself to ensure that multiple
 * changes aren't lost when we attempt concurrent changes.
 * The choice of lock level depends completely upon how that parameter
 * is used within the server, not upon how and when you'd like to change it.
 * Safety first. Existing choices are documented here, and elsewhere in
 * backend code where the parameters are used.
 *
 * In general, anything that affects the results obtained from a SELECT must be
 * protected by AccessExclusiveLock.
 *
 * Autovacuum related parameters can be set at ShareUpdateExclusiveLock
 * since they are only used by the AV procs and don't change anything
 * currently executing.
 *
 * Fillfactor can be set because it applies only to subsequent changes made to
 * data blocks, as documented in hio.c
 *
 * n_distinct options can be set at ShareUpdateExclusiveLock because they
 * are only used during ANALYZE, which uses a ShareUpdateExclusiveLock,
 * so the ANALYZE will not be affected by in-flight changes. Changing those
 * values has no effect until the next ANALYZE, so no need for stronger lock.
 *
 * Planner-related parameters can be set with ShareUpdateExclusiveLock because
 * they only affect planning and not the correctness of the execution. Plans
 * cannot be changed in mid-flight, so changes here could not easily result in
 * new improved plans in any case. So we allow existing queries to continue
 * and existing plans to survive, a small price to pay for allowing better
 * plans to be introduced concurrently without interfering with users.
 *
 * Setting parallel_workers is safe, since it acts the same as
 * max_parallel_workers_per_gather which is a USERSET parameter that doesn't
 * affect existing plans or queries.
 *
 * vacuum_truncate can be set at ShareUpdateExclusiveLock because it
 * is only used during VACUUM, which uses a ShareUpdateExclusiveLock,
 * so the VACUUM will not be affected by in-flight changes. Changing its
 * value has no effect until the next VACUUM, so no need for stronger lock.
 */

static relopt_bool boolRelOpts[] =
{
	{
		{
			"autosummarize",
			"Enables automatic summarization on this BRIN index",
			RELOPT_KIND_BRIN,
			AccessExclusiveLock
		},
		false
	},
	{
		{
			"autovacuum_enabled",
			"Enables autovacuum in this relation",
			RELOPT_KIND_HEAP | RELOPT_KIND_TOAST,
			ShareUpdateExclusiveLock
		},
		true
	},
	{
		{
			"user_catalog_table",
			"Declare a table as an additional catalog table, e.g. for the purpose of logical replication",
			RELOPT_KIND_HEAP,
			AccessExclusiveLock
		},
		false
	},
	{
		{
			"fastupdate",
			"Enables \"fast update\" feature for this GIN index",
			RELOPT_KIND_GIN,
			AccessExclusiveLock
		},
		true
	},
	{
		{
			"security_barrier",
			"View acts as a row security barrier",
			RELOPT_KIND_VIEW,
			AccessExclusiveLock
		},
		false
	},
	{
		{
			"vacuum_index_cleanup",
			"Enables index vacuuming and index cleanup",
			RELOPT_KIND_HEAP | RELOPT_KIND_TOAST,
			ShareUpdateExclusiveLock
		},
		true
	},
	{
		{
			"vacuum_truncate",
			"Enables vacuum to truncate empty pages at the end of this table",
			RELOPT_KIND_HEAP | RELOPT_KIND_TOAST,
			ShareUpdateExclusiveLock
		},
		true
	},
	{
		{
			"deduplicate_items",
			"Enables \"deduplicate items\" feature for this btree index",
			RELOPT_KIND_BTREE,
			ShareUpdateExclusiveLock	/* since it applies only to later
										 * inserts */
		},
		true
	},
	/* list terminator */
	{{NULL}}
};

static relopt_int intRelOpts[] =
{
	{
		{
			"fillfactor",
			"Packs table pages only to this percentage",
			RELOPT_KIND_HEAP,
			ShareUpdateExclusiveLock	/* since it applies only to later
										 * inserts */
		},
		HEAP_DEFAULT_FILLFACTOR, HEAP_MIN_FILLFACTOR, 100
	},
	{
		{
			"fillfactor",
			"Packs btree index pages only to this percentage",
			RELOPT_KIND_BTREE,
			ShareUpdateExclusiveLock	/* since it applies only to later
										 * inserts */
		},
		BTREE_DEFAULT_FILLFACTOR, BTREE_MIN_FILLFACTOR, 100
	},
	{
		{
			"fillfactor",
			"Packs hash index pages only to this percentage",
			RELOPT_KIND_HASH,
			ShareUpdateExclusiveLock	/* since it applies only to later
										 * inserts */
		},
		HASH_DEFAULT_FILLFACTOR, HASH_MIN_FILLFACTOR, 100
	},
	{
		{
			"fillfactor",
			"Packs gist index pages only to this percentage",
			RELOPT_KIND_GIST,
			ShareUpdateExclusiveLock	/* since it applies only to later
										 * inserts */
		},
		GIST_DEFAULT_FILLFACTOR, GIST_MIN_FILLFACTOR, 100
	},
	{
		{
			"fillfactor",
			"Packs spgist index pages only to this percentage",
			RELOPT_KIND_SPGIST,
			ShareUpdateExclusiveLock	/* since it applies only to later
										 * inserts */
		},
		SPGIST_DEFAULT_FILLFACTOR, SPGIST_MIN_FILLFACTOR, 100
	},
	{
		{
			"autovacuum_vacuum_threshold",
			"Minimum number of tuple updates or deletes prior to vacuum",
			RELOPT_KIND_HEAP | RELOPT_KIND_TOAST,
			ShareUpdateExclusiveLock
		},
		-1, 0, INT_MAX
	},
	{
		{
			"autovacuum_vacuum_insert_threshold",
			"Minimum number of tuple inserts prior to vacuum, or -1 to disable insert vacuums",
			RELOPT_KIND_HEAP | RELOPT_KIND_TOAST,
			ShareUpdateExclusiveLock
		},
		-2, -1, INT_MAX
	},
	{
		{
			"autovacuum_analyze_threshold",
			"Minimum number of tuple inserts, updates or deletes prior to analyze",
			RELOPT_KIND_HEAP,
			ShareUpdateExclusiveLock
		},
		-1, 0, INT_MAX
	},
	{
		{
			"autovacuum_vacuum_cost_limit",
			"Vacuum cost amount available before napping, for autovacuum",
			RELOPT_KIND_HEAP | RELOPT_KIND_TOAST,
			ShareUpdateExclusiveLock
		},
		-1, 1, 10000
	},
	{
		{
			"autovacuum_freeze_min_age",
			"Minimum age at which VACUUM should freeze a table row, for autovacuum",
			RELOPT_KIND_HEAP | RELOPT_KIND_TOAST,
			ShareUpdateExclusiveLock
		},
		-1, 0, 1000000000
	},
	{
		{
			"autovacuum_multixact_freeze_min_age",
			"Minimum multixact age at which VACUUM should freeze a row multixact's, for autovacuum",
			RELOPT_KIND_HEAP | RELOPT_KIND_TOAST,
			ShareUpdateExclusiveLock
		},
		-1, 0, 1000000000
	},
	{
		{
			"autovacuum_freeze_max_age",
			"Age at which to autovacuum a table to prevent transaction ID wraparound",
			RELOPT_KIND_HEAP | RELOPT_KIND_TOAST,
			ShareUpdateExclusiveLock
		},
		-1, 100000, 2000000000
	},
	{
		{
			"autovacuum_multixact_freeze_max_age",
			"Multixact age at which to autovacuum a table to prevent multixact wraparound",
			RELOPT_KIND_HEAP | RELOPT_KIND_TOAST,
			ShareUpdateExclusiveLock
		},
		-1, 10000, 2000000000
	},
	{
		{
			"autovacuum_freeze_table_age",
			"Age at which VACUUM should perform a full table sweep to freeze row versions",
			RELOPT_KIND_HEAP | RELOPT_KIND_TOAST,
			ShareUpdateExclusiveLock
		}, -1, 0, 2000000000
	},
	{
		{
			"autovacuum_multixact_freeze_table_age",
			"Age of multixact at which VACUUM should perform a full table sweep to freeze row versions",
			RELOPT_KIND_HEAP | RELOPT_KIND_TOAST,
			ShareUpdateExclusiveLock
		}, -1, 0, 2000000000
	},
	{
		{
			"log_autovacuum_min_duration",
			"Sets the minimum execution time above which autovacuum actions will be logged",
			RELOPT_KIND_HEAP | RELOPT_KIND_TOAST,
			ShareUpdateExclusiveLock
		},
		-1, -1, INT_MAX
	},
	{
		{
			"toast_tuple_target",
			"Sets the target tuple length at which external columns will be toasted",
			RELOPT_KIND_HEAP,
			ShareUpdateExclusiveLock
		},
		TOAST_TUPLE_TARGET, 128, TOAST_TUPLE_TARGET_MAIN
	},
	{
		{
			"pages_per_range",
			"Number of pages that each page range covers in a BRIN index",
			RELOPT_KIND_BRIN,
			AccessExclusiveLock
		}, 128, 1, 131072
	},
	{
		{
			"gin_pending_list_limit",
			"Maximum size of the pending list for this GIN index, in kilobytes.",
			RELOPT_KIND_GIN,
			AccessExclusiveLock
		},
		-1, 64, MAX_KILOBYTES
	},
	{
		{
			"effective_io_concurrency",
			"Number of simultaneous requests that can be handled efficiently by the disk subsystem.",
			RELOPT_KIND_TABLESPACE,
			ShareUpdateExclusiveLock
		},
#ifdef USE_PREFETCH
		-1, 0, MAX_IO_CONCURRENCY
#else
		0, 0, 0
#endif
	},
	{
		{
			"maintenance_io_concurrency",
			"Number of simultaneous requests that can be handled efficiently by the disk subsystem for maintenance work.",
			RELOPT_KIND_TABLESPACE,
			ShareUpdateExclusiveLock
		},
#ifdef USE_PREFETCH
		-1, 0, MAX_IO_CONCURRENCY
#else
		0, 0, 0
#endif
	},
	{
		{
			"parallel_workers",
			"Number of parallel processes that can be used per executor node for this relation.",
			RELOPT_KIND_HEAP,
			ShareUpdateExclusiveLock
		},
		-1, 0, 1024
	},

	/* list terminator */
	{{NULL}}
};

static relopt_real realRelOpts[] =
{
	{
		{
			"autovacuum_vacuum_cost_delay",
			"Vacuum cost delay in milliseconds, for autovacuum",
			RELOPT_KIND_HEAP | RELOPT_KIND_TOAST,
			ShareUpdateExclusiveLock
		},
		-1, 0.0, 100.0
	},
	{
		{
			"autovacuum_vacuum_scale_factor",
			"Number of tuple updates or deletes prior to vacuum as a fraction of reltuples",
			RELOPT_KIND_HEAP | RELOPT_KIND_TOAST,
			ShareUpdateExclusiveLock
		},
		-1, 0.0, 100.0
	},
	{
		{
			"autovacuum_vacuum_insert_scale_factor",
			"Number of tuple inserts prior to vacuum as a fraction of reltuples",
			RELOPT_KIND_HEAP | RELOPT_KIND_TOAST,
			ShareUpdateExclusiveLock
		},
		-1, 0.0, 100.0
	},
	{
		{
			"autovacuum_analyze_scale_factor",
			"Number of tuple inserts, updates or deletes prior to analyze as a fraction of reltuples",
			RELOPT_KIND_HEAP,
			ShareUpdateExclusiveLock
		},
		-1, 0.0, 100.0
	},
	{
		{
			"seq_page_cost",
			"Sets the planner's estimate of the cost of a sequentially fetched disk page.",
			RELOPT_KIND_TABLESPACE,
			ShareUpdateExclusiveLock
		},
		-1, 0.0, DBL_MAX
	},
	{
		{
			"random_page_cost",
			"Sets the planner's estimate of the cost of a nonsequentially fetched disk page.",
			RELOPT_KIND_TABLESPACE,
			ShareUpdateExclusiveLock
		},
		-1, 0.0, DBL_MAX
	},
	{
		{
			"n_distinct",
			"Sets the planner's estimate of the number of distinct values appearing in a column (excluding child relations).",
			RELOPT_KIND_ATTRIBUTE,
			ShareUpdateExclusiveLock
		},
		0, -1.0, DBL_MAX
	},
	{
		{
			"n_distinct_inherited",
			"Sets the planner's estimate of the number of distinct values appearing in a column (including child relations).",
			RELOPT_KIND_ATTRIBUTE,
			ShareUpdateExclusiveLock
		},
		0, -1.0, DBL_MAX
	},
	{
		{
			"vacuum_cleanup_index_scale_factor",
			"Number of tuple inserts prior to index cleanup as a fraction of reltuples.",
			RELOPT_KIND_BTREE,
			ShareUpdateExclusiveLock
		},
		-1, 0.0, 1e10
	},
	/* list terminator */
	{{NULL}}
};

/* values from GistOptBufferingMode */
relopt_enum_elt_def gistBufferingOptValues[] =
{
	{"auto", GIST_OPTION_BUFFERING_AUTO},
	{"on", GIST_OPTION_BUFFERING_ON},
	{"off", GIST_OPTION_BUFFERING_OFF},
	{(const char *) NULL}		/* list terminator */
};

/* values from ViewOptCheckOption */
relopt_enum_elt_def viewCheckOptValues[] =
{
	/* no value for NOT_SET */
	{"local", VIEW_OPTION_CHECK_OPTION_LOCAL},
	{"cascaded", VIEW_OPTION_CHECK_OPTION_CASCADED},
	{(const char *) NULL}		/* list terminator */
};

static relopt_enum enumRelOpts[] =
{
	{
		{
			"buffering",
			"Enables buffering build for this GiST index",
			RELOPT_KIND_GIST,
			AccessExclusiveLock
		},
		gistBufferingOptValues,
		GIST_OPTION_BUFFERING_AUTO,
		gettext_noop("Valid values are \"on\", \"off\", and \"auto\".")
	},
	{
		{
			"check_option",
			"View has WITH CHECK OPTION defined (local or cascaded).",
			RELOPT_KIND_VIEW,
			AccessExclusiveLock
		},
		viewCheckOptValues,
		VIEW_OPTION_CHECK_OPTION_NOT_SET,
		gettext_noop("Valid values are \"local\" and \"cascaded\".")
	},
	/* list terminator */
	{{NULL}}
};

static relopt_string stringRelOpts[] =
{
	/* list terminator */
	{{NULL}}
};

static relopt_gen **relOpts = NULL;
static bits32 last_assigned_kind = RELOPT_KIND_LAST_DEFAULT;

static int	num_custom_options = 0;
static relopt_gen **custom_options = NULL;
static bool need_initialization = true;

static void initialize_reloptions(void);
static void parse_one_reloption(relopt_value *option, char *text_str,
								int text_len, bool validate);

/*
 * Get the length of a string reloption (either default or the user-defined
 * value).  This is used for allocation purposes when building a set of
 * relation options.
 */
#define GET_STRING_RELOPTION_LEN(option) \
	((option).isset ? strlen((option).values.string_val) : \
	 ((relopt_string *) (option).gen)->default_len)

/*
 * initialize_reloptions
 *		initialization routine, must be called before parsing
 *
 * Initialize the relOpts array and fill each variable's type and name length.
 */
static void
initialize_reloptions(void)
{
	int			i;
	int			j;

	j = 0;
	for (i = 0; boolRelOpts[i].gen.name; i++)
	{
		Assert(DoLockModesConflict(boolRelOpts[i].gen.lockmode,
								   boolRelOpts[i].gen.lockmode));
		j++;
	}
	for (i = 0; intRelOpts[i].gen.name; i++)
	{
		Assert(DoLockModesConflict(intRelOpts[i].gen.lockmode,
								   intRelOpts[i].gen.lockmode));
		j++;
	}
	for (i = 0; realRelOpts[i].gen.name; i++)
	{
		Assert(DoLockModesConflict(realRelOpts[i].gen.lockmode,
								   realRelOpts[i].gen.lockmode));
		j++;
	}
	for (i = 0; enumRelOpts[i].gen.name; i++)
	{
		Assert(DoLockModesConflict(enumRelOpts[i].gen.lockmode,
								   enumRelOpts[i].gen.lockmode));
		j++;
	}
	for (i = 0; stringRelOpts[i].gen.name; i++)
	{
		Assert(DoLockModesConflict(stringRelOpts[i].gen.lockmode,
								   stringRelOpts[i].gen.lockmode));
		j++;
	}
	j += num_custom_options;

	if (relOpts)
		pfree(relOpts);
	relOpts = MemoryContextAlloc(TopMemoryContext,
								 (j + 1) * sizeof(relopt_gen *));

	j = 0;
	for (i = 0; boolRelOpts[i].gen.name; i++)
	{
		relOpts[j] = &boolRelOpts[i].gen;
		relOpts[j]->type = RELOPT_TYPE_BOOL;
		relOpts[j]->namelen = strlen(relOpts[j]->name);
		j++;
	}

	for (i = 0; intRelOpts[i].gen.name; i++)
	{
		relOpts[j] = &intRelOpts[i].gen;
		relOpts[j]->type = RELOPT_TYPE_INT;
		relOpts[j]->namelen = strlen(relOpts[j]->name);
		j++;
	}

	for (i = 0; realRelOpts[i].gen.name; i++)
	{
		relOpts[j] = &realRelOpts[i].gen;
		relOpts[j]->type = RELOPT_TYPE_REAL;
		relOpts[j]->namelen = strlen(relOpts[j]->name);
		j++;
	}

	for (i = 0; enumRelOpts[i].gen.name; i++)
	{
		relOpts[j] = &enumRelOpts[i].gen;
		relOpts[j]->type = RELOPT_TYPE_ENUM;
		relOpts[j]->namelen = strlen(relOpts[j]->name);
		j++;
	}

	for (i = 0; stringRelOpts[i].gen.name; i++)
	{
		relOpts[j] = &stringRelOpts[i].gen;
		relOpts[j]->type = RELOPT_TYPE_STRING;
		relOpts[j]->namelen = strlen(relOpts[j]->name);
		j++;
	}

	for (i = 0; i < num_custom_options; i++)
	{
		relOpts[j] = custom_options[i];
		j++;
	}

	/* add a list terminator */
	relOpts[j] = NULL;

	/* flag the work is complete */
	need_initialization = false;
}

/*
 * add_reloption_kind
 *		Create a new relopt_kind value, to be used in custom reloptions by
 *		user-defined AMs.
 */
relopt_kind
add_reloption_kind(void)
{
	/* don't hand out the last bit so that the enum's behavior is portable */
	if (last_assigned_kind >= RELOPT_KIND_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("user-defined relation parameter types limit exceeded")));
	last_assigned_kind <<= 1;
	return (relopt_kind) last_assigned_kind;
}

/*
 * add_reloption
 *		Add an already-created custom reloption to the list, and recompute the
 *		main parser table.
 */
static void
add_reloption(relopt_gen *newoption)
{
	static int	max_custom_options = 0;

	if (num_custom_options >= max_custom_options)
	{
		MemoryContext oldcxt;

		oldcxt = MemoryContextSwitchTo(TopMemoryContext);

		if (max_custom_options == 0)
		{
			max_custom_options = 8;
			custom_options = palloc(max_custom_options * sizeof(relopt_gen *));
		}
		else
		{
			max_custom_options *= 2;
			custom_options = repalloc(custom_options,
									  max_custom_options * sizeof(relopt_gen *));
		}
		MemoryContextSwitchTo(oldcxt);
	}
	custom_options[num_custom_options++] = newoption;

	need_initialization = true;
}

/*
 * init_local_reloptions
 *		Initialize local reloptions that will parsed into bytea structure of
 * 		'relopt_struct_size'.
 */
void
init_local_reloptions(local_relopts *opts, Size relopt_struct_size)
{
	opts->options = NIL;
	opts->validators = NIL;
	opts->relopt_struct_size = relopt_struct_size;
}

/*
 * register_reloptions_validator
 *		Register custom validation callback that will be called at the end of
 *		build_local_reloptions().
 */
void
register_reloptions_validator(local_relopts *opts, relopts_validator validator)
{
	opts->validators = lappend(opts->validators, validator);
}

/*
 * add_local_reloption
 *		Add an already-created custom reloption to the local list.
 */
static void
add_local_reloption(local_relopts *relopts, relopt_gen *newoption, int offset)
{
	local_relopt *opt = palloc(sizeof(*opt));

	Assert(offset < relopts->relopt_struct_size);

	opt->option = newoption;
	opt->offset = offset;

	relopts->options = lappend(relopts->options, opt);
}

/*
 * allocate_reloption
 *		Allocate a new reloption and initialize the type-agnostic fields
 *		(for types other than string)
 */
static relopt_gen *
allocate_reloption(bits32 kinds, int type, const char *name, const char *desc,
				   LOCKMODE lockmode)
{
	MemoryContext oldcxt;
	size_t		size;
	relopt_gen *newoption;

	if (kinds != RELOPT_KIND_LOCAL)
		oldcxt = MemoryContextSwitchTo(TopMemoryContext);
	else
		oldcxt = NULL;

	switch (type)
	{
		case RELOPT_TYPE_BOOL:
			size = sizeof(relopt_bool);
			break;
		case RELOPT_TYPE_INT:
			size = sizeof(relopt_int);
			break;
		case RELOPT_TYPE_REAL:
			size = sizeof(relopt_real);
			break;
		case RELOPT_TYPE_ENUM:
			size = sizeof(relopt_enum);
			break;
		case RELOPT_TYPE_STRING:
			size = sizeof(relopt_string);
			break;
		default:
			elog(ERROR, "unsupported reloption type %d", type);
			return NULL;		/* keep compiler quiet */
	}

	newoption = palloc(size);

	newoption->name = pstrdup(name);
	if (desc)
		newoption->desc = pstrdup(desc);
	else
		newoption->desc = NULL;
	newoption->kinds = kinds;
	newoption->namelen = strlen(name);
	newoption->type = type;
	newoption->lockmode = lockmode;

	if (oldcxt != NULL)
		MemoryContextSwitchTo(oldcxt);

	return newoption;
}

/*
 * init_bool_reloption
 *		Allocate and initialize a new boolean reloption
 */
static relopt_bool *
init_bool_reloption(bits32 kinds, const char *name, const char *desc,
					bool default_val, LOCKMODE lockmode)
{
	relopt_bool *newoption;

	newoption = (relopt_bool *) allocate_reloption(kinds, RELOPT_TYPE_BOOL,
												   name, desc, lockmode);
	newoption->default_val = default_val;

	return newoption;
}

/*
 * add_bool_reloption
 *		Add a new boolean reloption
 */
void
add_bool_reloption(bits32 kinds, const char *name, const char *desc,
				   bool default_val, LOCKMODE lockmode)
{
	relopt_bool *newoption = init_bool_reloption(kinds, name, desc,
												 default_val, lockmode);

	add_reloption((relopt_gen *) newoption);
}

/*
 * add_local_bool_reloption
 *		Add a new boolean local reloption
 *
 * 'offset' is offset of bool-typed field.
 */
void
add_local_bool_reloption(local_relopts *relopts, const char *name,
						 const char *desc, bool default_val, int offset)
{
	relopt_bool *newoption = init_bool_reloption(RELOPT_KIND_LOCAL,
												 name, desc,
												 default_val, 0);

	add_local_reloption(relopts, (relopt_gen *) newoption, offset);
}


/*
 * init_real_reloption
 *		Allocate and initialize a new integer reloption
 */
static relopt_int *
init_int_reloption(bits32 kinds, const char *name, const char *desc,
				   int default_val, int min_val, int max_val,
				   LOCKMODE lockmode)
{
	relopt_int *newoption;

	newoption = (relopt_int *) allocate_reloption(kinds, RELOPT_TYPE_INT,
												  name, desc, lockmode);
	newoption->default_val = default_val;
	newoption->min = min_val;
	newoption->max = max_val;

	return newoption;
}

/*
 * add_int_reloption
 *		Add a new integer reloption
 */
void
add_int_reloption(bits32 kinds, const char *name, const char *desc, int default_val,
				  int min_val, int max_val, LOCKMODE lockmode)
{
	relopt_int *newoption = init_int_reloption(kinds, name, desc,
											   default_val, min_val,
											   max_val, lockmode);

	add_reloption((relopt_gen *) newoption);
}

/*
 * add_local_int_reloption
 *		Add a new local integer reloption
 *
 * 'offset' is offset of int-typed field.
 */
void
add_local_int_reloption(local_relopts *relopts, const char *name,
						const char *desc, int default_val, int min_val,
						int max_val, int offset)
{
	relopt_int *newoption = init_int_reloption(RELOPT_KIND_LOCAL,
											   name, desc, default_val,
											   min_val, max_val, 0);

	add_local_reloption(relopts, (relopt_gen *) newoption, offset);
}

/*
 * init_real_reloption
 *		Allocate and initialize a new real reloption
 */
static relopt_real *
init_real_reloption(bits32 kinds, const char *name, const char *desc,
					double default_val, double min_val, double max_val,
					LOCKMODE lockmode)
{
	relopt_real *newoption;

	newoption = (relopt_real *) allocate_reloption(kinds, RELOPT_TYPE_REAL,
												   name, desc, lockmode);
	newoption->default_val = default_val;
	newoption->min = min_val;
	newoption->max = max_val;

	return newoption;
}

/*
 * add_real_reloption
 *		Add a new float reloption
 */
void
add_real_reloption(bits32 kinds, const char *name, const char *desc,
				   double default_val, double min_val, double max_val,
				   LOCKMODE lockmode)
{
	relopt_real *newoption = init_real_reloption(kinds, name, desc,
												 default_val, min_val,
												 max_val, lockmode);

	add_reloption((relopt_gen *) newoption);
}

/*
 * add_local_real_reloption
 *		Add a new local float reloption
 *
 * 'offset' is offset of double-typed field.
 */
void
add_local_real_reloption(local_relopts *relopts, const char *name,
						 const char *desc, double default_val,
						 double min_val, double max_val, int offset)
{
	relopt_real *newoption = init_real_reloption(RELOPT_KIND_LOCAL,
												 name, desc,
												 default_val, min_val,
												 max_val, 0);

	add_local_reloption(relopts, (relopt_gen *) newoption, offset);
}

/*
 * init_enum_reloption
 *		Allocate and initialize a new enum reloption
 */
static relopt_enum *
init_enum_reloption(bits32 kinds, const char *name, const char *desc,
					relopt_enum_elt_def *members, int default_val,
					const char *detailmsg, LOCKMODE lockmode)
{
	relopt_enum *newoption;

	newoption = (relopt_enum *) allocate_reloption(kinds, RELOPT_TYPE_ENUM,
												   name, desc, lockmode);
	newoption->members = members;
	newoption->default_val = default_val;
	newoption->detailmsg = detailmsg;

	return newoption;
}


/*
 * add_enum_reloption
 *		Add a new enum reloption
 *
 * The members array must have a terminating NULL entry.
 *
 * The detailmsg is shown when unsupported values are passed, and has this
 * form:   "Valid values are \"foo\", \"bar\", and \"bar\"."
 *
 * The members array and detailmsg are not copied -- caller must ensure that
 * they are valid throughout the life of the process.
 */
void
add_enum_reloption(bits32 kinds, const char *name, const char *desc,
				   relopt_enum_elt_def *members, int default_val,
				   const char *detailmsg, LOCKMODE lockmode)
{
	relopt_enum *newoption = init_enum_reloption(kinds, name, desc,
												 members, default_val,
												 detailmsg, lockmode);

	add_reloption((relopt_gen *) newoption);
}

/*
 * add_local_enum_reloption
 *		Add a new local enum reloption
 *
 * 'offset' is offset of int-typed field.
 */
void
add_local_enum_reloption(local_relopts *relopts, const char *name,
						 const char *desc, relopt_enum_elt_def *members,
						 int default_val, const char *detailmsg, int offset)
{
	relopt_enum *newoption = init_enum_reloption(RELOPT_KIND_LOCAL,
												 name, desc,
												 members, default_val,
												 detailmsg, 0);

	add_local_reloption(relopts, (relopt_gen *) newoption, offset);
}

/*
 * init_string_reloption
 *		Allocate and initialize a new string reloption
 */
static relopt_string *
init_string_reloption(bits32 kinds, const char *name, const char *desc,
					  const char *default_val,
					  validate_string_relopt validator,
					  fill_string_relopt filler,
					  LOCKMODE lockmode)
{
	relopt_string *newoption;

	/* make sure the validator/default combination is sane */
	if (validator)
		(validator) (default_val);

	newoption = (relopt_string *) allocate_reloption(kinds, RELOPT_TYPE_STRING,
													 name, desc, lockmode);
	newoption->validate_cb = validator;
	newoption->fill_cb = filler;
	if (default_val)
	{
		if (kinds == RELOPT_KIND_LOCAL)
			newoption->default_val = strdup(default_val);
		else
			newoption->default_val = MemoryContextStrdup(TopMemoryContext, default_val);
		newoption->default_len = strlen(default_val);
		newoption->default_isnull = false;
	}
	else
	{
		newoption->default_val = "";
		newoption->default_len = 0;
		newoption->default_isnull = true;
	}

	return newoption;
}

/*
 * add_string_reloption
 *		Add a new string reloption
 *
 * "validator" is an optional function pointer that can be used to test the
 * validity of the values.  It must elog(ERROR) when the argument string is
 * not acceptable for the variable.  Note that the default value must pass
 * the validation.
 */
void
add_string_reloption(bits32 kinds, const char *name, const char *desc,
					 const char *default_val, validate_string_relopt validator,
					 LOCKMODE lockmode)
{
	relopt_string *newoption = init_string_reloption(kinds, name, desc,
													 default_val,
													 validator, NULL,
													 lockmode);

	add_reloption((relopt_gen *) newoption);
}

/*
 * add_local_string_reloption
 *		Add a new local string reloption
 *
 * 'offset' is offset of int-typed field that will store offset of string value
 * in the resulting bytea structure.
 */
void
add_local_string_reloption(local_relopts *relopts, const char *name,
						   const char *desc, const char *default_val,
						   validate_string_relopt validator,
						   fill_string_relopt filler, int offset)
{
	relopt_string *newoption = init_string_reloption(RELOPT_KIND_LOCAL,
													 name, desc,
													 default_val,
													 validator, filler,
													 0);

	add_local_reloption(relopts, (relopt_gen *) newoption, offset);
}

/*
 * Transform a relation options list (list of DefElem) into the text array
 * format that is kept in pg_class.reloptions, including only those options
 * that are in the passed namespace.  The output values do not include the
 * namespace.
 *
 * This is used for three cases: CREATE TABLE/INDEX, ALTER TABLE SET, and
 * ALTER TABLE RESET.  In the ALTER cases, oldOptions is the existing
 * reloptions value (possibly NULL), and we replace or remove entries
 * as needed.
 *
 * If acceptOidsOff is true, then we allow oids = false, but throw error when
 * on. This is solely needed for backwards compatibility.
 *
 * Note that this is not responsible for determining whether the options
 * are valid, but it does check that namespaces for all the options given are
 * listed in validnsps.  The NULL namespace is always valid and need not be
 * explicitly listed.  Passing a NULL pointer means that only the NULL
 * namespace is valid.
 *
 * Both oldOptions and the result are text arrays (or NULL for "default"),
 * but we declare them as Datums to avoid including array.h in reloptions.h.
 */
Datum
transformRelOptions(Datum oldOptions, List *defList, const char *namspace,
					char *validnsps[], bool acceptOidsOff, bool isReset)
{
	Datum		result;
	ArrayBuildState *astate;
	ListCell   *cell;

	/* no change if empty list */
	if (defList == NIL)
		return oldOptions;

	/* We build new array using accumArrayResult */
	astate = NULL;

	/* Copy any oldOptions that aren't to be replaced */
	if (PointerIsValid(DatumGetPointer(oldOptions)))
	{
		ArrayType  *array = DatumGetArrayTypeP(oldOptions);
		Datum	   *oldoptions;
		int			noldoptions;
		int			i;

		deconstruct_array(array, TEXTOID, -1, false, TYPALIGN_INT,
						  &oldoptions, NULL, &noldoptions);

		for (i = 0; i < noldoptions; i++)
		{
			char	   *text_str = VARDATA(oldoptions[i]);
			int			text_len = VARSIZE(oldoptions[i]) - VARHDRSZ;

			/* Search for a match in defList */
			foreach(cell, defList)
			{
				DefElem    *def = (DefElem *) lfirst(cell);
				int			kw_len;

				/* ignore if not in the same namespace */
				if (namspace == NULL)
				{
					if (def->defnamespace != NULL)
						continue;
				}
				else if (def->defnamespace == NULL)
					continue;
				else if (strcmp(def->defnamespace, namspace) != 0)
					continue;

				kw_len = strlen(def->defname);
				if (text_len > kw_len && text_str[kw_len] == '=' &&
					strncmp(text_str, def->defname, kw_len) == 0)
					break;
			}
			if (!cell)
			{
				/* No match, so keep old option */
				astate = accumArrayResult(astate, oldoptions[i],
										  false, TEXTOID,
										  CurrentMemoryContext);
			}
		}
	}

	/*
	 * If CREATE/SET, add new options to array; if RESET, just check that the
	 * user didn't say RESET (option=val).  (Must do this because the grammar
	 * doesn't enforce it.)
	 */
	foreach(cell, defList)
	{
		DefElem    *def = (DefElem *) lfirst(cell);

		if (isReset)
		{
			if (def->arg != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("RESET must not include values for parameters")));
		}
		else
		{
			text	   *t;
			const char *value;
			Size		len;

			/*
			 * Error out if the namespace is not valid.  A NULL namespace is
			 * always valid.
			 */
			if (def->defnamespace != NULL)
			{
				bool		valid = false;
				int			i;

				if (validnsps)
				{
					for (i = 0; validnsps[i]; i++)
					{
						if (strcmp(def->defnamespace, validnsps[i]) == 0)
						{
							valid = true;
							break;
						}
					}
				}

				if (!valid)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("unrecognized parameter namespace \"%s\"",
									def->defnamespace)));
			}

			/* ignore if not in the same namespace */
			if (namspace == NULL)
			{
				if (def->defnamespace != NULL)
					continue;
			}
			else if (def->defnamespace == NULL)
				continue;
			else if (strcmp(def->defnamespace, namspace) != 0)
				continue;

			/*
			 * Flatten the DefElem into a text string like "name=arg". If we
			 * have just "name", assume "name=true" is meant.  Note: the
			 * namespace is not output.
			 */
			if (def->arg != NULL)
				value = defGetString(def);
			else
				value = "true";

			/*
			 * This is not a great place for this test, but there's no other
			 * convenient place to filter the option out. As WITH (oids =
			 * false) will be removed someday, this seems like an acceptable
			 * amount of ugly.
			 */
			if (acceptOidsOff && def->defnamespace == NULL &&
				strcmp(def->defname, "oids") == 0)
			{
				if (defGetBoolean(def))
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("tables declared WITH OIDS are not supported")));
				/* skip over option, reloptions machinery doesn't know it */
				continue;
			}

			len = VARHDRSZ + strlen(def->defname) + 1 + strlen(value);
			/* +1 leaves room for sprintf's trailing null */
			t = (text *) palloc(len + 1);
			SET_VARSIZE(t, len);
			sprintf(VARDATA(t), "%s=%s", def->defname, value);

			astate = accumArrayResult(astate, PointerGetDatum(t),
									  false, TEXTOID,
									  CurrentMemoryContext);
		}
	}

	if (astate)
		result = makeArrayResult(astate, CurrentMemoryContext);
	else
		result = (Datum) 0;

	return result;
}


/*
 * Convert the text-array format of reloptions into a List of DefElem.
 * This is the inverse of transformRelOptions().
 */
List *
untransformRelOptions(Datum options)
{
	List	   *result = NIL;
	ArrayType  *array;
	Datum	   *optiondatums;
	int			noptions;
	int			i;

	/* Nothing to do if no options */
	if (!PointerIsValid(DatumGetPointer(options)))
		return result;

	array = DatumGetArrayTypeP(options);

	deconstruct_array(array, TEXTOID, -1, false, TYPALIGN_INT,
					  &optiondatums, NULL, &noptions);

	for (i = 0; i < noptions; i++)
	{
		char	   *s;
		char	   *p;
		Node	   *val = NULL;

		s = TextDatumGetCString(optiondatums[i]);
		p = strchr(s, '=');
		if (p)
		{
			*p++ = '\0';
			val = (Node *) makeString(pstrdup(p));
		}
		result = lappend(result, makeDefElem(pstrdup(s), val, -1));
	}

	return result;
}

/*
 * Extract and parse reloptions from a pg_class tuple.
 *
 * This is a low-level routine, expected to be used by relcache code and
 * callers that do not have a table's relcache entry (e.g. autovacuum).  For
 * other uses, consider grabbing the rd_options pointer from the relcache entry
 * instead.
 *
 * tupdesc is pg_class' tuple descriptor.  amoptions is a pointer to the index
 * AM's options parser function in the case of a tuple corresponding to an
 * index, or NULL otherwise.
 */
bytea *
extractRelOptions(HeapTuple tuple, TupleDesc tupdesc,
				  amoptions_function amoptions)
{
	bytea	   *options;
	bool		isnull;
	Datum		datum;
	Form_pg_class classForm;

	datum = fastgetattr(tuple,
						Anum_pg_class_reloptions,
						tupdesc,
						&isnull);
	if (isnull)
		return NULL;

	classForm = (Form_pg_class) GETSTRUCT(tuple);

	/* Parse into appropriate format; don't error out here */
	switch (classForm->relkind)
	{
		case RELKIND_RELATION:
		case RELKIND_TOASTVALUE:
		case RELKIND_MATVIEW:
			options = heap_reloptions(classForm->relkind, datum, false);
			break;
		case RELKIND_PARTITIONED_TABLE:
			options = partitioned_table_reloptions(datum, false);
			break;
		case RELKIND_VIEW:
			options = view_reloptions(datum, false);
			break;
		case RELKIND_INDEX:
		case RELKIND_PARTITIONED_INDEX:
			options = index_reloptions(amoptions, datum, false);
			break;
		case RELKIND_FOREIGN_TABLE:
			options = NULL;
			break;
		default:
			Assert(false);		/* can't get here */
			options = NULL;		/* keep compiler quiet */
			break;
	}

	return options;
}

static void
parseRelOptionsInternal(Datum options, bool validate,
						relopt_value *reloptions, int numoptions)
{
	ArrayType  *array = DatumGetArrayTypeP(options);
	Datum	   *optiondatums;
	int			noptions;
	int			i;

	deconstruct_array(array, TEXTOID, -1, false, TYPALIGN_INT,
					  &optiondatums, NULL, &noptions);

	for (i = 0; i < noptions; i++)
	{
		char	   *text_str = VARDATA(optiondatums[i]);
		int			text_len = VARSIZE(optiondatums[i]) - VARHDRSZ;
		int			j;

		/* Search for a match in reloptions */
		for (j = 0; j < numoptions; j++)
		{
			int			kw_len = reloptions[j].gen->namelen;

			if (text_len > kw_len && text_str[kw_len] == '=' &&
				strncmp(text_str, reloptions[j].gen->name, kw_len) == 0)
			{
				parse_one_reloption(&reloptions[j], text_str, text_len,
									validate);
				break;
			}
		}

		if (j >= numoptions && validate)
		{
			char	   *s;
			char	   *p;

			s = TextDatumGetCString(optiondatums[i]);
			p = strchr(s, '=');
			if (p)
				*p = '\0';
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("unrecognized parameter \"%s\"", s)));
		}
	}

	/* It's worth avoiding memory leaks in this function */
	pfree(optiondatums);

	if (((void *) array) != DatumGetPointer(options))
		pfree(array);
}

/*
 * Interpret reloptions that are given in text-array format.
 *
 * options is a reloption text array as constructed by transformRelOptions.
 * kind specifies the family of options to be processed.
 *
 * The return value is a relopt_value * array on which the options actually
 * set in the options array are marked with isset=true.  The length of this
 * array is returned in *numrelopts.  Options not set are also present in the
 * array; this is so that the caller can easily locate the default values.
 *
 * If there are no options of the given kind, numrelopts is set to 0 and NULL
 * is returned (unless options are illegally supplied despite none being
 * defined, in which case an error occurs).
 *
 * Note: values of type int, bool and real are allocated as part of the
 * returned array.  Values of type string are allocated separately and must
 * be freed by the caller.
 */
static relopt_value *
parseRelOptions(Datum options, bool validate, relopt_kind kind,
				int *numrelopts)
{
	relopt_value *reloptions = NULL;
	int			numoptions = 0;
	int			i;
	int			j;

	if (need_initialization)
		initialize_reloptions();

	/* Build a list of expected options, based on kind */

	for (i = 0; relOpts[i]; i++)
		if (relOpts[i]->kinds & kind)
			numoptions++;

	if (numoptions > 0)
	{
		reloptions = palloc(numoptions * sizeof(relopt_value));

		for (i = 0, j = 0; relOpts[i]; i++)
		{
			if (relOpts[i]->kinds & kind)
			{
				reloptions[j].gen = relOpts[i];
				reloptions[j].isset = false;
				j++;
			}
		}
	}

	/* Done if no options */
	if (PointerIsValid(DatumGetPointer(options)))
		parseRelOptionsInternal(options, validate, reloptions, numoptions);

	*numrelopts = numoptions;
	return reloptions;
}

/* Parse local unregistered options. */
static relopt_value *
parseLocalRelOptions(local_relopts *relopts, Datum options, bool validate)
{
	int			nopts = list_length(relopts->options);
	relopt_value *values = palloc(sizeof(*values) * nopts);
	ListCell   *lc;
	int			i = 0;

	foreach(lc, relopts->options)
	{
		local_relopt *opt = lfirst(lc);

		values[i].gen = opt->option;
		values[i].isset = false;

		i++;
	}

	if (options != (Datum) 0)
		parseRelOptionsInternal(options, validate, values, nopts);

	return values;
}

/*
 * Subroutine for parseRelOptions, to parse and validate a single option's
 * value
 */
static void
parse_one_reloption(relopt_value *option, char *text_str, int text_len,
					bool validate)
{
	char	   *value;
	int			value_len;
	bool		parsed;
	bool		nofree = false;

	if (option->isset && validate)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("parameter \"%s\" specified more than once",
						option->gen->name)));

	value_len = text_len - option->gen->namelen - 1;
	value = (char *) palloc(value_len + 1);
	memcpy(value, text_str + option->gen->namelen + 1, value_len);
	value[value_len] = '\0';

	switch (option->gen->type)
	{
		case RELOPT_TYPE_BOOL:
			{
				parsed = parse_bool(value, &option->values.bool_val);
				if (validate && !parsed)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("invalid value for boolean option \"%s\": %s",
									option->gen->name, value)));
			}
			break;
		case RELOPT_TYPE_INT:
			{
				relopt_int *optint = (relopt_int *) option->gen;

				parsed = parse_int(value, &option->values.int_val, 0, NULL);
				if (validate && !parsed)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("invalid value for integer option \"%s\": %s",
									option->gen->name, value)));
				if (validate && (option->values.int_val < optint->min ||
								 option->values.int_val > optint->max))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("value %s out of bounds for option \"%s\"",
									value, option->gen->name),
							 errdetail("Valid values are between \"%d\" and \"%d\".",
									   optint->min, optint->max)));
			}
			break;
		case RELOPT_TYPE_REAL:
			{
				relopt_real *optreal = (relopt_real *) option->gen;

				parsed = parse_real(value, &option->values.real_val, 0, NULL);
				if (validate && !parsed)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("invalid value for floating point option \"%s\": %s",
									option->gen->name, value)));
				if (validate && (option->values.real_val < optreal->min ||
								 option->values.real_val > optreal->max))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("value %s out of bounds for option \"%s\"",
									value, option->gen->name),
							 errdetail("Valid values are between \"%f\" and \"%f\".",
									   optreal->min, optreal->max)));
			}
			break;
		case RELOPT_TYPE_ENUM:
			{
				relopt_enum *optenum = (relopt_enum *) option->gen;
				relopt_enum_elt_def *elt;

				parsed = false;
				for (elt = optenum->members; elt->string_val; elt++)
				{
					if (pg_strcasecmp(value, elt->string_val) == 0)
					{
						option->values.enum_val = elt->symbol_val;
						parsed = true;
						break;
					}
				}
				if (validate && !parsed)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("invalid value for enum option \"%s\": %s",
									option->gen->name, value),
							 optenum->detailmsg ?
							 errdetail_internal("%s", _(optenum->detailmsg)) : 0));

				/*
				 * If value is not among the allowed string values, but we are
				 * not asked to validate, just use the default numeric value.
				 */
				if (!parsed)
					option->values.enum_val = optenum->default_val;
			}
			break;
		case RELOPT_TYPE_STRING:
			{
				relopt_string *optstring = (relopt_string *) option->gen;

				option->values.string_val = value;
				nofree = true;
				if (validate && optstring->validate_cb)
					(optstring->validate_cb) (value);
				parsed = true;
			}
			break;
		default:
			elog(ERROR, "unsupported reloption type %d", option->gen->type);
			parsed = true;		/* quiet compiler */
			break;
	}

	if (parsed)
		option->isset = true;
	if (!nofree)
		pfree(value);
}

/*
 * Given the result from parseRelOptions, allocate a struct that's of the
 * specified base size plus any extra space that's needed for string variables.
 *
 * "base" should be sizeof(struct) of the reloptions struct (StdRdOptions or
 * equivalent).
 */
static void *
allocateReloptStruct(Size base, relopt_value *options, int numoptions)
{
	Size		size = base;
	int			i;

	for (i = 0; i < numoptions; i++)
	{
		relopt_value *optval = &options[i];

		if (optval->gen->type == RELOPT_TYPE_STRING)
		{
			relopt_string *optstr = (relopt_string *) optval->gen;

			if (optstr->fill_cb)
			{
				const char *val = optval->isset ? optval->values.string_val :
				optstr->default_isnull ? NULL : optstr->default_val;

				size += optstr->fill_cb(val, NULL);
			}
			else
				size += GET_STRING_RELOPTION_LEN(*optval) + 1;
		}
	}

	return palloc0(size);
}

/*
 * Given the result of parseRelOptions and a parsing table, fill in the
 * struct (previously allocated with allocateReloptStruct) with the parsed
 * values.
 *
 * rdopts is the pointer to the allocated struct to be filled.
 * basesize is the sizeof(struct) that was passed to allocateReloptStruct.
 * options, of length numoptions, is parseRelOptions' output.
 * elems, of length numelems, is the table describing the allowed options.
 * When validate is true, it is expected that all options appear in elems.
 */
static void
fillRelOptions(void *rdopts, Size basesize,
			   relopt_value *options, int numoptions,
			   bool validate,
			   const relopt_parse_elt *elems, int numelems)
{
	int			i;
	int			offset = basesize;

	for (i = 0; i < numoptions; i++)
	{
		int			j;
		bool		found = false;

		for (j = 0; j < numelems; j++)
		{
			if (strcmp(options[i].gen->name, elems[j].optname) == 0)
			{
				relopt_string *optstring;
				char	   *itempos = ((char *) rdopts) + elems[j].offset;
				char	   *string_val;

				switch (options[i].gen->type)
				{
					case RELOPT_TYPE_BOOL:
						*(bool *) itempos = options[i].isset ?
							options[i].values.bool_val :
							((relopt_bool *) options[i].gen)->default_val;
						break;
					case RELOPT_TYPE_INT:
						*(int *) itempos = options[i].isset ?
							options[i].values.int_val :
							((relopt_int *) options[i].gen)->default_val;
						break;
					case RELOPT_TYPE_REAL:
						*(double *) itempos = options[i].isset ?
							options[i].values.real_val :
							((relopt_real *) options[i].gen)->default_val;
						break;
					case RELOPT_TYPE_ENUM:
						*(int *) itempos = options[i].isset ?
							options[i].values.enum_val :
							((relopt_enum *) options[i].gen)->default_val;
						break;
					case RELOPT_TYPE_STRING:
						optstring = (relopt_string *) options[i].gen;
						if (options[i].isset)
							string_val = options[i].values.string_val;
						else if (!optstring->default_isnull)
							string_val = optstring->default_val;
						else
							string_val = NULL;

						if (optstring->fill_cb)
						{
							Size		size =
							optstring->fill_cb(string_val,
											   (char *) rdopts + offset);

							if (size)
							{
								*(int *) itempos = offset;
								offset += size;
							}
							else
								*(int *) itempos = 0;
						}
						else if (string_val == NULL)
							*(int *) itempos = 0;
						else
						{
							strcpy((char *) rdopts + offset, string_val);
							*(int *) itempos = offset;
							offset += strlen(string_val) + 1;
						}
						break;
					default:
						elog(ERROR, "unsupported reloption type %d",
							 options[i].gen->type);
						break;
				}
				found = true;
				break;
			}
		}
		if (validate && !found)
			elog(ERROR, "reloption \"%s\" not found in parse table",
				 options[i].gen->name);
	}
	SET_VARSIZE(rdopts, offset);
}


/*
 * Option parser for anything that uses StdRdOptions.
 */
bytea *
default_reloptions(Datum reloptions, bool validate, relopt_kind kind)
{
	static const relopt_parse_elt tab[] = {
		{"fillfactor", RELOPT_TYPE_INT, offsetof(StdRdOptions, fillfactor)},
		{"autovacuum_enabled", RELOPT_TYPE_BOOL,
		offsetof(StdRdOptions, autovacuum) + offsetof(AutoVacOpts, enabled)},
		{"autovacuum_vacuum_threshold", RELOPT_TYPE_INT,
		offsetof(StdRdOptions, autovacuum) + offsetof(AutoVacOpts, vacuum_threshold)},
		{"autovacuum_vacuum_insert_threshold", RELOPT_TYPE_INT,
		offsetof(StdRdOptions, autovacuum) + offsetof(AutoVacOpts, vacuum_ins_threshold)},
		{"autovacuum_analyze_threshold", RELOPT_TYPE_INT,
		offsetof(StdRdOptions, autovacuum) + offsetof(AutoVacOpts, analyze_threshold)},
		{"autovacuum_vacuum_cost_limit", RELOPT_TYPE_INT,
		offsetof(StdRdOptions, autovacuum) + offsetof(AutoVacOpts, vacuum_cost_limit)},
		{"autovacuum_freeze_min_age", RELOPT_TYPE_INT,
		offsetof(StdRdOptions, autovacuum) + offsetof(AutoVacOpts, freeze_min_age)},
		{"autovacuum_freeze_max_age", RELOPT_TYPE_INT,
		offsetof(StdRdOptions, autovacuum) + offsetof(AutoVacOpts, freeze_max_age)},
		{"autovacuum_freeze_table_age", RELOPT_TYPE_INT,
		offsetof(StdRdOptions, autovacuum) + offsetof(AutoVacOpts, freeze_table_age)},
		{"autovacuum_multixact_freeze_min_age", RELOPT_TYPE_INT,
		offsetof(StdRdOptions, autovacuum) + offsetof(AutoVacOpts, multixact_freeze_min_age)},
		{"autovacuum_multixact_freeze_max_age", RELOPT_TYPE_INT,
		offsetof(StdRdOptions, autovacuum) + offsetof(AutoVacOpts, multixact_freeze_max_age)},
		{"autovacuum_multixact_freeze_table_age", RELOPT_TYPE_INT,
		offsetof(StdRdOptions, autovacuum) + offsetof(AutoVacOpts, multixact_freeze_table_age)},
		{"log_autovacuum_min_duration", RELOPT_TYPE_INT,
		offsetof(StdRdOptions, autovacuum) + offsetof(AutoVacOpts, log_min_duration)},
		{"toast_tuple_target", RELOPT_TYPE_INT,
		offsetof(StdRdOptions, toast_tuple_target)},
		{"autovacuum_vacuum_cost_delay", RELOPT_TYPE_REAL,
		offsetof(StdRdOptions, autovacuum) + offsetof(AutoVacOpts, vacuum_cost_delay)},
		{"autovacuum_vacuum_scale_factor", RELOPT_TYPE_REAL,
		offsetof(StdRdOptions, autovacuum) + offsetof(AutoVacOpts, vacuum_scale_factor)},
		{"autovacuum_vacuum_insert_scale_factor", RELOPT_TYPE_REAL,
		offsetof(StdRdOptions, autovacuum) + offsetof(AutoVacOpts, vacuum_ins_scale_factor)},
		{"autovacuum_analyze_scale_factor", RELOPT_TYPE_REAL,
		offsetof(StdRdOptions, autovacuum) + offsetof(AutoVacOpts, analyze_scale_factor)},
		{"user_catalog_table", RELOPT_TYPE_BOOL,
		offsetof(StdRdOptions, user_catalog_table)},
		{"parallel_workers", RELOPT_TYPE_INT,
		offsetof(StdRdOptions, parallel_workers)},
		{"vacuum_index_cleanup", RELOPT_TYPE_BOOL,
		offsetof(StdRdOptions, vacuum_index_cleanup)},
		{"vacuum_truncate", RELOPT_TYPE_BOOL,
		offsetof(StdRdOptions, vacuum_truncate)}
	};

	return (bytea *) build_reloptions(reloptions, validate, kind,
									  sizeof(StdRdOptions),
									  tab, lengthof(tab));
}

/*
 * build_reloptions
 *
 * Parses "reloptions" provided by the caller, returning them in a
 * structure containing the parsed options.  The parsing is done with
 * the help of a parsing table describing the allowed options, defined
 * by "relopt_elems" of length "num_relopt_elems".
 *
 * "validate" must be true if reloptions value is freshly built by
 * transformRelOptions(), as opposed to being read from the catalog, in which
 * case the values contained in it must already be valid.
 *
 * NULL is returned if the passed-in options did not match any of the options
 * in the parsing table, unless validate is true in which case an error would
 * be reported.
 */
void *
build_reloptions(Datum reloptions, bool validate,
				 relopt_kind kind,
				 Size relopt_struct_size,
				 const relopt_parse_elt *relopt_elems,
				 int num_relopt_elems)
{
	int			numoptions;
	relopt_value *options;
	void	   *rdopts;

	/* parse options specific to given relation option kind */
	options = parseRelOptions(reloptions, validate, kind, &numoptions);
	Assert(numoptions <= num_relopt_elems);

	/* if none set, we're done */
	if (numoptions == 0)
	{
		Assert(options == NULL);
		return NULL;
	}

	/* allocate and fill the structure */
	rdopts = allocateReloptStruct(relopt_struct_size, options, numoptions);
	fillRelOptions(rdopts, relopt_struct_size, options, numoptions,
				   validate, relopt_elems, num_relopt_elems);

	pfree(options);

	return rdopts;
}

/*
 * Parse local options, allocate a bytea struct that's of the specified
 * 'base_size' plus any extra space that's needed for string variables,
 * fill its option's fields located at the given offsets and return it.
 */
void *
build_local_reloptions(local_relopts *relopts, Datum options, bool validate)
{
	int			noptions = list_length(relopts->options);
	relopt_parse_elt *elems = palloc(sizeof(*elems) * noptions);
	relopt_value *vals;
	void	   *opts;
	int			i = 0;
	ListCell   *lc;

	foreach(lc, relopts->options)
	{
		local_relopt *opt = lfirst(lc);

		elems[i].optname = opt->option->name;
		elems[i].opttype = opt->option->type;
		elems[i].offset = opt->offset;

		i++;
	}

	vals = parseLocalRelOptions(relopts, options, validate);
	opts = allocateReloptStruct(relopts->relopt_struct_size, vals, noptions);
	fillRelOptions(opts, relopts->relopt_struct_size, vals, noptions, validate,
				   elems, noptions);

	if (validate)
		foreach(lc, relopts->validators)
			((relopts_validator) lfirst(lc)) (opts, vals, noptions);

	if (elems)
		pfree(elems);

	return opts;
}

/*
 * Option parser for partitioned tables
 */
bytea *
partitioned_table_reloptions(Datum reloptions, bool validate)
{
	/*
	 * There are no options for partitioned tables yet, but this is able to do
	 * some validation.
	 */
	return (bytea *) build_reloptions(reloptions, validate,
									  RELOPT_KIND_PARTITIONED,
									  0, NULL, 0);
}

/*
 * Option parser for views
 */
bytea *
view_reloptions(Datum reloptions, bool validate)
{
	static const relopt_parse_elt tab[] = {
		{"security_barrier", RELOPT_TYPE_BOOL,
		offsetof(ViewOptions, security_barrier)},
		{"check_option", RELOPT_TYPE_ENUM,
		offsetof(ViewOptions, check_option)}
	};

	return (bytea *) build_reloptions(reloptions, validate,
									  RELOPT_KIND_VIEW,
									  sizeof(ViewOptions),
									  tab, lengthof(tab));
}

/*
 * Parse options for heaps, views and toast tables.
 */
bytea *
heap_reloptions(char relkind, Datum reloptions, bool validate)
{
	StdRdOptions *rdopts;

	switch (relkind)
	{
		case RELKIND_TOASTVALUE:
			rdopts = (StdRdOptions *)
				default_reloptions(reloptions, validate, RELOPT_KIND_TOAST);
			if (rdopts != NULL)
			{
				/* adjust default-only parameters for TOAST relations */
				rdopts->fillfactor = 100;
				rdopts->autovacuum.analyze_threshold = -1;
				rdopts->autovacuum.analyze_scale_factor = -1;
			}
			return (bytea *) rdopts;
		case RELKIND_RELATION:
		case RELKIND_MATVIEW:
			return default_reloptions(reloptions, validate, RELOPT_KIND_HEAP);
		default:
			/* other relkinds are not supported */
			return NULL;
	}
}


/*
 * Parse options for indexes.
 *
 *	amoptions	index AM's option parser function
 *	reloptions	options as text[] datum
 *	validate	error flag
 */
bytea *
index_reloptions(amoptions_function amoptions, Datum reloptions, bool validate)
{
	Assert(amoptions != NULL);

	/* Assume function is strict */
	if (!PointerIsValid(DatumGetPointer(reloptions)))
		return NULL;

	return amoptions(reloptions, validate);
}

/*
 * Option parser for attribute reloptions
 */
bytea *
attribute_reloptions(Datum reloptions, bool validate)
{
	static const relopt_parse_elt tab[] = {
		{"n_distinct", RELOPT_TYPE_REAL, offsetof(AttributeOpts, n_distinct)},
		{"n_distinct_inherited", RELOPT_TYPE_REAL, offsetof(AttributeOpts, n_distinct_inherited)}
	};

	return (bytea *) build_reloptions(reloptions, validate,
									  RELOPT_KIND_ATTRIBUTE,
									  sizeof(AttributeOpts),
									  tab, lengthof(tab));
}

/*
 * Option parser for tablespace reloptions
 */
bytea *
tablespace_reloptions(Datum reloptions, bool validate)
{
	static const relopt_parse_elt tab[] = {
		{"random_page_cost", RELOPT_TYPE_REAL, offsetof(TableSpaceOpts, random_page_cost)},
		{"seq_page_cost", RELOPT_TYPE_REAL, offsetof(TableSpaceOpts, seq_page_cost)},
		{"effective_io_concurrency", RELOPT_TYPE_INT, offsetof(TableSpaceOpts, effective_io_concurrency)},
		{"maintenance_io_concurrency", RELOPT_TYPE_INT, offsetof(TableSpaceOpts, maintenance_io_concurrency)}
	};

	return (bytea *) build_reloptions(reloptions, validate,
									  RELOPT_KIND_TABLESPACE,
									  sizeof(TableSpaceOpts),
									  tab, lengthof(tab));
}

/*
 * Determine the required LOCKMODE from an option list.
 *
 * Called from AlterTableGetLockLevel(), see that function
 * for a longer explanation of how this works.
 */
LOCKMODE
AlterTableGetRelOptionsLockLevel(List *defList)
{
	LOCKMODE	lockmode = NoLock;
	ListCell   *cell;

	if (defList == NIL)
		return AccessExclusiveLock;

	if (need_initialization)
		initialize_reloptions();

	foreach(cell, defList)
	{
		DefElem    *def = (DefElem *) lfirst(cell);
		int			i;

		for (i = 0; relOpts[i]; i++)
		{
			if (strncmp(relOpts[i]->name,
						def->defname,
						relOpts[i]->namelen + 1) == 0)
			{
				if (lockmode < relOpts[i]->lockmode)
					lockmode = relOpts[i]->lockmode;
			}
		}
	}

	return lockmode;
}

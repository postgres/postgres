/*-------------------------------------------------------------------------
 *
 * dependencies.c
 *	  POSTGRES functional dependencies
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/statistics/dependencies.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/sysattr.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_statistic_ext.h"
#include "catalog/pg_statistic_ext_data.h"
#include "lib/stringinfo.h"
#include "nodes/nodeFuncs.h"
#include "nodes/nodes.h"
#include "nodes/pathnodes.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "parser/parsetree.h"
#include "statistics/extended_stats_internal.h"
#include "statistics/statistics.h"
#include "utils/bytea.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/selfuncs.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

/* size of the struct header fields (magic, type, ndeps) */
#define SizeOfHeader		(3 * sizeof(uint32))

/* size of a serialized dependency (degree, natts, atts) */
#define SizeOfItem(natts) \
	(sizeof(double) + sizeof(AttrNumber) * (1 + (natts)))

/* minimal size of a dependency (with two attributes) */
#define MinSizeOfItem	SizeOfItem(2)

/* minimal size of dependencies, when all deps are minimal */
#define MinSizeOfItems(ndeps) \
	(SizeOfHeader + (ndeps) * MinSizeOfItem)

/*
 * Internal state for DependencyGenerator of dependencies. Dependencies are similar to
 * k-permutations of n elements, except that the order does not matter for the
 * first (k-1) elements. That is, (a,b=>c) and (b,a=>c) are equivalent.
 */
typedef struct DependencyGeneratorData
{
	int			k;				/* size of the dependency */
	int			n;				/* number of possible attributes */
	int			current;		/* next dependency to return (index) */
	AttrNumber	ndependencies;	/* number of dependencies generated */
	AttrNumber *dependencies;	/* array of pre-generated dependencies	*/
} DependencyGeneratorData;

typedef DependencyGeneratorData *DependencyGenerator;

static void generate_dependencies_recurse(DependencyGenerator state,
										  int index, AttrNumber start, AttrNumber *current);
static void generate_dependencies(DependencyGenerator state);
static DependencyGenerator DependencyGenerator_init(int n, int k);
static void DependencyGenerator_free(DependencyGenerator state);
static AttrNumber *DependencyGenerator_next(DependencyGenerator state);
static double dependency_degree(StatsBuildData *data, int k, AttrNumber *dependency);
static bool dependency_is_fully_matched(MVDependency *dependency,
										Bitmapset *attnums);
static bool dependency_is_compatible_clause(Node *clause, Index relid,
											AttrNumber *attnum);
static bool dependency_is_compatible_expression(Node *clause, Index relid,
												List *statlist, Node **expr);
static MVDependency *find_strongest_dependency(MVDependencies **dependencies,
											   int ndependencies, Bitmapset *attnums);
static Selectivity clauselist_apply_dependencies(PlannerInfo *root, List *clauses,
												 int varRelid, JoinType jointype,
												 SpecialJoinInfo *sjinfo,
												 MVDependency **dependencies,
												 int ndependencies,
												 AttrNumber *list_attnums,
												 Bitmapset **estimatedclauses);

static void
generate_dependencies_recurse(DependencyGenerator state, int index,
							  AttrNumber start, AttrNumber *current)
{
	/*
	 * The generator handles the first (k-1) elements differently from the
	 * last element.
	 */
	if (index < (state->k - 1))
	{
		AttrNumber	i;

		/*
		 * The first (k-1) values have to be in ascending order, which we
		 * generate recursively.
		 */

		for (i = start; i < state->n; i++)
		{
			current[index] = i;
			generate_dependencies_recurse(state, (index + 1), (i + 1), current);
		}
	}
	else
	{
		int			i;

		/*
		 * the last element is the implied value, which does not respect the
		 * ascending order. We just need to check that the value is not in the
		 * first (k-1) elements.
		 */

		for (i = 0; i < state->n; i++)
		{
			int			j;
			bool		match = false;

			current[index] = i;

			for (j = 0; j < index; j++)
			{
				if (current[j] == i)
				{
					match = true;
					break;
				}
			}

			/*
			 * If the value is not found in the first part of the dependency,
			 * we're done.
			 */
			if (!match)
			{
				state->dependencies = (AttrNumber *) repalloc(state->dependencies,
															  state->k * (state->ndependencies + 1) * sizeof(AttrNumber));
				memcpy(&state->dependencies[(state->k * state->ndependencies)],
					   current, state->k * sizeof(AttrNumber));
				state->ndependencies++;
			}
		}
	}
}

/* generate all dependencies (k-permutations of n elements) */
static void
generate_dependencies(DependencyGenerator state)
{
	AttrNumber *current = (AttrNumber *) palloc0(sizeof(AttrNumber) * state->k);

	generate_dependencies_recurse(state, 0, 0, current);

	pfree(current);
}

/*
 * initialize the DependencyGenerator of variations, and prebuild the variations
 *
 * This pre-builds all the variations. We could also generate them in
 * DependencyGenerator_next(), but this seems simpler.
 */
static DependencyGenerator
DependencyGenerator_init(int n, int k)
{
	DependencyGenerator state;

	Assert((n >= k) && (k > 0));

	/* allocate the DependencyGenerator state */
	state = (DependencyGenerator) palloc0(sizeof(DependencyGeneratorData));
	state->dependencies = (AttrNumber *) palloc(k * sizeof(AttrNumber));

	state->ndependencies = 0;
	state->current = 0;
	state->k = k;
	state->n = n;

	/* now actually pre-generate all the variations */
	generate_dependencies(state);

	return state;
}

/* free the DependencyGenerator state */
static void
DependencyGenerator_free(DependencyGenerator state)
{
	pfree(state->dependencies);
	pfree(state);
}

/* generate next combination */
static AttrNumber *
DependencyGenerator_next(DependencyGenerator state)
{
	if (state->current == state->ndependencies)
		return NULL;

	return &state->dependencies[state->k * state->current++];
}


/*
 * validates functional dependency on the data
 *
 * An actual work horse of detecting functional dependencies. Given a variation
 * of k attributes, it checks that the first (k-1) are sufficient to determine
 * the last one.
 */
static double
dependency_degree(StatsBuildData *data, int k, AttrNumber *dependency)
{
	int			i,
				nitems;
	MultiSortSupport mss;
	SortItem   *items;
	AttrNumber *attnums_dep;

	/* counters valid within a group */
	int			group_size = 0;
	int			n_violations = 0;

	/* total number of rows supporting (consistent with) the dependency */
	int			n_supporting_rows = 0;

	/* Make sure we have at least two input attributes. */
	Assert(k >= 2);

	/* sort info for all attributes columns */
	mss = multi_sort_init(k);

	/*
	 * Translate the array of indexes to regular attnums for the dependency
	 * (we will need this to identify the columns in StatsBuildData).
	 */
	attnums_dep = (AttrNumber *) palloc(k * sizeof(AttrNumber));
	for (i = 0; i < k; i++)
		attnums_dep[i] = data->attnums[dependency[i]];

	/*
	 * Verify the dependency (a,b,...)->z, using a rather simple algorithm:
	 *
	 * (a) sort the data lexicographically
	 *
	 * (b) split the data into groups by first (k-1) columns
	 *
	 * (c) for each group count different values in the last column
	 *
	 * We use the column data types' default sort operators and collations;
	 * perhaps at some point it'd be worth using column-specific collations?
	 */

	/* prepare the sort function for the dimensions */
	for (i = 0; i < k; i++)
	{
		VacAttrStats *colstat = data->stats[dependency[i]];
		TypeCacheEntry *type;

		type = lookup_type_cache(colstat->attrtypid, TYPECACHE_LT_OPR);
		if (type->lt_opr == InvalidOid) /* shouldn't happen */
			elog(ERROR, "cache lookup failed for ordering operator for type %u",
				 colstat->attrtypid);

		/* prepare the sort function for this dimension */
		multi_sort_add_dimension(mss, i, type->lt_opr, colstat->attrcollid);
	}

	/*
	 * build an array of SortItem(s) sorted using the multi-sort support
	 *
	 * XXX This relies on all stats entries pointing to the same tuple
	 * descriptor.  For now that assumption holds, but it might change in the
	 * future for example if we support statistics on multiple tables.
	 */
	items = build_sorted_items(data, &nitems, mss, k, attnums_dep);

	/*
	 * Walk through the sorted array, split it into rows according to the
	 * first (k-1) columns. If there's a single value in the last column, we
	 * count the group as 'supporting' the functional dependency. Otherwise we
	 * count it as contradicting.
	 */

	/* start with the first row forming a group */
	group_size = 1;

	/* loop 1 beyond the end of the array so that we count the final group */
	for (i = 1; i <= nitems; i++)
	{
		/*
		 * Check if the group ended, which may be either because we processed
		 * all the items (i==nitems), or because the i-th item is not equal to
		 * the preceding one.
		 */
		if (i == nitems ||
			multi_sort_compare_dims(0, k - 2, &items[i - 1], &items[i], mss) != 0)
		{
			/*
			 * If no violations were found in the group then track the rows of
			 * the group as supporting the functional dependency.
			 */
			if (n_violations == 0)
				n_supporting_rows += group_size;

			/* Reset counters for the new group */
			n_violations = 0;
			group_size = 1;
			continue;
		}
		/* first columns match, but the last one does not (so contradicting) */
		else if (multi_sort_compare_dim(k - 1, &items[i - 1], &items[i], mss) != 0)
			n_violations++;

		group_size++;
	}

	/* Compute the 'degree of validity' as (supporting/total). */
	return (n_supporting_rows * 1.0 / data->numrows);
}

/*
 * detects functional dependencies between groups of columns
 *
 * Generates all possible subsets of columns (variations) and computes
 * the degree of validity for each one. For example when creating statistics
 * on three columns (a,b,c) there are 9 possible dependencies
 *
 *	   two columns			  three columns
 *	   -----------			  -------------
 *	   (a) -> b				  (a,b) -> c
 *	   (a) -> c				  (a,c) -> b
 *	   (b) -> a				  (b,c) -> a
 *	   (b) -> c
 *	   (c) -> a
 *	   (c) -> b
 */
MVDependencies *
statext_dependencies_build(StatsBuildData *data)
{
	int			i,
				k;

	/* result */
	MVDependencies *dependencies = NULL;
	MemoryContext cxt;

	Assert(data->nattnums >= 2);

	/* tracks memory allocated by dependency_degree calls */
	cxt = AllocSetContextCreate(CurrentMemoryContext,
								"dependency_degree cxt",
								ALLOCSET_DEFAULT_SIZES);

	/*
	 * We'll try build functional dependencies starting from the smallest ones
	 * covering just 2 columns, to the largest ones, covering all columns
	 * included in the statistics object.  We start from the smallest ones
	 * because we want to be able to skip already implied ones.
	 */
	for (k = 2; k <= data->nattnums; k++)
	{
		AttrNumber *dependency; /* array with k elements */

		/* prepare a DependencyGenerator of variation */
		DependencyGenerator DependencyGenerator = DependencyGenerator_init(data->nattnums, k);

		/* generate all possible variations of k values (out of n) */
		while ((dependency = DependencyGenerator_next(DependencyGenerator)))
		{
			double		degree;
			MVDependency *d;
			MemoryContext oldcxt;

			/* release memory used by dependency degree calculation */
			oldcxt = MemoryContextSwitchTo(cxt);

			/* compute how valid the dependency seems */
			degree = dependency_degree(data, k, dependency);

			MemoryContextSwitchTo(oldcxt);
			MemoryContextReset(cxt);

			/*
			 * if the dependency seems entirely invalid, don't store it
			 */
			if (degree == 0.0)
				continue;

			d = (MVDependency *) palloc0(offsetof(MVDependency, attributes)
										 + k * sizeof(AttrNumber));

			/* copy the dependency (and keep the indexes into stxkeys) */
			d->degree = degree;
			d->nattributes = k;
			for (i = 0; i < k; i++)
				d->attributes[i] = data->attnums[dependency[i]];

			/* initialize the list of dependencies */
			if (dependencies == NULL)
			{
				dependencies
					= (MVDependencies *) palloc0(sizeof(MVDependencies));

				dependencies->magic = STATS_DEPS_MAGIC;
				dependencies->type = STATS_DEPS_TYPE_BASIC;
				dependencies->ndeps = 0;
			}

			dependencies->ndeps++;
			dependencies = (MVDependencies *) repalloc(dependencies,
													   offsetof(MVDependencies, deps)
													   + dependencies->ndeps * sizeof(MVDependency *));

			dependencies->deps[dependencies->ndeps - 1] = d;
		}

		/*
		 * we're done with variations of k elements, so free the
		 * DependencyGenerator
		 */
		DependencyGenerator_free(DependencyGenerator);
	}

	MemoryContextDelete(cxt);

	return dependencies;
}


/*
 * Serialize list of dependencies into a bytea value.
 */
bytea *
statext_dependencies_serialize(MVDependencies *dependencies)
{
	int			i;
	bytea	   *output;
	char	   *tmp;
	Size		len;

	/* we need to store ndeps, with a number of attributes for each one */
	len = VARHDRSZ + SizeOfHeader;

	/* and also include space for the actual attribute numbers and degrees */
	for (i = 0; i < dependencies->ndeps; i++)
		len += SizeOfItem(dependencies->deps[i]->nattributes);

	output = (bytea *) palloc0(len);
	SET_VARSIZE(output, len);

	tmp = VARDATA(output);

	/* Store the base struct values (magic, type, ndeps) */
	memcpy(tmp, &dependencies->magic, sizeof(uint32));
	tmp += sizeof(uint32);
	memcpy(tmp, &dependencies->type, sizeof(uint32));
	tmp += sizeof(uint32);
	memcpy(tmp, &dependencies->ndeps, sizeof(uint32));
	tmp += sizeof(uint32);

	/* store number of attributes and attribute numbers for each dependency */
	for (i = 0; i < dependencies->ndeps; i++)
	{
		MVDependency *d = dependencies->deps[i];

		memcpy(tmp, &d->degree, sizeof(double));
		tmp += sizeof(double);

		memcpy(tmp, &d->nattributes, sizeof(AttrNumber));
		tmp += sizeof(AttrNumber);

		memcpy(tmp, d->attributes, sizeof(AttrNumber) * d->nattributes);
		tmp += sizeof(AttrNumber) * d->nattributes;

		/* protect against overflow */
		Assert(tmp <= ((char *) output + len));
	}

	/* make sure we've produced exactly the right amount of data */
	Assert(tmp == ((char *) output + len));

	return output;
}

/*
 * Reads serialized dependencies into MVDependencies structure.
 */
MVDependencies *
statext_dependencies_deserialize(bytea *data)
{
	int			i;
	Size		min_expected_size;
	MVDependencies *dependencies;
	char	   *tmp;

	if (data == NULL)
		return NULL;

	if (VARSIZE_ANY_EXHDR(data) < SizeOfHeader)
		elog(ERROR, "invalid MVDependencies size %zd (expected at least %zd)",
			 VARSIZE_ANY_EXHDR(data), SizeOfHeader);

	/* read the MVDependencies header */
	dependencies = (MVDependencies *) palloc0(sizeof(MVDependencies));

	/* initialize pointer to the data part (skip the varlena header) */
	tmp = VARDATA_ANY(data);

	/* read the header fields and perform basic sanity checks */
	memcpy(&dependencies->magic, tmp, sizeof(uint32));
	tmp += sizeof(uint32);
	memcpy(&dependencies->type, tmp, sizeof(uint32));
	tmp += sizeof(uint32);
	memcpy(&dependencies->ndeps, tmp, sizeof(uint32));
	tmp += sizeof(uint32);

	if (dependencies->magic != STATS_DEPS_MAGIC)
		elog(ERROR, "invalid dependency magic %d (expected %d)",
			 dependencies->magic, STATS_DEPS_MAGIC);

	if (dependencies->type != STATS_DEPS_TYPE_BASIC)
		elog(ERROR, "invalid dependency type %d (expected %d)",
			 dependencies->type, STATS_DEPS_TYPE_BASIC);

	if (dependencies->ndeps == 0)
		elog(ERROR, "invalid zero-length item array in MVDependencies");

	/* what minimum bytea size do we expect for those parameters */
	min_expected_size = SizeOfItem(dependencies->ndeps);

	if (VARSIZE_ANY_EXHDR(data) < min_expected_size)
		elog(ERROR, "invalid dependencies size %zd (expected at least %zd)",
			 VARSIZE_ANY_EXHDR(data), min_expected_size);

	/* allocate space for the MCV items */
	dependencies = repalloc(dependencies, offsetof(MVDependencies, deps)
							+ (dependencies->ndeps * sizeof(MVDependency *)));

	for (i = 0; i < dependencies->ndeps; i++)
	{
		double		degree;
		AttrNumber	k;
		MVDependency *d;

		/* degree of validity */
		memcpy(&degree, tmp, sizeof(double));
		tmp += sizeof(double);

		/* number of attributes */
		memcpy(&k, tmp, sizeof(AttrNumber));
		tmp += sizeof(AttrNumber);

		/* is the number of attributes valid? */
		Assert((k >= 2) && (k <= STATS_MAX_DIMENSIONS));

		/* now that we know the number of attributes, allocate the dependency */
		d = (MVDependency *) palloc0(offsetof(MVDependency, attributes)
									 + (k * sizeof(AttrNumber)));

		d->degree = degree;
		d->nattributes = k;

		/* copy attribute numbers */
		memcpy(d->attributes, tmp, sizeof(AttrNumber) * d->nattributes);
		tmp += sizeof(AttrNumber) * d->nattributes;

		dependencies->deps[i] = d;

		/* still within the bytea */
		Assert(tmp <= ((char *) data + VARSIZE_ANY(data)));
	}

	/* we should have consumed the whole bytea exactly */
	Assert(tmp == ((char *) data + VARSIZE_ANY(data)));

	return dependencies;
}

/*
 * dependency_is_fully_matched
 *		checks that a functional dependency is fully matched given clauses on
 *		attributes (assuming the clauses are suitable equality clauses)
 */
static bool
dependency_is_fully_matched(MVDependency *dependency, Bitmapset *attnums)
{
	int			j;

	/*
	 * Check that the dependency actually is fully covered by clauses. We have
	 * to translate all attribute numbers, as those are referenced
	 */
	for (j = 0; j < dependency->nattributes; j++)
	{
		int			attnum = dependency->attributes[j];

		if (!bms_is_member(attnum, attnums))
			return false;
	}

	return true;
}

/*
 * statext_dependencies_load
 *		Load the functional dependencies for the indicated pg_statistic_ext tuple
 */
MVDependencies *
statext_dependencies_load(Oid mvoid, bool inh)
{
	MVDependencies *result;
	bool		isnull;
	Datum		deps;
	HeapTuple	htup;

	htup = SearchSysCache2(STATEXTDATASTXOID,
						   ObjectIdGetDatum(mvoid),
						   BoolGetDatum(inh));
	if (!HeapTupleIsValid(htup))
		elog(ERROR, "cache lookup failed for statistics object %u", mvoid);

	deps = SysCacheGetAttr(STATEXTDATASTXOID, htup,
						   Anum_pg_statistic_ext_data_stxddependencies, &isnull);
	if (isnull)
		elog(ERROR,
			 "requested statistics kind \"%c\" is not yet built for statistics object %u",
			 STATS_EXT_DEPENDENCIES, mvoid);

	result = statext_dependencies_deserialize(DatumGetByteaPP(deps));

	ReleaseSysCache(htup);

	return result;
}

/*
 * pg_dependencies_in		- input routine for type pg_dependencies.
 *
 * pg_dependencies is real enough to be a table column, but it has no operations
 * of its own, and disallows input too
 */
Datum
pg_dependencies_in(PG_FUNCTION_ARGS)
{
	/*
	 * pg_node_list stores the data in binary form and parsing text input is
	 * not needed, so disallow this.
	 */
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot accept a value of type %s", "pg_dependencies")));

	PG_RETURN_VOID();			/* keep compiler quiet */
}

/*
 * pg_dependencies		- output routine for type pg_dependencies.
 */
Datum
pg_dependencies_out(PG_FUNCTION_ARGS)
{
	bytea	   *data = PG_GETARG_BYTEA_PP(0);
	MVDependencies *dependencies = statext_dependencies_deserialize(data);
	int			i,
				j;
	StringInfoData str;

	initStringInfo(&str);
	appendStringInfoChar(&str, '{');

	for (i = 0; i < dependencies->ndeps; i++)
	{
		MVDependency *dependency = dependencies->deps[i];

		if (i > 0)
			appendStringInfoString(&str, ", ");

		appendStringInfoChar(&str, '"');
		for (j = 0; j < dependency->nattributes; j++)
		{
			if (j == dependency->nattributes - 1)
				appendStringInfoString(&str, " => ");
			else if (j > 0)
				appendStringInfoString(&str, ", ");

			appendStringInfo(&str, "%d", dependency->attributes[j]);
		}
		appendStringInfo(&str, "\": %f", dependency->degree);
	}

	appendStringInfoChar(&str, '}');

	PG_RETURN_CSTRING(str.data);
}

/*
 * pg_dependencies_recv		- binary input routine for type pg_dependencies.
 */
Datum
pg_dependencies_recv(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot accept a value of type %s", "pg_dependencies")));

	PG_RETURN_VOID();			/* keep compiler quiet */
}

/*
 * pg_dependencies_send		- binary output routine for type pg_dependencies.
 *
 * Functional dependencies are serialized in a bytea value (although the type
 * is named differently), so let's just send that.
 */
Datum
pg_dependencies_send(PG_FUNCTION_ARGS)
{
	return byteasend(fcinfo);
}

/*
 * dependency_is_compatible_clause
 *		Determines if the clause is compatible with functional dependencies
 *
 * Only clauses that have the form of equality to a pseudoconstant, or can be
 * interpreted that way, are currently accepted.  Furthermore the variable
 * part of the clause must be a simple Var belonging to the specified
 * relation, whose attribute number we return in *attnum on success.
 */
static bool
dependency_is_compatible_clause(Node *clause, Index relid, AttrNumber *attnum)
{
	Var		   *var;
	Node	   *clause_expr;

	if (IsA(clause, RestrictInfo))
	{
		RestrictInfo *rinfo = (RestrictInfo *) clause;

		/* Pseudoconstants are not interesting (they couldn't contain a Var) */
		if (rinfo->pseudoconstant)
			return false;

		/* Clauses referencing multiple, or no, varnos are incompatible */
		if (bms_membership(rinfo->clause_relids) != BMS_SINGLETON)
			return false;

		clause = (Node *) rinfo->clause;
	}

	if (is_opclause(clause))
	{
		/* If it's an opclause, check for Var = Const or Const = Var. */
		OpExpr	   *expr = (OpExpr *) clause;

		/* Only expressions with two arguments are candidates. */
		if (list_length(expr->args) != 2)
			return false;

		/* Make sure non-selected argument is a pseudoconstant. */
		if (is_pseudo_constant_clause(lsecond(expr->args)))
			clause_expr = linitial(expr->args);
		else if (is_pseudo_constant_clause(linitial(expr->args)))
			clause_expr = lsecond(expr->args);
		else
			return false;

		/*
		 * If it's not an "=" operator, just ignore the clause, as it's not
		 * compatible with functional dependencies.
		 *
		 * This uses the function for estimating selectivity, not the operator
		 * directly (a bit awkward, but well ...).
		 *
		 * XXX this is pretty dubious; probably it'd be better to check btree
		 * or hash opclass membership, so as not to be fooled by custom
		 * selectivity functions, and to be more consistent with decisions
		 * elsewhere in the planner.
		 */
		if (get_oprrest(expr->opno) != F_EQSEL)
			return false;

		/* OK to proceed with checking "var" */
	}
	else if (IsA(clause, ScalarArrayOpExpr))
	{
		/* If it's an scalar array operator, check for Var IN Const. */
		ScalarArrayOpExpr *expr = (ScalarArrayOpExpr *) clause;

		/*
		 * Reject ALL() variant, we only care about ANY/IN.
		 *
		 * XXX Maybe we should check if all the values are the same, and allow
		 * ALL in that case? Doesn't seem very practical, though.
		 */
		if (!expr->useOr)
			return false;

		/* Only expressions with two arguments are candidates. */
		if (list_length(expr->args) != 2)
			return false;

		/*
		 * We know it's always (Var IN Const), so we assume the var is the
		 * first argument, and pseudoconstant is the second one.
		 */
		if (!is_pseudo_constant_clause(lsecond(expr->args)))
			return false;

		clause_expr = linitial(expr->args);

		/*
		 * If it's not an "=" operator, just ignore the clause, as it's not
		 * compatible with functional dependencies. The operator is identified
		 * simply by looking at which function it uses to estimate
		 * selectivity. That's a bit strange, but it's what other similar
		 * places do.
		 */
		if (get_oprrest(expr->opno) != F_EQSEL)
			return false;

		/* OK to proceed with checking "var" */
	}
	else if (is_orclause(clause))
	{
		BoolExpr   *bool_expr = (BoolExpr *) clause;
		ListCell   *lc;

		/* start with no attribute number */
		*attnum = InvalidAttrNumber;

		foreach(lc, bool_expr->args)
		{
			AttrNumber	clause_attnum;

			/*
			 * Had we found incompatible clause in the arguments, treat the
			 * whole clause as incompatible.
			 */
			if (!dependency_is_compatible_clause((Node *) lfirst(lc),
												 relid, &clause_attnum))
				return false;

			if (*attnum == InvalidAttrNumber)
				*attnum = clause_attnum;

			/* ensure all the variables are the same (same attnum) */
			if (*attnum != clause_attnum)
				return false;
		}

		/* the Var is already checked by the recursive call */
		return true;
	}
	else if (is_notclause(clause))
	{
		/*
		 * "NOT x" can be interpreted as "x = false", so get the argument and
		 * proceed with seeing if it's a suitable Var.
		 */
		clause_expr = (Node *) get_notclausearg(clause);
	}
	else
	{
		/*
		 * A boolean expression "x" can be interpreted as "x = true", so
		 * proceed with seeing if it's a suitable Var.
		 */
		clause_expr = (Node *) clause;
	}

	/*
	 * We may ignore any RelabelType node above the operand.  (There won't be
	 * more than one, since eval_const_expressions has been applied already.)
	 */
	if (IsA(clause_expr, RelabelType))
		clause_expr = (Node *) ((RelabelType *) clause_expr)->arg;

	/* We only support plain Vars for now */
	if (!IsA(clause_expr, Var))
		return false;

	/* OK, we know we have a Var */
	var = (Var *) clause_expr;

	/* Ensure Var is from the correct relation */
	if (var->varno != relid)
		return false;

	/* We also better ensure the Var is from the current level */
	if (var->varlevelsup != 0)
		return false;

	/* Also ignore system attributes (we don't allow stats on those) */
	if (!AttrNumberIsForUserDefinedAttr(var->varattno))
		return false;

	*attnum = var->varattno;
	return true;
}

/*
 * find_strongest_dependency
 *		find the strongest dependency on the attributes
 *
 * When applying functional dependencies, we start with the strongest
 * dependencies. That is, we select the dependency that:
 *
 * (a) has all attributes covered by equality clauses
 *
 * (b) has the most attributes
 *
 * (c) has the highest degree of validity
 *
 * This guarantees that we eliminate the most redundant conditions first
 * (see the comment in dependencies_clauselist_selectivity).
 */
static MVDependency *
find_strongest_dependency(MVDependencies **dependencies, int ndependencies,
						  Bitmapset *attnums)
{
	int			i,
				j;
	MVDependency *strongest = NULL;

	/* number of attnums in clauses */
	int			nattnums = bms_num_members(attnums);

	/*
	 * Iterate over the MVDependency items and find the strongest one from the
	 * fully-matched dependencies. We do the cheap checks first, before
	 * matching it against the attnums.
	 */
	for (i = 0; i < ndependencies; i++)
	{
		for (j = 0; j < dependencies[i]->ndeps; j++)
		{
			MVDependency *dependency = dependencies[i]->deps[j];

			/*
			 * Skip dependencies referencing more attributes than available
			 * clauses, as those can't be fully matched.
			 */
			if (dependency->nattributes > nattnums)
				continue;

			if (strongest)
			{
				/* skip dependencies on fewer attributes than the strongest. */
				if (dependency->nattributes < strongest->nattributes)
					continue;

				/* also skip weaker dependencies when attribute count matches */
				if (strongest->nattributes == dependency->nattributes &&
					strongest->degree > dependency->degree)
					continue;
			}

			/*
			 * this dependency is stronger, but we must still check that it's
			 * fully matched to these attnums. We perform this check last as
			 * it's slightly more expensive than the previous checks.
			 */
			if (dependency_is_fully_matched(dependency, attnums))
				strongest = dependency; /* save new best match */
		}
	}

	return strongest;
}

/*
 * clauselist_apply_dependencies
 *		Apply the specified functional dependencies to a list of clauses and
 *		return the estimated selectivity of the clauses that are compatible
 *		with any of the given dependencies.
 *
 * This will estimate all not-already-estimated clauses that are compatible
 * with functional dependencies, and which have an attribute mentioned by any
 * of the given dependencies (either as an implying or implied attribute).
 *
 * Given (lists of) clauses on attributes (a,b) and a functional dependency
 * (a=>b), the per-column selectivities P(a) and P(b) are notionally combined
 * using the formula
 *
 *		P(a,b) = f * P(a) + (1-f) * P(a) * P(b)
 *
 * where 'f' is the degree of dependency.  This reflects the fact that we
 * expect a fraction f of all rows to be consistent with the dependency
 * (a=>b), and so have a selectivity of P(a), while the remaining rows are
 * treated as independent.
 *
 * In practice, we use a slightly modified version of this formula, which uses
 * a selectivity of Min(P(a), P(b)) for the dependent rows, since the result
 * should obviously not exceed either column's individual selectivity.  I.e.,
 * we actually combine selectivities using the formula
 *
 *		P(a,b) = f * Min(P(a), P(b)) + (1-f) * P(a) * P(b)
 *
 * This can make quite a difference if the specific values matching the
 * clauses are not consistent with the functional dependency.
 */
static Selectivity
clauselist_apply_dependencies(PlannerInfo *root, List *clauses,
							  int varRelid, JoinType jointype,
							  SpecialJoinInfo *sjinfo,
							  MVDependency **dependencies, int ndependencies,
							  AttrNumber *list_attnums,
							  Bitmapset **estimatedclauses)
{
	Bitmapset  *attnums;
	int			i;
	int			j;
	int			nattrs;
	Selectivity *attr_sel;
	int			attidx;
	int			listidx;
	ListCell   *l;
	Selectivity s1;

	/*
	 * Extract the attnums of all implying and implied attributes from all the
	 * given dependencies.  Each of these attributes is expected to have at
	 * least 1 not-already-estimated compatible clause that we will estimate
	 * here.
	 */
	attnums = NULL;
	for (i = 0; i < ndependencies; i++)
	{
		for (j = 0; j < dependencies[i]->nattributes; j++)
		{
			AttrNumber	attnum = dependencies[i]->attributes[j];

			attnums = bms_add_member(attnums, attnum);
		}
	}

	/*
	 * Compute per-column selectivity estimates for each of these attributes,
	 * and mark all the corresponding clauses as estimated.
	 */
	nattrs = bms_num_members(attnums);
	attr_sel = (Selectivity *) palloc(sizeof(Selectivity) * nattrs);

	attidx = 0;
	i = -1;
	while ((i = bms_next_member(attnums, i)) >= 0)
	{
		List	   *attr_clauses = NIL;
		Selectivity simple_sel;

		listidx = -1;
		foreach(l, clauses)
		{
			Node	   *clause = (Node *) lfirst(l);

			listidx++;
			if (list_attnums[listidx] == i)
			{
				attr_clauses = lappend(attr_clauses, clause);
				*estimatedclauses = bms_add_member(*estimatedclauses, listidx);
			}
		}

		simple_sel = clauselist_selectivity_ext(root, attr_clauses, varRelid,
												jointype, sjinfo, false);
		attr_sel[attidx++] = simple_sel;
	}

	/*
	 * Now combine these selectivities using the dependency information.  For
	 * chains of dependencies such as a -> b -> c, the b -> c dependency will
	 * come before the a -> b dependency in the array, so we traverse the
	 * array backwards to ensure such chains are computed in the right order.
	 *
	 * As explained above, pairs of selectivities are combined using the
	 * formula
	 *
	 * P(a,b) = f * Min(P(a), P(b)) + (1-f) * P(a) * P(b)
	 *
	 * to ensure that the combined selectivity is never greater than either
	 * individual selectivity.
	 *
	 * Where multiple dependencies apply (e.g., a -> b -> c), we use
	 * conditional probabilities to compute the overall result as follows:
	 *
	 * P(a,b,c) = P(c|a,b) * P(a,b) = P(c|a,b) * P(b|a) * P(a)
	 *
	 * so we replace the selectivities of all implied attributes with
	 * conditional probabilities, that are conditional on all their implying
	 * attributes.  The selectivities of all other non-implied attributes are
	 * left as they are.
	 */
	for (i = ndependencies - 1; i >= 0; i--)
	{
		MVDependency *dependency = dependencies[i];
		AttrNumber	attnum;
		Selectivity s2;
		double		f;

		/* Selectivity of all the implying attributes */
		s1 = 1.0;
		for (j = 0; j < dependency->nattributes - 1; j++)
		{
			attnum = dependency->attributes[j];
			attidx = bms_member_index(attnums, attnum);
			s1 *= attr_sel[attidx];
		}

		/* Original selectivity of the implied attribute */
		attnum = dependency->attributes[j];
		attidx = bms_member_index(attnums, attnum);
		s2 = attr_sel[attidx];

		/*
		 * Replace s2 with the conditional probability s2 given s1, computed
		 * using the formula P(b|a) = P(a,b) / P(a), which simplifies to
		 *
		 * P(b|a) = f * Min(P(a), P(b)) / P(a) + (1-f) * P(b)
		 *
		 * where P(a) = s1, the selectivity of the implying attributes, and
		 * P(b) = s2, the selectivity of the implied attribute.
		 */
		f = dependency->degree;

		if (s1 <= s2)
			attr_sel[attidx] = f + (1 - f) * s2;
		else
			attr_sel[attidx] = f * s2 / s1 + (1 - f) * s2;
	}

	/*
	 * The overall selectivity of all the clauses on all these attributes is
	 * then the product of all the original (non-implied) probabilities and
	 * the new conditional (implied) probabilities.
	 */
	s1 = 1.0;
	for (i = 0; i < nattrs; i++)
		s1 *= attr_sel[i];

	CLAMP_PROBABILITY(s1);

	pfree(attr_sel);
	bms_free(attnums);

	return s1;
}

/*
 * dependency_is_compatible_expression
 *		Determines if the expression is compatible with functional dependencies
 *
 * Similar to dependency_is_compatible_clause, but doesn't enforce that the
 * expression is a simple Var. OTOH we check that there's at least one
 * statistics object matching the expression.
 */
static bool
dependency_is_compatible_expression(Node *clause, Index relid, List *statlist, Node **expr)
{
	List	   *vars;
	ListCell   *lc,
			   *lc2;
	Node	   *clause_expr;

	if (IsA(clause, RestrictInfo))
	{
		RestrictInfo *rinfo = (RestrictInfo *) clause;

		/* Pseudoconstants are not interesting (they couldn't contain a Var) */
		if (rinfo->pseudoconstant)
			return false;

		/* Clauses referencing multiple, or no, varnos are incompatible */
		if (bms_membership(rinfo->clause_relids) != BMS_SINGLETON)
			return false;

		clause = (Node *) rinfo->clause;
	}

	if (is_opclause(clause))
	{
		/* If it's an opclause, check for Var = Const or Const = Var. */
		OpExpr	   *expr = (OpExpr *) clause;

		/* Only expressions with two arguments are candidates. */
		if (list_length(expr->args) != 2)
			return false;

		/* Make sure non-selected argument is a pseudoconstant. */
		if (is_pseudo_constant_clause(lsecond(expr->args)))
			clause_expr = linitial(expr->args);
		else if (is_pseudo_constant_clause(linitial(expr->args)))
			clause_expr = lsecond(expr->args);
		else
			return false;

		/*
		 * If it's not an "=" operator, just ignore the clause, as it's not
		 * compatible with functional dependencies.
		 *
		 * This uses the function for estimating selectivity, not the operator
		 * directly (a bit awkward, but well ...).
		 *
		 * XXX this is pretty dubious; probably it'd be better to check btree
		 * or hash opclass membership, so as not to be fooled by custom
		 * selectivity functions, and to be more consistent with decisions
		 * elsewhere in the planner.
		 */
		if (get_oprrest(expr->opno) != F_EQSEL)
			return false;

		/* OK to proceed with checking "var" */
	}
	else if (IsA(clause, ScalarArrayOpExpr))
	{
		/* If it's an scalar array operator, check for Var IN Const. */
		ScalarArrayOpExpr *expr = (ScalarArrayOpExpr *) clause;

		/*
		 * Reject ALL() variant, we only care about ANY/IN.
		 *
		 * FIXME Maybe we should check if all the values are the same, and
		 * allow ALL in that case? Doesn't seem very practical, though.
		 */
		if (!expr->useOr)
			return false;

		/* Only expressions with two arguments are candidates. */
		if (list_length(expr->args) != 2)
			return false;

		/*
		 * We know it's always (Var IN Const), so we assume the var is the
		 * first argument, and pseudoconstant is the second one.
		 */
		if (!is_pseudo_constant_clause(lsecond(expr->args)))
			return false;

		clause_expr = linitial(expr->args);

		/*
		 * If it's not an "=" operator, just ignore the clause, as it's not
		 * compatible with functional dependencies. The operator is identified
		 * simply by looking at which function it uses to estimate
		 * selectivity. That's a bit strange, but it's what other similar
		 * places do.
		 */
		if (get_oprrest(expr->opno) != F_EQSEL)
			return false;

		/* OK to proceed with checking "var" */
	}
	else if (is_orclause(clause))
	{
		BoolExpr   *bool_expr = (BoolExpr *) clause;
		ListCell   *lc;

		/* start with no expression (we'll use the first match) */
		*expr = NULL;

		foreach(lc, bool_expr->args)
		{
			Node	   *or_expr = NULL;

			/*
			 * Had we found incompatible expression in the arguments, treat
			 * the whole expression as incompatible.
			 */
			if (!dependency_is_compatible_expression((Node *) lfirst(lc), relid,
													 statlist, &or_expr))
				return false;

			if (*expr == NULL)
				*expr = or_expr;

			/* ensure all the expressions are the same */
			if (!equal(or_expr, *expr))
				return false;
		}

		/* the expression is already checked by the recursive call */
		return true;
	}
	else if (is_notclause(clause))
	{
		/*
		 * "NOT x" can be interpreted as "x = false", so get the argument and
		 * proceed with seeing if it's a suitable Var.
		 */
		clause_expr = (Node *) get_notclausearg(clause);
	}
	else
	{
		/*
		 * A boolean expression "x" can be interpreted as "x = true", so
		 * proceed with seeing if it's a suitable Var.
		 */
		clause_expr = (Node *) clause;
	}

	/*
	 * We may ignore any RelabelType node above the operand.  (There won't be
	 * more than one, since eval_const_expressions has been applied already.)
	 */
	if (IsA(clause_expr, RelabelType))
		clause_expr = (Node *) ((RelabelType *) clause_expr)->arg;

	vars = pull_var_clause(clause_expr, 0);

	foreach(lc, vars)
	{
		Var		   *var = (Var *) lfirst(lc);

		/* Ensure Var is from the correct relation */
		if (var->varno != relid)
			return false;

		/* We also better ensure the Var is from the current level */
		if (var->varlevelsup != 0)
			return false;

		/* Also ignore system attributes (we don't allow stats on those) */
		if (!AttrNumberIsForUserDefinedAttr(var->varattno))
			return false;
	}

	/*
	 * Check if we actually have a matching statistics for the expression.
	 *
	 * XXX Maybe this is an overkill. We'll eliminate the expressions later.
	 */
	foreach(lc, statlist)
	{
		StatisticExtInfo *info = (StatisticExtInfo *) lfirst(lc);

		/* ignore stats without dependencies */
		if (info->kind != STATS_EXT_DEPENDENCIES)
			continue;

		foreach(lc2, info->exprs)
		{
			Node	   *stat_expr = (Node *) lfirst(lc2);

			if (equal(clause_expr, stat_expr))
			{
				*expr = stat_expr;
				return true;
			}
		}
	}

	return false;
}

/*
 * dependencies_clauselist_selectivity
 *		Return the estimated selectivity of (a subset of) the given clauses
 *		using functional dependency statistics, or 1.0 if no useful functional
 *		dependency statistic exists.
 *
 * 'estimatedclauses' is an input/output argument that gets a bit set
 * corresponding to the (zero-based) list index of each clause that is included
 * in the estimated selectivity.
 *
 * Given equality clauses on attributes (a,b) we find the strongest dependency
 * between them, i.e. either (a=>b) or (b=>a). Assuming (a=>b) is the selected
 * dependency, we then combine the per-clause selectivities using the formula
 *
 *	   P(a,b) = f * P(a) + (1-f) * P(a) * P(b)
 *
 * where 'f' is the degree of the dependency.  (Actually we use a slightly
 * modified version of this formula -- see clauselist_apply_dependencies()).
 *
 * With clauses on more than two attributes, the dependencies are applied
 * recursively, starting with the widest/strongest dependencies. For example
 * P(a,b,c) is first split like this:
 *
 *	   P(a,b,c) = f * P(a,b) + (1-f) * P(a,b) * P(c)
 *
 * assuming (a,b=>c) is the strongest dependency.
 */
Selectivity
dependencies_clauselist_selectivity(PlannerInfo *root,
									List *clauses,
									int varRelid,
									JoinType jointype,
									SpecialJoinInfo *sjinfo,
									RelOptInfo *rel,
									Bitmapset **estimatedclauses)
{
	Selectivity s1 = 1.0;
	ListCell   *l;
	Bitmapset  *clauses_attnums = NULL;
	AttrNumber *list_attnums;
	int			listidx;
	MVDependencies **func_dependencies;
	int			nfunc_dependencies;
	int			total_ndeps;
	MVDependency **dependencies;
	int			ndependencies;
	int			i;
	AttrNumber	attnum_offset;
	RangeTblEntry *rte = planner_rt_fetch(rel->relid, root);

	/* unique expressions */
	Node	  **unique_exprs;
	int			unique_exprs_cnt;

	/* check if there's any stats that might be useful for us. */
	if (!has_stats_of_kind(rel->statlist, STATS_EXT_DEPENDENCIES))
		return 1.0;

	list_attnums = (AttrNumber *) palloc(sizeof(AttrNumber) *
										 list_length(clauses));

	/*
	 * We allocate space as if every clause was a unique expression, although
	 * that's probably overkill. Some will be simple column references that
	 * we'll translate to attnums, and there might be duplicates. But it's
	 * easier and cheaper to just do one allocation than repalloc later.
	 */
	unique_exprs = (Node **) palloc(sizeof(Node *) * list_length(clauses));
	unique_exprs_cnt = 0;

	/*
	 * Pre-process the clauses list to extract the attnums seen in each item.
	 * We need to determine if there's any clauses which will be useful for
	 * dependency selectivity estimations. Along the way we'll record all of
	 * the attnums for each clause in a list which we'll reference later so we
	 * don't need to repeat the same work again. We'll also keep track of all
	 * attnums seen.
	 *
	 * We also skip clauses that we already estimated using different types of
	 * statistics (we treat them as incompatible).
	 *
	 * To handle expressions, we assign them negative attnums, as if it was a
	 * system attribute (this is fine, as we only allow extended stats on user
	 * attributes). And then we offset everything by the number of
	 * expressions, so that we can store the values in a bitmapset.
	 */
	listidx = 0;
	foreach(l, clauses)
	{
		Node	   *clause = (Node *) lfirst(l);
		AttrNumber	attnum;
		Node	   *expr = NULL;

		/* ignore clause by default */
		list_attnums[listidx] = InvalidAttrNumber;

		if (!bms_is_member(listidx, *estimatedclauses))
		{
			/*
			 * If it's a simple column reference, just extract the attnum. If
			 * it's an expression, assign a negative attnum as if it was a
			 * system attribute.
			 */
			if (dependency_is_compatible_clause(clause, rel->relid, &attnum))
			{
				list_attnums[listidx] = attnum;
			}
			else if (dependency_is_compatible_expression(clause, rel->relid,
														 rel->statlist,
														 &expr))
			{
				/* special attnum assigned to this expression */
				attnum = InvalidAttrNumber;

				Assert(expr != NULL);

				/* If the expression is duplicate, use the same attnum. */
				for (i = 0; i < unique_exprs_cnt; i++)
				{
					if (equal(unique_exprs[i], expr))
					{
						/* negative attribute number to expression */
						attnum = -(i + 1);
						break;
					}
				}

				/* not found in the list, so add it */
				if (attnum == InvalidAttrNumber)
				{
					unique_exprs[unique_exprs_cnt++] = expr;

					/* after incrementing the value, to get -1, -2, ... */
					attnum = (-unique_exprs_cnt);
				}

				/* remember which attnum was assigned to this clause */
				list_attnums[listidx] = attnum;
			}
		}

		listidx++;
	}

	Assert(listidx == list_length(clauses));

	/*
	 * How much we need to offset the attnums? If there are no expressions,
	 * then no offset is needed. Otherwise we need to offset enough for the
	 * lowest value (-unique_exprs_cnt) to become 1.
	 */
	if (unique_exprs_cnt > 0)
		attnum_offset = (unique_exprs_cnt + 1);
	else
		attnum_offset = 0;

	/*
	 * Now that we know how many expressions there are, we can offset the
	 * values just enough to build the bitmapset.
	 */
	for (i = 0; i < list_length(clauses); i++)
	{
		AttrNumber	attnum;

		/* ignore incompatible or already estimated clauses */
		if (list_attnums[i] == InvalidAttrNumber)
			continue;

		/* make sure the attnum is in the expected range */
		Assert(list_attnums[i] >= (-unique_exprs_cnt));
		Assert(list_attnums[i] <= MaxHeapAttributeNumber);

		/* make sure the attnum is positive (valid AttrNumber) */
		attnum = list_attnums[i] + attnum_offset;

		/*
		 * Either it's a regular attribute, or it's an expression, in which
		 * case we must not have seen it before (expressions are unique).
		 *
		 * XXX Check whether it's a regular attribute has to be done using the
		 * original attnum, while the second check has to use the value with
		 * an offset.
		 */
		Assert(AttrNumberIsForUserDefinedAttr(list_attnums[i]) ||
			   !bms_is_member(attnum, clauses_attnums));

		/*
		 * Remember the offset attnum, both for attributes and expressions.
		 * We'll pass list_attnums to clauselist_apply_dependencies, which
		 * uses it to identify clauses in a bitmap. We could also pass the
		 * offset, but this is more convenient.
		 */
		list_attnums[i] = attnum;

		clauses_attnums = bms_add_member(clauses_attnums, attnum);
	}

	/*
	 * If there's not at least two distinct attnums and expressions, then
	 * reject the whole list of clauses. We must return 1.0 so the calling
	 * function's selectivity is unaffected.
	 */
	if (bms_membership(clauses_attnums) != BMS_MULTIPLE)
	{
		bms_free(clauses_attnums);
		pfree(list_attnums);
		return 1.0;
	}

	/*
	 * Load all functional dependencies matching at least two parameters. We
	 * can simply consider all dependencies at once, without having to search
	 * for the best statistics object.
	 *
	 * To not waste cycles and memory, we deserialize dependencies only for
	 * statistics that match at least two attributes. The array is allocated
	 * with the assumption that all objects match - we could grow the array to
	 * make it just the right size, but it's likely wasteful anyway thanks to
	 * moving the freed chunks to freelists etc.
	 */
	func_dependencies = (MVDependencies **) palloc(sizeof(MVDependencies *) *
												   list_length(rel->statlist));
	nfunc_dependencies = 0;
	total_ndeps = 0;

	foreach(l, rel->statlist)
	{
		StatisticExtInfo *stat = (StatisticExtInfo *) lfirst(l);
		int			nmatched;
		int			nexprs;
		int			k;
		MVDependencies *deps;

		/* skip statistics that are not of the correct type */
		if (stat->kind != STATS_EXT_DEPENDENCIES)
			continue;

		/* skip statistics with mismatching stxdinherit value */
		if (stat->inherit != rte->inh)
			continue;

		/*
		 * Count matching attributes - we have to undo the attnum offsets. The
		 * input attribute numbers are not offset (expressions are not
		 * included in stat->keys, so it's not necessary). But we need to
		 * offset it before checking against clauses_attnums.
		 */
		nmatched = 0;
		k = -1;
		while ((k = bms_next_member(stat->keys, k)) >= 0)
		{
			AttrNumber	attnum = (AttrNumber) k;

			/* skip expressions */
			if (!AttrNumberIsForUserDefinedAttr(attnum))
				continue;

			/* apply the same offset as above */
			attnum += attnum_offset;

			if (bms_is_member(attnum, clauses_attnums))
				nmatched++;
		}

		/* count matching expressions */
		nexprs = 0;
		for (i = 0; i < unique_exprs_cnt; i++)
		{
			ListCell   *lc;

			foreach(lc, stat->exprs)
			{
				Node	   *stat_expr = (Node *) lfirst(lc);

				/* try to match it */
				if (equal(stat_expr, unique_exprs[i]))
					nexprs++;
			}
		}

		/*
		 * Skip objects matching fewer than two attributes/expressions from
		 * clauses.
		 */
		if (nmatched + nexprs < 2)
			continue;

		deps = statext_dependencies_load(stat->statOid, rte->inh);

		/*
		 * The expressions may be represented by different attnums in the
		 * stats, we need to remap them to be consistent with the clauses.
		 * That will make the later steps (e.g. picking the strongest item and
		 * so on) much simpler and cheaper, because it won't need to care
		 * about the offset at all.
		 *
		 * When we're at it, we can ignore dependencies that are not fully
		 * matched by clauses (i.e. referencing attributes or expressions that
		 * are not in the clauses).
		 *
		 * We have to do this for all statistics, as long as there are any
		 * expressions - we need to shift the attnums in all dependencies.
		 *
		 * XXX Maybe we should do this always, because it also eliminates some
		 * of the dependencies early. It might be cheaper than having to walk
		 * the longer list in find_strongest_dependency later, especially as
		 * we need to do that repeatedly?
		 *
		 * XXX We have to do this even when there are no expressions in
		 * clauses, otherwise find_strongest_dependency may fail for stats
		 * with expressions (due to lookup of negative value in bitmap). So we
		 * need to at least filter out those dependencies. Maybe we could do
		 * it in a cheaper way (if there are no expr clauses, we can just
		 * discard all negative attnums without any lookups).
		 */
		if (unique_exprs_cnt > 0 || stat->exprs != NIL)
		{
			int			ndeps = 0;

			for (i = 0; i < deps->ndeps; i++)
			{
				bool		skip = false;
				MVDependency *dep = deps->deps[i];
				int			j;

				for (j = 0; j < dep->nattributes; j++)
				{
					int			idx;
					Node	   *expr;
					int			k;
					AttrNumber	unique_attnum = InvalidAttrNumber;
					AttrNumber	attnum;

					/* undo the per-statistics offset */
					attnum = dep->attributes[j];

					/*
					 * For regular attributes we can simply check if it
					 * matches any clause. If there's no matching clause, we
					 * can just ignore it. We need to offset the attnum
					 * though.
					 */
					if (AttrNumberIsForUserDefinedAttr(attnum))
					{
						dep->attributes[j] = attnum + attnum_offset;

						if (!bms_is_member(dep->attributes[j], clauses_attnums))
						{
							skip = true;
							break;
						}

						continue;
					}

					/*
					 * the attnum should be a valid system attnum (-1, -2,
					 * ...)
					 */
					Assert(AttributeNumberIsValid(attnum));

					/*
					 * For expressions, we need to do two translations. First
					 * we have to translate the negative attnum to index in
					 * the list of expressions (in the statistics object).
					 * Then we need to see if there's a matching clause. The
					 * index of the unique expression determines the attnum
					 * (and we offset it).
					 */
					idx = -(1 + attnum);

					/* Is the expression index is valid? */
					Assert((idx >= 0) && (idx < list_length(stat->exprs)));

					expr = (Node *) list_nth(stat->exprs, idx);

					/* try to find the expression in the unique list */
					for (k = 0; k < unique_exprs_cnt; k++)
					{
						/*
						 * found a matching unique expression, use the attnum
						 * (derived from index of the unique expression)
						 */
						if (equal(unique_exprs[k], expr))
						{
							unique_attnum = -(k + 1) + attnum_offset;
							break;
						}
					}

					/*
					 * Found no matching expression, so we can simply skip
					 * this dependency, because there's no chance it will be
					 * fully covered.
					 */
					if (unique_attnum == InvalidAttrNumber)
					{
						skip = true;
						break;
					}

					/* otherwise remap it to the new attnum */
					dep->attributes[j] = unique_attnum;
				}

				/* if found a matching dependency, keep it */
				if (!skip)
				{
					/* maybe we've skipped something earlier, so move it */
					if (ndeps != i)
						deps->deps[ndeps] = deps->deps[i];

					ndeps++;
				}
			}

			deps->ndeps = ndeps;
		}

		/*
		 * It's possible we've removed all dependencies, in which case we
		 * don't bother adding it to the list.
		 */
		if (deps->ndeps > 0)
		{
			func_dependencies[nfunc_dependencies] = deps;
			total_ndeps += deps->ndeps;
			nfunc_dependencies++;
		}
	}

	/* if no matching stats could be found then we've nothing to do */
	if (nfunc_dependencies == 0)
	{
		pfree(func_dependencies);
		bms_free(clauses_attnums);
		pfree(list_attnums);
		pfree(unique_exprs);
		return 1.0;
	}

	/*
	 * Work out which dependencies we can apply, starting with the
	 * widest/strongest ones, and proceeding to smaller/weaker ones.
	 */
	dependencies = (MVDependency **) palloc(sizeof(MVDependency *) *
											total_ndeps);
	ndependencies = 0;

	while (true)
	{
		MVDependency *dependency;
		AttrNumber	attnum;

		/* the widest/strongest dependency, fully matched by clauses */
		dependency = find_strongest_dependency(func_dependencies,
											   nfunc_dependencies,
											   clauses_attnums);
		if (!dependency)
			break;

		dependencies[ndependencies++] = dependency;

		/* Ignore dependencies using this implied attribute in later loops */
		attnum = dependency->attributes[dependency->nattributes - 1];
		clauses_attnums = bms_del_member(clauses_attnums, attnum);
	}

	/*
	 * If we found applicable dependencies, use them to estimate all
	 * compatible clauses on attributes that they refer to.
	 */
	if (ndependencies != 0)
		s1 = clauselist_apply_dependencies(root, clauses, varRelid, jointype,
										   sjinfo, dependencies, ndependencies,
										   list_attnums, estimatedclauses);

	/* free deserialized functional dependencies (and then the array) */
	for (i = 0; i < nfunc_dependencies; i++)
		pfree(func_dependencies[i]);

	pfree(dependencies);
	pfree(func_dependencies);
	bms_free(clauses_attnums);
	pfree(list_attnums);
	pfree(unique_exprs);

	return s1;
}

/*-------------------------------------------------------------------------
 *
 * execGrouping.c
 *	  executor utility routines for grouping, hashing, and aggregation
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/execGrouping.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/parallel.h"
#include "common/hashfn.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "utils/lsyscache.h"

static int	TupleHashTableMatch(struct tuplehash_hash *tb, const MinimalTuple tuple1, const MinimalTuple tuple2);
static inline uint32 TupleHashTableHash_internal(struct tuplehash_hash *tb,
												 const MinimalTuple tuple);
static inline TupleHashEntry LookupTupleHashEntry_internal(TupleHashTable hashtable,
														   TupleTableSlot *slot,
														   bool *isnew, uint32 hash);

/*
 * Define parameters for tuple hash table code generation. The interface is
 * *also* declared in execnodes.h (to generate the types, which are externally
 * visible).
 */
#define SH_PREFIX tuplehash
#define SH_ELEMENT_TYPE TupleHashEntryData
#define SH_KEY_TYPE MinimalTuple
#define SH_KEY firstTuple
#define SH_HASH_KEY(tb, key) TupleHashTableHash_internal(tb, key)
#define SH_EQUAL(tb, a, b) TupleHashTableMatch(tb, a, b) == 0
#define SH_SCOPE extern
#define SH_STORE_HASH
#define SH_GET_HASH(tb, a) a->hash
#define SH_DEFINE
#include "lib/simplehash.h"


/*****************************************************************************
 *		Utility routines for grouping tuples together
 *****************************************************************************/

/*
 * execTuplesMatchPrepare
 *		Build expression that can be evaluated using ExecQual(), returning
 *		whether an ExprContext's inner/outer tuples are NOT DISTINCT
 */
ExprState *
execTuplesMatchPrepare(TupleDesc desc,
					   int numCols,
					   const AttrNumber *keyColIdx,
					   const Oid *eqOperators,
					   const Oid *collations,
					   PlanState *parent)
{
	Oid		   *eqFunctions;
	int			i;
	ExprState  *expr;

	if (numCols == 0)
		return NULL;

	eqFunctions = (Oid *) palloc(numCols * sizeof(Oid));

	/* lookup equality functions */
	for (i = 0; i < numCols; i++)
		eqFunctions[i] = get_opcode(eqOperators[i]);

	/* build actual expression */
	expr = ExecBuildGroupingEqual(desc, desc, NULL, NULL,
								  numCols, keyColIdx, eqFunctions, collations,
								  parent);

	return expr;
}

/*
 * execTuplesHashPrepare
 *		Look up the equality and hashing functions needed for a TupleHashTable.
 *
 * This is similar to execTuplesMatchPrepare, but we also need to find the
 * hash functions associated with the equality operators.  *eqFunctions and
 * *hashFunctions receive the palloc'd result arrays.
 *
 * Note: we expect that the given operators are not cross-type comparisons.
 */
void
execTuplesHashPrepare(int numCols,
					  const Oid *eqOperators,
					  Oid **eqFuncOids,
					  FmgrInfo **hashFunctions)
{
	int			i;

	*eqFuncOids = (Oid *) palloc(numCols * sizeof(Oid));
	*hashFunctions = (FmgrInfo *) palloc(numCols * sizeof(FmgrInfo));

	for (i = 0; i < numCols; i++)
	{
		Oid			eq_opr = eqOperators[i];
		Oid			eq_function;
		Oid			left_hash_function;
		Oid			right_hash_function;

		eq_function = get_opcode(eq_opr);
		if (!get_op_hash_functions(eq_opr,
								   &left_hash_function, &right_hash_function))
			elog(ERROR, "could not find hash function for hash operator %u",
				 eq_opr);
		/* We're not supporting cross-type cases here */
		Assert(left_hash_function == right_hash_function);
		(*eqFuncOids)[i] = eq_function;
		fmgr_info(right_hash_function, &(*hashFunctions)[i]);
	}
}


/*****************************************************************************
 *		Utility routines for all-in-memory hash tables
 *
 * These routines build hash tables for grouping tuples together (eg, for
 * hash aggregation).  There is one entry for each not-distinct set of tuples
 * presented.
 *****************************************************************************/

/*
 * Construct an empty TupleHashTable
 *
 *	numCols, keyColIdx: identify the tuple fields to use as lookup key
 *	eqfunctions: equality comparison functions to use
 *	hashfunctions: datatype-specific hashing functions to use
 *	nbuckets: initial estimate of hashtable size
 *	additionalsize: size of data stored in ->additional
 *	metacxt: memory context for long-lived allocation, but not per-entry data
 *	tablecxt: memory context in which to store table entries
 *	tempcxt: short-lived context for evaluation hash and comparison functions
 *
 * The function arrays may be made with execTuplesHashPrepare().  Note they
 * are not cross-type functions, but expect to see the table datatype(s)
 * on both sides.
 *
 * Note that keyColIdx, eqfunctions, and hashfunctions must be allocated in
 * storage that will live as long as the hashtable does.
 */
TupleHashTable
BuildTupleHashTableExt(PlanState *parent,
					   TupleDesc inputDesc,
					   int numCols, AttrNumber *keyColIdx,
					   const Oid *eqfuncoids,
					   FmgrInfo *hashfunctions,
					   Oid *collations,
					   long nbuckets, Size additionalsize,
					   MemoryContext metacxt,
					   MemoryContext tablecxt,
					   MemoryContext tempcxt,
					   bool use_variable_hash_iv)
{
	TupleHashTable hashtable;
	Size		entrysize = sizeof(TupleHashEntryData) + additionalsize;
	Size		hash_mem_limit;
	MemoryContext oldcontext;
	bool		allow_jit;
	uint32		hash_iv = 0;

	Assert(nbuckets > 0);

	/* Limit initial table size request to not more than hash_mem */
	hash_mem_limit = get_hash_memory_limit() / entrysize;
	if (nbuckets > hash_mem_limit)
		nbuckets = hash_mem_limit;

	oldcontext = MemoryContextSwitchTo(metacxt);

	hashtable = (TupleHashTable) palloc(sizeof(TupleHashTableData));

	hashtable->numCols = numCols;
	hashtable->keyColIdx = keyColIdx;
	hashtable->tab_collations = collations;
	hashtable->tablecxt = tablecxt;
	hashtable->tempcxt = tempcxt;
	hashtable->entrysize = entrysize;
	hashtable->tableslot = NULL;	/* will be made on first lookup */
	hashtable->inputslot = NULL;
	hashtable->in_hash_expr = NULL;
	hashtable->cur_eq_func = NULL;

	/*
	 * If parallelism is in use, even if the leader backend is performing the
	 * scan itself, we don't want to create the hashtable exactly the same way
	 * in all workers. As hashtables are iterated over in keyspace-order,
	 * doing so in all processes in the same way is likely to lead to
	 * "unbalanced" hashtables when the table size initially is
	 * underestimated.
	 */
	if (use_variable_hash_iv)
		hash_iv = murmurhash32(ParallelWorkerNumber);

	hashtable->hashtab = tuplehash_create(metacxt, nbuckets, hashtable);

	/*
	 * We copy the input tuple descriptor just for safety --- we assume all
	 * input tuples will have equivalent descriptors.
	 */
	hashtable->tableslot = MakeSingleTupleTableSlot(CreateTupleDescCopy(inputDesc),
													&TTSOpsMinimalTuple);

	/*
	 * If the old reset interface is used (i.e. BuildTupleHashTable, rather
	 * than BuildTupleHashTableExt), allowing JIT would lead to the generated
	 * functions to a) live longer than the query b) be re-generated each time
	 * the table is being reset. Therefore prevent JIT from being used in that
	 * case, by not providing a parent node (which prevents accessing the
	 * JitContext in the EState).
	 */
	allow_jit = metacxt != tablecxt;

	/* build hash ExprState for all columns */
	hashtable->tab_hash_expr = ExecBuildHash32FromAttrs(inputDesc,
														&TTSOpsMinimalTuple,
														hashfunctions,
														collations,
														numCols,
														keyColIdx,
														allow_jit ? parent : NULL,
														hash_iv);

	/* build comparator for all columns */
	/* XXX: should we support non-minimal tuples for the inputslot? */
	hashtable->tab_eq_func = ExecBuildGroupingEqual(inputDesc, inputDesc,
													&TTSOpsMinimalTuple, &TTSOpsMinimalTuple,
													numCols,
													keyColIdx, eqfuncoids, collations,
													allow_jit ? parent : NULL);

	/*
	 * While not pretty, it's ok to not shut down this context, but instead
	 * rely on the containing memory context being reset, as
	 * ExecBuildGroupingEqual() only builds a very simple expression calling
	 * functions (i.e. nothing that'd employ RegisterExprContextCallback()).
	 */
	hashtable->exprcontext = CreateStandaloneExprContext();

	MemoryContextSwitchTo(oldcontext);

	return hashtable;
}

/*
 * BuildTupleHashTable is a backwards-compatibility wrapper for
 * BuildTupleHashTableExt(), that allocates the hashtable's metadata in
 * tablecxt. Note that hashtables created this way cannot be reset leak-free
 * with ResetTupleHashTable().
 */
TupleHashTable
BuildTupleHashTable(PlanState *parent,
					TupleDesc inputDesc,
					int numCols, AttrNumber *keyColIdx,
					const Oid *eqfuncoids,
					FmgrInfo *hashfunctions,
					Oid *collations,
					long nbuckets, Size additionalsize,
					MemoryContext tablecxt,
					MemoryContext tempcxt,
					bool use_variable_hash_iv)
{
	return BuildTupleHashTableExt(parent,
								  inputDesc,
								  numCols, keyColIdx,
								  eqfuncoids,
								  hashfunctions,
								  collations,
								  nbuckets, additionalsize,
								  tablecxt,
								  tablecxt,
								  tempcxt,
								  use_variable_hash_iv);
}

/*
 * Reset contents of the hashtable to be empty, preserving all the non-content
 * state. Note that the tablecxt passed to BuildTupleHashTableExt() should
 * also be reset, otherwise there will be leaks.
 */
void
ResetTupleHashTable(TupleHashTable hashtable)
{
	tuplehash_reset(hashtable->hashtab);
}

/*
 * Find or create a hashtable entry for the tuple group containing the
 * given tuple.  The tuple must be the same type as the hashtable entries.
 *
 * If isnew is NULL, we do not create new entries; we return NULL if no
 * match is found.
 *
 * If hash is not NULL, we set it to the calculated hash value. This allows
 * callers access to the hash value even if no entry is returned.
 *
 * If isnew isn't NULL, then a new entry is created if no existing entry
 * matches.  On return, *isnew is true if the entry is newly created,
 * false if it existed already.  ->additional_data in the new entry has
 * been zeroed.
 */
TupleHashEntry
LookupTupleHashEntry(TupleHashTable hashtable, TupleTableSlot *slot,
					 bool *isnew, uint32 *hash)
{
	TupleHashEntry entry;
	MemoryContext oldContext;
	uint32		local_hash;

	/* Need to run the hash functions in short-lived context */
	oldContext = MemoryContextSwitchTo(hashtable->tempcxt);

	/* set up data needed by hash and match functions */
	hashtable->inputslot = slot;
	hashtable->in_hash_expr = hashtable->tab_hash_expr;
	hashtable->cur_eq_func = hashtable->tab_eq_func;

	local_hash = TupleHashTableHash_internal(hashtable->hashtab, NULL);
	entry = LookupTupleHashEntry_internal(hashtable, slot, isnew, local_hash);

	if (hash != NULL)
		*hash = local_hash;

	Assert(entry == NULL || entry->hash == local_hash);

	MemoryContextSwitchTo(oldContext);

	return entry;
}

/*
 * Compute the hash value for a tuple
 */
uint32
TupleHashTableHash(TupleHashTable hashtable, TupleTableSlot *slot)
{
	MemoryContext oldContext;
	uint32		hash;

	hashtable->inputslot = slot;
	hashtable->in_hash_expr = hashtable->tab_hash_expr;

	/* Need to run the hash functions in short-lived context */
	oldContext = MemoryContextSwitchTo(hashtable->tempcxt);

	hash = TupleHashTableHash_internal(hashtable->hashtab, NULL);

	MemoryContextSwitchTo(oldContext);

	return hash;
}

/*
 * A variant of LookupTupleHashEntry for callers that have already computed
 * the hash value.
 */
TupleHashEntry
LookupTupleHashEntryHash(TupleHashTable hashtable, TupleTableSlot *slot,
						 bool *isnew, uint32 hash)
{
	TupleHashEntry entry;
	MemoryContext oldContext;

	/* Need to run the hash functions in short-lived context */
	oldContext = MemoryContextSwitchTo(hashtable->tempcxt);

	/* set up data needed by hash and match functions */
	hashtable->inputslot = slot;
	hashtable->in_hash_expr = hashtable->tab_hash_expr;
	hashtable->cur_eq_func = hashtable->tab_eq_func;

	entry = LookupTupleHashEntry_internal(hashtable, slot, isnew, hash);
	Assert(entry == NULL || entry->hash == hash);

	MemoryContextSwitchTo(oldContext);

	return entry;
}

/*
 * Search for a hashtable entry matching the given tuple.  No entry is
 * created if there's not a match.  This is similar to the non-creating
 * case of LookupTupleHashEntry, except that it supports cross-type
 * comparisons, in which the given tuple is not of the same type as the
 * table entries.  The caller must provide the hash ExprState to use for
 * the input tuple, as well as the equality ExprState, since these may be
 * different from the table's internal functions.
 */
TupleHashEntry
FindTupleHashEntry(TupleHashTable hashtable, TupleTableSlot *slot,
				   ExprState *eqcomp,
				   ExprState *hashexpr)
{
	TupleHashEntry entry;
	MemoryContext oldContext;
	MinimalTuple key;

	/* Need to run the hash functions in short-lived context */
	oldContext = MemoryContextSwitchTo(hashtable->tempcxt);

	/* Set up data needed by hash and match functions */
	hashtable->inputslot = slot;
	hashtable->in_hash_expr = hashexpr;
	hashtable->cur_eq_func = eqcomp;

	/* Search the hash table */
	key = NULL;					/* flag to reference inputslot */
	entry = tuplehash_lookup(hashtable->hashtab, key);
	MemoryContextSwitchTo(oldContext);

	return entry;
}

/*
 * If tuple is NULL, use the input slot instead. This convention avoids the
 * need to materialize virtual input tuples unless they actually need to get
 * copied into the table.
 *
 * Also, the caller must select an appropriate memory context for running
 * the hash functions.
 */
static uint32
TupleHashTableHash_internal(struct tuplehash_hash *tb,
							const MinimalTuple tuple)
{
	TupleHashTable hashtable = (TupleHashTable) tb->private_data;
	uint32		hashkey;
	TupleTableSlot *slot;
	bool		isnull;

	if (tuple == NULL)
	{
		/* Process the current input tuple for the table */
		hashtable->exprcontext->ecxt_innertuple = hashtable->inputslot;
		hashkey = DatumGetUInt32(ExecEvalExpr(hashtable->in_hash_expr,
											  hashtable->exprcontext,
											  &isnull));
	}
	else
	{
		/*
		 * Process a tuple already stored in the table.
		 *
		 * (this case never actually occurs due to the way simplehash.h is
		 * used, as the hash-value is stored in the entries)
		 */
		slot = hashtable->exprcontext->ecxt_innertuple = hashtable->tableslot;
		ExecStoreMinimalTuple(tuple, slot, false);
		hashkey = DatumGetUInt32(ExecEvalExpr(hashtable->tab_hash_expr,
											  hashtable->exprcontext,
											  &isnull));
	}

	/*
	 * The hashing done above, even with an initial value, doesn't tend to
	 * result in good hash perturbation.  Running the value produced above
	 * through murmurhash32 leads to near perfect hash perturbation.
	 */
	return murmurhash32(hashkey);
}

/*
 * Does the work of LookupTupleHashEntry and LookupTupleHashEntryHash. Useful
 * so that we can avoid switching the memory context multiple times for
 * LookupTupleHashEntry.
 *
 * NB: This function may or may not change the memory context. Caller is
 * expected to change it back.
 */
static inline TupleHashEntry
LookupTupleHashEntry_internal(TupleHashTable hashtable, TupleTableSlot *slot,
							  bool *isnew, uint32 hash)
{
	TupleHashEntryData *entry;
	bool		found;
	MinimalTuple key;

	key = NULL;					/* flag to reference inputslot */

	if (isnew)
	{
		entry = tuplehash_insert_hash(hashtable->hashtab, key, hash, &found);

		if (found)
		{
			/* found pre-existing entry */
			*isnew = false;
		}
		else
		{
			/* created new entry */
			*isnew = true;
			/* zero caller data */
			entry->additional = NULL;
			MemoryContextSwitchTo(hashtable->tablecxt);
			/* Copy the first tuple into the table context */
			entry->firstTuple = ExecCopySlotMinimalTuple(slot);
		}
	}
	else
	{
		entry = tuplehash_lookup_hash(hashtable->hashtab, key, hash);
	}

	return entry;
}

/*
 * See whether two tuples (presumably of the same hash value) match
 */
static int
TupleHashTableMatch(struct tuplehash_hash *tb, const MinimalTuple tuple1, const MinimalTuple tuple2)
{
	TupleTableSlot *slot1;
	TupleTableSlot *slot2;
	TupleHashTable hashtable = (TupleHashTable) tb->private_data;
	ExprContext *econtext = hashtable->exprcontext;

	/*
	 * We assume that simplehash.h will only ever call us with the first
	 * argument being an actual table entry, and the second argument being
	 * LookupTupleHashEntry's dummy TupleHashEntryData.  The other direction
	 * could be supported too, but is not currently required.
	 */
	Assert(tuple1 != NULL);
	slot1 = hashtable->tableslot;
	ExecStoreMinimalTuple(tuple1, slot1, false);
	Assert(tuple2 == NULL);
	slot2 = hashtable->inputslot;

	/* For crosstype comparisons, the inputslot must be first */
	econtext->ecxt_innertuple = slot2;
	econtext->ecxt_outertuple = slot1;
	return !ExecQualAndReset(hashtable->cur_eq_func, econtext);
}

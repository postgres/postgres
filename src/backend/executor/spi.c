/*-------------------------------------------------------------------------
 *
 * spi.c
 *				Server Programming Interface
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/executor/spi.c,v 1.187 2008/01/01 19:45:49 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/printtup.h"
#include "catalog/heap.h"
#include "commands/trigger.h"
#include "executor/spi_priv.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/typcache.h"


uint32		SPI_processed = 0;
Oid			SPI_lastoid = InvalidOid;
SPITupleTable *SPI_tuptable = NULL;
int			SPI_result;

static _SPI_connection *_SPI_stack = NULL;
static _SPI_connection *_SPI_current = NULL;
static int	_SPI_stack_depth = 0;		/* allocated size of _SPI_stack */
static int	_SPI_connected = -1;
static int	_SPI_curid = -1;

static void _SPI_prepare_plan(const char *src, SPIPlanPtr plan);

static int _SPI_execute_plan(SPIPlanPtr plan,
				  Datum *Values, const char *Nulls,
				  Snapshot snapshot, Snapshot crosscheck_snapshot,
				  bool read_only, bool fire_triggers, long tcount);

static int	_SPI_pquery(QueryDesc *queryDesc, bool fire_triggers, long tcount);

static void _SPI_error_callback(void *arg);

static void _SPI_cursor_operation(Portal portal,
					  FetchDirection direction, long count,
					  DestReceiver *dest);

static SPIPlanPtr _SPI_copy_plan(SPIPlanPtr plan, MemoryContext parentcxt);
static SPIPlanPtr _SPI_save_plan(SPIPlanPtr plan);

static int	_SPI_begin_call(bool execmem);
static int	_SPI_end_call(bool procmem);
static MemoryContext _SPI_execmem(void);
static MemoryContext _SPI_procmem(void);
static bool _SPI_checktuples(void);


/* =================== interface functions =================== */

int
SPI_connect(void)
{
	int			newdepth;

	/*
	 * When procedure called by Executor _SPI_curid expected to be equal to
	 * _SPI_connected
	 */
	if (_SPI_curid != _SPI_connected)
		return SPI_ERROR_CONNECT;

	if (_SPI_stack == NULL)
	{
		if (_SPI_connected != -1 || _SPI_stack_depth != 0)
			elog(ERROR, "SPI stack corrupted");
		newdepth = 16;
		_SPI_stack = (_SPI_connection *)
			MemoryContextAlloc(TopTransactionContext,
							   newdepth * sizeof(_SPI_connection));
		_SPI_stack_depth = newdepth;
	}
	else
	{
		if (_SPI_stack_depth <= 0 || _SPI_stack_depth <= _SPI_connected)
			elog(ERROR, "SPI stack corrupted");
		if (_SPI_stack_depth == _SPI_connected + 1)
		{
			newdepth = _SPI_stack_depth * 2;
			_SPI_stack = (_SPI_connection *)
				repalloc(_SPI_stack,
						 newdepth * sizeof(_SPI_connection));
			_SPI_stack_depth = newdepth;
		}
	}

	/*
	 * We're entering procedure where _SPI_curid == _SPI_connected - 1
	 */
	_SPI_connected++;
	Assert(_SPI_connected >= 0 && _SPI_connected < _SPI_stack_depth);

	_SPI_current = &(_SPI_stack[_SPI_connected]);
	_SPI_current->processed = 0;
	_SPI_current->lastoid = InvalidOid;
	_SPI_current->tuptable = NULL;
	_SPI_current->procCxt = NULL;		/* in case we fail to create 'em */
	_SPI_current->execCxt = NULL;
	_SPI_current->connectSubid = GetCurrentSubTransactionId();

	/*
	 * Create memory contexts for this procedure
	 *
	 * XXX it would be better to use PortalContext as the parent context, but
	 * we may not be inside a portal (consider deferred-trigger execution).
	 * Perhaps CurTransactionContext would do?	For now it doesn't matter
	 * because we clean up explicitly in AtEOSubXact_SPI().
	 */
	_SPI_current->procCxt = AllocSetContextCreate(TopTransactionContext,
												  "SPI Proc",
												  ALLOCSET_DEFAULT_MINSIZE,
												  ALLOCSET_DEFAULT_INITSIZE,
												  ALLOCSET_DEFAULT_MAXSIZE);
	_SPI_current->execCxt = AllocSetContextCreate(TopTransactionContext,
												  "SPI Exec",
												  ALLOCSET_DEFAULT_MINSIZE,
												  ALLOCSET_DEFAULT_INITSIZE,
												  ALLOCSET_DEFAULT_MAXSIZE);
	/* ... and switch to procedure's context */
	_SPI_current->savedcxt = MemoryContextSwitchTo(_SPI_current->procCxt);

	return SPI_OK_CONNECT;
}

int
SPI_finish(void)
{
	int			res;

	res = _SPI_begin_call(false);		/* live in procedure memory */
	if (res < 0)
		return res;

	/* Restore memory context as it was before procedure call */
	MemoryContextSwitchTo(_SPI_current->savedcxt);

	/* Release memory used in procedure call */
	MemoryContextDelete(_SPI_current->execCxt);
	_SPI_current->execCxt = NULL;
	MemoryContextDelete(_SPI_current->procCxt);
	_SPI_current->procCxt = NULL;

	/*
	 * Reset result variables, especially SPI_tuptable which is probably
	 * pointing at a just-deleted tuptable
	 */
	SPI_processed = 0;
	SPI_lastoid = InvalidOid;
	SPI_tuptable = NULL;

	/*
	 * After _SPI_begin_call _SPI_connected == _SPI_curid. Now we are closing
	 * connection to SPI and returning to upper Executor and so _SPI_connected
	 * must be equal to _SPI_curid.
	 */
	_SPI_connected--;
	_SPI_curid--;
	if (_SPI_connected == -1)
		_SPI_current = NULL;
	else
		_SPI_current = &(_SPI_stack[_SPI_connected]);

	return SPI_OK_FINISH;
}

/*
 * Clean up SPI state at transaction commit or abort.
 */
void
AtEOXact_SPI(bool isCommit)
{
	/*
	 * Note that memory contexts belonging to SPI stack entries will be freed
	 * automatically, so we can ignore them here.  We just need to restore our
	 * static variables to initial state.
	 */
	if (isCommit && _SPI_connected != -1)
		ereport(WARNING,
				(errcode(ERRCODE_WARNING),
				 errmsg("transaction left non-empty SPI stack"),
				 errhint("Check for missing \"SPI_finish\" calls.")));

	_SPI_current = _SPI_stack = NULL;
	_SPI_stack_depth = 0;
	_SPI_connected = _SPI_curid = -1;
	SPI_processed = 0;
	SPI_lastoid = InvalidOid;
	SPI_tuptable = NULL;
}

/*
 * Clean up SPI state at subtransaction commit or abort.
 *
 * During commit, there shouldn't be any unclosed entries remaining from
 * the current subtransaction; we emit a warning if any are found.
 */
void
AtEOSubXact_SPI(bool isCommit, SubTransactionId mySubid)
{
	bool		found = false;

	while (_SPI_connected >= 0)
	{
		_SPI_connection *connection = &(_SPI_stack[_SPI_connected]);

		if (connection->connectSubid != mySubid)
			break;				/* couldn't be any underneath it either */

		found = true;

		/*
		 * Release procedure memory explicitly (see note in SPI_connect)
		 */
		if (connection->execCxt)
		{
			MemoryContextDelete(connection->execCxt);
			connection->execCxt = NULL;
		}
		if (connection->procCxt)
		{
			MemoryContextDelete(connection->procCxt);
			connection->procCxt = NULL;
		}

		/*
		 * Pop the stack entry and reset global variables.	Unlike
		 * SPI_finish(), we don't risk switching to memory contexts that might
		 * be already gone.
		 */
		_SPI_connected--;
		_SPI_curid = _SPI_connected;
		if (_SPI_connected == -1)
			_SPI_current = NULL;
		else
			_SPI_current = &(_SPI_stack[_SPI_connected]);
		SPI_processed = 0;
		SPI_lastoid = InvalidOid;
		SPI_tuptable = NULL;
	}

	if (found && isCommit)
		ereport(WARNING,
				(errcode(ERRCODE_WARNING),
				 errmsg("subtransaction left non-empty SPI stack"),
				 errhint("Check for missing \"SPI_finish\" calls.")));

	/*
	 * If we are aborting a subtransaction and there is an open SPI context
	 * surrounding the subxact, clean up to prevent memory leakage.
	 */
	if (_SPI_current && !isCommit)
	{
		/* free Executor memory the same as _SPI_end_call would do */
		MemoryContextResetAndDeleteChildren(_SPI_current->execCxt);
		/* throw away any partially created tuple-table */
		SPI_freetuptable(_SPI_current->tuptable);
		_SPI_current->tuptable = NULL;
	}
}


/* Pushes SPI stack to allow recursive SPI calls */
void
SPI_push(void)
{
	_SPI_curid++;
}

/* Pops SPI stack to allow recursive SPI calls */
void
SPI_pop(void)
{
	_SPI_curid--;
}

/* Restore state of SPI stack after aborting a subtransaction */
void
SPI_restore_connection(void)
{
	Assert(_SPI_connected >= 0);
	_SPI_curid = _SPI_connected - 1;
}

/* Parse, plan, and execute a query string */
int
SPI_execute(const char *src, bool read_only, long tcount)
{
	_SPI_plan	plan;
	int			res;

	if (src == NULL || tcount < 0)
		return SPI_ERROR_ARGUMENT;

	res = _SPI_begin_call(true);
	if (res < 0)
		return res;

	memset(&plan, 0, sizeof(_SPI_plan));
	plan.magic = _SPI_PLAN_MAGIC;
	plan.cursor_options = 0;

	_SPI_prepare_plan(src, &plan);

	res = _SPI_execute_plan(&plan, NULL, NULL,
							InvalidSnapshot, InvalidSnapshot,
							read_only, true, tcount);

	_SPI_end_call(true);
	return res;
}

/* Obsolete version of SPI_execute */
int
SPI_exec(const char *src, long tcount)
{
	return SPI_execute(src, false, tcount);
}

/* Execute a previously prepared plan */
int
SPI_execute_plan(SPIPlanPtr plan, Datum *Values, const char *Nulls,
				 bool read_only, long tcount)
{
	int			res;

	if (plan == NULL || plan->magic != _SPI_PLAN_MAGIC || tcount < 0)
		return SPI_ERROR_ARGUMENT;

	if (plan->nargs > 0 && Values == NULL)
		return SPI_ERROR_PARAM;

	res = _SPI_begin_call(true);
	if (res < 0)
		return res;

	res = _SPI_execute_plan(plan,
							Values, Nulls,
							InvalidSnapshot, InvalidSnapshot,
							read_only, true, tcount);

	_SPI_end_call(true);
	return res;
}

/* Obsolete version of SPI_execute_plan */
int
SPI_execp(SPIPlanPtr plan, Datum *Values, const char *Nulls, long tcount)
{
	return SPI_execute_plan(plan, Values, Nulls, false, tcount);
}

/*
 * SPI_execute_snapshot -- identical to SPI_execute_plan, except that we allow
 * the caller to specify exactly which snapshots to use.  Also, the caller
 * may specify that AFTER triggers should be queued as part of the outer
 * query rather than being fired immediately at the end of the command.
 *
 * This is currently not documented in spi.sgml because it is only intended
 * for use by RI triggers.
 *
 * Passing snapshot == InvalidSnapshot will select the normal behavior of
 * fetching a new snapshot for each query.
 */
int
SPI_execute_snapshot(SPIPlanPtr plan,
					 Datum *Values, const char *Nulls,
					 Snapshot snapshot, Snapshot crosscheck_snapshot,
					 bool read_only, bool fire_triggers, long tcount)
{
	int			res;

	if (plan == NULL || plan->magic != _SPI_PLAN_MAGIC || tcount < 0)
		return SPI_ERROR_ARGUMENT;

	if (plan->nargs > 0 && Values == NULL)
		return SPI_ERROR_PARAM;

	res = _SPI_begin_call(true);
	if (res < 0)
		return res;

	res = _SPI_execute_plan(plan,
							Values, Nulls,
							snapshot, crosscheck_snapshot,
							read_only, fire_triggers, tcount);

	_SPI_end_call(true);
	return res;
}

SPIPlanPtr
SPI_prepare(const char *src, int nargs, Oid *argtypes)
{
	return SPI_prepare_cursor(src, nargs, argtypes, 0);
}

SPIPlanPtr
SPI_prepare_cursor(const char *src, int nargs, Oid *argtypes,
				   int cursorOptions)
{
	_SPI_plan	plan;
	SPIPlanPtr	result;

	if (src == NULL || nargs < 0 || (nargs > 0 && argtypes == NULL))
	{
		SPI_result = SPI_ERROR_ARGUMENT;
		return NULL;
	}

	SPI_result = _SPI_begin_call(true);
	if (SPI_result < 0)
		return NULL;

	memset(&plan, 0, sizeof(_SPI_plan));
	plan.magic = _SPI_PLAN_MAGIC;
	plan.cursor_options = cursorOptions;
	plan.nargs = nargs;
	plan.argtypes = argtypes;

	_SPI_prepare_plan(src, &plan);

	/* copy plan to procedure context */
	result = _SPI_copy_plan(&plan, _SPI_current->procCxt);

	_SPI_end_call(true);

	return result;
}

SPIPlanPtr
SPI_saveplan(SPIPlanPtr plan)
{
	SPIPlanPtr	newplan;

	/* We don't currently support copying an already-saved plan */
	if (plan == NULL || plan->magic != _SPI_PLAN_MAGIC || plan->saved)
	{
		SPI_result = SPI_ERROR_ARGUMENT;
		return NULL;
	}

	SPI_result = _SPI_begin_call(false);		/* don't change context */
	if (SPI_result < 0)
		return NULL;

	newplan = _SPI_save_plan(plan);

	_SPI_curid--;
	SPI_result = 0;

	return newplan;
}

int
SPI_freeplan(SPIPlanPtr plan)
{
	if (plan == NULL || plan->magic != _SPI_PLAN_MAGIC)
		return SPI_ERROR_ARGUMENT;

	/* If plancache.c owns the plancache entries, we must release them */
	if (plan->saved)
	{
		ListCell   *lc;

		foreach(lc, plan->plancache_list)
		{
			CachedPlanSource *plansource = (CachedPlanSource *) lfirst(lc);

			DropCachedPlan(plansource);
		}
	}

	/* Now get rid of the _SPI_plan and subsidiary data in its plancxt */
	MemoryContextDelete(plan->plancxt);

	return 0;
}

HeapTuple
SPI_copytuple(HeapTuple tuple)
{
	MemoryContext oldcxt = NULL;
	HeapTuple	ctuple;

	if (tuple == NULL)
	{
		SPI_result = SPI_ERROR_ARGUMENT;
		return NULL;
	}

	if (_SPI_curid + 1 == _SPI_connected)		/* connected */
	{
		if (_SPI_current != &(_SPI_stack[_SPI_curid + 1]))
			elog(ERROR, "SPI stack corrupted");
		oldcxt = MemoryContextSwitchTo(_SPI_current->savedcxt);
	}

	ctuple = heap_copytuple(tuple);

	if (oldcxt)
		MemoryContextSwitchTo(oldcxt);

	return ctuple;
}

HeapTupleHeader
SPI_returntuple(HeapTuple tuple, TupleDesc tupdesc)
{
	MemoryContext oldcxt = NULL;
	HeapTupleHeader dtup;

	if (tuple == NULL || tupdesc == NULL)
	{
		SPI_result = SPI_ERROR_ARGUMENT;
		return NULL;
	}

	/* For RECORD results, make sure a typmod has been assigned */
	if (tupdesc->tdtypeid == RECORDOID &&
		tupdesc->tdtypmod < 0)
		assign_record_type_typmod(tupdesc);

	if (_SPI_curid + 1 == _SPI_connected)		/* connected */
	{
		if (_SPI_current != &(_SPI_stack[_SPI_curid + 1]))
			elog(ERROR, "SPI stack corrupted");
		oldcxt = MemoryContextSwitchTo(_SPI_current->savedcxt);
	}

	dtup = (HeapTupleHeader) palloc(tuple->t_len);
	memcpy((char *) dtup, (char *) tuple->t_data, tuple->t_len);

	HeapTupleHeaderSetDatumLength(dtup, tuple->t_len);
	HeapTupleHeaderSetTypeId(dtup, tupdesc->tdtypeid);
	HeapTupleHeaderSetTypMod(dtup, tupdesc->tdtypmod);

	if (oldcxt)
		MemoryContextSwitchTo(oldcxt);

	return dtup;
}

HeapTuple
SPI_modifytuple(Relation rel, HeapTuple tuple, int natts, int *attnum,
				Datum *Values, const char *Nulls)
{
	MemoryContext oldcxt = NULL;
	HeapTuple	mtuple;
	int			numberOfAttributes;
	Datum	   *v;
	char	   *n;
	int			i;

	if (rel == NULL || tuple == NULL || natts < 0 || attnum == NULL || Values == NULL)
	{
		SPI_result = SPI_ERROR_ARGUMENT;
		return NULL;
	}

	if (_SPI_curid + 1 == _SPI_connected)		/* connected */
	{
		if (_SPI_current != &(_SPI_stack[_SPI_curid + 1]))
			elog(ERROR, "SPI stack corrupted");
		oldcxt = MemoryContextSwitchTo(_SPI_current->savedcxt);
	}
	SPI_result = 0;
	numberOfAttributes = rel->rd_att->natts;
	v = (Datum *) palloc(numberOfAttributes * sizeof(Datum));
	n = (char *) palloc(numberOfAttributes * sizeof(char));

	/* fetch old values and nulls */
	heap_deformtuple(tuple, rel->rd_att, v, n);

	/* replace values and nulls */
	for (i = 0; i < natts; i++)
	{
		if (attnum[i] <= 0 || attnum[i] > numberOfAttributes)
			break;
		v[attnum[i] - 1] = Values[i];
		n[attnum[i] - 1] = (Nulls && Nulls[i] == 'n') ? 'n' : ' ';
	}

	if (i == natts)				/* no errors in *attnum */
	{
		mtuple = heap_formtuple(rel->rd_att, v, n);

		/*
		 * copy the identification info of the old tuple: t_ctid, t_self, and
		 * OID (if any)
		 */
		mtuple->t_data->t_ctid = tuple->t_data->t_ctid;
		mtuple->t_self = tuple->t_self;
		mtuple->t_tableOid = tuple->t_tableOid;
		if (rel->rd_att->tdhasoid)
			HeapTupleSetOid(mtuple, HeapTupleGetOid(tuple));
	}
	else
	{
		mtuple = NULL;
		SPI_result = SPI_ERROR_NOATTRIBUTE;
	}

	pfree(v);
	pfree(n);

	if (oldcxt)
		MemoryContextSwitchTo(oldcxt);

	return mtuple;
}

int
SPI_fnumber(TupleDesc tupdesc, const char *fname)
{
	int			res;
	Form_pg_attribute sysatt;

	for (res = 0; res < tupdesc->natts; res++)
	{
		if (namestrcmp(&tupdesc->attrs[res]->attname, fname) == 0)
			return res + 1;
	}

	sysatt = SystemAttributeByName(fname, true /* "oid" will be accepted */ );
	if (sysatt != NULL)
		return sysatt->attnum;

	/* SPI_ERROR_NOATTRIBUTE is different from all sys column numbers */
	return SPI_ERROR_NOATTRIBUTE;
}

char *
SPI_fname(TupleDesc tupdesc, int fnumber)
{
	Form_pg_attribute att;

	SPI_result = 0;

	if (fnumber > tupdesc->natts || fnumber == 0 ||
		fnumber <= FirstLowInvalidHeapAttributeNumber)
	{
		SPI_result = SPI_ERROR_NOATTRIBUTE;
		return NULL;
	}

	if (fnumber > 0)
		att = tupdesc->attrs[fnumber - 1];
	else
		att = SystemAttributeDefinition(fnumber, true);

	return pstrdup(NameStr(att->attname));
}

char *
SPI_getvalue(HeapTuple tuple, TupleDesc tupdesc, int fnumber)
{
	char	   *result;
	Datum		origval,
				val;
	bool		isnull;
	Oid			typoid,
				foutoid;
	bool		typisvarlena;

	SPI_result = 0;

	if (fnumber > HeapTupleHeaderGetNatts(tuple->t_data) || fnumber == 0 ||
		fnumber <= FirstLowInvalidHeapAttributeNumber)
	{
		SPI_result = SPI_ERROR_NOATTRIBUTE;
		return NULL;
	}

	origval = heap_getattr(tuple, fnumber, tupdesc, &isnull);
	if (isnull)
		return NULL;

	if (fnumber > 0)
		typoid = tupdesc->attrs[fnumber - 1]->atttypid;
	else
		typoid = (SystemAttributeDefinition(fnumber, true))->atttypid;

	getTypeOutputInfo(typoid, &foutoid, &typisvarlena);

	/*
	 * If we have a toasted datum, forcibly detoast it here to avoid memory
	 * leakage inside the type's output routine.
	 */
	if (typisvarlena)
		val = PointerGetDatum(PG_DETOAST_DATUM(origval));
	else
		val = origval;

	result = OidOutputFunctionCall(foutoid, val);

	/* Clean up detoasted copy, if any */
	if (val != origval)
		pfree(DatumGetPointer(val));

	return result;
}

Datum
SPI_getbinval(HeapTuple tuple, TupleDesc tupdesc, int fnumber, bool *isnull)
{
	SPI_result = 0;

	if (fnumber > HeapTupleHeaderGetNatts(tuple->t_data) || fnumber == 0 ||
		fnumber <= FirstLowInvalidHeapAttributeNumber)
	{
		SPI_result = SPI_ERROR_NOATTRIBUTE;
		*isnull = true;
		return (Datum) NULL;
	}

	return heap_getattr(tuple, fnumber, tupdesc, isnull);
}

char *
SPI_gettype(TupleDesc tupdesc, int fnumber)
{
	Oid			typoid;
	HeapTuple	typeTuple;
	char	   *result;

	SPI_result = 0;

	if (fnumber > tupdesc->natts || fnumber == 0 ||
		fnumber <= FirstLowInvalidHeapAttributeNumber)
	{
		SPI_result = SPI_ERROR_NOATTRIBUTE;
		return NULL;
	}

	if (fnumber > 0)
		typoid = tupdesc->attrs[fnumber - 1]->atttypid;
	else
		typoid = (SystemAttributeDefinition(fnumber, true))->atttypid;

	typeTuple = SearchSysCache(TYPEOID,
							   ObjectIdGetDatum(typoid),
							   0, 0, 0);

	if (!HeapTupleIsValid(typeTuple))
	{
		SPI_result = SPI_ERROR_TYPUNKNOWN;
		return NULL;
	}

	result = pstrdup(NameStr(((Form_pg_type) GETSTRUCT(typeTuple))->typname));
	ReleaseSysCache(typeTuple);
	return result;
}

Oid
SPI_gettypeid(TupleDesc tupdesc, int fnumber)
{
	SPI_result = 0;

	if (fnumber > tupdesc->natts || fnumber == 0 ||
		fnumber <= FirstLowInvalidHeapAttributeNumber)
	{
		SPI_result = SPI_ERROR_NOATTRIBUTE;
		return InvalidOid;
	}

	if (fnumber > 0)
		return tupdesc->attrs[fnumber - 1]->atttypid;
	else
		return (SystemAttributeDefinition(fnumber, true))->atttypid;
}

char *
SPI_getrelname(Relation rel)
{
	return pstrdup(RelationGetRelationName(rel));
}

char *
SPI_getnspname(Relation rel)
{
	return get_namespace_name(RelationGetNamespace(rel));
}

void *
SPI_palloc(Size size)
{
	MemoryContext oldcxt = NULL;
	void	   *pointer;

	if (_SPI_curid + 1 == _SPI_connected)		/* connected */
	{
		if (_SPI_current != &(_SPI_stack[_SPI_curid + 1]))
			elog(ERROR, "SPI stack corrupted");
		oldcxt = MemoryContextSwitchTo(_SPI_current->savedcxt);
	}

	pointer = palloc(size);

	if (oldcxt)
		MemoryContextSwitchTo(oldcxt);

	return pointer;
}

void *
SPI_repalloc(void *pointer, Size size)
{
	/* No longer need to worry which context chunk was in... */
	return repalloc(pointer, size);
}

void
SPI_pfree(void *pointer)
{
	/* No longer need to worry which context chunk was in... */
	pfree(pointer);
}

void
SPI_freetuple(HeapTuple tuple)
{
	/* No longer need to worry which context tuple was in... */
	heap_freetuple(tuple);
}

void
SPI_freetuptable(SPITupleTable *tuptable)
{
	if (tuptable != NULL)
		MemoryContextDelete(tuptable->tuptabcxt);
}


/*
 * SPI_cursor_open()
 *
 *	Open a prepared SPI plan as a portal
 */
Portal
SPI_cursor_open(const char *name, SPIPlanPtr plan,
				Datum *Values, const char *Nulls,
				bool read_only)
{
	CachedPlanSource *plansource;
	CachedPlan *cplan;
	List	   *stmt_list;
	ParamListInfo paramLI;
	Snapshot	snapshot;
	MemoryContext oldcontext;
	Portal		portal;
	int			k;

	/*
	 * Check that the plan is something the Portal code will special-case as
	 * returning one tupleset.
	 */
	if (!SPI_is_cursor_plan(plan))
	{
		/* try to give a good error message */
		if (list_length(plan->plancache_list) != 1)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_CURSOR_DEFINITION),
					 errmsg("cannot open multi-query plan as cursor")));
		plansource = (CachedPlanSource *) linitial(plan->plancache_list);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_CURSOR_DEFINITION),
		/* translator: %s is name of a SQL command, eg INSERT */
				 errmsg("cannot open %s query as cursor",
						plansource->commandTag)));
	}

	Assert(list_length(plan->plancache_list) == 1);
	plansource = (CachedPlanSource *) linitial(plan->plancache_list);

	/* Reset SPI result (note we deliberately don't touch lastoid) */
	SPI_processed = 0;
	SPI_tuptable = NULL;
	_SPI_current->processed = 0;
	_SPI_current->tuptable = NULL;

	/* Create the portal */
	if (name == NULL || name[0] == '\0')
	{
		/* Use a random nonconflicting name */
		portal = CreateNewPortal();
	}
	else
	{
		/* In this path, error if portal of same name already exists */
		portal = CreatePortal(name, false, false);
	}

	/* If the plan has parameters, copy them into the portal */
	if (plan->nargs > 0)
	{
		oldcontext = MemoryContextSwitchTo(PortalGetHeapMemory(portal));
		/* sizeof(ParamListInfoData) includes the first array element */
		paramLI = (ParamListInfo) palloc(sizeof(ParamListInfoData) +
								 (plan->nargs - 1) *sizeof(ParamExternData));
		paramLI->numParams = plan->nargs;

		for (k = 0; k < plan->nargs; k++)
		{
			ParamExternData *prm = &paramLI->params[k];

			prm->ptype = plan->argtypes[k];
			prm->pflags = 0;
			prm->isnull = (Nulls && Nulls[k] == 'n');
			if (prm->isnull)
			{
				/* nulls just copy */
				prm->value = Values[k];
			}
			else
			{
				/* pass-by-ref values must be copied into portal context */
				int16		paramTypLen;
				bool		paramTypByVal;

				get_typlenbyval(prm->ptype, &paramTypLen, &paramTypByVal);
				prm->value = datumCopy(Values[k],
									   paramTypByVal, paramTypLen);
			}
		}
		MemoryContextSwitchTo(oldcontext);
	}
	else
		paramLI = NULL;

	if (plan->saved)
	{
		/* Replan if needed, and increment plan refcount for portal */
		cplan = RevalidateCachedPlan(plansource, false);
		stmt_list = cplan->stmt_list;
	}
	else
	{
		/* No replan, but copy the plan into the portal's context */
		oldcontext = MemoryContextSwitchTo(PortalGetHeapMemory(portal));
		stmt_list = copyObject(plansource->plan->stmt_list);
		MemoryContextSwitchTo(oldcontext);
		cplan = NULL;			/* portal shouldn't depend on cplan */
	}

	/*
	 * Set up the portal.
	 */
	PortalDefineQuery(portal,
					  NULL,		/* no statement name */
					  plansource->query_string,
					  plansource->commandTag,
					  stmt_list,
					  cplan);

	/*
	 * Set up options for portal.  Default SCROLL type is chosen the same way
	 * as PerformCursorOpen does it.
	 */
	portal->cursorOptions = plan->cursor_options;
	if (!(portal->cursorOptions & (CURSOR_OPT_SCROLL | CURSOR_OPT_NO_SCROLL)))
	{
		if (list_length(stmt_list) == 1 &&
			IsA((Node *) linitial(stmt_list), PlannedStmt) &&
			((PlannedStmt *) linitial(stmt_list))->rowMarks == NIL &&
			ExecSupportsBackwardScan(((PlannedStmt *) linitial(stmt_list))->planTree))
			portal->cursorOptions |= CURSOR_OPT_SCROLL;
		else
			portal->cursorOptions |= CURSOR_OPT_NO_SCROLL;
	}

	/*
	 * Disallow SCROLL with SELECT FOR UPDATE.	This is not redundant with the
	 * check in transformDeclareCursorStmt because the cursor options might
	 * not have come through there.
	 */
	if (portal->cursorOptions & CURSOR_OPT_SCROLL)
	{
		if (list_length(stmt_list) == 1 &&
			IsA((Node *) linitial(stmt_list), PlannedStmt) &&
			((PlannedStmt *) linitial(stmt_list))->rowMarks != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("DECLARE SCROLL CURSOR ... FOR UPDATE/SHARE is not supported"),
					 errdetail("Scrollable cursors must be READ ONLY.")));
	}

	/*
	 * If told to be read-only, we'd better check for read-only queries. This
	 * can't be done earlier because we need to look at the finished, planned
	 * queries.  (In particular, we don't want to do it between
	 * RevalidateCachedPlan and PortalDefineQuery, because throwing an error
	 * between those steps would result in leaking our plancache refcount.)
	 */
	if (read_only)
	{
		ListCell   *lc;

		foreach(lc, stmt_list)
		{
			Node	   *pstmt = (Node *) lfirst(lc);

			if (!CommandIsReadOnly(pstmt))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				/* translator: %s is a SQL statement name */
					   errmsg("%s is not allowed in a non-volatile function",
							  CreateCommandTag(pstmt))));
		}
	}

	/*
	 * Set up the snapshot to use.	(PortalStart will do CopySnapshot, so we
	 * skip that here.)
	 */
	if (read_only)
		snapshot = ActiveSnapshot;
	else
	{
		CommandCounterIncrement();
		snapshot = GetTransactionSnapshot();
	}

	/*
	 * Start portal execution.
	 */
	PortalStart(portal, paramLI, snapshot);

	Assert(portal->strategy != PORTAL_MULTI_QUERY);

	/* Return the created portal */
	return portal;
}


/*
 * SPI_cursor_find()
 *
 *	Find the portal of an existing open cursor
 */
Portal
SPI_cursor_find(const char *name)
{
	return GetPortalByName(name);
}


/*
 * SPI_cursor_fetch()
 *
 *	Fetch rows in a cursor
 */
void
SPI_cursor_fetch(Portal portal, bool forward, long count)
{
	_SPI_cursor_operation(portal,
						  forward ? FETCH_FORWARD : FETCH_BACKWARD, count,
						  CreateDestReceiver(DestSPI, NULL));
	/* we know that the DestSPI receiver doesn't need a destroy call */
}


/*
 * SPI_cursor_move()
 *
 *	Move in a cursor
 */
void
SPI_cursor_move(Portal portal, bool forward, long count)
{
	_SPI_cursor_operation(portal,
						  forward ? FETCH_FORWARD : FETCH_BACKWARD, count,
						  None_Receiver);
}


/*
 * SPI_scroll_cursor_fetch()
 *
 *	Fetch rows in a scrollable cursor
 */
void
SPI_scroll_cursor_fetch(Portal portal, FetchDirection direction, long count)
{
	_SPI_cursor_operation(portal,
						  direction, count,
						  CreateDestReceiver(DestSPI, NULL));
	/* we know that the DestSPI receiver doesn't need a destroy call */
}


/*
 * SPI_scroll_cursor_move()
 *
 *	Move in a scrollable cursor
 */
void
SPI_scroll_cursor_move(Portal portal, FetchDirection direction, long count)
{
	_SPI_cursor_operation(portal, direction, count, None_Receiver);
}


/*
 * SPI_cursor_close()
 *
 *	Close a cursor
 */
void
SPI_cursor_close(Portal portal)
{
	if (!PortalIsValid(portal))
		elog(ERROR, "invalid portal in SPI cursor operation");

	PortalDrop(portal, false);
}

/*
 * Returns the Oid representing the type id for argument at argIndex. First
 * parameter is at index zero.
 */
Oid
SPI_getargtypeid(SPIPlanPtr plan, int argIndex)
{
	if (plan == NULL || plan->magic != _SPI_PLAN_MAGIC ||
		argIndex < 0 || argIndex >= plan->nargs)
	{
		SPI_result = SPI_ERROR_ARGUMENT;
		return InvalidOid;
	}
	return plan->argtypes[argIndex];
}

/*
 * Returns the number of arguments for the prepared plan.
 */
int
SPI_getargcount(SPIPlanPtr plan)
{
	if (plan == NULL || plan->magic != _SPI_PLAN_MAGIC)
	{
		SPI_result = SPI_ERROR_ARGUMENT;
		return -1;
	}
	return plan->nargs;
}

/*
 * Returns true if the plan contains exactly one command
 * and that command returns tuples to the caller (eg, SELECT or
 * INSERT ... RETURNING, but not SELECT ... INTO). In essence,
 * the result indicates if the command can be used with SPI_cursor_open
 *
 * Parameters
 *	  plan: A plan previously prepared using SPI_prepare
 */
bool
SPI_is_cursor_plan(SPIPlanPtr plan)
{
	CachedPlanSource *plansource;
	CachedPlan *cplan;

	if (plan == NULL || plan->magic != _SPI_PLAN_MAGIC)
	{
		SPI_result = SPI_ERROR_ARGUMENT;
		return false;
	}

	if (list_length(plan->plancache_list) != 1)
		return false;			/* not exactly 1 pre-rewrite command */
	plansource = (CachedPlanSource *) linitial(plan->plancache_list);

	if (plan->saved)
	{
		/* Make sure the plan is up to date */
		cplan = RevalidateCachedPlan(plansource, true);
		ReleaseCachedPlan(cplan, true);
	}

	/* Does it return tuples? */
	if (plansource->resultDesc)
		return true;

	return false;
}

/*
 * SPI_result_code_string --- convert any SPI return code to a string
 *
 * This is often useful in error messages.	Most callers will probably
 * only pass negative (error-case) codes, but for generality we recognize
 * the success codes too.
 */
const char *
SPI_result_code_string(int code)
{
	static char buf[64];

	switch (code)
	{
		case SPI_ERROR_CONNECT:
			return "SPI_ERROR_CONNECT";
		case SPI_ERROR_COPY:
			return "SPI_ERROR_COPY";
		case SPI_ERROR_OPUNKNOWN:
			return "SPI_ERROR_OPUNKNOWN";
		case SPI_ERROR_UNCONNECTED:
			return "SPI_ERROR_UNCONNECTED";
		case SPI_ERROR_ARGUMENT:
			return "SPI_ERROR_ARGUMENT";
		case SPI_ERROR_PARAM:
			return "SPI_ERROR_PARAM";
		case SPI_ERROR_TRANSACTION:
			return "SPI_ERROR_TRANSACTION";
		case SPI_ERROR_NOATTRIBUTE:
			return "SPI_ERROR_NOATTRIBUTE";
		case SPI_ERROR_NOOUTFUNC:
			return "SPI_ERROR_NOOUTFUNC";
		case SPI_ERROR_TYPUNKNOWN:
			return "SPI_ERROR_TYPUNKNOWN";
		case SPI_OK_CONNECT:
			return "SPI_OK_CONNECT";
		case SPI_OK_FINISH:
			return "SPI_OK_FINISH";
		case SPI_OK_FETCH:
			return "SPI_OK_FETCH";
		case SPI_OK_UTILITY:
			return "SPI_OK_UTILITY";
		case SPI_OK_SELECT:
			return "SPI_OK_SELECT";
		case SPI_OK_SELINTO:
			return "SPI_OK_SELINTO";
		case SPI_OK_INSERT:
			return "SPI_OK_INSERT";
		case SPI_OK_DELETE:
			return "SPI_OK_DELETE";
		case SPI_OK_UPDATE:
			return "SPI_OK_UPDATE";
		case SPI_OK_CURSOR:
			return "SPI_OK_CURSOR";
		case SPI_OK_INSERT_RETURNING:
			return "SPI_OK_INSERT_RETURNING";
		case SPI_OK_DELETE_RETURNING:
			return "SPI_OK_DELETE_RETURNING";
		case SPI_OK_UPDATE_RETURNING:
			return "SPI_OK_UPDATE_RETURNING";
	}
	/* Unrecognized code ... return something useful ... */
	sprintf(buf, "Unrecognized SPI code %d", code);
	return buf;
}

/* =================== private functions =================== */

/*
 * spi_dest_startup
 *		Initialize to receive tuples from Executor into SPITupleTable
 *		of current SPI procedure
 */
void
spi_dest_startup(DestReceiver *self, int operation, TupleDesc typeinfo)
{
	SPITupleTable *tuptable;
	MemoryContext oldcxt;
	MemoryContext tuptabcxt;

	/*
	 * When called by Executor _SPI_curid expected to be equal to
	 * _SPI_connected
	 */
	if (_SPI_curid != _SPI_connected || _SPI_connected < 0)
		elog(ERROR, "improper call to spi_dest_startup");
	if (_SPI_current != &(_SPI_stack[_SPI_curid]))
		elog(ERROR, "SPI stack corrupted");

	if (_SPI_current->tuptable != NULL)
		elog(ERROR, "improper call to spi_dest_startup");

	oldcxt = _SPI_procmem();	/* switch to procedure memory context */

	tuptabcxt = AllocSetContextCreate(CurrentMemoryContext,
									  "SPI TupTable",
									  ALLOCSET_DEFAULT_MINSIZE,
									  ALLOCSET_DEFAULT_INITSIZE,
									  ALLOCSET_DEFAULT_MAXSIZE);
	MemoryContextSwitchTo(tuptabcxt);

	_SPI_current->tuptable = tuptable = (SPITupleTable *)
		palloc(sizeof(SPITupleTable));
	tuptable->tuptabcxt = tuptabcxt;
	tuptable->alloced = tuptable->free = 128;
	tuptable->vals = (HeapTuple *) palloc(tuptable->alloced * sizeof(HeapTuple));
	tuptable->tupdesc = CreateTupleDescCopy(typeinfo);

	MemoryContextSwitchTo(oldcxt);
}

/*
 * spi_printtup
 *		store tuple retrieved by Executor into SPITupleTable
 *		of current SPI procedure
 */
void
spi_printtup(TupleTableSlot *slot, DestReceiver *self)
{
	SPITupleTable *tuptable;
	MemoryContext oldcxt;

	/*
	 * When called by Executor _SPI_curid expected to be equal to
	 * _SPI_connected
	 */
	if (_SPI_curid != _SPI_connected || _SPI_connected < 0)
		elog(ERROR, "improper call to spi_printtup");
	if (_SPI_current != &(_SPI_stack[_SPI_curid]))
		elog(ERROR, "SPI stack corrupted");

	tuptable = _SPI_current->tuptable;
	if (tuptable == NULL)
		elog(ERROR, "improper call to spi_printtup");

	oldcxt = MemoryContextSwitchTo(tuptable->tuptabcxt);

	if (tuptable->free == 0)
	{
		tuptable->free = 256;
		tuptable->alloced += tuptable->free;
		tuptable->vals = (HeapTuple *) repalloc(tuptable->vals,
									  tuptable->alloced * sizeof(HeapTuple));
	}

	tuptable->vals[tuptable->alloced - tuptable->free] =
		ExecCopySlotTuple(slot);
	(tuptable->free)--;

	MemoryContextSwitchTo(oldcxt);
}

/*
 * Static functions
 */

/*
 * Parse and plan a querystring.
 *
 * At entry, plan->argtypes, plan->nargs, and plan->cursor_options must be
 * valid.
 *
 * Results are stored into *plan (specifically, plan->plancache_list).
 * Note however that the result trees are all in CurrentMemoryContext
 * and need to be copied somewhere to survive.
 */
static void
_SPI_prepare_plan(const char *src, SPIPlanPtr plan)
{
	List	   *raw_parsetree_list;
	List	   *plancache_list;
	ListCell   *list_item;
	ErrorContextCallback spierrcontext;
	Oid		   *argtypes = plan->argtypes;
	int			nargs = plan->nargs;
	int			cursor_options = plan->cursor_options;

	/*
	 * Setup error traceback support for ereport()
	 */
	spierrcontext.callback = _SPI_error_callback;
	spierrcontext.arg = (void *) src;
	spierrcontext.previous = error_context_stack;
	error_context_stack = &spierrcontext;

	/*
	 * Parse the request string into a list of raw parse trees.
	 */
	raw_parsetree_list = pg_parse_query(src);

	/*
	 * Do parse analysis and rule rewrite for each raw parsetree, then cons up
	 * a phony plancache entry for each one.
	 */
	plancache_list = NIL;

	foreach(list_item, raw_parsetree_list)
	{
		Node	   *parsetree = (Node *) lfirst(list_item);
		List	   *stmt_list;
		CachedPlanSource *plansource;
		CachedPlan *cplan;

		/* Need a copyObject here to keep parser from modifying raw tree */
		stmt_list = pg_analyze_and_rewrite(copyObject(parsetree),
										   src, argtypes, nargs);
		stmt_list = pg_plan_queries(stmt_list, cursor_options, NULL, false);

		plansource = (CachedPlanSource *) palloc0(sizeof(CachedPlanSource));
		cplan = (CachedPlan *) palloc0(sizeof(CachedPlan));

		plansource->raw_parse_tree = parsetree;
		/* cast-away-const here is a bit ugly, but there's no reason to copy */
		plansource->query_string = (char *) src;
		plansource->commandTag = CreateCommandTag(parsetree);
		plansource->param_types = argtypes;
		plansource->num_params = nargs;
		plansource->fully_planned = true;
		plansource->fixed_result = false;
		/* no need to set search_path, generation or saved_xmin */
		plansource->resultDesc = PlanCacheComputeResultDesc(stmt_list);
		plansource->plan = cplan;

		cplan->stmt_list = stmt_list;
		cplan->fully_planned = true;

		plancache_list = lappend(plancache_list, plansource);
	}

	plan->plancache_list = plancache_list;

	/*
	 * Pop the error context stack
	 */
	error_context_stack = spierrcontext.previous;
}

/*
 * Execute the given plan with the given parameter values
 *
 * snapshot: query snapshot to use, or InvalidSnapshot for the normal
 *		behavior of taking a new snapshot for each query.
 * crosscheck_snapshot: for RI use, all others pass InvalidSnapshot
 * read_only: TRUE for read-only execution (no CommandCounterIncrement)
 * fire_triggers: TRUE to fire AFTER triggers at end of query (normal case);
 *		FALSE means any AFTER triggers are postponed to end of outer query
 * tcount: execution tuple-count limit, or 0 for none
 */
static int
_SPI_execute_plan(SPIPlanPtr plan, Datum *Values, const char *Nulls,
				  Snapshot snapshot, Snapshot crosscheck_snapshot,
				  bool read_only, bool fire_triggers, long tcount)
{
	volatile int my_res = 0;
	volatile uint32 my_processed = 0;
	volatile Oid my_lastoid = InvalidOid;
	SPITupleTable *volatile my_tuptable = NULL;
	volatile int res = 0;
	Snapshot	saveActiveSnapshot;

	/* Be sure to restore ActiveSnapshot on error exit */
	saveActiveSnapshot = ActiveSnapshot;
	PG_TRY();
	{
		ListCell   *lc1;
		ErrorContextCallback spierrcontext;
		int			nargs = plan->nargs;
		ParamListInfo paramLI;
		CachedPlan *cplan = NULL;

		/* Convert parameters to form wanted by executor */
		if (nargs > 0)
		{
			int			k;

			/* sizeof(ParamListInfoData) includes the first array element */
			paramLI = (ParamListInfo) palloc(sizeof(ParamListInfoData) +
									   (nargs - 1) *sizeof(ParamExternData));
			paramLI->numParams = nargs;

			for (k = 0; k < nargs; k++)
			{
				ParamExternData *prm = &paramLI->params[k];

				prm->value = Values[k];
				prm->isnull = (Nulls && Nulls[k] == 'n');
				prm->pflags = 0;
				prm->ptype = plan->argtypes[k];
			}
		}
		else
			paramLI = NULL;

		/*
		 * Setup error traceback support for ereport()
		 */
		spierrcontext.callback = _SPI_error_callback;
		spierrcontext.arg = NULL;
		spierrcontext.previous = error_context_stack;
		error_context_stack = &spierrcontext;

		foreach(lc1, plan->plancache_list)
		{
			CachedPlanSource *plansource = (CachedPlanSource *) lfirst(lc1);
			List	   *stmt_list;
			ListCell   *lc2;

			spierrcontext.arg = (void *) plansource->query_string;

			if (plan->saved)
			{
				/* Replan if needed, and increment plan refcount locally */
				cplan = RevalidateCachedPlan(plansource, true);
				stmt_list = cplan->stmt_list;
			}
			else
			{
				/* No replan here */
				cplan = NULL;
				stmt_list = plansource->plan->stmt_list;
			}

			foreach(lc2, stmt_list)
			{
				Node	   *stmt = (Node *) lfirst(lc2);
				bool		canSetTag;
				QueryDesc  *qdesc;
				DestReceiver *dest;

				_SPI_current->processed = 0;
				_SPI_current->lastoid = InvalidOid;
				_SPI_current->tuptable = NULL;

				if (IsA(stmt, PlannedStmt))
				{
					canSetTag = ((PlannedStmt *) stmt)->canSetTag;
				}
				else
				{
					/* utilities are canSetTag if only thing in list */
					canSetTag = (list_length(stmt_list) == 1);

					if (IsA(stmt, CopyStmt))
					{
						CopyStmt   *cstmt = (CopyStmt *) stmt;

						if (cstmt->filename == NULL)
						{
							my_res = SPI_ERROR_COPY;
							goto fail;
						}
					}
					else if (IsA(stmt, TransactionStmt))
					{
						my_res = SPI_ERROR_TRANSACTION;
						goto fail;
					}
				}

				if (read_only && !CommandIsReadOnly(stmt))
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					/* translator: %s is a SQL statement name */
					   errmsg("%s is not allowed in a non-volatile function",
							  CreateCommandTag(stmt))));

				/*
				 * If not read-only mode, advance the command counter before
				 * each command.
				 */
				if (!read_only)
					CommandCounterIncrement();

				dest = CreateDestReceiver(canSetTag ? DestSPI : DestNone,
										  NULL);

				if (snapshot == InvalidSnapshot)
				{
					/*
					 * Default read_only behavior is to use the entry-time
					 * ActiveSnapshot; if read-write, grab a full new snap.
					 */
					if (read_only)
						ActiveSnapshot = CopySnapshot(saveActiveSnapshot);
					else
						ActiveSnapshot = CopySnapshot(GetTransactionSnapshot());
				}
				else
				{
					/*
					 * We interpret read_only with a specified snapshot to be
					 * exactly that snapshot, but read-write means use the
					 * snap with advancing of command ID.
					 */
					ActiveSnapshot = CopySnapshot(snapshot);
					if (!read_only)
						ActiveSnapshot->curcid = GetCurrentCommandId(false);
				}

				if (IsA(stmt, PlannedStmt) &&
					((PlannedStmt *) stmt)->utilityStmt == NULL)
				{
					qdesc = CreateQueryDesc((PlannedStmt *) stmt,
											ActiveSnapshot,
											crosscheck_snapshot,
											dest,
											paramLI, false);
					res = _SPI_pquery(qdesc, fire_triggers,
									  canSetTag ? tcount : 0);
					FreeQueryDesc(qdesc);
				}
				else
				{
					ProcessUtility(stmt,
								   plansource->query_string,
								   paramLI,
								   false,		/* not top level */
								   dest,
								   NULL);
					/* Update "processed" if stmt returned tuples */
					if (_SPI_current->tuptable)
						_SPI_current->processed = _SPI_current->tuptable->alloced - _SPI_current->tuptable->free;
					res = SPI_OK_UTILITY;
				}
				FreeSnapshot(ActiveSnapshot);
				ActiveSnapshot = NULL;

				/*
				 * The last canSetTag query sets the status values returned to
				 * the caller.	Be careful to free any tuptables not returned,
				 * to avoid intratransaction memory leak.
				 */
				if (canSetTag)
				{
					my_processed = _SPI_current->processed;
					my_lastoid = _SPI_current->lastoid;
					SPI_freetuptable(my_tuptable);
					my_tuptable = _SPI_current->tuptable;
					my_res = res;
				}
				else
				{
					SPI_freetuptable(_SPI_current->tuptable);
					_SPI_current->tuptable = NULL;
				}
				/* we know that the receiver doesn't need a destroy call */
				if (res < 0)
				{
					my_res = res;
					goto fail;
				}
			}

			/* Done with this plan, so release refcount */
			if (cplan)
				ReleaseCachedPlan(cplan, true);
			cplan = NULL;

			/*
			 * If not read-only mode, advance the command counter after the
			 * last command.  This ensures that its effects are visible, in
			 * case it was DDL that would affect the next CachedPlanSource.
			 */
			if (!read_only)
				CommandCounterIncrement();
		}

fail:

		/* We no longer need the cached plan refcount, if any */
		if (cplan)
			ReleaseCachedPlan(cplan, true);

		/*
		 * Pop the error context stack
		 */
		error_context_stack = spierrcontext.previous;
	}
	PG_CATCH();
	{
		/* Restore global vars and propagate error */
		ActiveSnapshot = saveActiveSnapshot;
		PG_RE_THROW();
	}
	PG_END_TRY();

	ActiveSnapshot = saveActiveSnapshot;

	/* Save results for caller */
	SPI_processed = my_processed;
	SPI_lastoid = my_lastoid;
	SPI_tuptable = my_tuptable;

	/* tuptable now is caller's responsibility, not SPI's */
	_SPI_current->tuptable = NULL;

	/*
	 * If none of the queries had canSetTag, we return the last query's result
	 * code, but not its auxiliary results (for backwards compatibility).
	 */
	if (my_res == 0)
		my_res = res;

	return my_res;
}

static int
_SPI_pquery(QueryDesc *queryDesc, bool fire_triggers, long tcount)
{
	int			operation = queryDesc->operation;
	int			res;

	switch (operation)
	{
		case CMD_SELECT:
			Assert(queryDesc->plannedstmt->utilityStmt == NULL);
			if (queryDesc->plannedstmt->intoClause)		/* select into table? */
				res = SPI_OK_SELINTO;
			else if (queryDesc->dest->mydest != DestSPI)
			{
				/* Don't return SPI_OK_SELECT if we're discarding result */
				res = SPI_OK_UTILITY;
			}
			else
				res = SPI_OK_SELECT;
			break;
		case CMD_INSERT:
			if (queryDesc->plannedstmt->returningLists)
				res = SPI_OK_INSERT_RETURNING;
			else
				res = SPI_OK_INSERT;
			break;
		case CMD_DELETE:
			if (queryDesc->plannedstmt->returningLists)
				res = SPI_OK_DELETE_RETURNING;
			else
				res = SPI_OK_DELETE;
			break;
		case CMD_UPDATE:
			if (queryDesc->plannedstmt->returningLists)
				res = SPI_OK_UPDATE_RETURNING;
			else
				res = SPI_OK_UPDATE;
			break;
		default:
			return SPI_ERROR_OPUNKNOWN;
	}

#ifdef SPI_EXECUTOR_STATS
	if (ShowExecutorStats)
		ResetUsage();
#endif

	if (fire_triggers)
		AfterTriggerBeginQuery();

	ExecutorStart(queryDesc, 0);

	ExecutorRun(queryDesc, ForwardScanDirection, tcount);

	_SPI_current->processed = queryDesc->estate->es_processed;
	_SPI_current->lastoid = queryDesc->estate->es_lastoid;

	if ((res == SPI_OK_SELECT || queryDesc->plannedstmt->returningLists) &&
		queryDesc->dest->mydest == DestSPI)
	{
		if (_SPI_checktuples())
			elog(ERROR, "consistency check on SPI tuple count failed");
	}

	/* Take care of any queued AFTER triggers */
	if (fire_triggers)
		AfterTriggerEndQuery(queryDesc->estate);

	ExecutorEnd(queryDesc);

#ifdef SPI_EXECUTOR_STATS
	if (ShowExecutorStats)
		ShowUsage("SPI EXECUTOR STATS");
#endif

	return res;
}

/*
 * _SPI_error_callback
 *
 * Add context information when a query invoked via SPI fails
 */
static void
_SPI_error_callback(void *arg)
{
	const char *query = (const char *) arg;
	int			syntaxerrposition;

	/*
	 * If there is a syntax error position, convert to internal syntax error;
	 * otherwise treat the query as an item of context stack
	 */
	syntaxerrposition = geterrposition();
	if (syntaxerrposition > 0)
	{
		errposition(0);
		internalerrposition(syntaxerrposition);
		internalerrquery(query);
	}
	else
		errcontext("SQL statement \"%s\"", query);
}

/*
 * _SPI_cursor_operation()
 *
 *	Do a FETCH or MOVE in a cursor
 */
static void
_SPI_cursor_operation(Portal portal, FetchDirection direction, long count,
					  DestReceiver *dest)
{
	long		nfetched;

	/* Check that the portal is valid */
	if (!PortalIsValid(portal))
		elog(ERROR, "invalid portal in SPI cursor operation");

	/* Push the SPI stack */
	if (_SPI_begin_call(true) < 0)
		elog(ERROR, "SPI cursor operation called while not connected");

	/* Reset the SPI result (note we deliberately don't touch lastoid) */
	SPI_processed = 0;
	SPI_tuptable = NULL;
	_SPI_current->processed = 0;
	_SPI_current->tuptable = NULL;

	/* Run the cursor */
	nfetched = PortalRunFetch(portal,
							  direction,
							  count,
							  dest);

	/*
	 * Think not to combine this store with the preceding function call. If
	 * the portal contains calls to functions that use SPI, then SPI_stack is
	 * likely to move around while the portal runs.  When control returns,
	 * _SPI_current will point to the correct stack entry... but the pointer
	 * may be different than it was beforehand. So we must be sure to re-fetch
	 * the pointer after the function call completes.
	 */
	_SPI_current->processed = nfetched;

	if (dest->mydest == DestSPI && _SPI_checktuples())
		elog(ERROR, "consistency check on SPI tuple count failed");

	/* Put the result into place for access by caller */
	SPI_processed = _SPI_current->processed;
	SPI_tuptable = _SPI_current->tuptable;

	/* tuptable now is caller's responsibility, not SPI's */
	_SPI_current->tuptable = NULL;

	/* Pop the SPI stack */
	_SPI_end_call(true);
}


static MemoryContext
_SPI_execmem(void)
{
	return MemoryContextSwitchTo(_SPI_current->execCxt);
}

static MemoryContext
_SPI_procmem(void)
{
	return MemoryContextSwitchTo(_SPI_current->procCxt);
}

/*
 * _SPI_begin_call: begin a SPI operation within a connected procedure
 */
static int
_SPI_begin_call(bool execmem)
{
	if (_SPI_curid + 1 != _SPI_connected)
		return SPI_ERROR_UNCONNECTED;
	_SPI_curid++;
	if (_SPI_current != &(_SPI_stack[_SPI_curid]))
		elog(ERROR, "SPI stack corrupted");

	if (execmem)				/* switch to the Executor memory context */
		_SPI_execmem();

	return 0;
}

/*
 * _SPI_end_call: end a SPI operation within a connected procedure
 *
 * Note: this currently has no failure return cases, so callers don't check
 */
static int
_SPI_end_call(bool procmem)
{
	/*
	 * We're returning to procedure where _SPI_curid == _SPI_connected - 1
	 */
	_SPI_curid--;

	if (procmem)				/* switch to the procedure memory context */
	{
		_SPI_procmem();
		/* and free Executor memory */
		MemoryContextResetAndDeleteChildren(_SPI_current->execCxt);
	}

	return 0;
}

static bool
_SPI_checktuples(void)
{
	uint32		processed = _SPI_current->processed;
	SPITupleTable *tuptable = _SPI_current->tuptable;
	bool		failed = false;

	if (tuptable == NULL)		/* spi_dest_startup was not called */
		failed = true;
	else if (processed != (tuptable->alloced - tuptable->free))
		failed = true;

	return failed;
}

/*
 * Make an "unsaved" copy of the given plan, in a child context of parentcxt.
 */
static SPIPlanPtr
_SPI_copy_plan(SPIPlanPtr plan, MemoryContext parentcxt)
{
	SPIPlanPtr	newplan;
	MemoryContext plancxt;
	MemoryContext oldcxt;
	ListCell   *lc;

	Assert(!plan->saved);		/* not currently supported */

	/*
	 * Create a memory context for the plan.  We don't expect the plan to be
	 * very large, so use smaller-than-default alloc parameters.
	 */
	plancxt = AllocSetContextCreate(parentcxt,
									"SPI Plan",
									ALLOCSET_SMALL_MINSIZE,
									ALLOCSET_SMALL_INITSIZE,
									ALLOCSET_SMALL_MAXSIZE);
	oldcxt = MemoryContextSwitchTo(plancxt);

	/* Copy the SPI plan into its own context */
	newplan = (SPIPlanPtr) palloc(sizeof(_SPI_plan));
	newplan->magic = _SPI_PLAN_MAGIC;
	newplan->saved = false;
	newplan->plancache_list = NIL;
	newplan->plancxt = plancxt;
	newplan->cursor_options = plan->cursor_options;
	newplan->nargs = plan->nargs;
	if (plan->nargs > 0)
	{
		newplan->argtypes = (Oid *) palloc(plan->nargs * sizeof(Oid));
		memcpy(newplan->argtypes, plan->argtypes, plan->nargs * sizeof(Oid));
	}
	else
		newplan->argtypes = NULL;

	foreach(lc, plan->plancache_list)
	{
		CachedPlanSource *plansource = (CachedPlanSource *) lfirst(lc);
		CachedPlanSource *newsource;
		CachedPlan *cplan;
		CachedPlan *newcplan;

		/* Note: we assume we don't need to revalidate the plan */
		cplan = plansource->plan;

		newsource = (CachedPlanSource *) palloc0(sizeof(CachedPlanSource));
		newcplan = (CachedPlan *) palloc0(sizeof(CachedPlan));

		newsource->raw_parse_tree = copyObject(plansource->raw_parse_tree);
		newsource->query_string = pstrdup(plansource->query_string);
		newsource->commandTag = plansource->commandTag;
		newsource->param_types = newplan->argtypes;
		newsource->num_params = newplan->nargs;
		newsource->fully_planned = plansource->fully_planned;
		newsource->fixed_result = plansource->fixed_result;
		/* no need to worry about seach_path, generation or saved_xmin */
		if (plansource->resultDesc)
			newsource->resultDesc = CreateTupleDescCopy(plansource->resultDesc);
		newsource->plan = newcplan;

		newcplan->stmt_list = copyObject(cplan->stmt_list);
		newcplan->fully_planned = cplan->fully_planned;

		newplan->plancache_list = lappend(newplan->plancache_list, newsource);
	}

	MemoryContextSwitchTo(oldcxt);

	return newplan;
}

/*
 * Make a "saved" copy of the given plan, entrusting everything to plancache.c
 */
static SPIPlanPtr
_SPI_save_plan(SPIPlanPtr plan)
{
	SPIPlanPtr	newplan;
	MemoryContext plancxt;
	MemoryContext oldcxt;
	ListCell   *lc;

	Assert(!plan->saved);		/* not currently supported */

	/*
	 * Create a memory context for the plan.  We don't expect the plan to be
	 * very large, so use smaller-than-default alloc parameters.
	 */
	plancxt = AllocSetContextCreate(CacheMemoryContext,
									"SPI Plan",
									ALLOCSET_SMALL_MINSIZE,
									ALLOCSET_SMALL_INITSIZE,
									ALLOCSET_SMALL_MAXSIZE);
	oldcxt = MemoryContextSwitchTo(plancxt);

	/* Copy the SPI plan into its own context */
	newplan = (SPIPlanPtr) palloc(sizeof(_SPI_plan));
	newplan->magic = _SPI_PLAN_MAGIC;
	newplan->saved = true;
	newplan->plancache_list = NIL;
	newplan->plancxt = plancxt;
	newplan->cursor_options = plan->cursor_options;
	newplan->nargs = plan->nargs;
	if (plan->nargs > 0)
	{
		newplan->argtypes = (Oid *) palloc(plan->nargs * sizeof(Oid));
		memcpy(newplan->argtypes, plan->argtypes, plan->nargs * sizeof(Oid));
	}
	else
		newplan->argtypes = NULL;

	foreach(lc, plan->plancache_list)
	{
		CachedPlanSource *plansource = (CachedPlanSource *) lfirst(lc);
		CachedPlanSource *newsource;
		CachedPlan *cplan;

		/* Note: we assume we don't need to revalidate the plan */
		cplan = plansource->plan;

		newsource = CreateCachedPlan(plansource->raw_parse_tree,
									 plansource->query_string,
									 plansource->commandTag,
									 newplan->argtypes,
									 newplan->nargs,
									 newplan->cursor_options,
									 cplan->stmt_list,
									 true,
									 false);

		newplan->plancache_list = lappend(newplan->plancache_list, newsource);
	}

	MemoryContextSwitchTo(oldcxt);

	return newplan;
}

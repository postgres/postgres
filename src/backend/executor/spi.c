/*-------------------------------------------------------------------------
 *
 * spi.c
 *				Server Programming Interface
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/spi.c,v 1.107.2.1 2005/02/10 20:37:15 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/printtup.h"
#include "catalog/heap.h"
#include "executor/spi_priv.h"
#include "tcop/tcopprot.h"
#include "utils/lsyscache.h"


uint32		SPI_processed = 0;
Oid			SPI_lastoid = InvalidOid;
SPITupleTable *SPI_tuptable = NULL;
int			SPI_result;

static _SPI_connection *_SPI_stack = NULL;
static _SPI_connection *_SPI_current = NULL;
static int	_SPI_connected = -1;
static int	_SPI_curid = -1;

static int	_SPI_execute(const char *src, int tcount, _SPI_plan *plan);
static int	_SPI_pquery(QueryDesc *queryDesc, bool runit,
						bool useCurrentSnapshot, int tcount);

static int _SPI_execute_plan(_SPI_plan *plan,
							 Datum *Values, const char *Nulls,
							 bool useCurrentSnapshot, int tcount);

static void _SPI_cursor_operation(Portal portal, bool forward, int count,
					  DestReceiver *dest);

static _SPI_plan *_SPI_copy_plan(_SPI_plan *plan, int location);

static int	_SPI_begin_call(bool execmem);
static int	_SPI_end_call(bool procmem);
static MemoryContext _SPI_execmem(void);
static MemoryContext _SPI_procmem(void);
static bool _SPI_checktuples(void);


/* =================== interface functions =================== */

int
SPI_connect(void)
{
	_SPI_connection *new_SPI_stack;

	/*
	 * When procedure called by Executor _SPI_curid expected to be equal
	 * to _SPI_connected
	 */
	if (_SPI_curid != _SPI_connected)
		return SPI_ERROR_CONNECT;

	if (_SPI_stack == NULL)
	{
		if (_SPI_connected != -1)
			elog(ERROR, "SPI stack corrupted");
		new_SPI_stack = (_SPI_connection *) malloc(sizeof(_SPI_connection));
	}
	else
	{
		if (_SPI_connected < 0)
			elog(ERROR, "SPI stack corrupted");
		new_SPI_stack = (_SPI_connection *) realloc(_SPI_stack,
						 (_SPI_connected + 2) * sizeof(_SPI_connection));
	}

	if (new_SPI_stack == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));

	/*
	 * We' returning to procedure where _SPI_curid == _SPI_connected - 1
	 */
	_SPI_stack = new_SPI_stack;
	_SPI_connected++;

	_SPI_current = &(_SPI_stack[_SPI_connected]);
	_SPI_current->processed = 0;
	_SPI_current->tuptable = NULL;

	/*
	 * Create memory contexts for this procedure
	 *
	 * XXX it would be better to use PortalContext as the parent context, but
	 * we may not be inside a portal (consider deferred-trigger
	 * execution).
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
	MemoryContextDelete(_SPI_current->procCxt);

	/*
	 * Reset result variables, especially SPI_tuptable which is probably
	 * pointing at a just-deleted tuptable
	 */
	SPI_processed = 0;
	SPI_lastoid = InvalidOid;
	SPI_tuptable = NULL;

	/*
	 * After _SPI_begin_call _SPI_connected == _SPI_curid. Now we are
	 * closing connection to SPI and returning to upper Executor and so
	 * _SPI_connected must be equal to _SPI_curid.
	 */
	_SPI_connected--;
	_SPI_curid--;
	if (_SPI_connected == -1)
	{
		free(_SPI_stack);
		_SPI_stack = NULL;
		_SPI_current = NULL;
	}
	else
	{
		_SPI_connection *new_SPI_stack;

		new_SPI_stack = (_SPI_connection *) realloc(_SPI_stack,
						 (_SPI_connected + 1) * sizeof(_SPI_connection));
		/* This could only fail with a pretty stupid malloc package ... */
		if (new_SPI_stack == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
		_SPI_stack = new_SPI_stack;
		_SPI_current = &(_SPI_stack[_SPI_connected]);
	}

	return SPI_OK_FINISH;

}

/*
 * Clean up SPI state at transaction commit or abort (we don't care which).
 */
void
AtEOXact_SPI(void)
{
	/*
	 * Note that memory contexts belonging to SPI stack entries will be
	 * freed automatically, so we can ignore them here.  We just need to
	 * restore our static variables to initial state.
	 */
	if (_SPI_stack != NULL)		/* there was abort */
		free(_SPI_stack);
	_SPI_current = _SPI_stack = NULL;
	_SPI_connected = _SPI_curid = -1;
	SPI_processed = 0;
	SPI_lastoid = InvalidOid;
	SPI_tuptable = NULL;
}

void
SPI_push(void)
{
	_SPI_curid++;
}

void
SPI_pop(void)
{
	_SPI_curid--;
}

int
SPI_exec(const char *src, int tcount)
{
	int			res;

	if (src == NULL || tcount < 0)
		return SPI_ERROR_ARGUMENT;

	res = _SPI_begin_call(true);
	if (res < 0)
		return res;

	res = _SPI_execute(src, tcount, NULL);

	_SPI_end_call(true);
	return res;
}

int
SPI_execp(void *plan, Datum *Values, const char *Nulls, int tcount)
{
	int			res;

	if (plan == NULL || tcount < 0)
		return SPI_ERROR_ARGUMENT;

	if (((_SPI_plan *) plan)->nargs > 0 && Values == NULL)
		return SPI_ERROR_PARAM;

	res = _SPI_begin_call(true);
	if (res < 0)
		return res;

	res = _SPI_execute_plan((_SPI_plan *) plan, Values, Nulls, false, tcount);

	_SPI_end_call(true);
	return res;
}

/*
 * SPI_execp_current -- identical to SPI_execp, except that we expose the
 * Executor option to use a current snapshot instead of the normal
 * QuerySnapshot.  This is currently not documented in spi.sgml because
 * it is only intended for use by RI triggers.
 */
int
SPI_execp_current(void *plan, Datum *Values, const char *Nulls,
				  bool useCurrentSnapshot, int tcount)
{
	int			res;

	if (plan == NULL || tcount < 0)
		return SPI_ERROR_ARGUMENT;

	if (((_SPI_plan *) plan)->nargs > 0 && Values == NULL)
		return SPI_ERROR_PARAM;

	res = _SPI_begin_call(true);
	if (res < 0)
		return res;

	res = _SPI_execute_plan((_SPI_plan *) plan, Values, Nulls,
							useCurrentSnapshot, tcount);

	_SPI_end_call(true);
	return res;
}

void *
SPI_prepare(const char *src, int nargs, Oid *argtypes)
{
	_SPI_plan  *plan;

	if (src == NULL || nargs < 0 || (nargs > 0 && argtypes == NULL))
	{
		SPI_result = SPI_ERROR_ARGUMENT;
		return NULL;
	}

	SPI_result = _SPI_begin_call(true);
	if (SPI_result < 0)
		return NULL;

	plan = (_SPI_plan *) palloc(sizeof(_SPI_plan));		/* Executor context */
	plan->argtypes = argtypes;
	plan->nargs = nargs;

	SPI_result = _SPI_execute(src, 0, plan);

	if (SPI_result >= 0)		/* copy plan to procedure context */
		plan = _SPI_copy_plan(plan, _SPI_CPLAN_PROCXT);
	else
		plan = NULL;

	_SPI_end_call(true);

	return (void *) plan;
}

void *
SPI_saveplan(void *plan)
{
	_SPI_plan  *newplan;

	if (plan == NULL)
	{
		SPI_result = SPI_ERROR_ARGUMENT;
		return NULL;
	}

	SPI_result = _SPI_begin_call(false);		/* don't change context */
	if (SPI_result < 0)
		return NULL;

	newplan = _SPI_copy_plan((_SPI_plan *) plan, _SPI_CPLAN_TOPCXT);

	_SPI_curid--;
	SPI_result = 0;

	return (void *) newplan;

}

int
SPI_freeplan(void *plan)
{
	_SPI_plan  *spiplan = (_SPI_plan *) plan;

	if (plan == NULL)
		return SPI_ERROR_ARGUMENT;

	MemoryContextDelete(spiplan->plancxt);
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

TupleDesc
SPI_copytupledesc(TupleDesc tupdesc)
{
	MemoryContext oldcxt = NULL;
	TupleDesc	ctupdesc;

	if (tupdesc == NULL)
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

	ctupdesc = CreateTupleDescCopy(tupdesc);

	if (oldcxt)
		MemoryContextSwitchTo(oldcxt);

	return ctupdesc;
}

TupleTableSlot *
SPI_copytupleintoslot(HeapTuple tuple, TupleDesc tupdesc)
{
	MemoryContext oldcxt = NULL;
	TupleTableSlot *cslot;
	HeapTuple	ctuple;
	TupleDesc	ctupdesc;

	if (tuple == NULL || tupdesc == NULL)
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
	ctupdesc = CreateTupleDescCopy(tupdesc);

	cslot = MakeTupleTableSlot();
	ExecSetSlotDescriptor(cslot, ctupdesc, true);
	cslot = ExecStoreTuple(ctuple, cslot, InvalidBuffer, true);

	if (oldcxt)
		MemoryContextSwitchTo(oldcxt);

	return cslot;
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
	bool		isnull;
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
	for (i = 0; i < numberOfAttributes; i++)
	{
		v[i] = heap_getattr(tuple, i + 1, rel->rd_att, &isnull);
		n[i] = (isnull) ? 'n' : ' ';
	}

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
		 * copy the identification info of the old tuple: t_ctid, t_self,
		 * and OID (if any)
		 */
		mtuple->t_data->t_ctid = tuple->t_data->t_ctid;
		mtuple->t_self = tuple->t_self;
		mtuple->t_tableOid = tuple->t_tableOid;
		if (rel->rd_rel->relhasoids)
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
	Datum		origval,
				val,
				result;
	bool		isnull;
	Oid			typoid,
				foutoid,
				typelem;
	int32		typmod;
	bool		typisvarlena;

	SPI_result = 0;

	if (fnumber > tuple->t_data->t_natts || fnumber == 0 ||
		fnumber <= FirstLowInvalidHeapAttributeNumber)
	{
		SPI_result = SPI_ERROR_NOATTRIBUTE;
		return NULL;
	}

	origval = heap_getattr(tuple, fnumber, tupdesc, &isnull);
	if (isnull)
		return NULL;

	if (fnumber > 0)
	{
		typoid = tupdesc->attrs[fnumber - 1]->atttypid;
		typmod = tupdesc->attrs[fnumber - 1]->atttypmod;
	}
	else
	{
		typoid = (SystemAttributeDefinition(fnumber, true))->atttypid;
		typmod = -1;
	}

	getTypeOutputInfo(typoid, &foutoid, &typelem, &typisvarlena);

	/*
	 * If we have a toasted datum, forcibly detoast it here to avoid
	 * memory leakage inside the type's output routine.
	 */
	if (typisvarlena)
		val = PointerGetDatum(PG_DETOAST_DATUM(origval));
	else
		val = origval;

	result = OidFunctionCall3(foutoid,
							  val,
							  ObjectIdGetDatum(typelem),
							  Int32GetDatum(typmod));

	/* Clean up detoasted copy, if any */
	if (val != origval)
		pfree(DatumGetPointer(val));

	return DatumGetCString(result);
}

Datum
SPI_getbinval(HeapTuple tuple, TupleDesc tupdesc, int fnumber, bool *isnull)
{
	SPI_result = 0;

	if (fnumber > tuple->t_data->t_natts || fnumber == 0 ||
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
SPI_cursor_open(const char *name, void *plan, Datum *Values, const char *Nulls)
{
	_SPI_plan  *spiplan = (_SPI_plan *) plan;
	List	   *qtlist = spiplan->qtlist;
	List	   *ptlist = spiplan->ptlist;
	Query	   *queryTree;
	Plan	   *planTree;
	ParamListInfo paramLI;
	MemoryContext oldcontext;
	Portal		portal;
	int			k;

	/* Ensure that the plan contains only one regular SELECT query */
	if (length(ptlist) != 1 || length(qtlist) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_CURSOR_DEFINITION),
				 errmsg("cannot open multi-query plan as cursor")));
	queryTree = (Query *) lfirst((List *) lfirst(qtlist));
	planTree = (Plan *) lfirst(ptlist);

	if (queryTree->commandType != CMD_SELECT)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_CURSOR_DEFINITION),
				 errmsg("cannot open non-SELECT query as cursor")));
	if (queryTree->into != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_CURSOR_DEFINITION),
				 errmsg("cannot open SELECT INTO query as cursor")));

	/* Increment CommandCounter to see changes made by now */
	CommandCounterIncrement();

	/* Reset SPI result */
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

	/* Switch to portals memory and copy the parsetree and plan to there */
	oldcontext = MemoryContextSwitchTo(PortalGetHeapMemory(portal));
	queryTree = copyObject(queryTree);
	planTree = copyObject(planTree);

	/* If the plan has parameters, set them up */
	if (spiplan->nargs > 0)
	{
		paramLI = (ParamListInfo) palloc0((spiplan->nargs + 1) *
										  sizeof(ParamListInfoData));

		for (k = 0; k < spiplan->nargs; k++)
		{
			paramLI[k].kind = PARAM_NUM;
			paramLI[k].id = k + 1;
			paramLI[k].isnull = (Nulls && Nulls[k] == 'n');
			if (paramLI[k].isnull)
			{
				/* nulls just copy */
				paramLI[k].value = Values[k];
			}
			else
			{
				/* pass-by-ref values must be copied into portal context */
				int16		paramTypLen;
				bool		paramTypByVal;

				get_typlenbyval(spiplan->argtypes[k],
								&paramTypLen, &paramTypByVal);
				paramLI[k].value = datumCopy(Values[k],
											 paramTypByVal, paramTypLen);
			}
		}
		paramLI[k].kind = PARAM_INVALID;
	}
	else
		paramLI = NULL;

	/*
	 * Set up the portal.
	 */
	PortalDefineQuery(portal,
					  NULL,		/* unfortunately don't have sourceText */
					  "SELECT", /* cursor's query is always a SELECT */
					  makeList1(queryTree),
					  makeList1(planTree),
					  PortalGetHeapMemory(portal));

	MemoryContextSwitchTo(oldcontext);

	/*
	 * Set up options for portal.
	 */
	portal->cursorOptions &= ~(CURSOR_OPT_SCROLL | CURSOR_OPT_NO_SCROLL);
	if (ExecSupportsBackwardScan(planTree))
		portal->cursorOptions |= CURSOR_OPT_SCROLL;
	else
		portal->cursorOptions |= CURSOR_OPT_NO_SCROLL;

	/*
	 * Start portal execution.
	 */
	PortalStart(portal, paramLI);

	Assert(portal->strategy == PORTAL_ONE_SELECT);

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
SPI_cursor_fetch(Portal portal, bool forward, int count)
{
	_SPI_cursor_operation(portal, forward, count,
						  CreateDestReceiver(SPI, NULL));
	/* we know that the SPI receiver doesn't need a destroy call */
}


/*
 * SPI_cursor_move()
 *
 *	Move in a cursor
 */
void
SPI_cursor_move(Portal portal, bool forward, int count)
{
	_SPI_cursor_operation(portal, forward, count, None_Receiver);
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
spi_printtup(HeapTuple tuple, TupleDesc tupdesc, DestReceiver *self)
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

	tuptable->vals[tuptable->alloced - tuptable->free] = heap_copytuple(tuple);
	(tuptable->free)--;

	MemoryContextSwitchTo(oldcxt);
}

/*
 * Static functions
 */

/*
 * Plan and optionally execute a querystring.
 *
 * If plan != NULL, just prepare plan tree, else execute immediately.
 */
static int
_SPI_execute(const char *src, int tcount, _SPI_plan *plan)
{
	List	   *raw_parsetree_list;
	List	   *query_list_list;
	List	   *plan_list;
	List	   *list_item;
	int			nargs = 0;
	Oid		   *argtypes = NULL;
	int			res = 0;

	if (plan)
	{
		nargs = plan->nargs;
		argtypes = plan->argtypes;
	}

	/* Increment CommandCounter to see changes made by now */
	CommandCounterIncrement();

	/* Reset state (only needed in case string is empty) */
	SPI_processed = 0;
	SPI_lastoid = InvalidOid;
	SPI_tuptable = NULL;
	_SPI_current->tuptable = NULL;

	/*
	 * Parse the request string into a list of raw parse trees.
	 */
	raw_parsetree_list = pg_parse_query(src);

	/*
	 * Do parse analysis and rule rewrite for each raw parsetree.
	 *
	 * We save the querytrees from each raw parsetree as a separate sublist.
	 * This allows _SPI_execute_plan() to know where the boundaries
	 * between original queries fall.
	 */
	query_list_list = NIL;
	plan_list = NIL;

	foreach(list_item, raw_parsetree_list)
	{
		Node	   *parsetree = (Node *) lfirst(list_item);
		List	   *query_list;
		List	   *query_list_item;

		query_list = pg_analyze_and_rewrite(parsetree, argtypes, nargs);

		query_list_list = lappend(query_list_list, query_list);

		/* Reset state for each original parsetree */
		/* (at most one of its querytrees will be marked canSetTag) */
		SPI_processed = 0;
		SPI_lastoid = InvalidOid;
		SPI_tuptable = NULL;
		_SPI_current->tuptable = NULL;

		foreach(query_list_item, query_list)
		{
			Query	   *queryTree = (Query *) lfirst(query_list_item);
			Plan	   *planTree;
			QueryDesc  *qdesc;
			DestReceiver *dest;

			planTree = pg_plan_query(queryTree);
			plan_list = lappend(plan_list, planTree);

			dest = CreateDestReceiver(queryTree->canSetTag ? SPI : None, NULL);
			if (queryTree->commandType == CMD_UTILITY)
			{
				if (IsA(queryTree->utilityStmt, CopyStmt))
				{
					CopyStmt   *stmt = (CopyStmt *) queryTree->utilityStmt;

					if (stmt->filename == NULL)
						return SPI_ERROR_COPY;
				}
				else if (IsA(queryTree->utilityStmt, DeclareCursorStmt) ||
						 IsA(queryTree->utilityStmt, ClosePortalStmt) ||
						 IsA(queryTree->utilityStmt, FetchStmt))
					return SPI_ERROR_CURSOR;
				else if (IsA(queryTree->utilityStmt, TransactionStmt))
					return SPI_ERROR_TRANSACTION;
				res = SPI_OK_UTILITY;
				if (plan == NULL)
				{
					ProcessUtility(queryTree->utilityStmt, dest, NULL);
					CommandCounterIncrement();
				}
			}
			else if (plan == NULL)
			{
				qdesc = CreateQueryDesc(queryTree, planTree, dest,
										NULL, false);
				res = _SPI_pquery(qdesc, true, false,
								  queryTree->canSetTag ? tcount : 0);
				if (res < 0)
					return res;
				CommandCounterIncrement();
			}
			else
			{
				qdesc = CreateQueryDesc(queryTree, planTree, dest,
										NULL, false);
				res = _SPI_pquery(qdesc, false, false, 0);
				if (res < 0)
					return res;
			}
		}
	}

	if (plan)
	{
		plan->qtlist = query_list_list;
		plan->ptlist = plan_list;
	}

	return res;
}

static int
_SPI_execute_plan(_SPI_plan *plan, Datum *Values, const char *Nulls,
				  bool useCurrentSnapshot, int tcount)
{
	List	   *query_list_list = plan->qtlist;
	List	   *plan_list = plan->ptlist;
	List	   *query_list_list_item;
	int			nargs = plan->nargs;
	int			res = 0;
	ParamListInfo paramLI;

	/* Increment CommandCounter to see changes made by now */
	CommandCounterIncrement();

	/* Convert parameters to form wanted by executor */
	if (nargs > 0)
	{
		int			k;

		paramLI = (ParamListInfo)
			palloc0((nargs + 1) * sizeof(ParamListInfoData));

		for (k = 0; k < nargs; k++)
		{
			paramLI[k].kind = PARAM_NUM;
			paramLI[k].id = k + 1;
			paramLI[k].isnull = (Nulls && Nulls[k] == 'n');
			paramLI[k].value = Values[k];
		}
		paramLI[k].kind = PARAM_INVALID;
	}
	else
		paramLI = NULL;

	/* Reset state (only needed in case string is empty) */
	SPI_processed = 0;
	SPI_lastoid = InvalidOid;
	SPI_tuptable = NULL;
	_SPI_current->tuptable = NULL;

	foreach(query_list_list_item, query_list_list)
	{
		List	   *query_list = lfirst(query_list_list_item);
		List	   *query_list_item;

		/* Reset state for each original parsetree */
		/* (at most one of its querytrees will be marked canSetTag) */
		SPI_processed = 0;
		SPI_lastoid = InvalidOid;
		SPI_tuptable = NULL;
		_SPI_current->tuptable = NULL;

		foreach(query_list_item, query_list)
		{
			Query	   *queryTree = (Query *) lfirst(query_list_item);
			Plan	   *planTree;
			QueryDesc  *qdesc;
			DestReceiver *dest;

			planTree = lfirst(plan_list);
			plan_list = lnext(plan_list);

			dest = CreateDestReceiver(queryTree->canSetTag ? SPI : None, NULL);
			if (queryTree->commandType == CMD_UTILITY)
			{
				ProcessUtility(queryTree->utilityStmt, dest, NULL);
				res = SPI_OK_UTILITY;
				CommandCounterIncrement();
			}
			else
			{
				qdesc = CreateQueryDesc(queryTree, planTree, dest,
										paramLI, false);
				res = _SPI_pquery(qdesc, true, useCurrentSnapshot,
								  queryTree->canSetTag ? tcount : 0);
				if (res < 0)
					return res;
				CommandCounterIncrement();
			}
		}
	}

	return res;
}

static int
_SPI_pquery(QueryDesc *queryDesc, bool runit,
			bool useCurrentSnapshot, int tcount)
{
	int			operation = queryDesc->operation;
	int			res;
	Oid			save_lastoid;

	switch (operation)
	{
		case CMD_SELECT:
			res = SPI_OK_SELECT;
			if (queryDesc->parsetree->into != NULL)		/* select into table */
			{
				res = SPI_OK_SELINTO;
				queryDesc->dest = None_Receiver;		/* don't output results */
			}
			break;
		case CMD_INSERT:
			res = SPI_OK_INSERT;
			break;
		case CMD_DELETE:
			res = SPI_OK_DELETE;
			break;
		case CMD_UPDATE:
			res = SPI_OK_UPDATE;
			break;
		default:
			return SPI_ERROR_OPUNKNOWN;
	}

	if (!runit)					/* plan preparation, don't execute */
		return res;

#ifdef SPI_EXECUTOR_STATS
	if (ShowExecutorStats)
		ResetUsage();
#endif

	ExecutorStart(queryDesc, useCurrentSnapshot, false);

	ExecutorRun(queryDesc, ForwardScanDirection, (long) tcount);

	_SPI_current->processed = queryDesc->estate->es_processed;
	save_lastoid = queryDesc->estate->es_lastoid;

	if (operation == CMD_SELECT && queryDesc->dest->mydest == SPI)
	{
		if (_SPI_checktuples())
			elog(ERROR, "consistency check on SPI tuple count failed");
	}

	if (queryDesc->dest->mydest == SPI)
	{
		SPI_processed = _SPI_current->processed;
		SPI_lastoid = save_lastoid;
		SPI_tuptable = _SPI_current->tuptable;
	}
	else if (res == SPI_OK_SELECT)
	{
		/* Don't return SPI_OK_SELECT if we discarded the result */
		res = SPI_OK_UTILITY;
	}

	ExecutorEnd(queryDesc);

	FreeQueryDesc(queryDesc);

#ifdef SPI_EXECUTOR_STATS
	if (ShowExecutorStats)
		ShowUsage("SPI EXECUTOR STATS");
#endif

	return res;
}

/*
 * _SPI_cursor_operation()
 *
 *	Do a FETCH or MOVE in a cursor
 */
static void
_SPI_cursor_operation(Portal portal, bool forward, int count,
					  DestReceiver *dest)
{
	long		nfetched;

	/* Check that the portal is valid */
	if (!PortalIsValid(portal))
		elog(ERROR, "invalid portal in SPI cursor operation");

	/* Push the SPI stack */
	if (_SPI_begin_call(true) < 0)
		elog(ERROR, "SPI cursor operation called while not connected");

	/* Reset the SPI result */
	SPI_processed = 0;
	SPI_tuptable = NULL;
	_SPI_current->processed = 0;
	_SPI_current->tuptable = NULL;

	/* Run the cursor */
	nfetched = PortalRunFetch(portal,
							  forward ? FETCH_FORWARD : FETCH_BACKWARD,
							  (long) count,
							  dest);

	/*
	 * Think not to combine this store with the preceding function call.
	 * If the portal contains calls to functions that use SPI, then
	 * SPI_stack is likely to move around while the portal runs.  When
	 * control returns, _SPI_current will point to the correct stack
	 * entry... but the pointer may be different than it was beforehand.
	 * So we must be sure to re-fetch the pointer after the function call
	 * completes.
	 */
	_SPI_current->processed = nfetched;

	if (dest->mydest == SPI && _SPI_checktuples())
		elog(ERROR, "consistency check on SPI tuple count failed");

	/* Put the result into place for access by caller */
	SPI_processed = _SPI_current->processed;
	SPI_tuptable = _SPI_current->tuptable;

	/* Pop the SPI stack */
	_SPI_end_call(true);
}


static MemoryContext
_SPI_execmem()
{
	return MemoryContextSwitchTo(_SPI_current->execCxt);
}

static MemoryContext
_SPI_procmem()
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

static _SPI_plan *
_SPI_copy_plan(_SPI_plan *plan, int location)
{
	_SPI_plan  *newplan;
	MemoryContext oldcxt;
	MemoryContext plancxt;
	MemoryContext parentcxt;

	/* Determine correct parent for the plan's memory context */
	if (location == _SPI_CPLAN_PROCXT)
		parentcxt = _SPI_current->procCxt;
	else if (location == _SPI_CPLAN_TOPCXT)
		parentcxt = TopMemoryContext;
	else
/* (this case not currently used) */
		parentcxt = CurrentMemoryContext;

	/*
	 * Create a memory context for the plan.  We don't expect the plan to
	 * be very large, so use smaller-than-default alloc parameters.
	 */
	plancxt = AllocSetContextCreate(parentcxt,
									"SPI Plan",
									ALLOCSET_SMALL_MINSIZE,
									ALLOCSET_SMALL_INITSIZE,
									ALLOCSET_SMALL_MAXSIZE);
	oldcxt = MemoryContextSwitchTo(plancxt);

	/* Copy the SPI plan into its own context */
	newplan = (_SPI_plan *) palloc(sizeof(_SPI_plan));
	newplan->plancxt = plancxt;
	newplan->qtlist = (List *) copyObject(plan->qtlist);
	newplan->ptlist = (List *) copyObject(plan->ptlist);
	newplan->nargs = plan->nargs;
	if (plan->nargs > 0)
	{
		newplan->argtypes = (Oid *) palloc(plan->nargs * sizeof(Oid));
		memcpy(newplan->argtypes, plan->argtypes, plan->nargs * sizeof(Oid));
	}
	else
		newplan->argtypes = NULL;

	MemoryContextSwitchTo(oldcxt);

	return newplan;
}

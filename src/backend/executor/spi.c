/*-------------------------------------------------------------------------
 *
 * spi.c
 *				Server Programming Interface
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/spi.c,v 1.84 2003/01/21 22:06:12 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/printtup.h"
#include "catalog/heap.h"
#include "commands/portalcmds.h"
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
static int	_SPI_pquery(QueryDesc *queryDesc, bool runit, int tcount);

static int _SPI_execute_plan(_SPI_plan *plan,
				  Datum *Values, const char *Nulls, int tcount);

static void _SPI_cursor_operation(Portal portal, bool forward, int count,
					  CommandDest dest);

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
			elog(FATAL, "SPI_connect: no connection(s) expected");
		new_SPI_stack = (_SPI_connection *) malloc(sizeof(_SPI_connection));
	}
	else
	{
		if (_SPI_connected <= -1)
			elog(FATAL, "SPI_connect: some connection(s) expected");
		new_SPI_stack = (_SPI_connection *) realloc(_SPI_stack,
						 (_SPI_connected + 2) * sizeof(_SPI_connection));
	}

	if (new_SPI_stack == NULL)
		elog(ERROR, "Memory exhausted in SPI_connect");

	/*
	 * We' returning to procedure where _SPI_curid == _SPI_connected - 1
	 */
	_SPI_stack = new_SPI_stack;
	_SPI_connected++;

	_SPI_current = &(_SPI_stack[_SPI_connected]);
	_SPI_current->processed = 0;
	_SPI_current->tuptable = NULL;

	/* Create memory contexts for this procedure */
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
			elog(ERROR, "Memory exhausted in SPI_finish");
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

	res = _SPI_execute_plan((_SPI_plan *) plan, Values, Nulls, tcount);

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
			elog(FATAL, "SPI: stack corrupted");
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
			elog(FATAL, "SPI: stack corrupted");
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
			elog(FATAL, "SPI: stack corrupted");
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

	if (rel == NULL || tuple == NULL || natts <= 0 || attnum == NULL || Values == NULL)
	{
		SPI_result = SPI_ERROR_ARGUMENT;
		return NULL;
	}

	if (_SPI_curid + 1 == _SPI_connected)		/* connected */
	{
		if (_SPI_current != &(_SPI_stack[_SPI_curid + 1]))
			elog(FATAL, "SPI: stack corrupted");
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

	if (!getTypeOutputInfo(typoid, &foutoid, &typelem, &typisvarlena))
	{
		SPI_result = SPI_ERROR_NOOUTFUNC;
		return NULL;
	}

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
			elog(FATAL, "SPI: stack corrupted");
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
	static int	unnamed_portal_count = 0;

	_SPI_plan  *spiplan = (_SPI_plan *) plan;
	List	   *qtlist = spiplan->qtlist;
	List	   *ptlist = spiplan->ptlist;
	Query	   *queryTree;
	Plan	   *planTree;
	ParamListInfo paramLI;
	QueryDesc  *queryDesc;
	MemoryContext oldcontext;
	Portal		portal;
	char		portalname[64];
	int			k;

	/* Ensure that the plan contains only one regular SELECT query */
	if (length(ptlist) != 1 || length(qtlist) != 1)
		elog(ERROR, "cannot open multi-query plan as cursor");
	queryTree = (Query *) lfirst((List *) lfirst(qtlist));
	planTree = (Plan *) lfirst(ptlist);

	if (queryTree->commandType != CMD_SELECT)
		elog(ERROR, "plan in SPI_cursor_open() is not a SELECT");
	if (queryTree->isPortal)
		elog(ERROR, "plan in SPI_cursor_open() must NOT be a DECLARE already");
	else if (queryTree->into != NULL)
		elog(ERROR, "plan in SPI_cursor_open() must NOT be a SELECT INTO");

	/* Increment CommandCounter to see changes made by now */
	CommandCounterIncrement();

	/* Reset SPI result */
	SPI_processed = 0;
	SPI_tuptable = NULL;
	_SPI_current->processed = 0;
	_SPI_current->tuptable = NULL;

	if (name == NULL)
	{
		/* Make up a portal name if none given */
		for (;;)
		{
			unnamed_portal_count++;
			if (unnamed_portal_count < 0)
				unnamed_portal_count = 0;
			sprintf(portalname, "<unnamed cursor %d>", unnamed_portal_count);
			if (GetPortalByName(portalname) == NULL)
				break;
		}

		name = portalname;
	}
	else
	{
		/* Ensure the portal doesn't exist already */
		portal = GetPortalByName(name);
		if (portal != NULL)
			elog(ERROR, "cursor \"%s\" already in use", name);
	}

	/* Create the portal */
	portal = CreatePortal(name);
	if (portal == NULL)
		elog(ERROR, "failed to create portal \"%s\"", name);

	/* Switch to portals memory and copy the parsetree and plan to there */
	oldcontext = MemoryContextSwitchTo(PortalGetHeapMemory(portal));
	queryTree = copyObject(queryTree);
	planTree = copyObject(planTree);

	/* Modify the parsetree to be a cursor */
	queryTree->isPortal = true;
	queryTree->into = makeNode(RangeVar);
	queryTree->into->relname = pstrdup(name);
	queryTree->isBinary = false;

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

	/* Create the QueryDesc object */
	queryDesc = CreateQueryDesc(queryTree, planTree, SPI, NULL,
								paramLI, false);

	/* Start the executor */
	ExecutorStart(queryDesc);

	/* Arrange to shut down the executor if portal is dropped */
	PortalSetQuery(portal, queryDesc, PortalCleanup);

	/* Switch back to the callers memory context */
	MemoryContextSwitchTo(oldcontext);

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
	_SPI_cursor_operation(portal, forward, count, SPI);
}


/*
 * SPI_cursor_move()
 *
 *	Move in a cursor
 */
void
SPI_cursor_move(Portal portal, bool forward, int count)
{
	_SPI_cursor_operation(portal, forward, count, None);
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

	PortalDrop(portal);
}

/* =================== private functions =================== */

/*
 * spi_dest_setup
 *		Initialize to receive tuples from Executor into SPITupleTable
 *		of current SPI procedure
 */
void
spi_dest_setup(DestReceiver *self, int operation,
			   const char *portalName, TupleDesc typeinfo)
{
	SPITupleTable *tuptable;
	MemoryContext oldcxt;
	MemoryContext tuptabcxt;

	/*
	 * When called by Executor _SPI_curid expected to be equal to
	 * _SPI_connected
	 */
	if (_SPI_curid != _SPI_connected || _SPI_connected < 0)
		elog(FATAL, "SPI: improper call to spi_dest_setup");
	if (_SPI_current != &(_SPI_stack[_SPI_curid]))
		elog(FATAL, "SPI: stack corrupted in spi_dest_setup");

	if (_SPI_current->tuptable != NULL)
		elog(FATAL, "SPI: improper call to spi_dest_setup");

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
		elog(FATAL, "SPI: improper call to spi_printtup");
	if (_SPI_current != &(_SPI_stack[_SPI_curid]))
		elog(FATAL, "SPI: stack corrupted in spi_printtup");

	tuptable = _SPI_current->tuptable;
	if (tuptable == NULL)
		elog(FATAL, "SPI: improper call to spi_printtup");

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
	StringInfoData stri;
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
	initStringInfo(&stri);
	appendStringInfo(&stri, "%s", src);

	raw_parsetree_list = pg_parse_query(&stri, argtypes, nargs);

	/*
	 * Do parse analysis and rule rewrite for each raw parsetree.
	 *
	 * We save the querytrees from each raw parsetree as a separate
	 * sublist.  This allows _SPI_execute_plan() to know where the
	 * boundaries between original queries fall.
	 */
	query_list_list = NIL;
	plan_list = NIL;

	foreach(list_item, raw_parsetree_list)
	{
		Node	   *parsetree = (Node *) lfirst(list_item);
		CmdType		origCmdType;
		bool		foundOriginalQuery = false;
		List	   *query_list;
		List	   *query_list_item;

		switch (nodeTag(parsetree))
		{
			case T_InsertStmt:
				origCmdType = CMD_INSERT;
				break;
			case T_DeleteStmt:
				origCmdType = CMD_DELETE;
				break;
			case T_UpdateStmt:
				origCmdType = CMD_UPDATE;
				break;
			case T_SelectStmt:
				origCmdType = CMD_SELECT;
				break;
			default:
				/* Otherwise, never match commandType */
				origCmdType = CMD_UNKNOWN;
				break;
		}

		if (plan)
			plan->origCmdType = origCmdType;

		query_list = pg_analyze_and_rewrite(parsetree);

		query_list_list = lappend(query_list_list, query_list);

		/* Reset state for each original parsetree */
		SPI_processed = 0;
		SPI_lastoid = InvalidOid;
		SPI_tuptable = NULL;
		_SPI_current->tuptable = NULL;

		foreach(query_list_item, query_list)
		{
			Query	   *queryTree = (Query *) lfirst(query_list_item);
			Plan	   *planTree;
			bool		canSetResult;
			QueryDesc  *qdesc;

			planTree = pg_plan_query(queryTree);
			plan_list = lappend(plan_list, planTree);

			/*
			 * This query can set the SPI result if it is the original
			 * query, or if it is an INSTEAD query of the same kind as the
			 * original and we haven't yet seen the original query.
			 */
			if (queryTree->querySource == QSRC_ORIGINAL)
			{
				canSetResult = true;
				foundOriginalQuery = true;
			}
			else if (!foundOriginalQuery &&
					 queryTree->commandType == origCmdType &&
					 (queryTree->querySource == QSRC_INSTEAD_RULE ||
					  queryTree->querySource == QSRC_QUAL_INSTEAD_RULE))
				canSetResult = true;
			else
				canSetResult = false;

			if (queryTree->commandType == CMD_UTILITY)
			{
				if (IsA(queryTree->utilityStmt, CopyStmt))
				{
					CopyStmt   *stmt = (CopyStmt *) queryTree->utilityStmt;

					if (stmt->filename == NULL)
						return SPI_ERROR_COPY;
				}
				else if (IsA(queryTree->utilityStmt, ClosePortalStmt) ||
						 IsA(queryTree->utilityStmt, FetchStmt))
					return SPI_ERROR_CURSOR;
				else if (IsA(queryTree->utilityStmt, TransactionStmt))
					return SPI_ERROR_TRANSACTION;
				res = SPI_OK_UTILITY;
				if (plan == NULL)
				{
					ProcessUtility(queryTree->utilityStmt, None, NULL);
					CommandCounterIncrement();
				}
			}
			else if (plan == NULL)
			{
				qdesc = CreateQueryDesc(queryTree, planTree,
										canSetResult ? SPI : None,
										NULL, NULL, false);
				res = _SPI_pquery(qdesc, true, canSetResult ? tcount : 0);
				if (res < 0)
					return res;
				CommandCounterIncrement();
			}
			else
			{
				qdesc = CreateQueryDesc(queryTree, planTree,
										canSetResult ? SPI : None,
										NULL, NULL, false);
				res = _SPI_pquery(qdesc, false, 0);
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
				  int tcount)
{
	List	   *query_list_list = plan->qtlist;
	List	   *plan_list = plan->ptlist;
	List	   *query_list_list_item;
	int			nargs = plan->nargs;
	int			res = 0;

	/* Increment CommandCounter to see changes made by now */
	CommandCounterIncrement();

	/* Reset state (only needed in case string is empty) */
	SPI_processed = 0;
	SPI_lastoid = InvalidOid;
	SPI_tuptable = NULL;
	_SPI_current->tuptable = NULL;

	foreach(query_list_list_item, query_list_list)
	{
		List   *query_list = lfirst(query_list_list_item);
		List   *query_list_item;
		bool	foundOriginalQuery = false;

		/* Reset state for each original parsetree */
		SPI_processed = 0;
		SPI_lastoid = InvalidOid;
		SPI_tuptable = NULL;
		_SPI_current->tuptable = NULL;

		foreach(query_list_item, query_list)
		{
			Query  *queryTree = (Query *) lfirst(query_list_item);
			Plan	   *planTree;
			bool		canSetResult;
			QueryDesc  *qdesc;

			planTree = lfirst(plan_list);
			plan_list = lnext(plan_list);

			/*
			 * This query can set the SPI result if it is the original
			 * query, or if it is an INSTEAD query of the same kind as the
			 * original and we haven't yet seen the original query.
			 */
			if (queryTree->querySource == QSRC_ORIGINAL)
			{
				canSetResult = true;
				foundOriginalQuery = true;
			}
			else if (!foundOriginalQuery &&
					 queryTree->commandType == plan->origCmdType &&
					 (queryTree->querySource == QSRC_INSTEAD_RULE ||
					  queryTree->querySource == QSRC_QUAL_INSTEAD_RULE))
				canSetResult = true;
			else
				canSetResult = false;

			if (queryTree->commandType == CMD_UTILITY)
			{
				res = SPI_OK_UTILITY;
				ProcessUtility(queryTree->utilityStmt, None, NULL);
				CommandCounterIncrement();
			}
			else
			{
				ParamListInfo paramLI;

				if (nargs > 0)
				{
					int			k;

					paramLI = (ParamListInfo)
						palloc0((nargs + 1) * sizeof(ParamListInfoData));

					for (k = 0; k < plan->nargs; k++)
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

				qdesc = CreateQueryDesc(queryTree, planTree,
										canSetResult ? SPI : None,
										NULL, paramLI, false);
				res = _SPI_pquery(qdesc, true, canSetResult ? tcount : 0);
				if (res < 0)
					return res;
				CommandCounterIncrement();
			}
		}
	}

	return res;
}

static int
_SPI_pquery(QueryDesc *queryDesc, bool runit, int tcount)
{
	Query	   *parseTree = queryDesc->parsetree;
	int			operation = queryDesc->operation;
	CommandDest dest = queryDesc->dest;
	bool		isRetrieveIntoPortal = false;
	bool		isRetrieveIntoRelation = false;
	char	   *intoName = NULL;
	int			res;
	Oid			save_lastoid;

	switch (operation)
	{
		case CMD_SELECT:
			res = SPI_OK_SELECT;
			if (parseTree->isPortal)
			{
				isRetrieveIntoPortal = true;
				intoName = parseTree->into->relname;
				parseTree->isBinary = false;	/* */

				return SPI_ERROR_CURSOR;

			}
			else if (parseTree->into != NULL)	/* select into table */
			{
				res = SPI_OK_SELINTO;
				isRetrieveIntoRelation = true;
				queryDesc->dest = None; /* */
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

	ExecutorStart(queryDesc);

	/*
	 * Don't work currently --- need to rearrange callers so that we
	 * prepare the portal before doing ExecutorStart() etc. See
	 * pquery.c for the correct order of operations.
	 */
	if (isRetrieveIntoPortal)
		elog(FATAL, "SPI_select: retrieve into portal not implemented");

	ExecutorRun(queryDesc, ForwardScanDirection, (long) tcount);

	_SPI_current->processed = queryDesc->estate->es_processed;
	save_lastoid = queryDesc->estate->es_lastoid;

	if (operation == CMD_SELECT && queryDesc->dest == SPI)
	{
		if (_SPI_checktuples())
			elog(FATAL, "SPI_select: # of processed tuples check failed");
	}

	if (dest == SPI)
	{
		SPI_processed = _SPI_current->processed;
		SPI_lastoid = save_lastoid;
		SPI_tuptable = _SPI_current->tuptable;
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
					  CommandDest dest)
{
	QueryDesc  *querydesc;
	EState	   *estate;
	MemoryContext oldcontext;
	ScanDirection direction;
	CommandDest olddest;

	/* Check that the portal is valid */
	if (!PortalIsValid(portal))
		elog(ERROR, "invalid portal in SPI cursor operation");

	/* Push the SPI stack */
	_SPI_begin_call(true);

	/* Reset the SPI result */
	SPI_processed = 0;
	SPI_tuptable = NULL;
	_SPI_current->processed = 0;
	_SPI_current->tuptable = NULL;

	/* Switch to the portals memory context */
	oldcontext = MemoryContextSwitchTo(PortalGetHeapMemory(portal));

	querydesc = PortalGetQueryDesc(portal);
	estate = querydesc->estate;

	/* Save the queries command destination and set it to SPI (for fetch) */
	/* or None (for move) */
	olddest = querydesc->dest;
	querydesc->dest = dest;

	/* Run the executor like PerformPortalFetch and remember states */
	if (forward)
	{
		if (portal->atEnd)
			direction = NoMovementScanDirection;
		else
			direction = ForwardScanDirection;

		ExecutorRun(querydesc, direction, (long) count);

		if (estate->es_processed > 0)
			portal->atStart = false;	/* OK to back up now */
		if (count <= 0 || (int) estate->es_processed < count)
			portal->atEnd = true;		/* we retrieved 'em all */
	}
	else
	{
		if (portal->atStart)
			direction = NoMovementScanDirection;
		else
			direction = BackwardScanDirection;

		ExecutorRun(querydesc, direction, (long) count);

		if (estate->es_processed > 0)
			portal->atEnd = false;		/* OK to go forward now */
		if (count <= 0 || (int) estate->es_processed < count)
			portal->atStart = true;		/* we retrieved 'em all */
	}

	_SPI_current->processed = estate->es_processed;

	/* Restore the old command destination and switch back to callers */
	/* memory context */
	querydesc->dest = olddest;
	MemoryContextSwitchTo(oldcontext);

	if (dest == SPI && _SPI_checktuples())
		elog(FATAL, "SPI_fetch: # of processed tuples check failed");

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
 * _SPI_begin_call
 *
 */
static int
_SPI_begin_call(bool execmem)
{
	if (_SPI_curid + 1 != _SPI_connected)
		return SPI_ERROR_UNCONNECTED;
	_SPI_curid++;
	if (_SPI_current != &(_SPI_stack[_SPI_curid]))
		elog(FATAL, "SPI: stack corrupted");

	if (execmem)				/* switch to the Executor memory context */
		_SPI_execmem();

	return 0;
}

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

	if (tuptable == NULL)	/* spi_dest_setup was not called */
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
	else						/* (this case not currently used) */
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
	newplan->origCmdType = plan->origCmdType;

	MemoryContextSwitchTo(oldcxt);

	return newplan;
}

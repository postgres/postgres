/*-------------------------------------------------------------------------
 *
 * spi.c--
 *		Server Programming Interface
 *
 *-------------------------------------------------------------------------
 */
#include "executor/spi.h"
#include "access/printtup.h"
#include "fmgr.h"

typedef struct {
    QueryTreeList	*qtlist;	/* malloced */
    uint32		processed;	/* by Executor */
    SPITupleTable	*tuptable;
    Portal		portal;		/* portal per procedure */
    MemoryContext	savedcxt;
    CommandId		savedId;
} _SPI_connection;

static Portal _SPI_portal = (Portal) NULL;
static _SPI_connection *_SPI_stack = NULL;
static _SPI_connection *_SPI_current = NULL;
static int _SPI_connected = -1;
static int _SPI_curid = -1;

uint32 SPI_processed = 0;
SPITupleTable *SPI_tuptable;
int SPI_result;

void spi_printtup (HeapTuple tuple, TupleDesc tupdesc);

typedef struct {
    QueryTreeList	*qtlist;
    List		*ptlist;
    int			nargs;
    Oid			*argtypes;
} _SPI_plan;

static int _SPI_execute (char *src, int tcount, _SPI_plan *plan);
static int _SPI_pquery (QueryDesc *queryDesc, EState *state, int tcount);
#if 0
static void _SPI_fetch (FetchStmt *stmt);
#endif
static int _SPI_execute_plan (_SPI_plan *plan, 
				char **Values, char *Nulls, int tcount);

static _SPI_plan *_SPI_copy_plan (_SPI_plan *plan, bool local);

static int _SPI_begin_call (bool execmem);
static int _SPI_end_call (bool procmem);
static MemoryContext _SPI_execmem (void);
static MemoryContext _SPI_procmem (void);
static bool _SPI_checktuples (bool isRetrieveIntoRelation);

#ifdef SPI_EXECUTOR_STATS
extern int ShowExecutorStats;
extern void ResetUsage (void);
extern void ShowUsage (void);
#endif

int
SPI_connect ()
{
    char pname[64];
    PortalVariableMemory pvmem;
    
    /*
     * It's possible on startup and after commit/abort.
     * In future we'll catch commit/abort in some way...
     */
    strcpy (pname, "<SPI manager>");
    _SPI_portal = GetPortalByName (pname);
    if ( !PortalIsValid (_SPI_portal) )
    {
    	if ( _SPI_stack != NULL )	/* there was abort */
    	    free (_SPI_stack);
    	_SPI_current = _SPI_stack = NULL;
    	_SPI_connected = _SPI_curid = -1;
    	SPI_processed = 0;
    	SPI_tuptable = NULL;
    	_SPI_portal = CreatePortal (pname);
    	if ( !PortalIsValid (_SPI_portal) )
    	    elog (FATAL, "SPI_connect: global initialization failed");
    }
    	
    /*
     * When procedure called by Executor _SPI_curid expected to be
     * equal to _SPI_connected
     */
    if ( _SPI_curid != _SPI_connected )
    	return (SPI_ERROR_CONNECT);
    
    if ( _SPI_stack == NULL )
    {
    	if ( _SPI_connected != -1 )
    	    elog (FATAL, "SPI_connect: no connection(s) expected");
    	_SPI_stack = (_SPI_connection *) malloc (sizeof (_SPI_connection));
    }
    else
    {
    	if ( _SPI_connected <= -1 )
    	    elog (FATAL, "SPI_connect: some connection(s) expected");
    	_SPI_stack = (_SPI_connection *) realloc (_SPI_stack, 
    			(_SPI_connected + 1) * sizeof (_SPI_connection));
    }
    /*
     * We' returning to procedure where _SPI_curid == _SPI_connected - 1
     */
    _SPI_connected++;
    
    _SPI_current = &(_SPI_stack[_SPI_connected]);
    _SPI_current->qtlist = NULL;
    _SPI_current->processed = 0;
    _SPI_current->tuptable = NULL;
    
    /* Create Portal for this procedure ... */
    sprintf (pname, "<SPI %d>", _SPI_connected);
    _SPI_current->portal = CreatePortal (pname);
    if ( !PortalIsValid (_SPI_current->portal) )
    	elog (FATAL, "SPI_connect: initialization failed");
    
    /* ... and switch to Portal' Variable memory - procedure' context */
    pvmem = PortalGetVariableMemory (_SPI_current->portal);
    _SPI_current->savedcxt = MemoryContextSwitchTo ((MemoryContext)pvmem);
    
    _SPI_current->savedId = GetScanCommandId ();
    SetScanCommandId (GetCurrentCommandId ());
    
    return (SPI_OK_CONNECT);
    
}

int
SPI_finish ()
{
    int res;
    
    res = _SPI_begin_call (false);	/* live in procedure memory */
    if ( res < 0 )
    	return (res);
    
    /* Restore memory context as it was before procedure call */
    MemoryContextSwitchTo (_SPI_current->savedcxt);
    PortalDestroy (&(_SPI_current->portal));
    
    SetScanCommandId (_SPI_current->savedId);
    
    /* 
     * After _SPI_begin_call _SPI_connected == _SPI_curid.
     * Now we are closing connection to SPI and returning to upper 
     * Executor and so _SPI_connected must be equal to _SPI_curid.
     */
    _SPI_connected--;
    _SPI_curid--;
    if ( _SPI_connected == -1 )
    {
    	free (_SPI_stack);
    	_SPI_stack = NULL;
    }
    else
    {
    	_SPI_stack = (_SPI_connection *) realloc (_SPI_stack, 
    			(_SPI_connected + 1) * sizeof (_SPI_connection));
    	_SPI_current = &(_SPI_stack[_SPI_connected]);
    }
    
    return (SPI_OK_FINISH);
    
}

int
SPI_exec (char *src, int tcount)
{
    int res;
    
    if ( src == NULL || tcount < 0 )
    	return (SPI_ERROR_ARGUMENT);
    
    res = _SPI_begin_call (true);
    if ( res < 0 )
    	return (res);
    
    res = _SPI_execute (src, tcount, NULL);
    
    _SPI_end_call (true);
    return (res);
}

int 
SPI_execp (void *plan, char **Values, char *Nulls, int tcount)
{
    int res;
    
    if ( plan == NULL || tcount < 0 )
    	return (SPI_ERROR_ARGUMENT);
    
    if ( ((_SPI_plan *)plan)->nargs > 0 && 
    		( Values == NULL || Nulls == NULL ) )
    	return (SPI_ERROR_PARAM);
    
    res = _SPI_begin_call (true);
    if ( res < 0 )
    	return (res);
    
    res = _SPI_execute_plan ((_SPI_plan *)plan, Values, Nulls, tcount);
    
    _SPI_end_call (true);
    return (res);
}

void *
SPI_prepare (char *src, int nargs, Oid *argtypes)
{
    _SPI_plan *plan;
    
    if ( nargs < 0 || ( nargs > 0 && argtypes == NULL ) )
    {
    	SPI_result = SPI_ERROR_ARGUMENT;
    	return (NULL);
    }
    
    SPI_result = _SPI_begin_call (true);
    if ( SPI_result < 0 )
    	return (NULL);
    
    plan = (_SPI_plan *) palloc (sizeof (_SPI_plan));	/* Executor context */
    plan->argtypes = argtypes;
    plan->nargs = nargs;
    
    SPI_result = _SPI_execute (src, 0, plan);
    
    if ( SPI_result >= 0 )	/* copy plan to local space */
    	plan = _SPI_copy_plan (plan, true);
    else
    	plan = NULL;
    
    _SPI_end_call (true);
    
    return ((void *)plan);
    
}

void * 
SPI_saveplan (void *plan)
{
    _SPI_plan *newplan;
    
    if ( plan == NULL )
    {
    	SPI_result = SPI_ERROR_ARGUMENT;
    	return (NULL);
    }
    
    SPI_result = _SPI_begin_call (false);	/* don't change context */
    if ( SPI_result < 0 )
    	return (NULL);
    
    newplan = _SPI_copy_plan ((_SPI_plan *)plan, false);
    
    _SPI_curid--;
    SPI_result = 0;
    
    return ((void *)newplan);
    
}

int
SPI_fnumber (TupleDesc tupdesc, char *fname)
{
    int res;
        
    if ( _SPI_curid + 1 != _SPI_connected )
    	return (SPI_ERROR_UNCONNECTED);
    
    for (res = 0; res < tupdesc->natts; res++)
    {
    	if ( strcmp (tupdesc->attrs[res]->attname.data, fname) == 0 )
    	    return (res + 1);
    }
    
    return (SPI_ERROR_NOATTRIBUTE);
}

char *
SPI_getvalue (HeapTuple tuple, TupleDesc tupdesc, int fnumber)
{
    char *val;
    bool isnull;
    Oid foutoid;
    
    SPI_result = 0;
    if ( _SPI_curid + 1 != _SPI_connected )
    {
    	SPI_result = SPI_ERROR_UNCONNECTED;
    	return (NULL);
    }
    
    if ( tuple->t_natts < fnumber || fnumber <= 0 )
    	return (NULL);
    
    val = heap_getattr (tuple, InvalidBuffer, fnumber, tupdesc, &isnull);
    if ( isnull )
    	return (NULL);
    foutoid = typtoout ((Oid) tupdesc->attrs[fnumber - 1]->atttypid);
    if ( !OidIsValid (foutoid) )
    {
    	SPI_result = SPI_ERROR_NOOUTFUNC;
    	return (NULL);
    }
    
    return (fmgr (foutoid, val, gettypelem (tupdesc->attrs[fnumber - 1]->atttypid)));
}

char *
SPI_getbinval (HeapTuple tuple, TupleDesc tupdesc, int fnumber, bool *isnull)
{
    char *val;
    
    *isnull = true;
    SPI_result = 0;
    if ( _SPI_curid + 1 != _SPI_connected )
    {
    	SPI_result = SPI_ERROR_UNCONNECTED;
    	return (NULL);
    }
    
    if ( tuple->t_natts < fnumber || fnumber <= 0 )
    	return (NULL);
    
    val = heap_getattr (tuple, InvalidBuffer, fnumber, tupdesc, isnull);
    
    return (val);
}

char *
SPI_gettype (TupleDesc tupdesc, int fnumber)
{
    HeapTuple typeTuple;
    
    SPI_result = 0;
    if ( _SPI_curid + 1 != _SPI_connected )
    {
    	SPI_result = SPI_ERROR_UNCONNECTED;
    	return (NULL);
    }
    
    if ( tupdesc->natts < fnumber || fnumber <= 0 )
    {
    	SPI_result = SPI_ERROR_NOATTRIBUTE;
    	return (NULL);
    }
    
    typeTuple = SearchSysCacheTuple (TYPOID, 
    			ObjectIdGetDatum (tupdesc->attrs[fnumber - 1]->atttypid),
    			0, 0, 0);
    
    if ( !HeapTupleIsValid (typeTuple) )
    {
    	SPI_result = SPI_ERROR_TYPUNKNOWN;
    	return (NULL);
    }
    
    return (pstrdup (((TypeTupleForm) GETSTRUCT (typeTuple))->typname.data));
}

Oid
SPI_gettypeid (TupleDesc tupdesc, int fnumber)
{
    
    SPI_result = 0;
    if ( _SPI_curid + 1 != _SPI_connected )
    {
    	SPI_result = SPI_ERROR_UNCONNECTED;
    	return (InvalidOid);
    }
    
    if ( tupdesc->natts < fnumber || fnumber <= 0 )
    {
    	SPI_result = SPI_ERROR_NOATTRIBUTE;
    	return (InvalidOid);
    }
    
    return (tupdesc->attrs[fnumber - 1]->atttypid);
}

char *
SPI_getrelname (Relation rel)
{
    
    SPI_result = 0;
    if ( _SPI_curid + 1 != _SPI_connected )
    {
    	SPI_result = SPI_ERROR_UNCONNECTED;
    	return (NULL);
    }
    
    return (pstrdup (rel->rd_rel->relname.data));
}

/*
 * spi_printtup --
 *	store tuple retrieved by Executor into SPITupleTable
 *	of current SPI procedure
 *
 */
void
spi_printtup (HeapTuple tuple, TupleDesc tupdesc)
{
    SPITupleTable *tuptable;
    MemoryContext oldcxt;
    
    /*
     * When called by Executor _SPI_curid expected to be
     * equal to _SPI_connected
     */
    if ( _SPI_curid != _SPI_connected || _SPI_connected < 0 )
    	elog (FATAL, "SPI: improper call to spi_printtup");
    if ( _SPI_current != &(_SPI_stack[_SPI_curid]) )
    	elog (FATAL, "SPI: stack corrupted in spi_printtup");
    
    oldcxt = _SPI_procmem ();	/* switch to procedure memory context */
    
    tuptable = _SPI_current->tuptable;
    if ( tuptable == NULL )
    {
    	_SPI_current->tuptable = tuptable = (SPITupleTable *)
    				palloc (sizeof (SPITupleTable));
    	tuptable->alloced = tuptable->free = 128;
    	tuptable->vals = (HeapTuple *) palloc (tuptable->alloced * sizeof (HeapTuple));
    	tuptable->tupdesc = CreateTupleDescCopy (tupdesc);
    }
    else if ( tuptable->free == 0 )
    {
    	tuptable->free = 256;
    	tuptable->alloced += tuptable->free;
    	tuptable->vals = (HeapTuple *) repalloc (tuptable->vals,
    			tuptable->alloced * sizeof (HeapTuple));
    }
    
    tuptable->vals[tuptable->alloced - tuptable->free] = heap_copytuple (tuple);
    (tuptable->free)--;
    
    MemoryContextSwitchTo (oldcxt);
    return;
}

/*
 * Static functions
 */

static int
_SPI_execute (char *src, int tcount, _SPI_plan *plan)
{
    QueryTreeList	*queryTree_list;
    List	        *planTree_list;
    List		*ptlist;
    QueryDesc		*qdesc;
    Query		*queryTree;
    Plan		*planTree;
    EState		*state;
    int			qlen;
    int			nargs = 0;
    Oid			*argtypes = NULL;
    int			res;
    int			i;
    
    /* Increment CommandCounter to see changes made by now */
    CommandCounterIncrement ();
    
    SPI_processed = 0;
    SPI_tuptable = NULL;
    _SPI_current->tuptable = NULL;
    _SPI_current->qtlist = NULL;
    
    if ( plan )
    {
    	nargs = plan->nargs;
    	argtypes = plan->argtypes;
    }
    ptlist = planTree_list = (List *)
	pg_plan (src, argtypes, nargs, &queryTree_list, None);
    
    _SPI_current->qtlist = queryTree_list;
    
    qlen = queryTree_list->len;
    for (i=0; ;i++)
    {
    	queryTree = (Query*) (queryTree_list->qtrees[i]);
    	planTree = lfirst(planTree_list);
	
	planTree_list = lnext (planTree_list);
	
	if ( queryTree->commandType == CMD_UTILITY )
	{
	    if ( nodeTag (queryTree->utilityStmt ) == T_CopyStmt )
	    {
	    	CopyStmt *stmt = (CopyStmt *)(queryTree->utilityStmt);
	    	    
	    	if ( stmt->filename == NULL )
	    	    return (SPI_ERROR_COPY);
	    }
	    else if ( nodeTag (queryTree->utilityStmt ) == T_ClosePortalStmt || 
	    		nodeTag (queryTree->utilityStmt ) == T_FetchStmt )
	    	return (SPI_ERROR_CURSOR);
	    else if ( nodeTag (queryTree->utilityStmt ) == T_TransactionStmt )
	    	return (SPI_ERROR_TRANSACTION);
	    res = SPI_OK_UTILITY;
	    if ( plan == NULL )
	    {
	    	ProcessUtility (queryTree->utilityStmt, None);
	    	if ( i < qlen - 1 )
	    	    CommandCounterIncrement ();
	    	else
	    	    return (res);
	    }
	    else if ( i >= qlen - 1 )
	    	break;
	}
	else if ( plan == NULL )
	{
    	    qdesc = CreateQueryDesc (queryTree, planTree, 
    	    				( i < qlen - 1 ) ? None : SPI);
    	    state = CreateExecutorState();
    	    res = _SPI_pquery (qdesc, state, ( i < qlen - 1 ) ? 0 : tcount);
	    if ( res < 0 || i >= qlen - 1 )
	    	return (res);
	    CommandCounterIncrement ();
	}
	else
	{
    	    qdesc = CreateQueryDesc (queryTree, planTree, 
    	    				( i < qlen - 1 ) ? None : SPI);
    	    res = _SPI_pquery (qdesc, NULL, ( i < qlen - 1 ) ? 0 : tcount);
	    if ( res < 0 )
	    	return (res);
	    if ( i >= qlen - 1 )
	    	break;
	}
    }
    
    plan->qtlist = queryTree_list;
    plan->ptlist = ptlist;
    
    return (res);
    
}

static int
_SPI_execute_plan (_SPI_plan *plan, char **Values, char *Nulls, int tcount)
{
    QueryTreeList	*queryTree_list = plan->qtlist;
    List	        *planTree_list = plan->ptlist;
    QueryDesc		*qdesc;
    Query		*queryTree;
    Plan		*planTree;
    EState		*state;
    int			nargs = plan->nargs;
    int			qlen = queryTree_list->len;
    int			res;
    int			i, k;
    
    /* Increment CommandCounter to see changes made by now */
    CommandCounterIncrement ();
    
    SPI_processed = 0;
    SPI_tuptable = NULL;
    _SPI_current->tuptable = NULL;
    _SPI_current->qtlist = NULL;
    
    for (i=0; ;i++)
    {
    	queryTree = (Query*) (queryTree_list->qtrees[i]);
    	planTree = lfirst(planTree_list);
	
	planTree_list = lnext (planTree_list);
	
	if ( queryTree->commandType == CMD_UTILITY )
	{
	    ProcessUtility (queryTree->utilityStmt, None);
	    if ( i < qlen - 1 )
	    	CommandCounterIncrement ();
	    else
	    	return (SPI_OK_UTILITY);
	}
	else
	{
    	    qdesc = CreateQueryDesc (queryTree, planTree, 
    	    				( i < qlen - 1 ) ? None : SPI);
    	    state = CreateExecutorState();
    	    if ( nargs > 0 )
    	    {
    	    	ParamListInfo paramLI = (ParamListInfo) palloc ((nargs + 1) * 
    	    					sizeof (ParamListInfoData));
    	    	state->es_param_list_info = paramLI;
    		for (k = 0; k < plan->nargs; paramLI++, k++)
    		{
    	    	    paramLI->kind = PARAM_NUM;
    	    	    paramLI->id = k+1;
    	    	    paramLI->isnull = (Nulls[k] != 0);
    	    	    paramLI->value = (Datum) Values[k];
    		}
    		paramLI->kind = PARAM_INVALID;
    	    }
    	    else
    	    	state->es_param_list_info = NULL;
    	    res = _SPI_pquery (qdesc, state, ( i < qlen - 1 ) ? 0 : tcount);
	    if ( res < 0 || i >= qlen - 1 )
	    	return (res);
	    CommandCounterIncrement ();
	}
    }
    
    return (res);
    
}

static int
_SPI_pquery (QueryDesc *queryDesc, EState *state, int tcount)
{
    Query 	*parseTree;
    Plan 	*plan;
    int		operation;
    TupleDesc   tupdesc;
    bool	isRetrieveIntoPortal = false;
    bool	isRetrieveIntoRelation = false;
    char*	intoName = NULL;
    int		res;
    
    parseTree = queryDesc->parsetree;
    plan =	queryDesc->plantree;
    operation = queryDesc->operation;
    
    switch (operation)
    {
    	case CMD_SELECT:
    	    res = SPI_OK_SELECT;
	    if (parseTree->isPortal)
	    {
	    	isRetrieveIntoPortal = true;
	    	intoName = parseTree->into;
	    	parseTree->isBinary = false;	/* */
	    	
	    	return (SPI_ERROR_CURSOR);
	    	
	    }
	    else if (parseTree->into != NULL)	/* select into table */
	    {
	    	res = SPI_OK_SELINTO;
	    	isRetrieveIntoRelation = true;
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
    	    return (SPI_ERROR_OPUNKNOWN);
    }
    
    if ( state == NULL )		/* plan preparation */
    	return (res);
#ifdef SPI_EXECUTOR_STATS
    if ( ShowExecutorStats )
    	ResetUsage ();
#endif
    tupdesc = ExecutorStart (queryDesc, state);
    
    /* Don't work currently */
    if (isRetrieveIntoPortal)
    {
	ProcessPortal(intoName,
		      parseTree,
		      plan,
		      state,
		      tupdesc,
		      None);
	return (SPI_OK_CURSOR);
    }
    
    ExecutorRun (queryDesc, state, EXEC_RUN, tcount);
    
    _SPI_current->processed = state->es_processed;
    if ( operation == CMD_SELECT && queryDesc->dest == SPI )
    {
    	if ( _SPI_checktuples (isRetrieveIntoRelation) )
    	    elog (FATAL, "SPI_select: # of processed tuples check failed");
    }
    
    ExecutorEnd (queryDesc, state);
    
#ifdef SPI_EXECUTOR_STATS
    if ( ShowExecutorStats )
    {
    	fprintf (stderr, "! Executor Stats:\n");
    	ShowUsage ();
    }
#endif
    
    if ( queryDesc->dest == SPI )
    {
    	SPI_processed = _SPI_current->processed;
    	SPI_tuptable = _SPI_current->tuptable;
    }
    
    return (res);

}

#if 0
static void
_SPI_fetch (FetchStmt *stmt)
{
    char	  *name = stmt->portalname;
    int		  feature = ( stmt->direction == FORWARD ) ? EXEC_FOR : EXEC_BACK;
    int		  count = stmt->howMany;
    Portal	  portal;
    QueryDesc	  *queryDesc;
    EState	  *state;
    MemoryContext context;
    
    if ( name == NULL)
	elog (FATAL, "SPI_fetch from blank portal unsupported");
    
    portal = GetPortalByName (name);
    if ( !PortalIsValid (portal) )
	elog (FATAL, "SPI_fetch: portal \"%s\" not found", name);
    
    context = MemoryContextSwitchTo((MemoryContext)PortalGetHeapMemory(portal));
    
    queryDesc = PortalGetQueryDesc(portal);
    state = PortalGetState(portal);
    
    ExecutorRun(queryDesc, state, feature, count);
    
    MemoryContextSwitchTo (context);	/* switch to the normal Executor context */
    
    _SPI_current->processed = state->es_processed;
    if ( _SPI_checktuples (false) )
    	elog (FATAL, "SPI_fetch: # of processed tuples check failed");
    
    SPI_processed = _SPI_current->processed;
    SPI_tuptable = _SPI_current->tuptable;
    
}
#endif

static MemoryContext
_SPI_execmem ()
{
    MemoryContext oldcxt;
    PortalHeapMemory phmem;
    
    phmem = PortalGetHeapMemory (_SPI_current->portal);
    oldcxt = MemoryContextSwitchTo ((MemoryContext)phmem);
    
    return (oldcxt);
    
}

static MemoryContext
_SPI_procmem ()
{
    MemoryContext oldcxt;
    PortalVariableMemory pvmem;
    
    pvmem = PortalGetVariableMemory (_SPI_current->portal);
    oldcxt = MemoryContextSwitchTo ((MemoryContext)pvmem);
    
    return (oldcxt);
    
}

/*
 * _SPI_begin_call --
 *
 */
static int
_SPI_begin_call (bool execmem)
{
    if ( _SPI_curid + 1 != _SPI_connected )
    	return (SPI_ERROR_UNCONNECTED);
    _SPI_curid++;
    if ( _SPI_current != &(_SPI_stack[_SPI_curid]) )
    	elog (FATAL, "SPI: stack corrupted");
    
    if ( execmem )		/* switch to the Executor memory context */
    {
    	_SPI_execmem ();
    	StartPortalAllocMode (DefaultAllocMode, 0);
    }

    return (0);
}

static int
_SPI_end_call (bool procmem)
{
    /*
     * We' returning to procedure where _SPI_curid == _SPI_connected - 1
     */
    _SPI_curid--;
    
    if ( _SPI_current->qtlist)		/* free _SPI_plan allocations */
    {
	free (_SPI_current->qtlist->qtrees);
	free (_SPI_current->qtlist);
    	_SPI_current->qtlist = NULL;
    }
    
    if ( procmem )		/* switch to the procedure memory context */
    {				/* but free Executor memory before */
    	EndPortalAllocMode ();
    	_SPI_procmem ();
    }

    return (0);
}

static bool
_SPI_checktuples (bool isRetrieveIntoRelation)
{
    uint32 processed = _SPI_current->processed;
    SPITupleTable *tuptable = _SPI_current->tuptable;
    bool failed = false;
    	
    if ( processed == 0 )
    {
    	if ( tuptable != NULL )
    	    failed = true;
    }
    else	/* some tuples were processed */
    {
    	if ( tuptable == NULL )	/* spi_printtup was not called */
    	{
    	    if ( !isRetrieveIntoRelation )
    	    	failed = true;
    	}
    	else if ( isRetrieveIntoRelation )
    	    failed = true;
    	else if ( processed != ( tuptable->alloced - tuptable->free ) )
    	    failed = true;
    }
    
    return (failed);
}
    
static _SPI_plan *
_SPI_copy_plan (_SPI_plan *plan, bool local)
{
    _SPI_plan *newplan;
    MemoryContext oldcxt;
    int i;
    
    if ( local )
    	oldcxt = MemoryContextSwitchTo ((MemoryContext)
    			PortalGetVariableMemory (_SPI_current->portal));
    else
    	oldcxt = MemoryContextSwitchTo (TopMemoryContext);
    
    newplan = (_SPI_plan *) palloc (sizeof (_SPI_plan));
    newplan->qtlist = (QueryTreeList*) palloc (sizeof (QueryTreeList));
    newplan->qtlist->len = plan->qtlist->len;
    newplan->qtlist->qtrees = (Query**) palloc (plan->qtlist->len * 
    							sizeof (Query*));
    for (i = 0; i < plan->qtlist->len; i++)
    	newplan->qtlist->qtrees[i] = (Query *) 
    				copyObject (plan->qtlist->qtrees[i]);
    
    newplan->ptlist = (List *) copyObject (plan->ptlist);
    newplan->nargs = plan->nargs;
    if ( plan->nargs > 0 )
    {
    	newplan->argtypes = (Oid *) palloc (plan->nargs * sizeof (Oid));
    	memcpy (newplan->argtypes, plan->argtypes, plan->nargs * sizeof (Oid));
    }
    else
    	newplan->argtypes = NULL;
    
    MemoryContextSwitchTo (oldcxt);
    
    return (newplan);
}

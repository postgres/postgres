/*-------------------------------------------------------------------------
 *
 * spi.c--
 *		Server Programming Interface
 *
 *-------------------------------------------------------------------------
 */
#include "executor/spi.h"
#include "fmgr.h"

typedef struct {
    QueryTreeList	*qtlist;	/* malloced */
    uint32		processed;	/* by Executor */
    SPITupleTable	*tuptable;
    Portal		portal;		/* portal per procedure */
    MemoryContext	savedcntx;
    CommandId		savedId;
} _SPI_connection;

static Portal _SPI_portal = (Portal) NULL;
static _SPI_connection *_SPI_stack = NULL;
static _SPI_connection *_SPI_current = NULL;
static int _SPI_connected = -1;
static int _SPI_curid = -1;

uint32 SPI_processed = 0;
SPITupleTable *SPI_tuptable;

void spi_printtup (HeapTuple tuple, TupleDesc tupdesc);
static int _SPI_pquery (QueryDesc *queryDesc);
#if 0
static void _SPI_fetch (FetchStmt *stmt);
#endif
static int _SPI_begin_call (bool execmem);
static int _SPI_end_call (bool exfree, bool procmem);
static MemoryContext _SPI_execmem (void);
static MemoryContext _SPI_procmem (void);
static bool _SPI_checktuples (bool isRetrieveIntoRelation);


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
    _SPI_current->savedcntx = MemoryContextSwitchTo ((MemoryContext)pvmem);
    
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
    MemoryContextSwitchTo (_SPI_current->savedcntx);
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
SPI_exec (char *src)
{
    QueryTreeList	*queryTree_list;
    List	        *planTree_list;
    QueryDesc		*qdesc;
    Query		*queryTree;
    Plan		*planTree;
    int			res;
    int			i;
    
    res = _SPI_begin_call (true);
    if ( res < 0 )
    	return (res);
    
    /* Increment CommandCounter to see changes made by now */
    CommandCounterIncrement ();
    StartPortalAllocMode (DefaultAllocMode, 0);
    
    SPI_processed = 0;
    SPI_tuptable = NULL;
    _SPI_current->tuptable = NULL;
    
    planTree_list = (List *)
	pg_plan (src, NULL, 0, &queryTree_list, None);
    
    _SPI_current->qtlist = queryTree_list;
    
    for (i=0; i < queryTree_list->len - 1; i++)
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
	    	{
	    	    _SPI_end_call (true, true);
	    	    return (SPI_ERROR_COPY);
	    	}
	    }
	    else if ( nodeTag (queryTree->utilityStmt ) == T_ClosePortalStmt || 
	    		nodeTag (queryTree->utilityStmt ) == T_FetchStmt )
	    {
	    	_SPI_end_call (true, true);
	    	return (SPI_ERROR_CURSOR);
	    }
	    else if ( nodeTag (queryTree->utilityStmt ) == T_TransactionStmt )
	    {
	    	_SPI_end_call (true, true);
	    	return (SPI_ERROR_TRANSACTION);
	    }
	    ProcessUtility (queryTree->utilityStmt, None);
	}
	else
	    ProcessQuery (queryTree, planTree, NULL, NULL, 0, None);
	CommandCounterIncrement ();
    }
    
    /*
     * Last query in list. Note that we don't call CommandCounterIncrement
     * after last query - it will be done by up-level or by next call
     * to SPI_exec.
     */
    queryTree = (Query*) (queryTree_list->qtrees[i]);
    planTree = lfirst(planTree_list);
    
    if ( queryTree->commandType == CMD_UTILITY )
    {
	if ( nodeTag (queryTree->utilityStmt ) == T_CopyStmt )
	{
	    CopyStmt *stmt = (CopyStmt *)(queryTree->utilityStmt);
	    	    
	    if ( stmt->filename == NULL )
	    {
	    	_SPI_end_call (true, true);
	    	return (SPI_ERROR_COPY);
	    }
    	}
#if 0
	else if ( nodeTag (queryTree->utilityStmt ) == T_FetchStmt )
	{
	    _SPI_fetch ((FetchStmt *) (queryTree->utilityStmt));
	    _SPI_end_call (true, true);
	    return (SPI_OK_FETCH);
	}
#endif
	else if ( nodeTag (queryTree->utilityStmt ) == T_ClosePortalStmt || 
	    		nodeTag (queryTree->utilityStmt ) == T_FetchStmt )
	{
	    _SPI_end_call (true, true);
	    return (SPI_ERROR_CURSOR);
	}
	else if ( nodeTag (queryTree->utilityStmt ) == T_TransactionStmt )
	{
	    _SPI_end_call (true, true);
	    return (SPI_ERROR_TRANSACTION);
	}
	ProcessUtility (queryTree->utilityStmt, None);
	
	_SPI_end_call (true, true);
	return (SPI_OK_UTILITY);
    }
	
    qdesc = CreateQueryDesc (queryTree, planTree, SPI);
	
    res = _SPI_pquery (qdesc);
    
    _SPI_end_call (true, true);
    return (res);
    
}

static int
_SPI_pquery (QueryDesc *queryDesc)
{
    Query 	*parseTree;
    Plan 	*plan;
    int		operation;
    EState 	*state;
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
    
    state = CreateExecutorState();
    
    tupdesc = ExecutorStart(queryDesc, state);
    
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
    
    ExecutorRun (queryDesc, state, EXEC_RUN, 0);
    
    _SPI_current->processed = state->es_processed;
    if ( operation == CMD_SELECT )
    {
    	if ( _SPI_checktuples (isRetrieveIntoRelation) )
    	    elog (FATAL, "SPI_select: # of processed tuples check failed");
    }
    
    ExecutorEnd (queryDesc, state);
    
    SPI_processed = _SPI_current->processed;
    SPI_tuptable = _SPI_current->tuptable;
    
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
    MemoryContext oldcntx;
    
    /*
     * When called by Executor _SPI_curid expected to be
     * equal to _SPI_connected
     */
    if ( _SPI_curid != _SPI_connected || _SPI_connected < 0 )
    	elog (FATAL, "SPI: improper call to spi_printtup");
    if ( _SPI_current != &(_SPI_stack[_SPI_curid]) )
    	elog (FATAL, "SPI: stack corrupted in spi_printtup");
    
    oldcntx = _SPI_procmem ();	/* switch to procedure memory context */
    
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
    
    MemoryContextSwitchTo (oldcntx);
    return;
}

static MemoryContext
_SPI_execmem ()
{
    MemoryContext oldcntx;
    PortalHeapMemory phmem;
    
    phmem = PortalGetHeapMemory (_SPI_current->portal);
    oldcntx = MemoryContextSwitchTo ((MemoryContext)phmem);
    
    return (oldcntx);
    
}

static MemoryContext
_SPI_procmem ()
{
    MemoryContext oldcntx;
    PortalVariableMemory pvmem;
    
    pvmem = PortalGetVariableMemory (_SPI_current->portal);
    oldcntx = MemoryContextSwitchTo ((MemoryContext)pvmem);
    
    return (oldcntx);
    
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
    	_SPI_execmem ();

    return (0);
}

static int
_SPI_end_call (bool exfree, bool procmem)
{
    /*
     * We' returning to procedure where _SPI_curid == _SPI_connected - 1
     */
    _SPI_curid--;
    
    if ( exfree )		/* free SPI_exec allocations */
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

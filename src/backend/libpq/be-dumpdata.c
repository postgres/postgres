/*-------------------------------------------------------------------------
 *
 * be-dumpdata.c--
 *    support for collection of returned tuples from an internal
 *    PQ call into a backend buffer.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/libpq/Attic/be-dumpdata.c,v 1.5 1997/08/18 20:52:31 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *	be_portalinit    - initialize backend portal administration
 *	be_portalpush    - add a portal to the top of the portal stack
 *	be_portalpop     - remove portal on the top of the stack & return it
 *	be_currentportal - return the top portal on the portal stack
 *	be_newportal     - return a new portal.
 *	be_portalinit    - initialize backend portal expected to hold results.
 *	be_printtup      - add a tuple to a backend portal
 *
 * NOTES
 *	Since backend user-defined operators can call queries
 *	which in turn call user-defined operators can call queries...
 *	we have to keep track of portals on a stack.  BeginCommand()
 *	puts portals on the stack and the PQ functions remove them.
 *
 */
#include <string.h>

#include <postgres.h>

#include <lib/dllist.h>
#include <libpq/libpq-be.h>
#include <access/heapam.h>
#include <access/htup.h>
#include <storage/buf.h>
#include <utils/memutils.h>
#include <fmgr.h>
#include <utils/mcxt.h>
#include <utils/exc.h>
#include <utils/syscache.h>
#include <catalog/pg_type.h>
#include <catalog/catalog.h>
#include <access/printtup.h>

/* ----------------
 *	backend portal stack for recursive PQexec calls
 * ----------------
 */
static Dllist *be_portalstack;

/* ----------------
 *	be_portalinit - initialize backend portal administration
 *
 *	This is called once from InitPostgres() to initialize
 *	the portal stack.
 * ----------------
 */
void
be_portalinit(void)
{
  be_portalstack = DLNewList();
}

/* ----------------
 *	be_portalpush - add a portal to the top of the portal stack
 *
 *	used by BeginCommand()
 * ----------------
 */
void
be_portalpush(PortalEntry *entry)
{
  DLAddTail(be_portalstack, DLNewElem(entry));
}

/* ----------------
 *	be_portalpop - remove the portal on the top of the stack & return it
 *
 *	used by PQexec()
 * ----------------
 */
PortalEntry *
be_portalpop(void)
{
  PortalEntry *p;
  Dlelem* elt;
  elt = DLRemTail(be_portalstack);

  p = (elt ? (PortalEntry*)DLE_VAL(elt) : NULL);
  DLFreeElem(elt);
  return p;
    

}

/* ----------------
 *	be_currentportal - return the top portal on the portal stack
 *
 *	used by be_printtup()
 * ----------------
 */
PortalEntry *
be_currentportal(void)
{
  Dlelem* elt;
  elt = DLGetTail(be_portalstack);
  return (elt ? (PortalEntry*)DLE_VAL(elt) : NULL);
}

/* ----------------
 *	be_newportal - return a new portal.
 *
 *	If the user-defined function does not specify a portal name,
 *	we generate a unique one.  Names are generated from a combination
 *	of a postgres oid and an integer counter which is incremented
 *	every time we ask for a local portal.
 *
 *	used by BeginCommand()
 * ----------------
 */

static Oid	be_portaloid;
static u_int	be_portalcnt = 0;

PortalEntry *
be_newportal(void)   
{
    PortalEntry *entry;
    char 	buf[PortalNameLength];
    
    /* ----------------
     *	generate a new name
     * ----------------
     */
    if (be_portalcnt == 0)
	be_portaloid = newoid();
    be_portalcnt++;
    sprintf(buf, "be_%d_%d", be_portaloid, be_portalcnt);
    
    /* ----------------
     *	initialize the new portal entry and keep track
     *  of the current memory context for be_printtup().
     *  This is important - otherwise whatever we allocate
     *  will go away and the contents of the portal after
     *  PQexec() returns will be meaningless.
     * ----------------
     */
    entry = pbuf_setup(buf);
    entry->portalcxt = (Pointer) CurrentMemoryContext;
    
    return entry;
}

/* ----------------
 *	be_typeinit - initialize backend portal expected to hold
 *			query results.
 *
 *	used by BeginCommand()
 * ----------------
 */
void
be_typeinit(PortalEntry *entry,
	    TupleDesc tupDesc,
	    int natts)
{
    PortalBuffer 	*portal;
    GroupBuffer 	*group;
    int 		i;
    AttributeTupleForm *attrs = tupDesc->attrs;
    
    /* ----------------
     *	add a new portal group to the portal
     * ----------------
     */
    portal = entry->portal;
    portal->no_groups++;
    portal->groups = group = pbuf_addGroup(portal);
    group->no_fields = natts;
    
    /* ----------------
     *	initialize portal group type info
     * ----------------
     */
    if (natts > 0) {
	group->types = pbuf_addTypes(natts);
	for (i = 0; i < natts; ++i) {
	    strncpy(group->types[i].name, attrs[i]->attname.data, NAMEDATALEN);
	    group->types[i].adtid = attrs[i]->atttypid;
	    group->types[i].adtsize = attrs[i]->attlen;
	}
    }
}

/* ----------------
 *	be_printtup - add a tuple to a backend portal
 *
 *	used indirectly by ExecRetrieve()
 *
 *	This code is pretty much copied from printtup(), dump_type()
 *	and dump_data().  -cim 2/12/91
 * ----------------
 */
void
be_printtup(HeapTuple tuple, TupleDesc typeinfo)
{
    int		i;
    char	*attr;
    bool	isnull;
    Oid	typoutput;
    
    PortalEntry  *entry = NULL;
    PortalBuffer *portal = NULL;
    GroupBuffer  *group = NULL ;
    TupleBlock 	 *tuples = NULL;
    char 	 **values;
    int          *lengths;
    
    MemoryContext savecxt;
    
    /* ----------------
     *  get the current portal and group
     * ----------------
     */
    entry = be_currentportal();
    portal = entry->portal;
    group = portal->groups;
    
    /* ----------------
     *	switch to the portal's memory context so that
     *  the tuples we allocate are returned to the user.
     * ----------------
     */
    savecxt = MemoryContextSwitchTo((MemoryContext)entry->portalcxt);
    
    /* ----------------
     *	If no tuple block yet, allocate one.
     *  If the current block is full, allocate another one.
     * ----------------
     */
    if (group->tuples == NULL) {
	tuples = group->tuples = pbuf_addTuples();
	tuples->tuple_index = 0;
    } else {
	tuples = group->tuples;
	/* walk to the end of the linked list of TupleBlocks */
	while (tuples->next)
	    tuples = tuples->next;
	/* now, tuples is the last TupleBlock, check to see if it is full.  
	   If so, allocate a new TupleBlock and add it to the end of 
	   the chain */
	
	if (tuples->tuple_index == TupleBlockSize) {
	    tuples->next = pbuf_addTuples();
	    tuples = tuples->next;
	    tuples->tuple_index = 0;
	}
    }
    
    /* ----------------
     *	Allocate space for a tuple.
     * ----------------
     */
    tuples->values[tuples->tuple_index] = pbuf_addTuple(tuple->t_natts);
    tuples->lengths[tuples->tuple_index] = pbuf_addTupleValueLengths(tuple->t_natts);
    /* ----------------
     *	copy printable representations of the tuple's attributes
     *  to the portal.
     *
     *  This seems silly, because the user's function which is calling
     *  PQexec() or PQfn() will probably just convert this back into the
     *  internal form anyways, but the point here is to provide a uniform
     *  libpq interface and this is how the fe libpq interface currently
     *  works.  Pretty soon we'll have to add code to let the fe or be
     *  select the desired data representation and then deal with that.
     *  This should not be too hard, as there already exist typrecieve()
     *  and typsend() procedures for user-defined types (see pg_type.h)
     *  -cim 2/11/91
     * ----------------
     */
    
    values = tuples->values[tuples->tuple_index];
    lengths = tuples->lengths[tuples->tuple_index];
    
    for (i = 0; i < tuple->t_natts; i++) {
	attr = heap_getattr(tuple, InvalidBuffer, i+1, typeinfo, &isnull);
	typoutput = typtoout((Oid) typeinfo->attrs[i]->atttypid);
	
	lengths[i] = typeinfo->attrs[i]->attlen;
	
	if (lengths[i] == -1) /* variable length attribute */
	    if (!isnull)
		lengths[i] = VARSIZE(attr)-VARHDRSZ;
	    else
		lengths[i] = 0;
	
	if (!isnull && OidIsValid(typoutput)) {
	  values[i] = fmgr(typoutput, attr, gettypelem(typeinfo->attrs[i]->atttypid));
	} else 
	  values[i] = NULL;
	
    }
    
    /* ----------------
     *	increment tuple group counters
     * ----------------
     */
    portal->no_tuples++;
    group->no_tuples++;
    tuples->tuple_index++;
    
    /* ----------------
     *	return to the original memory context
     * ----------------
     */
    MemoryContextSwitchTo(savecxt);
}

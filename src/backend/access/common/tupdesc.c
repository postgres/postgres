/*-------------------------------------------------------------------------
 *
 * tupdesc.c--
 *    POSTGRES tuple descriptor support code
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/access/common/tupdesc.c,v 1.2 1996/10/19 04:51:44 scrappy Exp $
 *
 * NOTES
 *    some of the executor utility code such as "ExecTypeFromTL" should be
 *    moved here.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/tupdesc.h"
#include "access/htup.h"
#include "catalog/pg_type.h"
#include "utils/syscache.h"
#include "parser/catalog_utils.h"
#include "nodes/parsenodes.h"

/* ----------------------------------------------------------------
 *	CreateTemplateTupleDesc
 *
 *	This function allocates and zeros a tuple descriptor structure.
 * ----------------------------------------------------------------
 */
TupleDesc
CreateTemplateTupleDesc(int natts)
{
    uint32	size;
    TupleDesc desc;
    
    /* ----------------
     *	sanity checks
     * ----------------
     */
    AssertArg(natts >= 1);
    
    /* ----------------
     *  allocate enough memory for the tuple descriptor and
     *  zero it as TupleDescInitEntry assumes that the descriptor
     *  is filled with NULL pointers.
     * ----------------
     */
    size = natts * sizeof (AttributeTupleForm);
    desc = (TupleDesc) palloc(sizeof(struct tupleDesc));
    desc->attrs = (AttributeTupleForm*) palloc(size);
    memset(desc->attrs, 0, size);

    desc->natts = natts;

    return (desc);
}

/* ----------------------------------------------------------------
 *	CreateTupleDesc
 *
 *	This function allocates a new TupleDesc from AttributeTupleForm array
 * ----------------------------------------------------------------
 */
TupleDesc
CreateTupleDesc(int natts, AttributeTupleForm* attrs)
{
    TupleDesc desc;
    
    /* ----------------
     *	sanity checks
     * ----------------
     */
    AssertArg(natts >= 1);
    
    desc = (TupleDesc) palloc(sizeof(struct tupleDesc));
    desc->attrs = attrs;
    desc->natts = natts;    


    return (desc);
}

/* ----------------------------------------------------------------
 *	CreateTupleDescCopy
 *
 *	This function creates a new TupleDesc by copying from an existing
 *      TupleDesc
 * 
 * ----------------------------------------------------------------
 */
TupleDesc
CreateTupleDescCopy(TupleDesc tupdesc)
{
    TupleDesc desc;
    int i, size;

    desc = (TupleDesc) palloc(sizeof(struct tupleDesc));
    desc->natts = tupdesc->natts;
    size = desc->natts * sizeof (AttributeTupleForm);
    desc->attrs = (AttributeTupleForm*) palloc(size);
    for (i=0;i<desc->natts;i++) {
	desc->attrs[i] = 
	    (AttributeTupleForm)palloc(ATTRIBUTE_TUPLE_SIZE);
	memmove(desc->attrs[i],
		tupdesc->attrs[i],
		ATTRIBUTE_TUPLE_SIZE);
    }
    return desc;
}

/* ----------------------------------------------------------------
 *	TupleDescInitEntry
 *
 *	This function initializes a single attribute structure in
 *	a preallocated tuple descriptor.
 * ----------------------------------------------------------------
 */
bool
TupleDescInitEntry(TupleDesc desc,
		   AttrNumber attributeNumber,
		   char *attributeName,
		   char *typeName,
		   int attdim,
		   bool attisset)
{
    HeapTuple		tuple;
    TypeTupleForm	typeForm;
    AttributeTupleForm	att;
    
    /* ----------------
     *	sanity checks
     * ----------------
     */
    AssertArg(PointerIsValid(desc));
    AssertArg(attributeNumber >= 1);
    /* attributeName's are sometimes NULL, 
       from resdom's.  I don't know why that is, though -- Jolly */
/*    AssertArg(NameIsValid(attributeName));*/
/*    AssertArg(NameIsValid(typeName));*/
    
    AssertArg(!PointerIsValid(desc->attrs[attributeNumber - 1]));
    

    /* ----------------
     *	allocate storage for this attribute
     * ----------------
     */

    att = (AttributeTupleForm) palloc(ATTRIBUTE_TUPLE_SIZE);
    desc->attrs[attributeNumber - 1] = att;

    /* ----------------
     *	initialize some of the attribute fields
     * ----------------
     */
    att->attrelid  = 0;				/* dummy value */
    
    if (attributeName != NULL)
	namestrcpy(&(att->attname), attributeName);
    else
	memset(att->attname.data,0,NAMEDATALEN);

    
    att->attdefrel = 	0;			/* dummy value */
    att->attnvals  = 	0;			/* dummy value */
    att->atttyparg = 	0;			/* dummy value */
    att->attbound = 	0;			/* dummy value */
    att->attcanindex = 	0;			/* dummy value */
    att->attproc = 	0;			/* dummy value */
    att->attcacheoff = 	-1;
    
    att->attnum = attributeNumber;
    att->attnelems = attdim;
    att->attisset = attisset;
    
    /* ----------------
     *	search the system cache for the type tuple of the attribute
     *  we are creating so that we can get the typeid and some other
     *  stuff.
     *
     *  Note: in the special case of 
     *
     *	    create EMP (name = char16, manager = EMP)
     *
     *  RelationNameCreateHeapRelation() calls BuildDesc() which
     *  calls this routine and since EMP does not exist yet, the
     *  system cache lookup below fails.  That's fine, but rather
     *  then doing a elog(WARN) we just leave that information
     *  uninitialized, return false, then fix things up later.
     *  -cim 6/14/90
     * ----------------
     */
    tuple = SearchSysCacheTuple(TYPNAME, PointerGetDatum(typeName),
				0,0,0);
    if (! HeapTupleIsValid(tuple)) {
	/* ----------------
	 *   here type info does not exist yet so we just fill
	 *   the attribute with dummy information and return false.
	 * ----------------
	 */
	att->atttypid = InvalidOid;
	att->attlen   = (int16)	0;
	att->attbyval = (bool) 0;
	att->attalign = 'i';
	return false;
    }
    
    /* ----------------
     *	type info exists so we initialize our attribute
     *  information from the type tuple we found..
     * ----------------
     */
    typeForm = (TypeTupleForm) GETSTRUCT(tuple);
    
    att->atttypid = tuple->t_oid;
    att->attalign = typeForm->typalign;
    
    /* ------------------------
       If this attribute is a set, what is really stored in the
       attribute is the OID of a tuple in the pg_proc catalog.
       The pg_proc tuple contains the query string which defines
       this set - i.e., the query to run to get the set.
       So the atttypid (just assigned above) refers to the type returned
       by this query, but the actual length of this attribute is the
       length (size) of an OID.
       
       Why not just make the atttypid point to the OID type, instead
       of the type the query returns?  Because the executor uses the atttypid
       to tell the front end what type will be returned (in BeginCommand),
       and in the end the type returned will be the result of the query, not
       an OID.
       
       Why not wait until the return type of the set is known (i.e., the
       recursive call to the executor to execute the set has returned) 
       before telling the front end what the return type will be?  Because
       the executor is a delicate thing, and making sure that the correct
       order of front-end commands is maintained is messy, especially 
       considering that target lists may change as inherited attributes
       are considered, etc.  Ugh.
       -----------------------------------------
       */
    if (attisset) {
	Type t = type("oid");
	att->attlen = tlen(t);
	att->attbyval = tbyval(t);
    } else {
	att->attlen   = typeForm->typlen;
	att->attbyval = typeForm->typbyval;
    }
    
    
    return true;
}


/* ----------------------------------------------------------------
 *	TupleDescMakeSelfReference
 *
 *	This function initializes a "self-referential" attribute like
 *      manager in "create EMP (name=text, manager = EMP)".
 *	It calls TypeShellMake() which inserts a "shell" type
 *	tuple into pg_type.  A self-reference is one kind of set, so
 *      its size and byval are the same as for a set.  See the comments
 *      above in TupleDescInitEntry.
 * ----------------------------------------------------------------
 */
static void
TupleDescMakeSelfReference(TupleDesc desc,
			   AttrNumber attnum,
			   char *relname)
{
    AttributeTupleForm att;
    Type t = type("oid");
    
    att = desc->attrs[attnum-1];
    att->atttypid = TypeShellMake(relname);
    att->attlen   = tlen(t);
    att->attbyval = tbyval(t);
    att->attnelems = 0;
}

/* ----------------------------------------------------------------
 *	BuildDescForRelation
 *
 *	This is a general purpose function identical to BuildDesc
 *	but is used by the DefineRelation() code to catch the
 *	special case where you
 *
 *		create FOO ( ..., x = FOO )
 *
 *	here, the initial type lookup for "x = FOO" will fail
 *	because FOO isn't in the catalogs yet.  But since we
 *	are creating FOO, instead of doing an elog() we add
 *	a shell type tuple to pg_type and fix things later
 *	in amcreate().
 * ----------------------------------------------------------------
 */
TupleDesc
BuildDescForRelation(List *schema, char *relname)
{
    int			natts;
    AttrNumber		attnum;
    List		*p;
    TupleDesc		desc;
    char               *attname;
    char               *typename;
    int			attdim;
    bool                attisset;
    
    /* ----------------
     *	allocate a new tuple descriptor
     * ----------------
     */
    natts = 	length(schema);
    desc = 	CreateTemplateTupleDesc(natts);
    
    attnum = 0;
    
    typename = palloc(NAMEDATALEN+1);

    foreach(p, schema) {
	ColumnDef *entry;
	List	*arry;

	/* ----------------
	 *	for each entry in the list, get the name and type
	 *      information from the list and have TupleDescInitEntry
	 *	fill in the attribute information we need.
	 * ----------------
	 */	
	attnum++;
	
	entry = 	lfirst(p);
	attname = 	entry->colname;
	arry = entry->typename->arrayBounds;
	attisset = entry->typename->setof;

	if (arry != NIL) {
	    char buf[20];
	    
	    attdim = length(arry);
	    
	    /* array of XXX is _XXX (inherited from release 3) */
	    sprintf(buf, "_%.*s", NAMEDATALEN, entry->typename->name);
	    strcpy(typename, buf);
	} else {
	    strcpy(typename, entry->typename->name);
	    attdim = 0;
	}
	
	if (! TupleDescInitEntry(desc, attnum, attname, 
				 typename, attdim, attisset)) {
	    /* ----------------
	     *	if TupleDescInitEntry() fails, it means there is
	     *  no type in the system catalogs.  So now we check if
	     *  the type name equals the relation name.  If so we
	     *  have a self reference, otherwise it's an error.
	     * ----------------
	     */
	    if (!strcmp(typename, relname)) {
		TupleDescMakeSelfReference(desc, attnum, relname);
	    } else
		elog(WARN, "DefineRelation: no such type %.*s", 
		     NAMEDATALEN, typename);
	}

	/*
	 * this is for char() and varchar(). When an entry is of type
	 * char() or varchar(), typlen is set to the appropriate length,
	 * which we'll use here instead. (The catalog lookup only returns
	 * the length of bpchar and varchar which is not what we want!)
	 *						- ay 6/95
	 */
	if (entry->typename->typlen > 0) {
	    desc->attrs[attnum - 1]->attlen = entry->typename->typlen;
	}
    }
    return desc;
}


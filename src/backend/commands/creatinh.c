/*-------------------------------------------------------------------------
 *
 * creatinh.c--
 *    POSTGRES create/destroy relation with inheritance utility code.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/commands/Attic/creatinh.c,v 1.12 1997/08/19 04:43:30 vadim Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <string.h>

#include <postgres.h>

#include <utils/rel.h>
#include <nodes/parsenodes.h>
#include <catalog/heap.h>
#include <commands/creatinh.h>
#include <access/xact.h>
#include <access/heapam.h>
#include <utils/syscache.h>
#include <catalog/catname.h>
#include <catalog/pg_type.h>
#include <catalog/pg_inherits.h>
#include <catalog/pg_ipl.h>

/* ----------------
 *	local stuff
 * ----------------
 */

static int checkAttrExists(char *attributeName, 
 			   char *attributeType, List *schema);
static List *MergeAttributes(List *schema, List *supers);
static void StoreCatalogInheritance(Oid relationId, List *supers);

/* ----------------------------------------------------------------
 *	DefineRelation --
 *		Creates a new relation.
 * ----------------------------------------------------------------
 */
void
DefineRelation(CreateStmt *stmt)
{
    char *relname = palloc(NAMEDATALEN);
    List *schema = stmt->tableElts;
    int			numberOfAttributes;
    Oid			relationId;
    char		archChar;
    List		*inheritList 	= NULL;
    char *archiveName 	= NULL;
    TupleDesc	descriptor;
    int			heaploc, archloc;
    
    char*   typename = NULL;  /* the typename of this relation. not useod for now */

    if ( strlen(stmt->relname) >= NAMEDATALEN)
	elog(WARN, "the relation name %s is >= %d characters long", stmt->relname,
	     NAMEDATALEN);
    strNcpy(relname,stmt->relname,NAMEDATALEN-1);  /* make full length for copy */

    /* ----------------
     * 	Handle parameters
     * 	XXX parameter handling missing below.
     * ----------------
     */
    inheritList = stmt->inhRelnames;
    
    /* ----------------
     *	determine archive mode
     * 	XXX use symbolic constants...
     * ----------------
     */
    archChar = 'n';
    
    switch (stmt->archiveType) {
    case ARCH_NONE:
	archChar = 'n';
	break;
    case ARCH_LIGHT:
	archChar = 'l';
	break;
    case ARCH_HEAVY:
	archChar = 'h';
	break;
    default:
	elog(WARN, "Botched archive mode %d, ignoring",
	     stmt->archiveType);
	break;
    }
    
    if (stmt->location == -1)
	heaploc = 0;
    else
	heaploc = stmt->location;
    
    /*
     *  For now, any user-defined relation defaults to the magnetic
     *  disk storgage manager.  --mao 2 july 91
     */
    if (stmt->archiveLoc == -1) {
	archloc = 0;
    } else {
	if (archChar == 'n') {
	    elog(WARN, "Set archive location, but not mode, for %s",
		 relname);
	}
	archloc = stmt->archiveLoc;
    }
    
    /* ----------------
     *	generate relation schema, including inherited attributes.
     * ----------------
     */
    schema = MergeAttributes(schema, inheritList);
    
    numberOfAttributes = length(schema);
    if (numberOfAttributes <= 0) {
	elog(WARN, "DefineRelation: %s",
	     "please inherit from a relation or define an attribute");
    }
    
    /* ----------------
     *	create a relation descriptor from the relation schema
     *  and create the relation.  
     * ----------------
     */
    descriptor = BuildDescForRelation(schema, relname);
    relationId = heap_create(relname,
			     typename,
			     archChar,
			     heaploc,
			     descriptor);
    
    StoreCatalogInheritance(relationId, inheritList);
    
    /* ----------------
     *	create an archive relation if necessary
     * ----------------
     */
    if (archChar != 'n') {
	/*
	 *  Need to create an archive relation for this heap relation.
	 *  We cobble up the command by hand, and increment the command
	 *  counter ourselves.
	 */
	
	CommandCounterIncrement();
	archiveName = MakeArchiveName(relationId);
	
	relationId = heap_create(archiveName,
				 typename,
				 'n',		/* archive isn't archived */
				 archloc,
				 descriptor);
	
	pfree(archiveName);
    }
}

/*
 * RemoveRelation --
 *	Deletes a new relation.
 *
 * Exceptions:
 *	BadArg if name is invalid.
 *
 * Note:
 *	If the relation has indices defined on it, then the index relations
 * themselves will be destroyed, too.
 */
void
RemoveRelation(char *name)
{
    AssertArg(name);
    heap_destroy(name);
}


/*
 * MergeAttributes --
 *	Returns new schema given initial schema and supers.
 *
 *
 * 'schema' is the column/attribute definition for the table. (It's a list
 *	of ColumnDef's.) It is destructively changed.
 * 'inheritList' is the list of inherited relations (a list of Value(str)'s).
 *
 * Notes:
 *    The order in which the attributes are inherited is very important.
 *    Intuitively, the inherited attributes should come first. If a table
 *    inherits from multiple parents, the order of those attributes are
 *    according to the order of the parents specified in CREATE TABLE.
 *
 *    Here's an example:
 *
 *	create table person (name text, age int4, location point);
 *	create table emp (salary int4, manager char16) inherits(person);
 *	create table student (gpa float8) inherits (person);
 *	create table stud_emp (percent int4) inherits (emp, student);
 *
 *    the order of the attributes of stud_emp is as follow:
 *
 *
 *                          person {1:name, 2:age, 3:location}
 *                          /    \
 *             {6:gpa}  student   emp {4:salary, 5:manager}
 *                          \    /
 *                         stud_emp {7:percent}
 */
static List *
MergeAttributes(List *schema, List *supers)
{
    List *entry;
    List *inhSchema = NIL;
    
    /*
     * Validates that there are no duplications.
     * Validity checking of types occurs later.
     */
    foreach (entry, schema) {
	List	*rest;
	ColumnDef *coldef = lfirst(entry);
	
	foreach (rest, lnext(entry)) {
	    /*
	     * check for duplicated relation names
	     */
	    ColumnDef *restdef = lfirst(rest);
	    
	    if (!strcmp(coldef->colname, restdef->colname)) {
		elog(WARN, "attribute \"%s\" duplicated",
		     coldef->colname);
	    }
	}
    }
    foreach (entry, supers) {
	List	*rest;
	
	foreach (rest, lnext(entry)) {
	    if (!strcmp(strVal(lfirst(entry)), strVal(lfirst(rest)))) {
		elog(WARN, "relation \"%s\" duplicated",
		     strVal(lfirst(entry)));
	    }
	}
    }
    
    /*
     * merge the inherited attributes into the schema
     */
    foreach (entry, supers) {
	char		*name = strVal(lfirst(entry));
	Relation	relation;
	List		*partialResult = NIL;
	AttrNumber	attrno;
	TupleDesc	tupleDesc;
	
	relation =  heap_openr(name);
	if (relation==NULL) {
	    elog(WARN,
		 "MergeAttr: Can't inherit from non-existent superclass '%s'",
		 name);
	}
	if ( relation->rd_rel->relkind == 'S' )
	{
	    elog(WARN, "MergeAttr: Can't inherit from sequence superclass '%s'",
		 name);
	}
	tupleDesc = RelationGetTupleDescriptor(relation);
	
	for (attrno = relation->rd_rel->relnatts - 1; attrno >= 0; attrno--) {
	    AttributeTupleForm	attribute = tupleDesc->attrs[attrno];
	    char *attributeName;
	    char *attributeType;
	    AttrConstr  constraints;
	    HeapTuple	tuple;
	    ColumnDef	*def;
	    TypeName	*typename;
	    
	    /*
	     * form name, type and constraints
	     */
	    attributeName = (attribute->attname).data;
            constraints.has_not_null = attribute->attnotnull;
	    tuple =
		SearchSysCacheTuple(TYPOID,
				    ObjectIdGetDatum(attribute->atttypid),
				    0,0,0);
	    AssertState(HeapTupleIsValid(tuple));
	    attributeType =
		(((TypeTupleForm)GETSTRUCT(tuple))->typname).data;
	    /*
	     * check validity
	     *
	     */
	    if (checkAttrExists(attributeName, attributeType, inhSchema) ||
		checkAttrExists(attributeName, attributeType, schema)) {
		/*
		 * this entry already exists
		 */
		continue;
	    }

	    /*
	     * add an entry to the schema
	     */
	    def = makeNode(ColumnDef);
	    typename = makeNode(TypeName);
	    def->colname = pstrdup(attributeName);
	    typename->name = pstrdup(attributeType); 
	    def->typename = typename;
	    def->is_not_null = constraints.has_not_null;
            partialResult = lcons(def, partialResult);
	}
	
	/*
	 * iteration cleanup and result collection
	 */
	heap_close(relation);

	/*
	 * wants the inherited schema to appear in the order they are
	 * specified in CREATE TABLE
	 */
	inhSchema = nconc(inhSchema, partialResult);
    }

    /*
     * put the inherited schema before our the schema for this table
     */
    schema = nconc(inhSchema, schema);
    
    return (schema);
}

/*
 * StoreCatalogInheritance --
 *	Updates the system catalogs with proper inheritance information.
 */
static void
StoreCatalogInheritance(Oid relationId, List *supers)
{
    Relation	relation;
    TupleDesc	desc;
    int16	seqNumber;
    List	*entry;
    List	*idList;
    HeapTuple	tuple;
    
    /* ----------------
     *	sanity checks
     * ----------------
     */
    AssertArg(OidIsValid(relationId));
    
    if (supers==NIL)
	return;
    
    /* ----------------
     * Catalog INHERITS information.
     * ----------------
     */
    relation = heap_openr( InheritsRelationName );
    desc = RelationGetTupleDescriptor(relation);

    seqNumber = 1;
    idList = NIL;
    foreach (entry, supers) {
	Datum		datum[ Natts_pg_inherits ];
	char		nullarr[ Natts_pg_inherits ];
	
	tuple = SearchSysCacheTuple(RELNAME, 
				    PointerGetDatum(strVal(lfirst(entry))),
				    0,0,0);
	AssertArg(HeapTupleIsValid(tuple));
	
	/*
	 * build idList for use below
	 */
	idList = lappendi(idList, tuple->t_oid);
	
	datum[0] = ObjectIdGetDatum(relationId);	/* inhrel */
	datum[1] = ObjectIdGetDatum(tuple->t_oid);	/* inhparent */
	datum[2] = Int16GetDatum(seqNumber);		/* inhseqno */
	
	nullarr[0] = ' ';
	nullarr[1] = ' ';
	nullarr[2] = ' ';
	
	tuple = heap_formtuple(desc,datum, nullarr);
	
	heap_insert(relation, tuple);
	pfree(tuple);
	
	seqNumber += 1;
    }
    
    heap_close(relation);
    
    /* ----------------
     * Catalog IPL information.
     *
     * Algorithm:
     *	0. list superclasses (by Oid) in order given (see idList).
     *	1. append after each relationId, its superclasses, recursively.
     *	3. remove all but last of duplicates.
     *	4. store result.
     * ----------------
     */
    
    /* ----------------
     *	1.
     * ----------------
     */
    foreach (entry, idList) {
	HeapTuple		tuple;
	Oid			id;
	int16			number;
	List			*next;
	List			*current;
	
	id = (Oid)lfirsti(entry);
	current = entry;
	next = lnext(entry);
	
	for (number = 1; ; number += 1) {
	    tuple = SearchSysCacheTuple(INHRELID,
					ObjectIdGetDatum(id),
					Int16GetDatum(number),
					0,0);
	    
	    if (! HeapTupleIsValid(tuple))
		break;
	    
	    lnext(current) =
		lconsi(((InheritsTupleForm)
			 GETSTRUCT(tuple))->inhparent,
			NIL);
	    
	    current = lnext(current);
	}
	lnext(current) = next;
    }
    
    /* ----------------
     *	2.
     * ----------------
     */
    foreach (entry, idList) {
	Oid		name;
	List		*rest;
	bool		found = false;
	
    again:
	name = lfirsti(entry);
	foreach (rest, lnext(entry)) {
	    if (name == lfirsti(rest)) {
		found = true;
		break;
	    }
	}
	if (found) {
	    /*
	     * entry list must be of length >= 2 or else no match
	     *
	     * so, remove this entry.
	     */
	    lfirst(entry) = lfirst(lnext(entry));
	    lnext(entry) = lnext(lnext(entry));
	    
	    found = false;
	    goto again;
	}
    }
    
    /* ----------------
     *	3.
     * ----------------
     */
    relation = heap_openr( InheritancePrecidenceListRelationName );
    desc = RelationGetTupleDescriptor(relation);
    
    seqNumber = 1;
    
    foreach (entry, idList) {
	Datum	datum[ Natts_pg_ipl ];
	char	nullarr[ Natts_pg_ipl ];
	
	datum[0] = ObjectIdGetDatum(relationId);	/* iplrel */
	datum[1] = ObjectIdGetDatum(lfirsti(entry));
	/*iplinherits*/
	datum[2] = Int16GetDatum(seqNumber);		/* iplseqno */
	
	nullarr[0] = ' ';
	nullarr[1] = ' ';
	nullarr[2] = ' ';
	
	tuple = heap_formtuple( desc, datum, nullarr);
	
	heap_insert(relation, tuple);
	pfree(tuple);
	
	seqNumber += 1;
    }

    heap_close(relation);
}

/*
 * returns 1 if attribute already exists in schema, 0 otherwise.
 */
static int
checkAttrExists(char *attributeName, char *attributeType, List *schema)
{
    List *s;

    foreach (s, schema) {
	ColumnDef *def = lfirst(s);

	if (!strcmp(attributeName, def->colname)) {
	    /*
	     * attribute exists. Make sure the types are the same.
	     */
	    if (strcmp(attributeType, def->typename->name) != 0) {
		elog(WARN, "%s and %s conflict for %s",
		     attributeType, def->typename->name, attributeName);
	    }
	    return 1;
	}
    }
    return 0;
}

/*
 * MakeArchiveName
 *    make an archive rel name out of a regular rel name
 *
* the CALLER is responsible for freeing the memory allocated
 */

char*
MakeArchiveName(Oid relationId)
{
    char *arch;

    /*
     *  Archive relations are named a,XXXXX where XXXXX == the OID
     *  of the relation they archive.  Create a string containing
     *  this name and find the reldesc for the archive relation.
     */
    arch = palloc(NAMEDATALEN);
    sprintf(arch, "a,%d",relationId);

    return arch;
}


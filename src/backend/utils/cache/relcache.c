/*-------------------------------------------------------------------------
 *
 * relcache.c--
 *    POSTGRES relation descriptor cache code
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/utils/cache/relcache.c,v 1.11 1997/08/03 02:37:32 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *	RelationInitialize		- initialize relcache
 *	RelationIdCacheGetRelation	- get a reldesc from the cache (id)
 *	RelationNameCacheGetRelation	- get a reldesc from the cache (name)
 *	RelationIdGetRelation		- get a reldesc by relation id
 *	RelationNameGetRelation		- get a reldesc by relation name
 *	RelationClose			- close an open relation
 *	RelationFlushRelation		- flush relation information
 *
 * NOTES
 *	This file is in the process of being cleaned up
 *	before I add system attribute indexing.  -cim 1/13/91
 *
 *	The following code contains many undocumented hacks.  Please be
 *	careful....
 *
 */
#include <sys/types.h>
#include <stdio.h>		/* for sprintf() */
#include <errno.h>
#include <sys/file.h>
#include <fcntl.h>
#include <string.h>
 
#include "postgres.h"
#include "miscadmin.h"

#include <storage/smgr.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup.h"
#include "access/istrat.h"
#include "access/itup.h"
#include "access/skey.h"
#include "utils/builtins.h"
#include "access/tupdesc.h"
#include "access/tupmacs.h"
#include "access/xact.h"
 
#include "storage/buf.h"
#include "storage/fd.h"		/* for SEEK_ */
#include "storage/lmgr.h"
#include "storage/bufmgr.h"
 
#include "lib/hasht.h"
 
#include "utils/memutils.h"
#include "utils/mcxt.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/hsearch.h"
#include "utils/relcache.h"
 
#include "catalog/catname.h"
#include "catalog/catalog.h"
#include "utils/syscache.h"

#include "catalog/pg_attribute.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_index.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_class.h"
#include "catalog/pg_rewrite.h"
#include "catalog/pg_type.h"

#include "catalog/pg_variable.h"
#include "catalog/pg_log.h"
#include "catalog/pg_time.h"
#include "catalog/indexing.h"
#include "catalog/index.h"
#include "fmgr.h"

/* ----------------
 *	defines
 * ----------------
 */
#define private static
#define INIT_FILENAME	"pg_internal.init"

/* ----------------
 *	externs
 * ----------------
 */
extern bool	AMI_OVERRIDE;	/* XXX style */
extern GlobalMemory CacheCxt;	/* from utils/cache/catcache.c */

/* ----------------
 *	hardcoded tuple descriptors.  see lib/backend/catalog/pg_attribute.h
 * ----------------
 */
FormData_pg_attribute Desc_pg_class[Natts_pg_class] = { Schema_pg_class };
FormData_pg_attribute Desc_pg_attribute[Natts_pg_attribute] = { Schema_pg_attribute };
FormData_pg_attribute Desc_pg_proc[Natts_pg_proc] = { Schema_pg_proc };
FormData_pg_attribute Desc_pg_type[Natts_pg_type] = { Schema_pg_type };
FormData_pg_attribute Desc_pg_variable[Natts_pg_variable] = { Schema_pg_variable };
FormData_pg_attribute Desc_pg_log[Natts_pg_log] = { Schema_pg_log };
FormData_pg_attribute Desc_pg_time[Natts_pg_time] = { Schema_pg_time };

/* ----------------
 *	global variables
 *
 * 	Relations are cached two ways, by name and by id,
 *	thus there are two hash tables for referencing them. 
 * ----------------
 */
HTAB 	*RelationNameCache;
HTAB	*RelationIdCache;

/* ----------------
 *	RelationBuildDescInfo exists so code can be shared
 *      between RelationIdGetRelation() and RelationNameGetRelation()
 * ----------------
 */
typedef struct RelationBuildDescInfo {
    int infotype;		/* lookup by id or by name */
#define INFO_RELID 1
#define INFO_RELNAME 2
    union {
	Oid info_id;	/* relation object id */
	char *info_name;	/* relation name */
    } i;
} RelationBuildDescInfo;

typedef struct relidcacheent {
    Oid reloid;
    Relation reldesc;
} RelIdCacheEnt;

typedef struct relnamecacheent {
    NameData relname;
    Relation reldesc;
} RelNameCacheEnt;

/* -----------------
 *	macros to manipulate name cache and id cache
 * -----------------
 */
#define RelationCacheInsert(RELATION)	\
    {   RelIdCacheEnt *idhentry; RelNameCacheEnt *namehentry; \
	char *relname; Oid reloid; bool found; \
	relname = (RELATION->rd_rel->relname).data; \
	namehentry = (RelNameCacheEnt*)hash_search(RelationNameCache, \
					           relname, \
						   HASH_ENTER, \
						   &found); \
	if (namehentry == NULL) { \
	    elog(FATAL, "can't insert into relation descriptor cache"); \
	  } \
	if (found && !IsBootstrapProcessingMode()) { \
	    /* used to give notice -- now just keep quiet */ ; \
	  } \
	namehentry->reldesc = RELATION; \
	reloid = RELATION->rd_id; \
	idhentry = (RelIdCacheEnt*)hash_search(RelationIdCache, \
					       (char *)&reloid, \
					       HASH_ENTER, \
					       &found); \
	if (idhentry == NULL) { \
	    elog(FATAL, "can't insert into relation descriptor cache"); \
	  } \
	if (found && !IsBootstrapProcessingMode()) { \
	    /* used to give notice -- now just keep quiet */ ; \
	  } \
	idhentry->reldesc = RELATION; \
    }
#define RelationNameCacheLookup(NAME, RELATION)	\
    {   RelNameCacheEnt *hentry; bool found; \
	hentry = (RelNameCacheEnt*)hash_search(RelationNameCache, \
					       (char *)NAME,HASH_FIND,&found); \
	if (hentry == NULL) { \
	    elog(FATAL, "error in CACHE"); \
	  } \
	if (found) { \
	    RELATION = hentry->reldesc; \
	  } \
	else { \
	    RELATION = NULL; \
	  } \
    }
#define RelationIdCacheLookup(ID, RELATION)	\
    {   RelIdCacheEnt *hentry; bool found; \
	hentry = (RelIdCacheEnt*)hash_search(RelationIdCache, \
					     (char *)&(ID),HASH_FIND, &found); \
	if (hentry == NULL) { \
	    elog(FATAL, "error in CACHE"); \
	  } \
	if (found) { \
	    RELATION = hentry->reldesc; \
	  } \
	else { \
	    RELATION = NULL; \
	  } \
    }
#define RelationCacheDelete(RELATION)	\
    {   RelNameCacheEnt *namehentry; RelIdCacheEnt *idhentry; \
	char *relname; Oid reloid; bool found; \
	relname = (RELATION->rd_rel->relname).data; \
	namehentry = (RelNameCacheEnt*)hash_search(RelationNameCache, \
					           relname, \
						   HASH_REMOVE, \
						   &found); \
	if (namehentry == NULL) { \
	    elog(FATAL, "can't delete from relation descriptor cache"); \
	  } \
	if (!found) { \
	    elog(NOTICE, "trying to delete a reldesc that does not exist."); \
	  } \
	reloid = RELATION->rd_id; \
	idhentry = (RelIdCacheEnt*)hash_search(RelationIdCache, \
					       (char *)&reloid, \
					       HASH_REMOVE, &found); \
	if (idhentry == NULL) { \
	    elog(FATAL, "can't delete from relation descriptor cache"); \
	  } \
	if (!found) { \
	    elog(NOTICE, "trying to delete a reldesc that does not exist."); \
	  } \
    }

/* non-export function prototypes */
static void formrdesc(char *relationName, u_int natts,
		      FormData_pg_attribute att[]);

#if 0		/* See comments at line 1304 */
static void RelationFlushIndexes(Relation *r, Oid accessMethodId);
#endif

static HeapTuple ScanPgRelation(RelationBuildDescInfo buildinfo);
static HeapTuple scan_pg_rel_seq(RelationBuildDescInfo buildinfo);
static HeapTuple scan_pg_rel_ind(RelationBuildDescInfo buildinfo);
static Relation AllocateRelationDesc(u_int natts, Form_pg_class relp);
static void RelationBuildTupleDesc(RelationBuildDescInfo buildinfo,    
		Relation relation, AttributeTupleForm attp, u_int natts);
static void build_tupdesc_seq(RelationBuildDescInfo buildinfo,
		Relation relation, AttributeTupleForm attp, u_int natts);
static void build_tupdesc_ind(RelationBuildDescInfo buildinfo,
		Relation relation, AttributeTupleForm attp, u_int natts);
static Relation RelationBuildDesc(RelationBuildDescInfo buildinfo);
static void IndexedAccessMethodInitialize(Relation relation);

/*
 * newlyCreatedRelns -
 *    relations created during this transaction. We need to keep track of
 *    these.
 */
static List *newlyCreatedRelns = NULL;

/* ----------------------------------------------------------------
 *	RelationIdGetRelation() and RelationNameGetRelation()
 *			support functions
 * ----------------------------------------------------------------
 */
 

#if 0		/* XXX This doesn't seem to be used anywhere */
/* --------------------------------
 *	BuildDescInfoError returns a string appropriate to
 *	the buildinfo passed to it
 * --------------------------------
 */
static char *
BuildDescInfoError(RelationBuildDescInfo buildinfo)
{
    static char errBuf[64];
    
    memset(errBuf, 0, (int) sizeof(errBuf));
    switch(buildinfo.infotype) {
    case INFO_RELID:
	sprintf(errBuf, "(relation id %d)", buildinfo.i.info_id);
	break;
    case INFO_RELNAME:
	sprintf(errBuf, "(relation name %.*s)", NAMEDATALEN, buildinfo.i.info_name);
	break;
    }
    
    return errBuf;
}
#endif

/* --------------------------------
 *	ScanPgRelation
 *
 *	this is used by RelationBuildDesc to find a pg_class
 *	tuple matching either a relation name or a relation id
 *	as specified in buildinfo.
 * --------------------------------
 */
static HeapTuple
ScanPgRelation(RelationBuildDescInfo buildinfo)
{
    /*
     *  If this is bootstrap time (initdb), then we can't use the system
     *  catalog indices, because they may not exist yet.  Otherwise, we
     *  can, and do.
     */
    
    if (IsBootstrapProcessingMode())
	return (scan_pg_rel_seq(buildinfo));
    else
	return (scan_pg_rel_ind(buildinfo));
}

static HeapTuple
scan_pg_rel_seq(RelationBuildDescInfo buildinfo)
{
    HeapTuple	 pg_class_tuple;
    HeapTuple	 return_tuple;
    Relation	 pg_class_desc;
    HeapScanDesc pg_class_scan;
    ScanKeyData	 key;
    Buffer	 buf;
    
    /* ----------------
     *	form a scan key
     * ----------------
     */
    switch (buildinfo.infotype) {
    case INFO_RELID:
	ScanKeyEntryInitialize(&key, 0,
			       ObjectIdAttributeNumber,
			       ObjectIdEqualRegProcedure,
			       ObjectIdGetDatum(buildinfo.i.info_id));
	break;
	
    case INFO_RELNAME:
	ScanKeyEntryInitialize(&key, 0,
	                       Anum_pg_class_relname,
	                       Character16EqualRegProcedure,
	                       NameGetDatum(buildinfo.i.info_name));
	break;
	
    default:
	elog(WARN, "ScanPgRelation: bad buildinfo");
	return NULL;
    }
    
    /* ----------------
     *	open pg_class and fetch a tuple
     * ----------------
     */
    pg_class_desc =  heap_openr(RelationRelationName);
    if (!IsInitProcessingMode())
	RelationSetLockForRead(pg_class_desc);
    pg_class_scan =
	heap_beginscan(pg_class_desc, 0, NowTimeQual, 1, &key);
    pg_class_tuple = heap_getnext(pg_class_scan, 0, &buf);
    
    /* ----------------
     *	get set to return tuple
     * ----------------
     */
    if (! HeapTupleIsValid(pg_class_tuple)) {
	return_tuple = pg_class_tuple;
    } else {
	/* ------------------
	 *  a satanic bug used to live here: pg_class_tuple used to be
	 *  returned here without having the corresponding buffer pinned.
	 *  so when the buffer gets replaced, all hell breaks loose.
	 *  this bug is discovered and killed by wei on 9/27/91.
	 * -------------------
	 */
	return_tuple = (HeapTuple) palloc((Size) pg_class_tuple->t_len);
	memmove((char *) return_tuple,
		(char *) pg_class_tuple, 
		(int) pg_class_tuple->t_len);
	ReleaseBuffer(buf);
    }
    
    /* all done */
    heap_endscan(pg_class_scan);
    if (!IsInitProcessingMode())
	RelationUnsetLockForRead(pg_class_desc);
    heap_close(pg_class_desc);
    
    return return_tuple;
}

static HeapTuple
scan_pg_rel_ind(RelationBuildDescInfo buildinfo)
{
    Relation pg_class_desc;
    HeapTuple return_tuple;
    
    pg_class_desc = heap_openr(RelationRelationName);
    if (!IsInitProcessingMode())
	RelationSetLockForRead(pg_class_desc);
    
    switch (buildinfo.infotype) {
    case INFO_RELID:
	return_tuple = ClassOidIndexScan(pg_class_desc, buildinfo.i.info_id);
	break;
	
    case INFO_RELNAME:
	return_tuple = ClassNameIndexScan(pg_class_desc, 
					  buildinfo.i.info_name);
	break;
	
    default:
	elog(WARN, "ScanPgRelation: bad buildinfo");
	/* XXX I hope this is right.  It seems better than returning
	 * an uninitialized value */
	return_tuple = NULL;
    }
    
    /* all done */
    if (!IsInitProcessingMode())
	RelationUnsetLockForRead(pg_class_desc);
    heap_close(pg_class_desc);
    
    return return_tuple;
}

/* ----------------
 *	AllocateRelationDesc
 *
 *	This is used to allocate memory for a new relation descriptor
 *	and initialize the rd_rel field.
 * ----------------
 */
static Relation
AllocateRelationDesc(u_int natts, Form_pg_class relp)
{
    Relation 		relation;
    Size		len;
    Form_pg_class	relationTupleForm;
    
    /* ----------------
     *  allocate space for the relation tuple form
     * ----------------
     */
    relationTupleForm = (Form_pg_class)
	palloc((Size) (sizeof(FormData_pg_class)));
    
    memmove((char *) relationTupleForm, (char *) relp, CLASS_TUPLE_SIZE);
    
    /* ----------------
     *	allocate space for new relation descriptor
     */
    len = sizeof(RelationData) + 10;	/* + 10 is voodoo XXX mao */
    
    relation = (Relation) palloc(len);

    /* ----------------
     *	clear new reldesc 
     * ----------------
     */
     memset((char *) relation, 0, len); 

    /* initialize attribute tuple form */
    relation->rd_att = CreateTemplateTupleDesc(natts);

    /*and initialize relation tuple form */
    relation->rd_rel = relationTupleForm;
    
    return relation;
}

/* --------------------------------
 *	RelationBuildTupleDesc
 *
 *	Form the relation's tuple descriptor from information in
 *	the pg_attribute system catalog.
 * --------------------------------
 */
static void
RelationBuildTupleDesc(RelationBuildDescInfo buildinfo,    
		       Relation relation,
		       AttributeTupleForm attp,
		       u_int natts)
{
    /*
     *  If this is bootstrap time (initdb), then we can't use the system
     *  catalog indices, because they may not exist yet.  Otherwise, we
     *  can, and do.
     */
    
    if (IsBootstrapProcessingMode())
	build_tupdesc_seq(buildinfo, relation, attp, natts);
    else
	build_tupdesc_ind(buildinfo, relation, attp, natts);
}

static void
build_tupdesc_seq(RelationBuildDescInfo buildinfo,
		  Relation relation,
		  AttributeTupleForm attp,
		  u_int natts)
{
    HeapTuple    pg_attribute_tuple;
    Relation	 pg_attribute_desc;
    HeapScanDesc pg_attribute_scan;
    ScanKeyData	 key;
    int		 need;
    
    /* ----------------
     *	form a scan key
     * ----------------
     */
    ScanKeyEntryInitialize(&key, 0, 
                           Anum_pg_attribute_attrelid,
                           ObjectIdEqualRegProcedure,
                           ObjectIdGetDatum(relation->rd_id));
    
    /* ----------------
     *	open pg_attribute and begin a scan
     * ----------------
     */
    pg_attribute_desc = heap_openr(AttributeRelationName);
    pg_attribute_scan =
	heap_beginscan(pg_attribute_desc, 0, NowTimeQual, 1, &key);
    
    /* ----------------
     *	add attribute data to relation->rd_att
     * ----------------
     */
    need = natts;
    pg_attribute_tuple = heap_getnext(pg_attribute_scan, 0, (Buffer *) NULL);
    while (HeapTupleIsValid(pg_attribute_tuple) && need > 0) {
	attp = (AttributeTupleForm) GETSTRUCT(pg_attribute_tuple);
	
	if (attp->attnum > 0) {
	    relation->rd_att->attrs[attp->attnum - 1] = 
		(AttributeTupleForm)palloc(ATTRIBUTE_TUPLE_SIZE);
	    
	    memmove((char *) (relation->rd_att->attrs[attp->attnum - 1]),
		    (char *) attp,
		    ATTRIBUTE_TUPLE_SIZE);
	    need--;
	}
	pg_attribute_tuple = heap_getnext(pg_attribute_scan,
					  0, (Buffer *) NULL);
    }
    
    if (need > 0)
	elog(WARN, "catalog is missing %d attribute%s for relid %d",
	     need, (need == 1 ? "" : "s"), relation->rd_id);
    
    /* ----------------
     *	end the scan and close the attribute relation
     * ----------------
     */
    heap_endscan(pg_attribute_scan);
    heap_close(pg_attribute_desc);
}

static void
build_tupdesc_ind(RelationBuildDescInfo buildinfo,
		  Relation	relation,
		  AttributeTupleForm attp,
		  u_int natts)
{
    Relation attrel;
    HeapTuple atttup;
    int i;
     
    attrel = heap_openr(AttributeRelationName);
    
    for (i = 1; i <= relation->rd_rel->relnatts; i++) {
	
	atttup = (HeapTuple) AttributeNumIndexScan(attrel, relation->rd_id, i);
	
	if (!HeapTupleIsValid(atttup))
	    elog(WARN, "cannot find attribute %d of relation %.16s", i,
		 &(relation->rd_rel->relname.data[0]));
	attp = (AttributeTupleForm) GETSTRUCT(atttup);
	
	relation->rd_att->attrs[i - 1] = 
	    (AttributeTupleForm) palloc(ATTRIBUTE_TUPLE_SIZE);
	
	memmove((char *) (relation->rd_att->attrs[i - 1]),
		(char *) attp,
		ATTRIBUTE_TUPLE_SIZE);
    }
    
    heap_close(attrel);
}

/* --------------------------------
 *	RelationBuildRuleLock
 *
 *	Form the relation's rewrite rules from information in
 *	the pg_rewrite system catalog.
 * --------------------------------
 */
static void
RelationBuildRuleLock(Relation relation)
{
    HeapTuple    pg_rewrite_tuple;
    Relation	 pg_rewrite_desc;
    TupleDesc pg_rewrite_tupdesc;
    HeapScanDesc pg_rewrite_scan;
    ScanKeyData	 key;
    RuleLock 	*rulelock;
    int 	 numlocks;
    RewriteRule **rules;
    int 	 maxlocks;
    
    /* ----------------
     *	form an array to hold the rewrite rules (the array is extended if
     *  necessary)
     * ----------------
     */
    maxlocks = 4;
    rules = (RewriteRule **)palloc(sizeof(RewriteRule*)*maxlocks);
    numlocks = 0;

    /* ----------------
     *	form a scan key
     * ----------------
     */
    ScanKeyEntryInitialize(&key, 0, 
                           Anum_pg_rewrite_ev_class,
                           ObjectIdEqualRegProcedure,
                           ObjectIdGetDatum(relation->rd_id));
    
    /* ----------------
     *	open pg_attribute and begin a scan
     * ----------------
     */
    pg_rewrite_desc = heap_openr(RewriteRelationName);
    pg_rewrite_scan =
	heap_beginscan(pg_rewrite_desc, 0, NowTimeQual, 1, &key);
    pg_rewrite_tupdesc =
	RelationGetTupleDescriptor(pg_rewrite_desc);
    
    /* ----------------
     *	add attribute data to relation->rd_att
     * ----------------
     */
    while ((pg_rewrite_tuple = heap_getnext(pg_rewrite_scan, 0,
					    (Buffer *) NULL)) != NULL) {
	bool isnull;
	char *ruleaction = NULL;
	char *rule_evqual_string;
	RewriteRule *rule;

	rule = (RewriteRule *)palloc(sizeof(RewriteRule));

	rule->ruleId = pg_rewrite_tuple->t_oid;

	rule->event =
	    (int)heap_getattr(pg_rewrite_tuple, InvalidBuffer,
				  Anum_pg_rewrite_ev_type, pg_rewrite_tupdesc,
				  &isnull) - 48;
	rule->attrno = 
	    (int)heap_getattr(pg_rewrite_tuple, InvalidBuffer,
				  Anum_pg_rewrite_ev_attr, pg_rewrite_tupdesc,
				  &isnull);
	rule->isInstead = 
	    !!heap_getattr(pg_rewrite_tuple, InvalidBuffer,
			       Anum_pg_rewrite_is_instead, pg_rewrite_tupdesc,
			       &isnull);

	ruleaction =
	    heap_getattr(pg_rewrite_tuple, InvalidBuffer,
			 Anum_pg_rewrite_action, pg_rewrite_tupdesc,
			 &isnull);
	rule_evqual_string =
	    heap_getattr(pg_rewrite_tuple, InvalidBuffer,
			 Anum_pg_rewrite_ev_qual, pg_rewrite_tupdesc,
			 &isnull);

	ruleaction = textout((struct varlena *)ruleaction);
	rule_evqual_string = textout((struct varlena *)rule_evqual_string);

	rule->actions = (List*)stringToNode(ruleaction);
	rule->qual = (Node*)stringToNode(rule_evqual_string);

	rules[numlocks++] = rule;
	if (numlocks==maxlocks) {
	    maxlocks *= 2;
	    rules =
		(RewriteRule **)repalloc(rules, sizeof(RewriteRule*)*maxlocks);
	}
    }

    /* ----------------
     *	end the scan and close the attribute relation
     * ----------------
     */
    heap_endscan(pg_rewrite_scan);
    heap_close(pg_rewrite_desc);

    /* ----------------
     *	form a RuleLock and insert into relation
     * ----------------
     */
    rulelock = (RuleLock *)palloc(sizeof(RuleLock));
    rulelock->numLocks = numlocks;
    rulelock->rules = rules;

    relation->rd_rules = rulelock;
    return;
}


/* --------------------------------
 *	RelationBuildDesc
 *	
 *	To build a relation descriptor, we have to allocate space,
 *	open the underlying unix file and initialize the following
 *	fields:
 *
 *  File		   rd_fd;	 open file descriptor
 *  int                    rd_nblocks;   number of blocks in rel 
 *					 it will be set in ambeginscan()
 *  uint16		   rd_refcnt;	 reference count
 *  Form_pg_am  	   rd_am;	 AM tuple
 *  Form_pg_class	   rd_rel;	 RELATION tuple
 *  Oid		   	   rd_id;	 relations's object id 
 *  Pointer		   lockInfo;	 ptr. to misc. info.
 *  TupleDesc              rd_att;	 tuple desciptor
 *
 *	Note: rd_ismem (rel is in-memory only) is currently unused
 *      by any part of the system.  someday this will indicate that
 *	the relation lives only in the main-memory buffer pool
 *	-cim 2/4/91
 * --------------------------------
 */
static Relation
RelationBuildDesc(RelationBuildDescInfo buildinfo)
{
    File		fd;
    Relation		relation;
    u_int		natts;
    Oid			relid;
    Oid			relam;
    Form_pg_class	relp;
    AttributeTupleForm	attp = NULL;
    
    MemoryContext	oldcxt;
    
    HeapTuple		pg_class_tuple;
    
    oldcxt = MemoryContextSwitchTo((MemoryContext)CacheCxt);
    
    /* ----------------
     *	find the tuple in pg_class corresponding to the given relation id
     * ----------------
     */
    pg_class_tuple = ScanPgRelation(buildinfo);
    
    /* ----------------
     *	if no such tuple exists, return NULL
     * ----------------
     */
    if (! HeapTupleIsValid(pg_class_tuple)) {
	
	MemoryContextSwitchTo(oldcxt); 
	
	return NULL;
    }
    
    /* ----------------
     *	get information from the pg_class_tuple
     * ----------------
     */
    relid = pg_class_tuple->t_oid;
    relp = (Form_pg_class) GETSTRUCT(pg_class_tuple);
    natts = relp->relnatts;
    
    /* ----------------
     *	allocate storage for the relation descriptor,
     *  initialize relation->rd_rel and get the access method id.
     * ----------------
     */
    relation = AllocateRelationDesc(natts, relp);
    relam = relation->rd_rel->relam;
    
    /* ----------------
     *	initialize the relation's relation id (relation->rd_id)
     * ----------------
     */
    relation->rd_id = relid;
    
    /* ----------------
     *	initialize relation->rd_refcnt
     * ----------------
     */
    RelationSetReferenceCount(relation, 1);
    
    /* ----------------
     *   normal relations are not nailed into the cache
     * ----------------
     */
    relation->rd_isnailed = false;
    
    /* ----------------
     *	initialize the access method information (relation->rd_am)
     * ----------------
     */
    if (OidIsValid(relam)) {
	relation->rd_am = (Form_pg_am)
	    AccessMethodObjectIdGetAccessMethodTupleForm(relam);
    }
    
    /* ----------------
     *	initialize the tuple descriptor (relation->rd_att).
     *  remember, rd_att is an array of attribute pointers that lives
     *  off the end of the relation descriptor structure so space was
     *  already allocated for it by AllocateRelationDesc.
     * ----------------
     */
    RelationBuildTupleDesc(buildinfo, relation, attp, natts);

    /* ----------------
     *  initialize rules that affect this relation
     * ----------------
     */
    if (relp->relhasrules) {
	RelationBuildRuleLock(relation);
    } else {
	relation->rd_rules = NULL;
    }
    
    /* ----------------
     *	initialize index strategy and support information for this relation
     * ----------------
     */
    if (OidIsValid(relam)) {
	IndexedAccessMethodInitialize(relation);
    }
    
    /* ----------------
     *	initialize the relation lock manager information
     * ----------------
     */
    RelationInitLockInfo(relation); /* see lmgr.c */
    
    /* ----------------
     *	open the relation and assign the file descriptor returned
     *  by the storage manager code to rd_fd.
     * ----------------
     */
    fd = smgropen(relp->relsmgr, relation);
    
    Assert (fd >= -1);
    if (fd == -1)
	elog(NOTICE, "RelationIdBuildRelation: smgropen(%s): %m",
	     &relp->relname);
    
    relation->rd_fd = fd;
    
    /* ----------------
     *	insert newly created relation into proper relcaches,
     *  restore memory context and return the new reldesc.
     * ----------------
     */
    
    RelationCacheInsert(relation);
    
    /* -------------------
     *  free the memory allocated for pg_class_tuple
     *  and for lock data pointed to by pg_class_tuple
     * -------------------
     */
    pfree(pg_class_tuple);
    
    MemoryContextSwitchTo(oldcxt);
    
    return relation;
}

static void
IndexedAccessMethodInitialize(Relation relation)
{
    IndexStrategy 	strategy;
    RegProcedure	*support;
    int			natts;
    Size 		stratSize;
    Size		supportSize;
    uint16 		relamstrategies;
    uint16		relamsupport;
    
    natts = relation->rd_rel->relnatts;
    relamstrategies = relation->rd_am->amstrategies;
    stratSize = AttributeNumberGetIndexStrategySize(natts, relamstrategies);
    strategy = (IndexStrategy) palloc(stratSize);
    relamsupport = relation->rd_am->amsupport;
    
    if (relamsupport > 0) {
	supportSize = natts * (relamsupport * sizeof (RegProcedure));
	support = (RegProcedure *) palloc(supportSize);
    } else {
	support = (RegProcedure *) NULL;
    }
    
    IndexSupportInitialize(strategy, support,
			   relation->rd_att->attrs[0]->attrelid,
			   relation->rd_rel->relam,
			   relamstrategies, relamsupport, natts);
    
    RelationSetIndexSupport(relation, strategy, support);
}

/* --------------------------------
 *	formrdesc
 *
 *	This is a special version of RelationBuildDesc()
 *	used by RelationInitialize() in initializing the
 *	relcache.  The system relation descriptors built
 *	here are all nailed in the descriptor caches, for
 *	bootstrapping.
 * --------------------------------
 */
static void
formrdesc(char *relationName,
	  u_int natts,
	  FormData_pg_attribute att[])
{
    Relation	relation;
    Size	len;
    int		i;
    
    /* ----------------
     *	allocate new relation desc
     * ----------------
     */
    len = sizeof (RelationData);
    relation = (Relation) palloc(len);
    memset((char *)relation, 0,len); 

    /* ----------------
     *	don't open the unix file yet..
     * ----------------
     */
    relation->rd_fd = -1;
    
    /* ----------------
     *	initialize reference count
     * ----------------
     */
    RelationSetReferenceCount(relation, 1);
    
    /* ----------------
     *	initialize relation tuple form
     * ----------------
     */
    relation->rd_rel = (Form_pg_class)
	palloc((Size) (sizeof(*relation->rd_rel)));
    memset(relation->rd_rel, 0, sizeof(FormData_pg_class)); 
    namestrcpy(&relation->rd_rel->relname, relationName);
    
    /* ----------------
       initialize attribute tuple form
    */
    relation->rd_att = CreateTemplateTupleDesc(natts);
    
    /*
     *  For debugging purposes, it's important to distinguish between
     *  shared and non-shared relations, even at bootstrap time.  There's
     *  code in the buffer manager that traces allocations that has to
     *  know about this.
     */
    
    if (IsSystemRelationName(relationName)) {
	relation->rd_rel->relowner = 6;			/* XXX use sym const */
	relation->rd_rel->relisshared =
	    IsSharedSystemRelationName(relationName);
    } else {
	relation->rd_rel->relowner = InvalidOid;	/* XXX incorrect*/
	relation->rd_rel->relisshared = false;
    }
    
    relation->rd_rel->relpages = 1;			/* XXX */
    relation->rd_rel->reltuples = 1;			/* XXX */
    relation->rd_rel->relkind = RELKIND_RELATION;
    relation->rd_rel->relarch = 'n';
    relation->rd_rel->relnatts = (uint16) natts;
    relation->rd_isnailed = true;
    
    /* ----------------
     *	initialize tuple desc info
     * ----------------
     */
    for (i = 0; i < natts; i++) {
	relation->rd_att->attrs[i] = 
	    (AttributeTupleForm)palloc(ATTRIBUTE_TUPLE_SIZE);
	
	memset((char *)relation->rd_att->attrs[i], 0,
	       ATTRIBUTE_TUPLE_SIZE);
	memmove((char *)relation->rd_att->attrs[i],
		(char *)&att[i],
		ATTRIBUTE_TUPLE_SIZE);
    }
    
    /* ----------------
     *	initialize relation id
     * ----------------
     */
    relation->rd_id = relation->rd_att->attrs[0]->attrelid;
    
    /* ----------------
     *	add new reldesc to relcache
     * ----------------
     */
    RelationCacheInsert(relation);
    /*
     * Determining this requires a scan on pg_class, but to do the
     * scan the rdesc for pg_class must already exist.  Therefore
     * we must do the check (and possible set) after cache insertion.
     */
    relation->rd_rel->relhasindex =
	CatalogHasIndex(relationName, relation->rd_id);
}


/* ----------------------------------------------------------------
 *		 Relation Descriptor Lookup Interface
 * ----------------------------------------------------------------
 */

/* --------------------------------
 *	RelationIdCacheGetRelation
 *
 *	only try to get the reldesc by looking up the cache
 *	do not go to the disk.  this is used by BlockPrepareFile()
 *	and RelationIdGetRelation below.
 * --------------------------------
 */
Relation
RelationIdCacheGetRelation(Oid relationId)
{
    Relation	rd;
    
    RelationIdCacheLookup(relationId, rd);
    
    if (RelationIsValid(rd)) {
	if (rd->rd_fd == -1) {
	    rd->rd_fd = smgropen(rd->rd_rel->relsmgr, rd);
	    Assert(rd->rd_fd != -1);
	}
	
	RelationIncrementReferenceCount(rd);
	RelationSetLockForDescriptorOpen(rd);
	
    }
    
    return(rd);
}

/* --------------------------------
 *	RelationNameCacheGetRelation
 * --------------------------------
 */
Relation
RelationNameCacheGetRelation(char *relationName)
{
    Relation	rd;
    NameData	name;
    
    /* make sure that the name key used for hash lookup is properly
       null-padded */
    namestrcpy(&name, relationName);
    RelationNameCacheLookup(name.data, rd);
    
    if (RelationIsValid(rd)) {
	if (rd->rd_fd == -1) {
	    rd->rd_fd = smgropen(rd->rd_rel->relsmgr, rd);
	    Assert(rd->rd_fd != -1);
	}
	
	RelationIncrementReferenceCount(rd);
	RelationSetLockForDescriptorOpen(rd);
	
    }
    
    return(rd);
}

/* --------------------------------
 *	RelationIdGetRelation
 *
 *	return a relation descriptor based on its id.
 *	return a cached value if possible
 * --------------------------------
 */
Relation
RelationIdGetRelation(Oid relationId)
{
    Relation		  rd;
    RelationBuildDescInfo buildinfo;
    
    /* ----------------
     *	increment access statistics
     * ----------------
     */
    IncrHeapAccessStat(local_RelationIdGetRelation);
    IncrHeapAccessStat(global_RelationIdGetRelation);
    
    /* ----------------
     *	first try and get a reldesc from the cache
     * ----------------
     */
    rd = RelationIdCacheGetRelation(relationId);
    if (RelationIsValid(rd))
	return rd;
    
    /* ----------------
     *	no reldesc in the cache, so have RelationBuildDesc()
     *  build one and add it.
     * ----------------
     */
    buildinfo.infotype =  INFO_RELID;
    buildinfo.i.info_id = relationId;
    
    rd = RelationBuildDesc(buildinfo);
    return
	rd;
}

/* --------------------------------
 *	RelationNameGetRelation
 *
 *	return a relation descriptor based on its name.
 *	return a cached value if possible
 * --------------------------------
 */
Relation
RelationNameGetRelation(char *relationName)
{
    Relation		  rd;
    RelationBuildDescInfo buildinfo;
    
    /* ----------------
     *	increment access statistics
     * ----------------
     */
    IncrHeapAccessStat(local_RelationNameGetRelation);
    IncrHeapAccessStat(global_RelationNameGetRelation);
    
    /* ----------------
     *	first try and get a reldesc from the cache
     * ----------------
     */
    rd = RelationNameCacheGetRelation(relationName);
    if (RelationIsValid(rd))
	return rd;
    
    /* ----------------
     *	no reldesc in the cache, so have RelationBuildDesc()
     *  build one and add it.
     * ----------------
     */
    buildinfo.infotype =    INFO_RELNAME;
    buildinfo.i.info_name = relationName;
    
    rd = RelationBuildDesc(buildinfo);
    return rd;
}

/* ----------------
 *	old "getreldesc" interface.
 * ----------------
 */
Relation
getreldesc(char *relationName)
{
    /* ----------------
     *	increment access statistics
     * ----------------
     */
    IncrHeapAccessStat(local_getreldesc);
    IncrHeapAccessStat(global_getreldesc);
    
    return RelationNameGetRelation(relationName);
}

/* ----------------------------------------------------------------
 *		cache invalidation support routines
 * ----------------------------------------------------------------
 */

/* --------------------------------
 *	RelationClose - close an open relation
 * --------------------------------
 */
void
RelationClose(Relation relation)
{
    /* Note: no locking manipulations needed */
    RelationDecrementReferenceCount(relation);
}

/* --------------------------------
 * RelationFlushRelation
 *
 *   Actually blows away a relation... RelationFree doesn't do 
 *   anything anymore.
 * --------------------------------
 */
void
RelationFlushRelation(Relation *relationPtr,
		      bool onlyFlushReferenceCountZero)
{
    int			i;
    AttributeTupleForm	*p;
    MemoryContext	oldcxt;
    Relation 		relation = *relationPtr;
    
    if (relation->rd_isnailed) {
	/* this is a nailed special relation for bootstraping */
	return;
    }
    
    if (!onlyFlushReferenceCountZero || 
	RelationHasReferenceCountZero(relation)) {
	
	oldcxt = MemoryContextSwitchTo((MemoryContext)CacheCxt);
	
	RelationCacheDelete(relation);

	p = relation->rd_att->attrs;
	for (i = 0; i < relation->rd_rel->relnatts; i++, p++)
	    pfree (*p);
	pfree (relation->rd_att->attrs);
	pfree (relation->rd_att);

#if 0
	if (relation->rd_rules) {
	    int j;
	    for(j=0; j < relation->rd_rules->numLocks; j++) {
		pfree(relation->rd_rules->rules[j]);
	    }
	    pfree(relation->rd_rules->rules);
	    pfree(relation->rd_rules);
	}
#endif
	
	pfree(RelationGetLockInfo(relation));
	pfree(RelationGetRelationTupleForm(relation));
	pfree(relation);
	
	MemoryContextSwitchTo(oldcxt);
    }
}

/* --------------------------------
 *	RelationForgetRelation -
 *	   RelationFlushRelation + if the relation is local then get rid of
 *	   the relation descriptor from the newly created relation list. 
 * --------------------------------
 */
void
RelationForgetRelation (Oid rid)
{
    Relation relation;
    
    RelationIdCacheLookup (rid, relation);
    Assert ( PointerIsValid (relation) );
    
    if ( relation->rd_islocal )
    {
    	MemoryContext oldcxt;
    	List *curr;
    	List *prev = NIL;
    
    	oldcxt = MemoryContextSwitchTo((MemoryContext)CacheCxt);
    
    	foreach (curr, newlyCreatedRelns)
    	{
	    Relation reln = lfirst(curr);
    
    	    Assert ( reln != NULL && reln->rd_islocal );
    	    if ( reln->rd_id == rid )
    	    	break;
    	    prev = curr;
    	}
    	if ( curr == NIL )
    	    elog (FATAL, "Local relation %.*s not found in list",
    	    	NAMEDATALEN, (RelationGetRelationName(relation))->data);
    	if ( prev == NIL )
    	    newlyCreatedRelns = lnext (newlyCreatedRelns);
    	else
    	    lnext (prev) = lnext (curr);
    	pfree (curr);
    	MemoryContextSwitchTo(oldcxt);
    }

    RelationFlushRelation (&relation, false);
}

/* --------------------------------
 *	RelationIdInvalidateRelationCacheByRelationId
 * --------------------------------
 */
void
RelationIdInvalidateRelationCacheByRelationId(Oid relationId)
{
    Relation	relation;
    
    RelationIdCacheLookup(relationId, relation);

    /*
     * "local" relations are invalidated by RelationPurgeLocalRelation.
     * (This is to make LocalBufferSync's life easier: want the descriptor
     * to hang around for a while. In fact, won't we want this for
     * BufferSync also? But I'll leave it for now since I don't want to
     * break anything.)	- ay 3/95
     */
    if (PointerIsValid(relation) && !relation->rd_islocal) {
	/*
	 * The boolean onlyFlushReferenceCountZero in RelationFlushReln()
	 * should be set to true when we are incrementing the command
	 * counter and to false when we are starting a new xaction.  This
	 * can be determined by checking the current xaction status.
	 */
	RelationFlushRelation(&relation, CurrentXactInProgress());
    }
}

#if 0		/* See comments at line 1304 */
/* --------------------------------
 *	RelationIdInvalidateRelationCacheByAccessMethodId
 *
 *	RelationFlushIndexes is needed for use with HashTableWalk..
 * --------------------------------
 */
static void
RelationFlushIndexes(Relation *r, 
		     Oid accessMethodId)
{
    Relation relation = *r;
    
    if (!RelationIsValid(relation)) {
	elog(NOTICE, "inval call to RFI");
	return;
    }
    
    if (relation->rd_rel->relkind == RELKIND_INDEX &&	/* XXX style */
	(!OidIsValid(accessMethodId) ||
	 relation->rd_rel->relam == accessMethodId))
	{
	    RelationFlushRelation(&relation, false);
	}
}
#endif


void
RelationIdInvalidateRelationCacheByAccessMethodId(Oid accessMethodId)
{
# if 0
    /*
     *  25 aug 1992:  mao commented out the ht walk below.  it should be
     *  doing the right thing, in theory, but flushing reldescs for index
     *  relations apparently doesn't work.  we want to cut 4.0.1, and i
     *  don't want to introduce new bugs.  this code never executed before,
     *  so i'm turning it off for now.  after the release is cut, i'll
     *  fix this up.
     */
    
    HashTableWalk(RelationNameCache, (HashtFunc) RelationFlushIndexes,
		  accessMethodId);
# else
    return;
# endif
}

/*
 * RelationCacheInvalidate
 *
 *   Will blow away either all the cached relation descriptors or
 *   those that have a zero reference count.
 *
 */
void
RelationCacheInvalidate(bool onlyFlushReferenceCountZero)
{
    HashTableWalk(RelationNameCache, (HashtFunc) RelationFlushRelation,
		  onlyFlushReferenceCountZero);
    
    /*
     * nailed-in reldescs will still be in the cache...
     * 7 hardwired heaps + 3 hardwired indices == 10 total.
     */
    if (!onlyFlushReferenceCountZero) {
	Assert(RelationNameCache->hctl->nkeys == 10);
	Assert(RelationIdCache->hctl->nkeys == 10);
    }
}
					   

/* --------------------------------
 *	RelationRegisterRelation -
 *	   register the Relation descriptor of a newly created relation
 *	   with the relation descriptor Cache.
 * --------------------------------
 */
void
RelationRegisterRelation(Relation relation)
{
    MemoryContext   	oldcxt;
    
    oldcxt = MemoryContextSwitchTo((MemoryContext)CacheCxt);
    
    if (oldcxt != (MemoryContext)CacheCxt) 
	elog(NOIND,"RelationRegisterRelation: WARNING: Context != CacheCxt");
    
    RelationCacheInsert(relation);
    
    RelationInitLockInfo(relation);

    /*
     * we've just created the relation. It is invisible to anyone else
     * before the transaction is committed. Setting rd_islocal allows us
     * to use the local buffer manager for select/insert/etc before the end
     * of transaction. (We also need to keep track of relations
     * created during a transaction and does the necessary clean up at
     * the end of the transaction.)		- ay 3/95
     */
    relation->rd_islocal = TRUE;
    newlyCreatedRelns = lcons(relation, newlyCreatedRelns);
    
    MemoryContextSwitchTo(oldcxt);
}

/*
 * RelationPurgeLocalRelation -
 *    find all the Relation descriptors marked rd_islocal and reset them.
 *    This should be called at the end of a transaction (commit/abort) when
 *    the "local" relations will become visible to others and the multi-user
 *    buffer pool should be used.
 */
void
RelationPurgeLocalRelation(bool xactCommitted)
{
    MemoryContext   	oldcxt;

    if (newlyCreatedRelns==NULL)
	return;		
    
    oldcxt = MemoryContextSwitchTo((MemoryContext)CacheCxt);
    
    while (newlyCreatedRelns) {
	List *l = newlyCreatedRelns;
	Relation reln = lfirst(l);

	Assert(reln!=NULL && reln->rd_islocal);

	if (!xactCommitted) {
	    /*
	     * remove the file if we abort. This is so that files for 
	     * tables created inside a transaction block get removed.
	     */
	    if(reln->rd_istemp) {
	        if(!(reln->rd_tmpunlinked)) {
		    smgrunlink(reln->rd_rel->relsmgr, reln);
		    reln->rd_tmpunlinked = TRUE;
		} 
	    } else {
		smgrunlink(reln->rd_rel->relsmgr, reln);
	    }
	}
	else if ( !IsBootstrapProcessingMode () && !(reln->rd_istemp) )
	    /*
	     * RelationFlushRelation () below will flush relation information
	     * from the cache. We must call smgrclose to flush relation
	     * information from SMGR & FMGR, too. We assume that for temp
	     * relations smgrunlink is already called by heap_destroyr
	     * and we skip smgrclose for them.		- vadim 05/22/97
	     */
	    smgrclose(reln->rd_rel->relsmgr, reln);
	
	reln->rd_islocal = FALSE;

	if (!IsBootstrapProcessingMode())
	    RelationFlushRelation(&reln, FALSE);
	
	newlyCreatedRelns = lnext(newlyCreatedRelns);
	pfree(l);
    }

    MemoryContextSwitchTo(oldcxt);
}

/* --------------------------------
 *	RelationInitialize
 *
 *	This initializes the relation descriptor cache.
 * --------------------------------
 */

#define INITRELCACHESIZE	400

void
RelationInitialize(void)
{
    MemoryContext		oldcxt;
    HASHCTL			ctl;
    
    /* ----------------
     *	switch to cache memory context
     * ----------------
     */
    if (!CacheCxt)
	CacheCxt = CreateGlobalMemory("Cache");
    
    oldcxt = MemoryContextSwitchTo((MemoryContext)CacheCxt);
    
    /* ----------------
     *	create global caches
     * ----------------
     */
    memset(&ctl,0, (int) sizeof(ctl)); 
    ctl.keysize = sizeof(NameData);
    ctl.datasize = sizeof(Relation); 
    RelationNameCache = hash_create(INITRELCACHESIZE, &ctl, HASH_ELEM);
    
    ctl.keysize = sizeof(Oid);
    ctl.hash = tag_hash;
    RelationIdCache = hash_create(INITRELCACHESIZE, &ctl, 
				  HASH_ELEM | HASH_FUNCTION);
    
    /* ----------------
     *	initialize the cache with pre-made relation descriptors
     *  for some of the more important system relations.  These
     *  relations should always be in the cache.
     * ----------------
     */
    formrdesc(RelationRelationName, Natts_pg_class, Desc_pg_class);
    formrdesc(AttributeRelationName, Natts_pg_attribute, Desc_pg_attribute);
    formrdesc(ProcedureRelationName, Natts_pg_proc, Desc_pg_proc);
    formrdesc(TypeRelationName, Natts_pg_type, Desc_pg_type);
    formrdesc(VariableRelationName, Natts_pg_variable, Desc_pg_variable);
    formrdesc(LogRelationName, Natts_pg_log, Desc_pg_log);
    formrdesc(TimeRelationName, Natts_pg_time, Desc_pg_time);

    /*
     *  If this isn't initdb time, then we want to initialize some index
     *  relation descriptors, as well.  The descriptors are for pg_attnumind
     *  (to make building relation descriptors fast) and possibly others,
     *  as they're added.
     */
    
    if (!IsBootstrapProcessingMode())
	init_irels();
    
    MemoryContextSwitchTo(oldcxt);
}

/*
 *  init_irels(), write_irels() -- handle special-case initialization of
 *				   index relation descriptors.
 *
 *	In late 1992, we started regularly having databases with more than
 *	a thousand classes in them.  With this number of classes, it became
 *	critical to do indexed lookups on the system catalogs.
 *
 *	Bootstrapping these lookups is very hard.  We want to be able to
 *	use an index on pg_attribute, for example, but in order to do so,
 *	we must have read pg_attribute for the attributes in the index,
 *	which implies that we need to use the index.
 *
 *	In order to get around the problem, we do the following:
 *
 *	   +  When the database system is initialized (at initdb time), we
 *	      don't use indices on pg_attribute.  We do sequential scans.
 *
 *	   +  When the backend is started up in normal mode, we load an image
 *	      of the appropriate relation descriptors, in internal format,
 *	      from an initialization file in the data/base/... directory.
 *
 *	   +  If the initialization file isn't there, then we create the
 *	      relation descriptor using sequential scans and write it to
 *	      the initialization file for use by subsequent backends.
 *
 *	This is complicated and interferes with system changes, but
 *	performance is so bad that we're willing to pay the tax.
 */

/* pg_attnumind, pg_classnameind, pg_classoidind */
#define Num_indices_bootstrap	3

void
init_irels(void)
{
    Size len;
    int nread;
    File fd;
    Relation irel[Num_indices_bootstrap];
    Relation ird;
    Form_pg_am am;
    Form_pg_class relform;
    IndexStrategy strat;
    RegProcedure *support;
    int i;
    int relno;
    
    if ((fd = FileNameOpenFile(INIT_FILENAME, O_RDONLY, 0600)) < 0) {
	write_irels();
	return;
    }
    
    (void) FileSeek(fd, 0L, SEEK_SET);
    
    for (relno = 0; relno < Num_indices_bootstrap; relno++) {
	/* first read the relation descriptor length*/
	if ((nread = FileRead(fd, (char*)&len, sizeof(int))) != sizeof(int)) {
	    write_irels();
	    return;
	}
	
	ird = irel[relno] = (Relation) palloc(len);
	memset(ird, 0, len); 

	/* then, read the Relation structure */
	if ((nread = FileRead(fd, (char*)ird, len)) != len) {
	    write_irels();
	    return;
	}
	
	/* the file descriptor is not yet opened */
	ird->rd_fd = -1;
	
	/* lock info is not initialized */
	ird->lockInfo = (char *) NULL;
	
	/* next, read the access method tuple form */
	if ((nread = FileRead(fd, (char*)&len, sizeof(int))) != sizeof(int)) {
	    write_irels();
	    return;
	}
	
	am = (Form_pg_am) palloc(len);
	if ((nread = FileRead(fd, (char*)am, len)) != len) {
	    write_irels();
	    return;
	}
	
	ird->rd_am = am;
	
	/* next read the relation tuple form */
	if ((nread = FileRead(fd, (char*)&len, sizeof(int))) != sizeof(int)) {
	    write_irels();
	    return;
	}
	
	relform = (Form_pg_class) palloc(len);
	if ((nread = FileRead(fd, (char*)relform, len)) != len) {
	    write_irels();
	    return;
	}
	
	ird->rd_rel = relform;
	
	/* initialize attribute tuple forms */
	ird->rd_att = CreateTemplateTupleDesc(relform->relnatts);

	/* next read all the attribute tuple form data entries */
	len = ATTRIBUTE_TUPLE_SIZE;
	for (i = 0; i < relform->relnatts; i++) {
	    if ((nread = FileRead(fd, (char*)&len, sizeof(int))) != sizeof(int)) {
		write_irels();
		return;
	    }
	    
	    ird->rd_att->attrs[i] = (AttributeTupleForm) palloc(len);
	    
	    if ((nread = FileRead(fd, (char*)ird->rd_att->attrs[i], len)) != len) {
		write_irels();
		return;
	    }
	}
	
	/* next, read the index strategy map */
	if ((nread = FileRead(fd, (char*)&len, sizeof(int))) != sizeof(int)) {
	    write_irels();
	    return;
	}
	
	strat = (IndexStrategy) palloc(len);
	if ((nread = FileRead(fd, (char*)strat, len)) != len) {
	    write_irels();
	    return;
	}
	
	/* oh, for god's sake... */
#define SMD(i)	strat[0].strategyMapData[i].entry[0]
	
	/* have to reinit the function pointers in the strategy maps */
	for (i = 0; i < am->amstrategies; i++)
	    fmgr_info(SMD(i).sk_procedure,
		      &(SMD(i).sk_func), &(SMD(i).sk_nargs));
	
	
	/* use a real field called rd_istrat instead of the 
	   bogosity of hanging invisible fields off the end of a structure
	   - jolly */
	ird->rd_istrat = strat;

	/* finally, read the vector of support procedures */
	if ((nread = FileRead(fd, (char*)&len, sizeof(int))) != sizeof(int)) {
	    write_irels();
	    return;
	}
	
	support = (RegProcedure *) palloc(len);
	if ((nread = FileRead(fd, (char*)support, len)) != len) {
	    write_irels();
	    return;
	}
	
	/*
	p += sizeof(IndexStrategy);
	*((RegProcedure **) p) = support;
	*/

	ird->rd_support = support;
	
	RelationCacheInsert(ird);
    }
}

void
write_irels(void)
{
    int len;
    int nwritten;
    File fd;
    Relation irel[Num_indices_bootstrap];
    Relation ird;
    Form_pg_am am;
    Form_pg_class relform;
    IndexStrategy strat;
    RegProcedure *support;
    ProcessingMode oldmode;
    int i;
    int relno;
    RelationBuildDescInfo bi;
    
    fd = FileNameOpenFile(INIT_FILENAME, O_WRONLY|O_CREAT|O_TRUNC, 0600);
    if (fd < 0)
	elog(FATAL, "cannot create init file %s", INIT_FILENAME);
    
    (void) FileSeek(fd, 0L, SEEK_SET);
    
    /*
     *  Build a relation descriptor for pg_attnumind without resort to the
     *  descriptor cache.  In order to do this, we set ProcessingMode
     *  to Bootstrap.  The effect of this is to disable indexed relation
     *  searches -- a necessary step, since we're trying to instantiate
     *  the index relation descriptors here.
     */
    
    oldmode = GetProcessingMode();
    SetProcessingMode(BootstrapProcessing);
    
    bi.infotype = INFO_RELNAME;
    bi.i.info_name = AttributeNumIndex;
    irel[0] = RelationBuildDesc(bi);
    irel[0]->rd_isnailed = true;
    
    bi.i.info_name = ClassNameIndex;
    irel[1] = RelationBuildDesc(bi);
    irel[1]->rd_isnailed = true;
    
    bi.i.info_name = ClassOidIndex;
    irel[2] = RelationBuildDesc(bi);
    irel[2]->rd_isnailed = true;
    
    SetProcessingMode(oldmode);
    
    /* nail the descriptor in the cache */
    for (relno = 0; relno < Num_indices_bootstrap; relno++) {
	ird = irel[relno];
	
	/* save the volatile fields in the relation descriptor */
	am = ird->rd_am;
	ird->rd_am = (Form_pg_am) NULL;
	relform = ird->rd_rel;
	ird->rd_rel = (Form_pg_class) NULL;
	strat = ird->rd_istrat;
	support = ird->rd_support;
	
	/* first write the relation descriptor , excluding strategy and support */
	len = sizeof(RelationData);
	
	/* first, write the relation descriptor length */
	if ((nwritten = FileWrite(fd, (char*) &len, sizeof(int)))
	    != sizeof(int))
	    elog(FATAL, "cannot write init file -- descriptor length");
	
	/* next, write out the Relation structure */
	if ((nwritten = FileWrite(fd, (char*) ird, len)) != len)
	    elog(FATAL, "cannot write init file -- reldesc");
	
	/* next, write the access method tuple form */
	len = sizeof(FormData_pg_am);
	if ((nwritten = FileWrite(fd, (char*) &len, sizeof(int)))
	    != sizeof(int))
	    elog(FATAL, "cannot write init file -- am tuple form length");
	
	if ((nwritten = FileWrite(fd, (char*) am, len)) != len)
	    elog(FATAL, "cannot write init file -- am tuple form");
	
	/* next write the relation tuple form */
	len = sizeof(FormData_pg_class);
	if ((nwritten = FileWrite(fd, (char*) &len, sizeof(int)))
	    != sizeof(int))
	    elog(FATAL, "cannot write init file -- relation tuple form length");
	
	if ((nwritten = FileWrite(fd, (char*) relform, len)) != len)
	    elog(FATAL, "cannot write init file -- relation tuple form");
	
	/* next, do all the attribute tuple form data entries */
	len = ATTRIBUTE_TUPLE_SIZE;
	for (i = 0; i < relform->relnatts; i++) {
	    if ((nwritten = FileWrite(fd, (char*) &len, sizeof(int)))
		!= sizeof(int))
		elog(FATAL, "cannot write init file -- length of attdesc %d", i);
	    if ((nwritten = FileWrite(fd, (char*) ird->rd_att->attrs[i], len))
		!= len)
		elog(FATAL, "cannot write init file -- attdesc %d", i);
	}
	
	/* next, write the index strategy map */
	len = AttributeNumberGetIndexStrategySize(relform->relnatts,
						  am->amstrategies);
	if ((nwritten = FileWrite(fd, (char*) &len, sizeof(int)))
	    != sizeof(int))
	    elog(FATAL, "cannot write init file -- strategy map length");
	
	if ((nwritten = FileWrite(fd, (char*) strat, len)) != len)
	    elog(FATAL, "cannot write init file -- strategy map");
	
	/* finally, write the vector of support procedures */
	len = relform->relnatts * (am->amsupport * sizeof(RegProcedure));
	if ((nwritten = FileWrite(fd, (char*) &len, sizeof(int)))
	    != sizeof(int))
	    elog(FATAL, "cannot write init file -- support vector length");
	
	if ((nwritten = FileWrite(fd, (char*) support, len)) != len)
	    elog(FATAL, "cannot write init file -- support vector");
	
	/* restore volatile fields */
	ird->rd_am = am;
	ird->rd_rel = relform;
    }
    
    (void) FileClose(fd);
}

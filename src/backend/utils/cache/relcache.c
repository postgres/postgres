/*-------------------------------------------------------------------------
 *
 * relcache.c
 *	  POSTGRES relation descriptor cache code
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/cache/relcache.c,v 1.190 2003/09/25 06:58:05 petere Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *		RelationCacheInitialize			- initialize relcache
 *		RelationCacheInitializePhase2	- finish initializing relcache
 *		RelationIdGetRelation			- get a reldesc by relation id
 *		RelationSysNameGetRelation		- get a reldesc by system rel name
 *		RelationIdCacheGetRelation		- get a cached reldesc by relid
 *		RelationClose					- close an open relation
 *
 * NOTES
 *		The following code contains many undocumented hacks.  Please be
 *		careful....
 */
#include "postgres.h"

#include <errno.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "access/istrat.h"
#include "catalog/catalog.h"
#include "catalog/catname.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_attrdef.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_index.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_rewrite.h"
#include "catalog/pg_type.h"
#include "commands/trigger.h"
#include "miscadmin.h"
#include "optimizer/clauses.h"
#include "optimizer/planmain.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/relcache.h"
#include "utils/syscache.h"


/*
 * name of relcache init file, used to speed up backend startup
 */
#define RELCACHE_INIT_FILENAME	"pg_internal.init"

/*
 *		hardcoded tuple descriptors.  see include/catalog/pg_attribute.h
 */
static FormData_pg_attribute Desc_pg_class[Natts_pg_class] = {Schema_pg_class};
static FormData_pg_attribute Desc_pg_attribute[Natts_pg_attribute] = {Schema_pg_attribute};
static FormData_pg_attribute Desc_pg_proc[Natts_pg_proc] = {Schema_pg_proc};
static FormData_pg_attribute Desc_pg_type[Natts_pg_type] = {Schema_pg_type};

/*
 *		Hash tables that index the relation cache
 *
 *		Relations are looked up two ways, by OID and by name,
 *		thus there are two hash tables for referencing them.
 *
 *		The OID index covers all relcache entries.	The name index
 *		covers *only* system relations (only those in PG_CATALOG_NAMESPACE).
 */
static HTAB *RelationIdCache;
static HTAB *RelationSysNameCache;

/*
 * Bufmgr uses RelFileNode for lookup. Actually, I would like to do
 * not pass Relation to bufmgr & beyond at all and keep some cache
 * in smgr, but no time to do it right way now.		-- vadim 10/22/2000
 */
static HTAB *RelationNodeCache;

/*
 * This flag is false until we have prepared the critical relcache entries
 * that are needed to do indexscans on the tables read by relcache building.
 */
bool		criticalRelcachesBuilt = false;

/*
 * This flag is set if we discover that we need to write a new relcache
 * cache file at the end of startup.
 */
static bool needNewCacheFile = false;

/*
 * This counter counts relcache inval events received since backend startup
 * (but only for rels that are actually in cache).	Presently, we use it only
 * to detect whether data about to be written by write_relcache_init_file()
 * might already be obsolete.
 */
static long relcacheInvalsReceived = 0L;

/*
 * This list remembers the OIDs of the relations cached in the relcache
 * init file.
 */
static List *initFileRelationIds = NIL;

/*
 *		RelationBuildDescInfo exists so code can be shared
 *		between RelationIdGetRelation() and RelationSysNameGetRelation()
 */
typedef struct RelationBuildDescInfo
{
	int			infotype;		/* lookup by id or by name */
#define INFO_RELID 1
#define INFO_RELNAME 2
	union
	{
		Oid			info_id;	/* relation object id */
		char	   *info_name;	/* system relation name */
	}			i;
} RelationBuildDescInfo;

typedef struct relidcacheent
{
	Oid			reloid;
	Relation	reldesc;
} RelIdCacheEnt;

typedef struct relnamecacheent
{
	NameData	relname;
	Relation	reldesc;
} RelNameCacheEnt;

typedef struct relnodecacheent
{
	RelFileNode relnode;
	Relation	reldesc;
} RelNodeCacheEnt;

/*
 *		macros to manipulate the lookup hashtables
 */
#define RelationCacheInsert(RELATION)	\
do { \
	RelIdCacheEnt *idhentry; RelNodeCacheEnt *nodentry; bool found; \
	idhentry = (RelIdCacheEnt*)hash_search(RelationIdCache, \
										   (void *) &(RELATION->rd_id), \
										   HASH_ENTER, \
										   &found); \
	if (idhentry == NULL) \
		ereport(ERROR, \
				(errcode(ERRCODE_OUT_OF_MEMORY), \
				 errmsg("out of memory"))); \
	/* used to give notice if found -- now just keep quiet */ \
	idhentry->reldesc = RELATION; \
	nodentry = (RelNodeCacheEnt*)hash_search(RelationNodeCache, \
										   (void *) &(RELATION->rd_node), \
										   HASH_ENTER, \
										   &found); \
	if (nodentry == NULL) \
		ereport(ERROR, \
				(errcode(ERRCODE_OUT_OF_MEMORY), \
				 errmsg("out of memory"))); \
	/* used to give notice if found -- now just keep quiet */ \
	nodentry->reldesc = RELATION; \
	if (IsSystemNamespace(RelationGetNamespace(RELATION))) \
	{ \
		char *relname = RelationGetRelationName(RELATION); \
		RelNameCacheEnt *namehentry; \
		namehentry = (RelNameCacheEnt*)hash_search(RelationSysNameCache, \
												   relname, \
												   HASH_ENTER, \
												   &found); \
		if (namehentry == NULL) \
			ereport(ERROR, \
					(errcode(ERRCODE_OUT_OF_MEMORY), \
					 errmsg("out of memory"))); \
		/* used to give notice if found -- now just keep quiet */ \
		namehentry->reldesc = RELATION; \
	} \
} while(0)

#define RelationIdCacheLookup(ID, RELATION) \
do { \
	RelIdCacheEnt *hentry; \
	hentry = (RelIdCacheEnt*)hash_search(RelationIdCache, \
										 (void *)&(ID), HASH_FIND,NULL); \
	if (hentry) \
		RELATION = hentry->reldesc; \
	else \
		RELATION = NULL; \
} while(0)

#define RelationSysNameCacheLookup(NAME, RELATION) \
do { \
	RelNameCacheEnt *hentry; \
	hentry = (RelNameCacheEnt*)hash_search(RelationSysNameCache, \
										   (void *) (NAME), HASH_FIND,NULL); \
	if (hentry) \
		RELATION = hentry->reldesc; \
	else \
		RELATION = NULL; \
} while(0)

#define RelationNodeCacheLookup(NODE, RELATION) \
do { \
	RelNodeCacheEnt *hentry; \
	hentry = (RelNodeCacheEnt*)hash_search(RelationNodeCache, \
										   (void *)&(NODE), HASH_FIND,NULL); \
	if (hentry) \
		RELATION = hentry->reldesc; \
	else \
		RELATION = NULL; \
} while(0)

#define RelationCacheDelete(RELATION) \
do { \
	RelIdCacheEnt *idhentry; RelNodeCacheEnt *nodentry; \
	idhentry = (RelIdCacheEnt*)hash_search(RelationIdCache, \
										   (void *)&(RELATION->rd_id), \
										   HASH_REMOVE, NULL); \
	if (idhentry == NULL) \
		elog(WARNING, "trying to delete a rd_id reldesc that does not exist"); \
	nodentry = (RelNodeCacheEnt*)hash_search(RelationNodeCache, \
										   (void *)&(RELATION->rd_node), \
										   HASH_REMOVE, NULL); \
	if (nodentry == NULL) \
		elog(WARNING, "trying to delete a rd_node reldesc that does not exist"); \
	if (IsSystemNamespace(RelationGetNamespace(RELATION))) \
	{ \
		char *relname = RelationGetRelationName(RELATION); \
		RelNameCacheEnt *namehentry; \
		namehentry = (RelNameCacheEnt*)hash_search(RelationSysNameCache, \
												   relname, \
												   HASH_REMOVE, NULL); \
		if (namehentry == NULL) \
			elog(WARNING, "trying to delete a relname reldesc that does not exist"); \
	} \
} while(0)


/*
 * Special cache for opclass-related information
 */
typedef struct opclasscacheent
{
	Oid			opclassoid;		/* lookup key: OID of opclass */
	bool		valid;			/* set TRUE after successful fill-in */
	StrategyNumber numStrats;	/* max # of strategies (from pg_am) */
	StrategyNumber numSupport;	/* max # of support procs (from pg_am) */
	Oid		   *operatorOids;	/* strategy operators' OIDs */
	RegProcedure *operatorProcs;	/* strategy operators' procs */
	RegProcedure *supportProcs; /* support procs */
} OpClassCacheEnt;

static HTAB *OpClassCache = NULL;


/* non-export function prototypes */

static void RelationClearRelation(Relation relation, bool rebuild);

static void RelationReloadClassinfo(Relation relation);
static void RelationFlushRelation(Relation relation);
static Relation RelationSysNameCacheGetRelation(const char *relationName);
static bool load_relcache_init_file(void);
static void write_relcache_init_file(void);

static void formrdesc(const char *relationName, int natts,
		  FormData_pg_attribute *att);

static HeapTuple ScanPgRelation(RelationBuildDescInfo buildinfo, bool indexOK);
static Relation AllocateRelationDesc(Relation relation, Form_pg_class relp);
static void RelationBuildTupleDesc(RelationBuildDescInfo buildinfo,
					   Relation relation);
static Relation RelationBuildDesc(RelationBuildDescInfo buildinfo,
				  Relation oldrelation);
static void AttrDefaultFetch(Relation relation);
static void CheckConstraintFetch(Relation relation);
static List *insert_ordered_oid(List *list, Oid datum);
static void IndexSupportInitialize(Form_pg_index iform,
					   IndexStrategy indexStrategy,
					   Oid *indexOperator,
					   RegProcedure *indexSupport,
					   StrategyNumber maxStrategyNumber,
					   StrategyNumber maxSupportNumber,
					   AttrNumber maxAttributeNumber);
static OpClassCacheEnt *LookupOpclassInfo(Oid operatorClassOid,
				  StrategyNumber numStrats,
				  StrategyNumber numSupport);


/*
 *		ScanPgRelation
 *
 *		this is used by RelationBuildDesc to find a pg_class
 *		tuple matching either a relation name or a relation id
 *		as specified in buildinfo.
 *
 *		NB: the returned tuple has been copied into palloc'd storage
 *		and must eventually be freed with heap_freetuple.
 */
static HeapTuple
ScanPgRelation(RelationBuildDescInfo buildinfo, bool indexOK)
{
	HeapTuple	pg_class_tuple;
	Relation	pg_class_desc;
	const char *indexRelname;
	SysScanDesc pg_class_scan;
	ScanKeyData key[2];
	int			nkeys;

	/*
	 * form a scan key
	 */
	switch (buildinfo.infotype)
	{
		case INFO_RELID:
			ScanKeyEntryInitialize(&key[0], 0,
								   ObjectIdAttributeNumber,
								   F_OIDEQ,
								   ObjectIdGetDatum(buildinfo.i.info_id));
			nkeys = 1;
			indexRelname = ClassOidIndex;
			break;

		case INFO_RELNAME:
			ScanKeyEntryInitialize(&key[0], 0,
								   Anum_pg_class_relname,
								   F_NAMEEQ,
								   NameGetDatum(buildinfo.i.info_name));
			ScanKeyEntryInitialize(&key[1], 0,
								   Anum_pg_class_relnamespace,
								   F_OIDEQ,
								 ObjectIdGetDatum(PG_CATALOG_NAMESPACE));
			nkeys = 2;
			indexRelname = ClassNameNspIndex;
			break;

		default:
			elog(ERROR, "unrecognized buildinfo type: %d",
				 buildinfo.infotype);
			return NULL;		/* keep compiler quiet */
	}

	/*
	 * Open pg_class and fetch a tuple.  Force heap scan if we haven't yet
	 * built the critical relcache entries (this includes initdb and
	 * startup without a pg_internal.init file).  The caller can also
	 * force a heap scan by setting indexOK == false.
	 */
	pg_class_desc = heap_openr(RelationRelationName, AccessShareLock);
	pg_class_scan = systable_beginscan(pg_class_desc, indexRelname,
									   indexOK && criticalRelcachesBuilt,
									   SnapshotNow,
									   nkeys, key);

	pg_class_tuple = systable_getnext(pg_class_scan);

	/*
	 * Must copy tuple before releasing buffer.
	 */
	if (HeapTupleIsValid(pg_class_tuple))
		pg_class_tuple = heap_copytuple(pg_class_tuple);

	/* all done */
	systable_endscan(pg_class_scan);
	heap_close(pg_class_desc, AccessShareLock);

	return pg_class_tuple;
}

/*
 *		AllocateRelationDesc
 *
 *		This is used to allocate memory for a new relation descriptor
 *		and initialize the rd_rel field.
 *
 *		If 'relation' is NULL, allocate a new RelationData object.
 *		If not, reuse the given object (that path is taken only when
 *		we have to rebuild a relcache entry during RelationClearRelation).
 */
static Relation
AllocateRelationDesc(Relation relation, Form_pg_class relp)
{
	MemoryContext oldcxt;
	Form_pg_class relationForm;

	/* Relcache entries must live in CacheMemoryContext */
	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);

	/*
	 * allocate space for new relation descriptor, if needed
	 */
	if (relation == NULL)
		relation = (Relation) palloc(sizeof(RelationData));

	/*
	 * clear all fields of reldesc
	 */
	MemSet((char *) relation, 0, sizeof(RelationData));
	relation->rd_targblock = InvalidBlockNumber;

	/* make sure relation is marked as having no open file yet */
	relation->rd_fd = -1;

	/*
	 * Copy the relation tuple form
	 *
	 * We only allocate space for the fixed fields, ie, CLASS_TUPLE_SIZE.
	 * relacl is NOT stored in the relcache --- there'd be little point in
	 * it, since we don't copy the tuple's nullvalues bitmap and hence
	 * wouldn't know if the value is valid ... bottom line is that relacl
	 * *cannot* be retrieved from the relcache.  Get it from the syscache
	 * if you need it.
	 */
	relationForm = (Form_pg_class) palloc(CLASS_TUPLE_SIZE);

	memcpy((char *) relationForm, (char *) relp, CLASS_TUPLE_SIZE);

	/* initialize relation tuple form */
	relation->rd_rel = relationForm;

	/* and allocate attribute tuple form storage */
	relation->rd_att = CreateTemplateTupleDesc(relationForm->relnatts,
											   relationForm->relhasoids);

	MemoryContextSwitchTo(oldcxt);

	return relation;
}

/*
 *		RelationBuildTupleDesc
 *
 *		Form the relation's tuple descriptor from information in
 *		the pg_attribute, pg_attrdef & pg_constraint system catalogs.
 */
static void
RelationBuildTupleDesc(RelationBuildDescInfo buildinfo,
					   Relation relation)
{
	HeapTuple	pg_attribute_tuple;
	Relation	pg_attribute_desc;
	SysScanDesc pg_attribute_scan;
	ScanKeyData skey[2];
	int			need;
	TupleConstr *constr;
	AttrDefault *attrdef = NULL;
	int			ndef = 0;

	relation->rd_att->tdhasoid = RelationGetForm(relation)->relhasoids;

	constr = (TupleConstr *) MemoryContextAlloc(CacheMemoryContext,
												sizeof(TupleConstr));
	constr->has_not_null = false;

	/*
	 * Form a scan key that selects only user attributes (attnum > 0).
	 * (Eliminating system attribute rows at the index level is lots
	 * faster than fetching them.)
	 */
	ScanKeyEntryInitialize(&skey[0], 0,
						   Anum_pg_attribute_attrelid,
						   F_OIDEQ,
						   ObjectIdGetDatum(RelationGetRelid(relation)));
	ScanKeyEntryInitialize(&skey[1], 0,
						   Anum_pg_attribute_attnum,
						   F_INT2GT,
						   Int16GetDatum(0));

	/*
	 * Open pg_attribute and begin a scan.	Force heap scan if we haven't
	 * yet built the critical relcache entries (this includes initdb and
	 * startup without a pg_internal.init file).
	 */
	pg_attribute_desc = heap_openr(AttributeRelationName, AccessShareLock);
	pg_attribute_scan = systable_beginscan(pg_attribute_desc,
										   AttributeRelidNumIndex,
										   criticalRelcachesBuilt,
										   SnapshotNow,
										   2, skey);

	/*
	 * add attribute data to relation->rd_att
	 */
	need = relation->rd_rel->relnatts;

	while (HeapTupleIsValid(pg_attribute_tuple = systable_getnext(pg_attribute_scan)))
	{
		Form_pg_attribute attp;

		attp = (Form_pg_attribute) GETSTRUCT(pg_attribute_tuple);

		if (attp->attnum <= 0 ||
			attp->attnum > relation->rd_rel->relnatts)
			elog(ERROR, "invalid attribute number %d for %s",
				 attp->attnum, RelationGetRelationName(relation));

		relation->rd_att->attrs[attp->attnum - 1] =
			(Form_pg_attribute) MemoryContextAlloc(CacheMemoryContext,
												   ATTRIBUTE_TUPLE_SIZE);

		memcpy((char *) (relation->rd_att->attrs[attp->attnum - 1]),
			   (char *) attp,
			   ATTRIBUTE_TUPLE_SIZE);

		/* Update constraint/default info */
		if (attp->attnotnull)
			constr->has_not_null = true;

		if (attp->atthasdef)
		{
			if (attrdef == NULL)
			{
				attrdef = (AttrDefault *)
					MemoryContextAlloc(CacheMemoryContext,
									   relation->rd_rel->relnatts *
									   sizeof(AttrDefault));
				MemSet(attrdef, 0,
					   relation->rd_rel->relnatts * sizeof(AttrDefault));
			}
			attrdef[ndef].adnum = attp->attnum;
			attrdef[ndef].adbin = NULL;
			ndef++;
		}
		need--;
		if (need == 0)
			break;
	}

	/*
	 * end the scan and close the attribute relation
	 */
	systable_endscan(pg_attribute_scan);
	heap_close(pg_attribute_desc, AccessShareLock);

	if (need != 0)
		elog(ERROR, "catalog is missing %d attribute(s) for relid %u",
			 need, RelationGetRelid(relation));

	/*
	 * The attcacheoff values we read from pg_attribute should all be -1
	 * ("unknown").  Verify this if assert checking is on.	They will be
	 * computed when and if needed during tuple access.
	 */
#ifdef USE_ASSERT_CHECKING
	{
		int			i;

		for (i = 0; i < relation->rd_rel->relnatts; i++)
			Assert(relation->rd_att->attrs[i]->attcacheoff == -1);
	}
#endif

	/*
	 * However, we can easily set the attcacheoff value for the first
	 * attribute: it must be zero.	This eliminates the need for special
	 * cases for attnum=1 that used to exist in fastgetattr() and
	 * index_getattr().
	 */
	if (relation->rd_rel->relnatts > 0)
		relation->rd_att->attrs[0]->attcacheoff = 0;

	/*
	 * Set up constraint/default info
	 */
	if (constr->has_not_null || ndef > 0 || relation->rd_rel->relchecks)
	{
		relation->rd_att->constr = constr;

		if (ndef > 0)			/* DEFAULTs */
		{
			if (ndef < relation->rd_rel->relnatts)
				constr->defval = (AttrDefault *)
					repalloc(attrdef, ndef * sizeof(AttrDefault));
			else
				constr->defval = attrdef;
			constr->num_defval = ndef;
			AttrDefaultFetch(relation);
		}
		else
			constr->num_defval = 0;

		if (relation->rd_rel->relchecks > 0)	/* CHECKs */
		{
			constr->num_check = relation->rd_rel->relchecks;
			constr->check = (ConstrCheck *)
				MemoryContextAlloc(CacheMemoryContext,
								constr->num_check * sizeof(ConstrCheck));
			MemSet(constr->check, 0, constr->num_check * sizeof(ConstrCheck));
			CheckConstraintFetch(relation);
		}
		else
			constr->num_check = 0;
	}
	else
	{
		pfree(constr);
		relation->rd_att->constr = NULL;
	}
}

/*
 *		RelationBuildRuleLock
 *
 *		Form the relation's rewrite rules from information in
 *		the pg_rewrite system catalog.
 *
 * Note: The rule parsetrees are potentially very complex node structures.
 * To allow these trees to be freed when the relcache entry is flushed,
 * we make a private memory context to hold the RuleLock information for
 * each relcache entry that has associated rules.  The context is used
 * just for rule info, not for any other subsidiary data of the relcache
 * entry, because that keeps the update logic in RelationClearRelation()
 * manageable.	The other subsidiary data structures are simple enough
 * to be easy to free explicitly, anyway.
 */
static void
RelationBuildRuleLock(Relation relation)
{
	MemoryContext rulescxt;
	MemoryContext oldcxt;
	HeapTuple	rewrite_tuple;
	Relation	rewrite_desc;
	TupleDesc	rewrite_tupdesc;
	SysScanDesc rewrite_scan;
	ScanKeyData key;
	RuleLock   *rulelock;
	int			numlocks;
	RewriteRule **rules;
	int			maxlocks;

	/*
	 * Make the private context.  Parameters are set on the assumption
	 * that it'll probably not contain much data.
	 */
	rulescxt = AllocSetContextCreate(CacheMemoryContext,
									 RelationGetRelationName(relation),
									 ALLOCSET_SMALL_MINSIZE,
									 ALLOCSET_SMALL_INITSIZE,
									 ALLOCSET_SMALL_MAXSIZE);
	relation->rd_rulescxt = rulescxt;

	/*
	 * allocate an array to hold the rewrite rules (the array is extended
	 * if necessary)
	 */
	maxlocks = 4;
	rules = (RewriteRule **)
		MemoryContextAlloc(rulescxt, sizeof(RewriteRule *) * maxlocks);
	numlocks = 0;

	/*
	 * form a scan key
	 */
	ScanKeyEntryInitialize(&key, 0,
						   Anum_pg_rewrite_ev_class,
						   F_OIDEQ,
						   ObjectIdGetDatum(RelationGetRelid(relation)));

	/*
	 * open pg_rewrite and begin a scan
	 *
	 * Note: since we scan the rules using RewriteRelRulenameIndex, we will
	 * be reading the rules in name order, except possibly during
	 * emergency-recovery operations (ie, IsIgnoringSystemIndexes). This
	 * in turn ensures that rules will be fired in name order.
	 */
	rewrite_desc = heap_openr(RewriteRelationName, AccessShareLock);
	rewrite_tupdesc = RelationGetDescr(rewrite_desc);
	rewrite_scan = systable_beginscan(rewrite_desc,
									  RewriteRelRulenameIndex,
									  true, SnapshotNow,
									  1, &key);

	while (HeapTupleIsValid(rewrite_tuple = systable_getnext(rewrite_scan)))
	{
		Form_pg_rewrite rewrite_form = (Form_pg_rewrite) GETSTRUCT(rewrite_tuple);
		bool		isnull;
		Datum		ruleaction;
		Datum		rule_evqual;
		char	   *ruleaction_str;
		char	   *rule_evqual_str;
		RewriteRule *rule;

		rule = (RewriteRule *) MemoryContextAlloc(rulescxt,
												  sizeof(RewriteRule));

		rule->ruleId = HeapTupleGetOid(rewrite_tuple);

		rule->event = rewrite_form->ev_type - '0';
		rule->attrno = rewrite_form->ev_attr;
		rule->isInstead = rewrite_form->is_instead;

		/* Must use heap_getattr to fetch ev_qual and ev_action */

		ruleaction = heap_getattr(rewrite_tuple,
								  Anum_pg_rewrite_ev_action,
								  rewrite_tupdesc,
								  &isnull);
		Assert(!isnull);
		ruleaction_str = DatumGetCString(DirectFunctionCall1(textout,
															 ruleaction));
		oldcxt = MemoryContextSwitchTo(rulescxt);
		rule->actions = (List *) stringToNode(ruleaction_str);
		MemoryContextSwitchTo(oldcxt);
		pfree(ruleaction_str);

		rule_evqual = heap_getattr(rewrite_tuple,
								   Anum_pg_rewrite_ev_qual,
								   rewrite_tupdesc,
								   &isnull);
		Assert(!isnull);
		rule_evqual_str = DatumGetCString(DirectFunctionCall1(textout,
														   rule_evqual));
		oldcxt = MemoryContextSwitchTo(rulescxt);
		rule->qual = (Node *) stringToNode(rule_evqual_str);
		MemoryContextSwitchTo(oldcxt);
		pfree(rule_evqual_str);

		if (numlocks >= maxlocks)
		{
			maxlocks *= 2;
			rules = (RewriteRule **)
				repalloc(rules, sizeof(RewriteRule *) * maxlocks);
		}
		rules[numlocks++] = rule;
	}

	/*
	 * end the scan and close the attribute relation
	 */
	systable_endscan(rewrite_scan);
	heap_close(rewrite_desc, AccessShareLock);

	/*
	 * form a RuleLock and insert into relation
	 */
	rulelock = (RuleLock *) MemoryContextAlloc(rulescxt, sizeof(RuleLock));
	rulelock->numLocks = numlocks;
	rulelock->rules = rules;

	relation->rd_rules = rulelock;
}

/*
 *		equalRuleLocks
 *
 *		Determine whether two RuleLocks are equivalent
 *
 *		Probably this should be in the rules code someplace...
 */
static bool
equalRuleLocks(RuleLock *rlock1, RuleLock *rlock2)
{
	int			i;

	/*
	 * As of 7.3 we assume the rule ordering is repeatable, because
	 * RelationBuildRuleLock should read 'em in a consistent order.  So
	 * just compare corresponding slots.
	 */
	if (rlock1 != NULL)
	{
		if (rlock2 == NULL)
			return false;
		if (rlock1->numLocks != rlock2->numLocks)
			return false;
		for (i = 0; i < rlock1->numLocks; i++)
		{
			RewriteRule *rule1 = rlock1->rules[i];
			RewriteRule *rule2 = rlock2->rules[i];

			if (rule1->ruleId != rule2->ruleId)
				return false;
			if (rule1->event != rule2->event)
				return false;
			if (rule1->attrno != rule2->attrno)
				return false;
			if (rule1->isInstead != rule2->isInstead)
				return false;
			if (!equal(rule1->qual, rule2->qual))
				return false;
			if (!equal(rule1->actions, rule2->actions))
				return false;
		}
	}
	else if (rlock2 != NULL)
		return false;
	return true;
}


/* ----------------------------------
 *		RelationBuildDesc
 *
 *		Build a relation descriptor --- either a new one, or by
 *		recycling the given old relation object.  The latter case
 *		supports rebuilding a relcache entry without invalidating
 *		pointers to it.
 * --------------------------------
 */
static Relation
RelationBuildDesc(RelationBuildDescInfo buildinfo,
				  Relation oldrelation)
{
	Relation	relation;
	Oid			relid;
	HeapTuple	pg_class_tuple;
	Form_pg_class relp;
	MemoryContext oldcxt;

	/*
	 * find the tuple in pg_class corresponding to the given relation id
	 */
	pg_class_tuple = ScanPgRelation(buildinfo, true);

	/*
	 * if no such tuple exists, return NULL
	 */
	if (!HeapTupleIsValid(pg_class_tuple))
		return NULL;

	/*
	 * get information from the pg_class_tuple
	 */
	relid = HeapTupleGetOid(pg_class_tuple);
	relp = (Form_pg_class) GETSTRUCT(pg_class_tuple);

	/*
	 * allocate storage for the relation descriptor, and copy
	 * pg_class_tuple to relation->rd_rel.
	 */
	relation = AllocateRelationDesc(oldrelation, relp);

	/*
	 * now we can free the memory allocated for pg_class_tuple
	 */
	heap_freetuple(pg_class_tuple);

	/*
	 * initialize the relation's relation id (relation->rd_id)
	 */
	RelationGetRelid(relation) = relid;

	/*
	 * initialize relation->rd_refcnt
	 */
	RelationSetReferenceCount(relation, 1);

	/*
	 * normal relations are not nailed into the cache; nor can a
	 * pre-existing relation be new.  It could be temp though.	(Actually,
	 * it could be new too, but it's okay to forget that fact if forced to
	 * flush the entry.)
	 */
	relation->rd_isnailed = 0;
	relation->rd_isnew = false;
	relation->rd_istemp = isTempNamespace(relation->rd_rel->relnamespace);

	/*
	 * initialize the tuple descriptor (relation->rd_att).
	 */
	RelationBuildTupleDesc(buildinfo, relation);

	/*
	 * Fetch rules and triggers that affect this relation
	 */
	if (relation->rd_rel->relhasrules)
		RelationBuildRuleLock(relation);
	else
	{
		relation->rd_rules = NULL;
		relation->rd_rulescxt = NULL;
	}

	if (relation->rd_rel->reltriggers > 0)
		RelationBuildTriggers(relation);
	else
		relation->trigdesc = NULL;

	/*
	 * if it's an index, initialize index-related information
	 */
	if (OidIsValid(relation->rd_rel->relam))
		RelationInitIndexAccessInfo(relation);

	/*
	 * initialize the relation lock manager information
	 */
	RelationInitLockInfo(relation);		/* see lmgr.c */

	if (relation->rd_rel->relisshared)
		relation->rd_node.tblNode = InvalidOid;
	else
		relation->rd_node.tblNode = MyDatabaseId;
	relation->rd_node.relNode = relation->rd_rel->relfilenode;

	/* make sure relation is marked as having no open file yet */
	relation->rd_fd = -1;

	/*
	 * Insert newly created relation into relcache hash tables.
	 */
	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);
	RelationCacheInsert(relation);
	MemoryContextSwitchTo(oldcxt);

	/*
	 * If it's a temp rel, RelationGetNumberOfBlocks will assume that
	 * rd_nblocks is correct.  Must forcibly update the block count when
	 * creating the relcache entry.  But if we are doing a rebuild, don't
	 * do this yet; leave it to RelationClearRelation to do at the end.
	 * (Otherwise, an elog in RelationUpdateNumberOfBlocks would leave us
	 * with inconsistent relcache state.)
	 */
	if (relation->rd_istemp && oldrelation == NULL)
		RelationUpdateNumberOfBlocks(relation);

	return relation;
}

/*
 * Initialize index-access-method support data for an index relation
 */
void
RelationInitIndexAccessInfo(Relation relation)
{
	HeapTuple	tuple;
	Form_pg_am	aform;
	MemoryContext indexcxt;
	MemoryContext oldcontext;
	IndexStrategy strategy;
	Oid		   *operator;
	RegProcedure *support;
	FmgrInfo   *supportinfo;
	int			natts;
	uint16		amstrategies;
	uint16		amsupport;

	/*
	 * Make a copy of the pg_index entry for the index.  Since pg_index
	 * contains variable-length and possibly-null fields, we have to do
	 * this honestly rather than just treating it as a Form_pg_index
	 * struct.
	 */
	tuple = SearchSysCache(INDEXRELID,
						   ObjectIdGetDatum(RelationGetRelid(relation)),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for index %u",
			 RelationGetRelid(relation));
	oldcontext = MemoryContextSwitchTo(CacheMemoryContext);
	relation->rd_indextuple = heap_copytuple(tuple);
	relation->rd_index = (Form_pg_index) GETSTRUCT(relation->rd_indextuple);
	MemoryContextSwitchTo(oldcontext);
	ReleaseSysCache(tuple);

	/*
	 * Make a copy of the pg_am entry for the index's access method
	 */
	tuple = SearchSysCache(AMOID,
						   ObjectIdGetDatum(relation->rd_rel->relam),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for access method %u",
			 relation->rd_rel->relam);
	aform = (Form_pg_am) MemoryContextAlloc(CacheMemoryContext, sizeof *aform);
	memcpy(aform, GETSTRUCT(tuple), sizeof *aform);
	ReleaseSysCache(tuple);
	relation->rd_am = aform;

	natts = relation->rd_rel->relnatts;
	if (natts != relation->rd_index->indnatts)
		elog(ERROR, "relnatts disagrees with indnatts for index %u",
			 RelationGetRelid(relation));
	amstrategies = aform->amstrategies;
	amsupport = aform->amsupport;

	/*
	 * Make the private context to hold index access info.	The reason we
	 * need a context, and not just a couple of pallocs, is so that we
	 * won't leak any subsidiary info attached to fmgr lookup records.
	 *
	 * Context parameters are set on the assumption that it'll probably not
	 * contain much data.
	 */
	indexcxt = AllocSetContextCreate(CacheMemoryContext,
									 RelationGetRelationName(relation),
									 ALLOCSET_SMALL_MINSIZE,
									 ALLOCSET_SMALL_INITSIZE,
									 ALLOCSET_SMALL_MAXSIZE);
	relation->rd_indexcxt = indexcxt;

	/*
	 * Allocate arrays to hold data
	 */
	if (amstrategies > 0)
	{
		int			noperators = natts * amstrategies;
		Size		stratSize;

		stratSize = AttributeNumberGetIndexStrategySize(natts, amstrategies);
		strategy = (IndexStrategy) MemoryContextAlloc(indexcxt, stratSize);
		MemSet(strategy, 0, stratSize);
		operator = (Oid *)
			MemoryContextAlloc(indexcxt, noperators * sizeof(Oid));
		MemSet(operator, 0, noperators * sizeof(Oid));
	}
	else
	{
		strategy = NULL;
		operator = NULL;
	}

	if (amsupport > 0)
	{
		int			nsupport = natts * amsupport;

		support = (RegProcedure *)
			MemoryContextAlloc(indexcxt, nsupport * sizeof(RegProcedure));
		MemSet(support, 0, nsupport * sizeof(RegProcedure));
		supportinfo = (FmgrInfo *)
			MemoryContextAlloc(indexcxt, nsupport * sizeof(FmgrInfo));
		MemSet(supportinfo, 0, nsupport * sizeof(FmgrInfo));
	}
	else
	{
		support = NULL;
		supportinfo = NULL;
	}

	relation->rd_istrat = strategy;
	relation->rd_operator = operator;
	relation->rd_support = support;
	relation->rd_supportinfo = supportinfo;

	/*
	 * Fill the strategy map and the support RegProcedure arrays.
	 * (supportinfo is left as zeroes, and is filled on-the-fly when used)
	 */
	IndexSupportInitialize(relation->rd_index,
						   strategy, operator, support,
						   amstrategies, amsupport, natts);

	/*
	 * expressions and predicate cache will be filled later
	 */
	relation->rd_indexprs = NIL;
	relation->rd_indpred = NIL;
}

/*
 * IndexSupportInitialize
 *		Initializes an index strategy and associated support procedures,
 *		given the index's pg_index tuple.
 *
 * Data is returned into *indexStrategy, *indexOperator, and *indexSupport,
 * all of which are objects allocated by the caller.
 *
 * The caller also passes maxStrategyNumber, maxSupportNumber, and
 * maxAttributeNumber, since these indicate the size of the arrays
 * it has allocated --- but in practice these numbers must always match
 * those obtainable from the system catalog entries for the index and
 * access method.
 */
static void
IndexSupportInitialize(Form_pg_index iform,
					   IndexStrategy indexStrategy,
					   Oid *indexOperator,
					   RegProcedure *indexSupport,
					   StrategyNumber maxStrategyNumber,
					   StrategyNumber maxSupportNumber,
					   AttrNumber maxAttributeNumber)
{
	int			attIndex;

	maxStrategyNumber = AMStrategies(maxStrategyNumber);

	/*
	 * XXX note that the following assumes the INDEX tuple is well formed
	 * and that the *key and *class are 0 terminated.
	 */
	for (attIndex = 0; attIndex < maxAttributeNumber; attIndex++)
	{
		OpClassCacheEnt *opcentry;

		if (!OidIsValid(iform->indclass[attIndex]))
			elog(ERROR, "bogus pg_index tuple");

		/* look up the info for this opclass, using a cache */
		opcentry = LookupOpclassInfo(iform->indclass[attIndex],
									 maxStrategyNumber,
									 maxSupportNumber);

		/* load the strategy information for the index operators */
		if (maxStrategyNumber > 0)
		{
			StrategyMap map;
			Oid		   *opers;
			StrategyNumber strategy;

			map = IndexStrategyGetStrategyMap(indexStrategy,
											  maxStrategyNumber,
											  attIndex + 1);
			opers = &indexOperator[attIndex * maxStrategyNumber];

			for (strategy = 0; strategy < maxStrategyNumber; strategy++)
			{
				ScanKey		mapentry;

				mapentry = StrategyMapGetScanKeyEntry(map, strategy + 1);
				if (RegProcedureIsValid(opcentry->operatorProcs[strategy]))
				{
					MemSet(mapentry, 0, sizeof(*mapentry));
					mapentry->sk_flags = 0;
					mapentry->sk_procedure = opcentry->operatorProcs[strategy];

					/*
					 * Mark mapentry->sk_func invalid, until and unless
					 * someone sets it up.
					 */
					mapentry->sk_func.fn_oid = InvalidOid;
				}
				else
					ScanKeyEntrySetIllegal(mapentry);
				opers[strategy] = opcentry->operatorOids[strategy];
			}
		}

		/* if support routines exist for this access method, load them */
		if (maxSupportNumber > 0)
		{
			RegProcedure *procs;
			StrategyNumber support;

			procs = &indexSupport[attIndex * maxSupportNumber];

			for (support = 0; support < maxSupportNumber; ++support)
				procs[support] = opcentry->supportProcs[support];
		}
	}
}

/*
 * LookupOpclassInfo
 *
 * This routine maintains a per-opclass cache of the information needed
 * by IndexSupportInitialize().  This is more efficient than relying on
 * the catalog cache, because we can load all the info about a particular
 * opclass in a single indexscan of pg_amproc or pg_amop.
 *
 * The information from pg_am about expected range of strategy and support
 * numbers is passed in, rather than being looked up, mainly because the
 * caller will have it already.
 *
 * XXX There isn't any provision for flushing the cache.  However, there
 * isn't any provision for flushing relcache entries when opclass info
 * changes, either :-(
 */
static OpClassCacheEnt *
LookupOpclassInfo(Oid operatorClassOid,
				  StrategyNumber numStrats,
				  StrategyNumber numSupport)
{
	OpClassCacheEnt *opcentry;
	bool		found;
	Relation	pg_amop_desc;
	Relation	pg_amproc_desc;
	SysScanDesc pg_amop_scan;
	SysScanDesc pg_amproc_scan;
	ScanKeyData key;
	HeapTuple	htup;
	bool		indexOK;

	if (OpClassCache == NULL)
	{
		/* First time through: initialize the opclass cache */
		HASHCTL		ctl;

		if (!CacheMemoryContext)
			CreateCacheMemoryContext();

		MemSet(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(Oid);
		ctl.entrysize = sizeof(OpClassCacheEnt);
		ctl.hash = tag_hash;
		OpClassCache = hash_create("Operator class cache", 64,
								   &ctl, HASH_ELEM | HASH_FUNCTION);
	}

	opcentry = (OpClassCacheEnt *) hash_search(OpClassCache,
											   (void *) &operatorClassOid,
											   HASH_ENTER, &found);
	if (opcentry == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));

	if (found && opcentry->valid)
	{
		/* Already made an entry for it */
		Assert(numStrats == opcentry->numStrats);
		Assert(numSupport == opcentry->numSupport);
		return opcentry;
	}

	/* Need to fill in new entry */
	opcentry->valid = false;	/* until known OK */
	opcentry->numStrats = numStrats;
	opcentry->numSupport = numSupport;

	if (numStrats > 0)
	{
		opcentry->operatorOids = (Oid *)
			MemoryContextAlloc(CacheMemoryContext,
							   numStrats * sizeof(Oid));
		MemSet(opcentry->operatorOids, 0, numStrats * sizeof(Oid));
		opcentry->operatorProcs = (RegProcedure *)
			MemoryContextAlloc(CacheMemoryContext,
							   numStrats * sizeof(RegProcedure));
		MemSet(opcentry->operatorProcs, 0, numStrats * sizeof(RegProcedure));
	}
	else
	{
		opcentry->operatorOids = NULL;
		opcentry->operatorProcs = NULL;
	}

	if (numSupport > 0)
	{
		opcentry->supportProcs = (RegProcedure *)
			MemoryContextAlloc(CacheMemoryContext,
							   numSupport * sizeof(RegProcedure));
		MemSet(opcentry->supportProcs, 0, numSupport * sizeof(RegProcedure));
	}
	else
		opcentry->supportProcs = NULL;

	/*
	 * To avoid infinite recursion during startup, force a heap scan if
	 * we're looking up info for the opclasses used by the indexes we
	 * would like to reference here.
	 */
	indexOK = criticalRelcachesBuilt ||
		(operatorClassOid != OID_BTREE_OPS_OID &&
		 operatorClassOid != INT2_BTREE_OPS_OID);

	/*
	 * Scan pg_amop to obtain operators for the opclass
	 */
	if (numStrats > 0)
	{
		ScanKeyEntryInitialize(&key, 0,
							   Anum_pg_amop_amopclaid,
							   F_OIDEQ,
							   ObjectIdGetDatum(operatorClassOid));
		pg_amop_desc = heap_openr(AccessMethodOperatorRelationName,
								  AccessShareLock);
		pg_amop_scan = systable_beginscan(pg_amop_desc,
										  AccessMethodStrategyIndex,
										  indexOK,
										  SnapshotNow,
										  1, &key);

		while (HeapTupleIsValid(htup = systable_getnext(pg_amop_scan)))
		{
			Form_pg_amop amopform = (Form_pg_amop) GETSTRUCT(htup);

			if (amopform->amopstrategy <= 0 ||
				(StrategyNumber) amopform->amopstrategy > numStrats)
				elog(ERROR, "invalid amopstrategy number %d for opclass %u",
					 amopform->amopstrategy, operatorClassOid);
			opcentry->operatorOids[amopform->amopstrategy - 1] =
				amopform->amopopr;
			opcentry->operatorProcs[amopform->amopstrategy - 1] =
				get_opcode(amopform->amopopr);
		}

		systable_endscan(pg_amop_scan);
		heap_close(pg_amop_desc, AccessShareLock);
	}

	/*
	 * Scan pg_amproc to obtain support procs for the opclass
	 */
	if (numSupport > 0)
	{
		ScanKeyEntryInitialize(&key, 0,
							   Anum_pg_amproc_amopclaid,
							   F_OIDEQ,
							   ObjectIdGetDatum(operatorClassOid));
		pg_amproc_desc = heap_openr(AccessMethodProcedureRelationName,
									AccessShareLock);
		pg_amproc_scan = systable_beginscan(pg_amproc_desc,
											AccessMethodProcedureIndex,
											indexOK,
											SnapshotNow,
											1, &key);

		while (HeapTupleIsValid(htup = systable_getnext(pg_amproc_scan)))
		{
			Form_pg_amproc amprocform = (Form_pg_amproc) GETSTRUCT(htup);

			if (amprocform->amprocnum <= 0 ||
				(StrategyNumber) amprocform->amprocnum > numSupport)
				elog(ERROR, "invalid amproc number %d for opclass %u",
					 amprocform->amprocnum, operatorClassOid);

			opcentry->supportProcs[amprocform->amprocnum - 1] =
				amprocform->amproc;
		}

		systable_endscan(pg_amproc_scan);
		heap_close(pg_amproc_desc, AccessShareLock);
	}

	opcentry->valid = true;
	return opcentry;
}


/*
 *		formrdesc
 *
 *		This is a special cut-down version of RelationBuildDesc()
 *		used by RelationCacheInitialize() in initializing the relcache.
 *		The relation descriptor is built just from the supplied parameters,
 *		without actually looking at any system table entries.  We cheat
 *		quite a lot since we only need to work for a few basic system
 *		catalogs.
 *
 * formrdesc is currently used for: pg_class, pg_attribute, pg_proc,
 * and pg_type (see RelationCacheInitialize).
 *
 * Note that these catalogs can't have constraints (except attnotnull),
 * default values, rules, or triggers, since we don't cope with any of that.
 *
 * NOTE: we assume we are already switched into CacheMemoryContext.
 */
static void
formrdesc(const char *relationName,
		  int natts,
		  FormData_pg_attribute *att)
{
	Relation	relation;
	int			i;
	bool		has_not_null;

	/*
	 * allocate new relation desc clear all fields of reldesc
	 */
	relation = (Relation) palloc0(sizeof(RelationData));
	relation->rd_targblock = InvalidBlockNumber;

	/* make sure relation is marked as having no open file yet */
	relation->rd_fd = -1;

	/*
	 * initialize reference count
	 */
	RelationSetReferenceCount(relation, 1);

	/*
	 * all entries built with this routine are nailed-in-cache; none are
	 * for new or temp relations.
	 */
	relation->rd_isnailed = 1;
	relation->rd_isnew = false;
	relation->rd_istemp = false;

	/*
	 * initialize relation tuple form
	 *
	 * The data we insert here is pretty incomplete/bogus, but it'll serve to
	 * get us launched.  RelationCacheInitializePhase2() will read the
	 * real data from pg_class and replace what we've done here.
	 */
	relation->rd_rel = (Form_pg_class) palloc0(CLASS_TUPLE_SIZE);

	namestrcpy(&relation->rd_rel->relname, relationName);
	relation->rd_rel->relnamespace = PG_CATALOG_NAMESPACE;

	/*
	 * It's important to distinguish between shared and non-shared
	 * relations, even at bootstrap time, to make sure we know where they
	 * are stored.	At present, all relations that formrdesc is used for
	 * are not shared.
	 */
	relation->rd_rel->relisshared = false;

	relation->rd_rel->relpages = 1;
	relation->rd_rel->reltuples = 1;
	relation->rd_rel->relkind = RELKIND_RELATION;
	relation->rd_rel->relhasoids = true;
	relation->rd_rel->relnatts = (int16) natts;

	/*
	 * initialize attribute tuple form
	 *
	 * Unlike the case with the relation tuple, this data had better be right
	 * because it will never be replaced.  The input values must be
	 * correctly defined by macros in src/include/catalog/ headers.
	 */
	relation->rd_att = CreateTemplateTupleDesc(natts,
										   relation->rd_rel->relhasoids);

	/*
	 * initialize tuple desc info
	 */
	has_not_null = false;
	for (i = 0; i < natts; i++)
	{
		relation->rd_att->attrs[i] = (Form_pg_attribute) palloc(ATTRIBUTE_TUPLE_SIZE);
		memcpy((char *) relation->rd_att->attrs[i],
			   (char *) &att[i],
			   ATTRIBUTE_TUPLE_SIZE);
		has_not_null |= att[i].attnotnull;
		/* make sure attcacheoff is valid */
		relation->rd_att->attrs[i]->attcacheoff = -1;
	}

	/* initialize first attribute's attcacheoff, cf RelationBuildTupleDesc */
	relation->rd_att->attrs[0]->attcacheoff = 0;

	/* mark not-null status */
	if (has_not_null)
	{
		TupleConstr *constr = (TupleConstr *) palloc0(sizeof(TupleConstr));

		constr->has_not_null = true;
		relation->rd_att->constr = constr;
	}

	/*
	 * initialize relation id from info in att array (my, this is ugly)
	 */
	RelationGetRelid(relation) = relation->rd_att->attrs[0]->attrelid;

	/*
	 * initialize the relation's lock manager and RelFileNode information
	 */
	RelationInitLockInfo(relation);		/* see lmgr.c */

	if (relation->rd_rel->relisshared)
		relation->rd_node.tblNode = InvalidOid;
	else
		relation->rd_node.tblNode = MyDatabaseId;
	relation->rd_node.relNode =
		relation->rd_rel->relfilenode = RelationGetRelid(relation);

	/*
	 * initialize the rel-has-index flag, using hardwired knowledge
	 */
	relation->rd_rel->relhasindex = false;

	/* In bootstrap mode, we have no indexes */
	if (!IsBootstrapProcessingMode())
	{
		/* Otherwise, all the rels formrdesc is used for have indexes */
		relation->rd_rel->relhasindex = true;
	}

	/*
	 * add new reldesc to relcache
	 */
	RelationCacheInsert(relation);
}


/* ----------------------------------------------------------------
 *				 Relation Descriptor Lookup Interface
 * ----------------------------------------------------------------
 */

/*
 *		RelationIdCacheGetRelation
 *
 *		Lookup an existing reldesc by OID.
 *
 *		Only try to get the reldesc by looking in the cache,
 *		do not go to the disk if it's not present.
 *
 *		NB: relation ref count is incremented if successful.
 *		Caller should eventually decrement count.  (Usually,
 *		that happens by calling RelationClose().)
 */
Relation
RelationIdCacheGetRelation(Oid relationId)
{
	Relation	rd;

	RelationIdCacheLookup(relationId, rd);

	if (RelationIsValid(rd))
	{
		RelationIncrementReferenceCount(rd);
		/* revalidate nailed index if necessary */
		if (rd->rd_isnailed == 2)
			RelationReloadClassinfo(rd);
	}

	return rd;
}

/*
 *		RelationSysNameCacheGetRelation
 *
 *		As above, but lookup by name; only works for system catalogs.
 */
static Relation
RelationSysNameCacheGetRelation(const char *relationName)
{
	Relation	rd;
	NameData	name;

	/*
	 * make sure that the name key used for hash lookup is properly
	 * null-padded
	 */
	namestrcpy(&name, relationName);
	RelationSysNameCacheLookup(NameStr(name), rd);

	if (RelationIsValid(rd))
	{
		RelationIncrementReferenceCount(rd);
		/* revalidate nailed index if necessary */
		if (rd->rd_isnailed == 2)
			RelationReloadClassinfo(rd);
	}

	return rd;
}

/*
 *		RelationNodeCacheGetRelation
 *
 *		As above, but lookup by relfilenode.
 *
 * NOTE: this must NOT try to revalidate invalidated nailed indexes, since
 * that could cause us to return an entry with a different relfilenode than
 * the caller asked for.  Currently this is used only by the buffer manager.
 * Really the bufmgr's idea of relations should be separated out from the
 * relcache ...
 */
Relation
RelationNodeCacheGetRelation(RelFileNode rnode)
{
	Relation	rd;

	RelationNodeCacheLookup(rnode, rd);

	if (RelationIsValid(rd))
		RelationIncrementReferenceCount(rd);

	return rd;
}

/*
 *		RelationIdGetRelation
 *
 *		Lookup a reldesc by OID; make one if not already in cache.
 *
 *		NB: relation ref count is incremented, or set to 1 if new entry.
 *		Caller should eventually decrement count.  (Usually,
 *		that happens by calling RelationClose().)
 */
Relation
RelationIdGetRelation(Oid relationId)
{
	Relation	rd;
	RelationBuildDescInfo buildinfo;

	/*
	 * first try and get a reldesc from the cache
	 */
	rd = RelationIdCacheGetRelation(relationId);
	if (RelationIsValid(rd))
		return rd;

	/*
	 * no reldesc in the cache, so have RelationBuildDesc() build one and
	 * add it.
	 */
	buildinfo.infotype = INFO_RELID;
	buildinfo.i.info_id = relationId;

	rd = RelationBuildDesc(buildinfo, NULL);
	return rd;
}

/*
 *		RelationSysNameGetRelation
 *
 *		As above, but lookup by name; only works for system catalogs.
 */
Relation
RelationSysNameGetRelation(const char *relationName)
{
	Relation	rd;
	RelationBuildDescInfo buildinfo;

	/*
	 * first try and get a reldesc from the cache
	 */
	rd = RelationSysNameCacheGetRelation(relationName);
	if (RelationIsValid(rd))
		return rd;

	/*
	 * no reldesc in the cache, so have RelationBuildDesc() build one and
	 * add it.
	 */
	buildinfo.infotype = INFO_RELNAME;
	buildinfo.i.info_name = (char *) relationName;

	rd = RelationBuildDesc(buildinfo, NULL);
	return rd;
}

/* ----------------------------------------------------------------
 *				cache invalidation support routines
 * ----------------------------------------------------------------
 */

/*
 * RelationClose - close an open relation
 *
 *	Actually, we just decrement the refcount.
 *
 *	NOTE: if compiled with -DRELCACHE_FORCE_RELEASE then relcache entries
 *	will be freed as soon as their refcount goes to zero.  In combination
 *	with aset.c's CLOBBER_FREED_MEMORY option, this provides a good test
 *	to catch references to already-released relcache entries.  It slows
 *	things down quite a bit, however.
 */
void
RelationClose(Relation relation)
{
	/* Note: no locking manipulations needed */
	RelationDecrementReferenceCount(relation);

#ifdef RELCACHE_FORCE_RELEASE
	if (RelationHasReferenceCountZero(relation) &&
		!relation->rd_isnew)
		RelationClearRelation(relation, false);
#endif
}

/*
 * RelationReloadClassinfo - reload the pg_class row (only)
 *
 *	This function is used only for nailed indexes.  Since a REINDEX can
 *	change the relfilenode value for a nailed index, we have to reread
 *	the pg_class row anytime we get an SI invalidation on a nailed index
 *	(without throwing away the whole relcache entry, since we'd be unable
 *	to rebuild it).
 *
 *	We can't necessarily reread the pg_class row right away; we might be
 *	in a failed transaction when we receive the SI notification.  If so,
 *	RelationClearRelation just marks the entry as invalid by setting
 *	rd_isnailed to 2.  This routine is called to fix the entry when it
 *	is next needed.
 */
static void
RelationReloadClassinfo(Relation relation)
{
	RelationBuildDescInfo buildinfo;
	bool		indexOK;
	HeapTuple	pg_class_tuple;
	Form_pg_class relp;

	/* Should be called only for invalidated nailed indexes */
	Assert(relation->rd_isnailed == 2 &&
		   relation->rd_rel->relkind == RELKIND_INDEX);
	/* Read the pg_class row */
	buildinfo.infotype = INFO_RELID;
	buildinfo.i.info_id = relation->rd_id;
	/*
	 * Don't try to use an indexscan of pg_class_oid_index to reload the
	 * info for pg_class_oid_index ...
	 */
	indexOK = strcmp(RelationGetRelationName(relation), ClassOidIndex) != 0;
	pg_class_tuple = ScanPgRelation(buildinfo, indexOK);
	if (!HeapTupleIsValid(pg_class_tuple))
		elog(ERROR, "could not find tuple for system relation %u",
			 relation->rd_id);
	relp = (Form_pg_class) GETSTRUCT(pg_class_tuple);
	if (relation->rd_node.relNode != relp->relfilenode)
	{
		/* We have to re-insert the entry into the relcache indexes */
		RelationCacheDelete(relation);
		memcpy((char *) relation->rd_rel, (char *) relp, CLASS_TUPLE_SIZE);
		relation->rd_node.relNode = relp->relfilenode;
		RelationCacheInsert(relation);
	}
	heap_freetuple(pg_class_tuple);
	/* Must adjust number of blocks after we know the new relfilenode */
	relation->rd_targblock = InvalidBlockNumber;
	RelationUpdateNumberOfBlocks(relation);
	/* Okay, now it's valid again */
	relation->rd_isnailed = 1;
}

/*
 * RelationClearRelation
 *
 *	 Physically blow away a relation cache entry, or reset it and rebuild
 *	 it from scratch (that is, from catalog entries).  The latter path is
 *	 usually used when we are notified of a change to an open relation
 *	 (one with refcount > 0).  However, this routine just does whichever
 *	 it's told to do; callers must determine which they want.
 */
static void
RelationClearRelation(Relation relation, bool rebuild)
{
	MemoryContext oldcxt;

	/*
	 * Make sure smgr and lower levels close the relation's files, if they
	 * weren't closed already.  If the relation is not getting deleted,
	 * the next smgr access should reopen the files automatically.	This
	 * ensures that the low-level file access state is updated after, say,
	 * a vacuum truncation.
	 */
	if (relation->rd_fd >= 0)
	{
		smgrclose(DEFAULT_SMGR, relation);
		relation->rd_fd = -1;
	}

	/*
	 * Never, never ever blow away a nailed-in system relation, because
	 * we'd be unable to recover.  However, we must update rd_nblocks and
	 * reset rd_targblock, in case we got called because of a relation
	 * cache flush that was triggered by VACUUM.  If it's a nailed index,
	 * then we need to re-read the pg_class row to see if its relfilenode
	 * changed.  We can't necessarily do that here, because we might be in
	 * a failed transaction.  We assume it's okay to do it if there are open
	 * references to the relcache entry (cf notes for AtEOXact_RelationCache).
	 * Otherwise just mark the entry as possibly invalid, and it'll be fixed
	 * when next opened.
	 */
	if (relation->rd_isnailed)
	{
		if (relation->rd_rel->relkind == RELKIND_INDEX)
		{
			relation->rd_isnailed = 2;	/* needs to be revalidated */
			if (relation->rd_refcnt > 1)
				RelationReloadClassinfo(relation);
		}
		else
		{
			relation->rd_targblock = InvalidBlockNumber;
			RelationUpdateNumberOfBlocks(relation);
		}
		return;
	}

	/*
	 * Remove relation from hash tables
	 *
	 * Note: we might be reinserting it momentarily, but we must not have it
	 * visible in the hash tables until it's valid again, so don't try to
	 * optimize this away...
	 */
	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);
	RelationCacheDelete(relation);
	MemoryContextSwitchTo(oldcxt);

	/* Clear out catcache's entries for this relation */
	CatalogCacheFlushRelation(RelationGetRelid(relation));

	/*
	 * Free all the subsidiary data structures of the relcache entry. We
	 * cannot free rd_att if we are trying to rebuild the entry, however,
	 * because pointers to it may be cached in various places. The rule
	 * manager might also have pointers into the rewrite rules. So to
	 * begin with, we can only get rid of these fields:
	 */
	FreeTriggerDesc(relation->trigdesc);
	if (relation->rd_indextuple)
		pfree(relation->rd_indextuple);
	if (relation->rd_am)
		pfree(relation->rd_am);
	if (relation->rd_rel)
		pfree(relation->rd_rel);
	freeList(relation->rd_indexlist);
	if (relation->rd_indexcxt)
		MemoryContextDelete(relation->rd_indexcxt);

	/*
	 * If we're really done with the relcache entry, blow it away. But if
	 * someone is still using it, reconstruct the whole deal without
	 * moving the physical RelationData record (so that the someone's
	 * pointer is still valid).
	 */
	if (!rebuild)
	{
		/* ok to zap remaining substructure */
		FreeTupleDesc(relation->rd_att);
		if (relation->rd_rulescxt)
			MemoryContextDelete(relation->rd_rulescxt);
		pfree(relation);
	}
	else
	{
		/*
		 * When rebuilding an open relcache entry, must preserve ref count
		 * and rd_isnew flag.  Also attempt to preserve the tupledesc and
		 * rewrite-rule substructures in place.
		 */
		int			old_refcnt = relation->rd_refcnt;
		bool		old_isnew = relation->rd_isnew;
		TupleDesc	old_att = relation->rd_att;
		RuleLock   *old_rules = relation->rd_rules;
		MemoryContext old_rulescxt = relation->rd_rulescxt;
		RelationBuildDescInfo buildinfo;

		buildinfo.infotype = INFO_RELID;
		buildinfo.i.info_id = RelationGetRelid(relation);

		if (RelationBuildDesc(buildinfo, relation) != relation)
		{
			/* Should only get here if relation was deleted */
			FreeTupleDesc(old_att);
			if (old_rulescxt)
				MemoryContextDelete(old_rulescxt);
			pfree(relation);
			elog(ERROR, "relation %u deleted while still in use",
				 buildinfo.i.info_id);
		}
		RelationSetReferenceCount(relation, old_refcnt);
		relation->rd_isnew = old_isnew;
		if (equalTupleDescs(old_att, relation->rd_att))
		{
			FreeTupleDesc(relation->rd_att);
			relation->rd_att = old_att;
		}
		else
			FreeTupleDesc(old_att);
		if (equalRuleLocks(old_rules, relation->rd_rules))
		{
			if (relation->rd_rulescxt)
				MemoryContextDelete(relation->rd_rulescxt);
			relation->rd_rules = old_rules;
			relation->rd_rulescxt = old_rulescxt;
		}
		else
		{
			if (old_rulescxt)
				MemoryContextDelete(old_rulescxt);
		}

		/*
		 * Update rd_nblocks.  This is kind of expensive, but I think we
		 * must do it in case relation has been truncated... we definitely
		 * must do it if the rel is new or temp, since
		 * RelationGetNumberOfBlocks will subsequently assume that the
		 * block count is correct.
		 */
		RelationUpdateNumberOfBlocks(relation);
	}
}

/*
 * RelationFlushRelation
 *
 *	 Rebuild the relation if it is open (refcount > 0), else blow it away.
 */
static void
RelationFlushRelation(Relation relation)
{
	bool		rebuild;

	if (relation->rd_isnew)
	{
		/*
		 * New relcache entries are always rebuilt, not flushed; else we'd
		 * forget the "new" status of the relation, which is a useful
		 * optimization to have.
		 */
		rebuild = true;
	}
	else
	{
		/*
		 * Pre-existing rels can be dropped from the relcache if not open.
		 */
		rebuild = !RelationHasReferenceCountZero(relation);
	}

	RelationClearRelation(relation, rebuild);
}

/*
 * RelationForgetRelation - unconditionally remove a relcache entry
 *
 *		   External interface for destroying a relcache entry when we
 *		   drop the relation.
 */
void
RelationForgetRelation(Oid rid)
{
	Relation	relation;

	RelationIdCacheLookup(rid, relation);

	if (!PointerIsValid(relation))
		return;					/* not in cache, nothing to do */

	if (!RelationHasReferenceCountZero(relation))
		elog(ERROR, "relation %u is still open", rid);

	/* Unconditionally destroy the relcache entry */
	RelationClearRelation(relation, false);
}

/*
 *		RelationIdInvalidateRelationCacheByRelationId
 *
 *		This routine is invoked for SI cache flush messages.
 *
 *		We used to skip local relations, on the grounds that they could
 *		not be targets of cross-backend SI update messages; but it seems
 *		safer to process them, so that our *own* SI update messages will
 *		have the same effects during CommandCounterIncrement for both
 *		local and nonlocal relations.
 */
void
RelationIdInvalidateRelationCacheByRelationId(Oid relationId)
{
	Relation	relation;

	RelationIdCacheLookup(relationId, relation);

	if (PointerIsValid(relation))
	{
		relcacheInvalsReceived++;
		RelationFlushRelation(relation);
	}
}

/*
 * RelationCacheInvalidate
 *	 Blow away cached relation descriptors that have zero reference counts,
 *	 and rebuild those with positive reference counts.
 *
 *	 This is currently used only to recover from SI message buffer overflow,
 *	 so we do not touch new-in-transaction relations; they cannot be targets
 *	 of cross-backend SI updates (and our own updates now go through a
 *	 separate linked list that isn't limited by the SI message buffer size).
 *
 *	 We do this in two phases: the first pass deletes deletable items, and
 *	 the second one rebuilds the rebuildable items.  This is essential for
 *	 safety, because hash_seq_search only copes with concurrent deletion of
 *	 the element it is currently visiting.	If a second SI overflow were to
 *	 occur while we are walking the table, resulting in recursive entry to
 *	 this routine, we could crash because the inner invocation blows away
 *	 the entry next to be visited by the outer scan.  But this way is OK,
 *	 because (a) during the first pass we won't process any more SI messages,
 *	 so hash_seq_search will complete safely; (b) during the second pass we
 *	 only hold onto pointers to nondeletable entries.
 *
 *	 The two-phase approach also makes it easy to ensure that we process
 *	 nailed-in-cache indexes before other nondeletable items, and that we
 *	 process pg_class_oid_index first of all.  In scenarios where a nailed
 *	 index has been given a new relfilenode, we have to detect that update
 *	 before the nailed index is used in reloading any other relcache entry.
 */
void
RelationCacheInvalidate(void)
{
	HASH_SEQ_STATUS status;
	RelIdCacheEnt *idhentry;
	Relation	relation;
	List	   *rebuildFirstList = NIL;
	List	   *rebuildList = NIL;
	List	   *l;

	/* Phase 1 */
	hash_seq_init(&status, RelationIdCache);

	while ((idhentry = (RelIdCacheEnt *) hash_seq_search(&status)) != NULL)
	{
		relation = idhentry->reldesc;

		/* Ignore new relations, since they are never SI targets */
		if (relation->rd_isnew)
			continue;

		relcacheInvalsReceived++;

		if (RelationHasReferenceCountZero(relation))
		{
			/* Delete this entry immediately */
			Assert(!relation->rd_isnailed);
			RelationClearRelation(relation, false);
		}
		else
		{
			/*
			 * Add this entry to list of stuff to rebuild in second pass.
			 * pg_class_oid_index goes on the front of rebuildFirstList,
			 * other nailed indexes on the back, and everything else into
			 * rebuildList (in no particular order).
			 */
			if (relation->rd_isnailed &&
				relation->rd_rel->relkind == RELKIND_INDEX)
			{
				if (strcmp(RelationGetRelationName(relation),
						   ClassOidIndex) == 0)
					rebuildFirstList = lcons(relation, rebuildFirstList);
				else
					rebuildFirstList = lappend(rebuildFirstList, relation);
			}
			else
				rebuildList = lcons(relation, rebuildList);
		}
	}

	rebuildList = nconc(rebuildFirstList, rebuildList);

	/* Phase 2: rebuild the items found to need rebuild in phase 1 */
	foreach(l, rebuildList)
	{
		relation = (Relation) lfirst(l);
		RelationClearRelation(relation, true);
	}
	freeList(rebuildList);
}

/*
 * AtEOXact_RelationCache
 *
 *	Clean up the relcache at transaction commit or abort.
 *
 * Note: this must be called *before* processing invalidation messages.
 * In the case of abort, we don't want to try to rebuild any invalidated
 * cache entries (since we can't safely do database accesses).  Therefore
 * we must reset refcnts before handling pending invalidations.
 */
void
AtEOXact_RelationCache(bool commit)
{
	HASH_SEQ_STATUS status;
	RelIdCacheEnt *idhentry;

	hash_seq_init(&status, RelationIdCache);

	while ((idhentry = (RelIdCacheEnt *) hash_seq_search(&status)) != NULL)
	{
		Relation	relation = idhentry->reldesc;
		int			expected_refcnt;

		/*
		 * Is it a relation created in the current transaction?
		 *
		 * During commit, reset the flag to false, since we are now out of
		 * the creating transaction.  During abort, simply delete the
		 * relcache entry --- it isn't interesting any longer.  (NOTE: if
		 * we have forgotten the isnew state of a new relation due to a
		 * forced cache flush, the entry will get deleted anyway by
		 * shared-cache-inval processing of the aborted pg_class
		 * insertion.)
		 */
		if (relation->rd_isnew)
		{
			if (commit)
				relation->rd_isnew = false;
			else
			{
				RelationClearRelation(relation, false);
				continue;
			}
		}

		/*
		 * During transaction abort, we must also reset relcache entry ref
		 * counts to their normal not-in-a-transaction state.  A ref count
		 * may be too high because some routine was exited by ereport()
		 * between incrementing and decrementing the count.
		 *
		 * During commit, we should not have to do this, but it's still
		 * useful to check that the counts are correct to catch missed
		 * relcache closes.
		 *
		 * In bootstrap mode, do NOT reset the refcnt nor complain that it's
		 * nonzero --- the bootstrap code expects relations to stay open
		 * across start/commit transaction calls.  (That seems bogus, but
		 * it's not worth fixing.)
		 */
		expected_refcnt = relation->rd_isnailed ? 1 : 0;

		if (commit)
		{
			if (relation->rd_refcnt != expected_refcnt &&
				!IsBootstrapProcessingMode())
			{
				elog(WARNING, "relcache reference leak: relation \"%s\" has refcnt %d instead of %d",
					 RelationGetRelationName(relation),
					 relation->rd_refcnt, expected_refcnt);
				RelationSetReferenceCount(relation, expected_refcnt);
			}
		}
		else
		{
			/* abort case, just reset it quietly */
			RelationSetReferenceCount(relation, expected_refcnt);
		}

		/*
		 * Flush any temporary index list.
		 */
		if (relation->rd_indexvalid == 2)
		{
			freeList(relation->rd_indexlist);
			relation->rd_indexlist = NIL;
			relation->rd_indexvalid = 0;
		}
	}
}

/*
 *		RelationBuildLocalRelation
 *			Build a relcache entry for an about-to-be-created relation,
 *			and enter it into the relcache.
 */
Relation
RelationBuildLocalRelation(const char *relname,
						   Oid relnamespace,
						   TupleDesc tupDesc,
						   Oid relid, Oid dbid,
						   RelFileNode rnode,
						   bool nailit)
{
	Relation	rel;
	MemoryContext oldcxt;
	int			natts = tupDesc->natts;
	int			i;
	bool		has_not_null;

	AssertArg(natts >= 0);

	/*
	 * switch to the cache context to create the relcache entry.
	 */
	if (!CacheMemoryContext)
		CreateCacheMemoryContext();

	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);

	/*
	 * allocate a new relation descriptor and fill in basic state fields.
	 */
	rel = (Relation) palloc0(sizeof(RelationData));

	rel->rd_targblock = InvalidBlockNumber;

	/* make sure relation is marked as having no open file yet */
	rel->rd_fd = -1;

	RelationSetReferenceCount(rel, 1);

	/* it's being created in this transaction */
	rel->rd_isnew = true;

	/* is it a temporary relation? */
	rel->rd_istemp = isTempNamespace(relnamespace);

	/*
	 * nail the reldesc if this is a bootstrap create reln and we may need
	 * it in the cache later on in the bootstrap process so we don't ever
	 * want it kicked out.	e.g. pg_attribute!!!
	 */
	if (nailit)
		rel->rd_isnailed = 1;

	/*
	 * create a new tuple descriptor from the one passed in.  We do this
	 * partly to copy it into the cache context, and partly because the
	 * new relation can't have any defaults or constraints yet; they have
	 * to be added in later steps, because they require additions to
	 * multiple system catalogs.  We can copy attnotnull constraints here,
	 * however.
	 */
	rel->rd_att = CreateTupleDescCopy(tupDesc);
	has_not_null = false;
	for (i = 0; i < natts; i++)
	{
		rel->rd_att->attrs[i]->attnotnull = tupDesc->attrs[i]->attnotnull;
		has_not_null |= tupDesc->attrs[i]->attnotnull;
	}

	if (has_not_null)
	{
		TupleConstr *constr = (TupleConstr *) palloc0(sizeof(TupleConstr));

		constr->has_not_null = true;
		rel->rd_att->constr = constr;
	}

	/*
	 * initialize relation tuple form (caller may add/override data later)
	 */
	rel->rd_rel = (Form_pg_class) palloc0(CLASS_TUPLE_SIZE);

	namestrcpy(&rel->rd_rel->relname, relname);
	rel->rd_rel->relnamespace = relnamespace;

	rel->rd_rel->relkind = RELKIND_UNCATALOGED;
	rel->rd_rel->relhasoids = rel->rd_att->tdhasoid;
	rel->rd_rel->relnatts = natts;
	rel->rd_rel->reltype = InvalidOid;

	/*
	 * Insert relation physical and logical identifiers (OIDs) into the
	 * right places.
	 */
	rel->rd_rel->relisshared = (dbid == InvalidOid);

	RelationGetRelid(rel) = relid;

	for (i = 0; i < natts; i++)
		rel->rd_att->attrs[i]->attrelid = relid;

	rel->rd_node = rnode;
	rel->rd_rel->relfilenode = rnode.relNode;

	RelationInitLockInfo(rel);	/* see lmgr.c */

	/*
	 * Okay to insert into the relcache hash tables.
	 */
	RelationCacheInsert(rel);

	/*
	 * done building relcache entry.
	 */
	MemoryContextSwitchTo(oldcxt);

	return rel;
}

/*
 *		RelationCacheInitialize
 *
 *		This initializes the relation descriptor cache.  At the time
 *		that this is invoked, we can't do database access yet (mainly
 *		because the transaction subsystem is not up), so we can't get
 *		"real" info.  However it's okay to read the pg_internal.init
 *		cache file, if one is available.  Otherwise we make phony
 *		entries for the minimum set of nailed-in-cache relations.
 */

#define INITRELCACHESIZE		400

void
RelationCacheInitialize(void)
{
	MemoryContext oldcxt;
	HASHCTL		ctl;

	/*
	 * switch to cache memory context
	 */
	if (!CacheMemoryContext)
		CreateCacheMemoryContext();

	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);

	/*
	 * create hashtables that index the relcache
	 */
	MemSet(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(NameData);
	ctl.entrysize = sizeof(RelNameCacheEnt);
	RelationSysNameCache = hash_create("Relcache by name", INITRELCACHESIZE,
									   &ctl, HASH_ELEM);

	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(RelIdCacheEnt);
	ctl.hash = tag_hash;
	RelationIdCache = hash_create("Relcache by OID", INITRELCACHESIZE,
								  &ctl, HASH_ELEM | HASH_FUNCTION);

	ctl.keysize = sizeof(RelFileNode);
	ctl.entrysize = sizeof(RelNodeCacheEnt);
	ctl.hash = tag_hash;
	RelationNodeCache = hash_create("Relcache by rnode", INITRELCACHESIZE,
									&ctl, HASH_ELEM | HASH_FUNCTION);

	/*
	 * Try to load the relcache cache file.  If successful, we're done for
	 * now.  Otherwise, initialize the cache with pre-made descriptors for
	 * the critical "nailed-in" system catalogs.
	 */
	if (IsBootstrapProcessingMode() ||
		!load_relcache_init_file())
	{
		formrdesc(RelationRelationName,
				  Natts_pg_class, Desc_pg_class);
		formrdesc(AttributeRelationName,
				  Natts_pg_attribute, Desc_pg_attribute);
		formrdesc(ProcedureRelationName,
				  Natts_pg_proc, Desc_pg_proc);
		formrdesc(TypeRelationName,
				  Natts_pg_type, Desc_pg_type);

#define NUM_CRITICAL_RELS	4	/* fix if you change list above */
	}

	MemoryContextSwitchTo(oldcxt);
}

/*
 *		RelationCacheInitializePhase2
 *
 *		This is called as soon as the catcache and transaction system
 *		are functional.  At this point we can actually read data from
 *		the system catalogs.  Update the relcache entries made during
 *		RelationCacheInitialize, and make sure we have entries for the
 *		critical system indexes.
 */
void
RelationCacheInitializePhase2(void)
{
	HASH_SEQ_STATUS status;
	RelIdCacheEnt *idhentry;

	if (IsBootstrapProcessingMode())
		return;

	/*
	 * If we didn't get the critical system indexes loaded into relcache,
	 * do so now.  These are critical because the catcache depends on them
	 * for catcache fetches that are done during relcache load.  Thus, we
	 * have an infinite-recursion problem.	We can break the recursion by
	 * doing heapscans instead of indexscans at certain key spots. To
	 * avoid hobbling performance, we only want to do that until we have
	 * the critical indexes loaded into relcache.  Thus, the flag
	 * criticalRelcachesBuilt is used to decide whether to do heapscan or
	 * indexscan at the key spots, and we set it true after we've loaded
	 * the critical indexes.
	 *
	 * The critical indexes are marked as "nailed in cache", partly to make
	 * it easy for load_relcache_init_file to count them, but mainly
	 * because we cannot flush and rebuild them once we've set
	 * criticalRelcachesBuilt to true.	(NOTE: perhaps it would be
	 * possible to reload them by temporarily setting
	 * criticalRelcachesBuilt to false again.  For now, though, we just
	 * nail 'em in.)
	 */
	if (!criticalRelcachesBuilt)
	{
		RelationBuildDescInfo buildinfo;
		Relation	ird;

#define LOAD_CRIT_INDEX(indname) \
		do { \
			buildinfo.infotype = INFO_RELNAME; \
			buildinfo.i.info_name = (indname); \
			ird = RelationBuildDesc(buildinfo, NULL); \
			ird->rd_isnailed = 1; \
			RelationSetReferenceCount(ird, 1); \
		} while (0)

		LOAD_CRIT_INDEX(ClassNameNspIndex);
		LOAD_CRIT_INDEX(ClassOidIndex);
		LOAD_CRIT_INDEX(AttributeRelidNumIndex);
		LOAD_CRIT_INDEX(IndexRelidIndex);
		LOAD_CRIT_INDEX(AccessMethodStrategyIndex);
		LOAD_CRIT_INDEX(AccessMethodProcedureIndex);
		LOAD_CRIT_INDEX(OperatorOidIndex);

#define NUM_CRITICAL_INDEXES	7		/* fix if you change list above */

		criticalRelcachesBuilt = true;
	}

	/*
	 * Now, scan all the relcache entries and update anything that might
	 * be wrong in the results from formrdesc or the relcache cache file.
	 * If we faked up relcache entries using formrdesc, then read the real
	 * pg_class rows and replace the fake entries with them. Also, if any
	 * of the relcache entries have rules or triggers, load that info the
	 * hard way since it isn't recorded in the cache file.
	 */
	hash_seq_init(&status, RelationIdCache);

	while ((idhentry = (RelIdCacheEnt *) hash_seq_search(&status)) != NULL)
	{
		Relation	relation = idhentry->reldesc;

		/*
		 * If it's a faked-up entry, read the real pg_class tuple.
		 */
		if (needNewCacheFile && relation->rd_isnailed)
		{
			HeapTuple	htup;
			Form_pg_class relp;

			htup = SearchSysCache(RELOID,
							ObjectIdGetDatum(RelationGetRelid(relation)),
								  0, 0, 0);
			if (!HeapTupleIsValid(htup))
				elog(FATAL, "cache lookup failed for relation %u",
					 RelationGetRelid(relation));
			relp = (Form_pg_class) GETSTRUCT(htup);

			/*
			 * Copy tuple to relation->rd_rel. (See notes in
			 * AllocateRelationDesc())
			 */
			Assert(relation->rd_rel != NULL);
			memcpy((char *) relation->rd_rel, (char *) relp, CLASS_TUPLE_SIZE);
			relation->rd_att->tdhasoid = relp->relhasoids;

			ReleaseSysCache(htup);
		}

		/*
		 * Fix data that isn't saved in relcache cache file.
		 */
		if (relation->rd_rel->relhasrules && relation->rd_rules == NULL)
			RelationBuildRuleLock(relation);
		if (relation->rd_rel->reltriggers > 0 && relation->trigdesc == NULL)
			RelationBuildTriggers(relation);
	}
}

/*
 *		RelationCacheInitializePhase3
 *
 *		Final step of relcache initialization: write out a new relcache
 *		cache file if one is needed.
 */
void
RelationCacheInitializePhase3(void)
{
	if (IsBootstrapProcessingMode())
		return;

	if (needNewCacheFile)
	{
		/*
		 * Force all the catcaches to finish initializing and thereby open
		 * the catalogs and indexes they use.  This will preload the
		 * relcache with entries for all the most important system
		 * catalogs and indexes, so that the init file will be most useful
		 * for future backends.
		 */
		InitCatalogCachePhase2();

		/* now write the file */
		write_relcache_init_file();
	}
}


/* used by XLogInitCache */
void		CreateDummyCaches(void);
void		DestroyDummyCaches(void);

void
CreateDummyCaches(void)
{
	MemoryContext oldcxt;
	HASHCTL		ctl;

	if (!CacheMemoryContext)
		CreateCacheMemoryContext();

	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);

	MemSet(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(NameData);
	ctl.entrysize = sizeof(RelNameCacheEnt);
	RelationSysNameCache = hash_create("Relcache by name", INITRELCACHESIZE,
									   &ctl, HASH_ELEM);

	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(RelIdCacheEnt);
	ctl.hash = tag_hash;
	RelationIdCache = hash_create("Relcache by OID", INITRELCACHESIZE,
								  &ctl, HASH_ELEM | HASH_FUNCTION);

	ctl.keysize = sizeof(RelFileNode);
	ctl.entrysize = sizeof(RelNodeCacheEnt);
	ctl.hash = tag_hash;
	RelationNodeCache = hash_create("Relcache by rnode", INITRELCACHESIZE,
									&ctl, HASH_ELEM | HASH_FUNCTION);

	MemoryContextSwitchTo(oldcxt);
}

void
DestroyDummyCaches(void)
{
	MemoryContext oldcxt;

	if (!CacheMemoryContext)
		return;

	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);

	if (RelationIdCache)
		hash_destroy(RelationIdCache);
	if (RelationSysNameCache)
		hash_destroy(RelationSysNameCache);
	if (RelationNodeCache)
		hash_destroy(RelationNodeCache);

	RelationIdCache = RelationSysNameCache = RelationNodeCache = NULL;

	MemoryContextSwitchTo(oldcxt);
}

static void
AttrDefaultFetch(Relation relation)
{
	AttrDefault *attrdef = relation->rd_att->constr->defval;
	int			ndef = relation->rd_att->constr->num_defval;
	Relation	adrel;
	SysScanDesc adscan;
	ScanKeyData skey;
	HeapTuple	htup;
	Datum		val;
	bool		isnull;
	int			found;
	int			i;

	ScanKeyEntryInitialize(&skey,
						   (bits16) 0x0,
						   (AttrNumber) Anum_pg_attrdef_adrelid,
						   (RegProcedure) F_OIDEQ,
						   ObjectIdGetDatum(RelationGetRelid(relation)));

	adrel = heap_openr(AttrDefaultRelationName, AccessShareLock);
	adscan = systable_beginscan(adrel, AttrDefaultIndex, true,
								SnapshotNow,
								1, &skey);
	found = 0;

	while (HeapTupleIsValid(htup = systable_getnext(adscan)))
	{
		Form_pg_attrdef adform = (Form_pg_attrdef) GETSTRUCT(htup);

		for (i = 0; i < ndef; i++)
		{
			if (adform->adnum != attrdef[i].adnum)
				continue;
			if (attrdef[i].adbin != NULL)
				elog(WARNING, "multiple attrdef records found for attr %s of rel %s",
					 NameStr(relation->rd_att->attrs[adform->adnum - 1]->attname),
					 RelationGetRelationName(relation));
			else
				found++;

			val = fastgetattr(htup,
							  Anum_pg_attrdef_adbin,
							  adrel->rd_att, &isnull);
			if (isnull)
				elog(WARNING, "null adbin for attr %s of rel %s",
					 NameStr(relation->rd_att->attrs[adform->adnum - 1]->attname),
					 RelationGetRelationName(relation));
			else
				attrdef[i].adbin = MemoryContextStrdup(CacheMemoryContext,
							 DatumGetCString(DirectFunctionCall1(textout,
																 val)));
			break;
		}

		if (i >= ndef)
			elog(WARNING, "unexpected attrdef record found for attr %d of rel %s",
				 adform->adnum, RelationGetRelationName(relation));
	}

	systable_endscan(adscan);
	heap_close(adrel, AccessShareLock);

	if (found != ndef)
		elog(WARNING, "%d attrdef record(s) missing for rel %s",
			 ndef - found, RelationGetRelationName(relation));
}

static void
CheckConstraintFetch(Relation relation)
{
	ConstrCheck *check = relation->rd_att->constr->check;
	int			ncheck = relation->rd_att->constr->num_check;
	Relation	conrel;
	SysScanDesc conscan;
	ScanKeyData skey[1];
	HeapTuple	htup;
	Datum		val;
	bool		isnull;
	int			found = 0;

	ScanKeyEntryInitialize(&skey[0], 0x0,
						   Anum_pg_constraint_conrelid, F_OIDEQ,
						   ObjectIdGetDatum(RelationGetRelid(relation)));

	conrel = heap_openr(ConstraintRelationName, AccessShareLock);
	conscan = systable_beginscan(conrel, ConstraintRelidIndex, true,
								 SnapshotNow, 1, skey);

	while (HeapTupleIsValid(htup = systable_getnext(conscan)))
	{
		Form_pg_constraint conform = (Form_pg_constraint) GETSTRUCT(htup);

		/* We want check constraints only */
		if (conform->contype != CONSTRAINT_CHECK)
			continue;

		if (found >= ncheck)
			elog(ERROR, "unexpected constraint record found for rel %s",
				 RelationGetRelationName(relation));

		check[found].ccname = MemoryContextStrdup(CacheMemoryContext,
											  NameStr(conform->conname));

		/* Grab and test conbin is actually set */
		val = fastgetattr(htup,
						  Anum_pg_constraint_conbin,
						  conrel->rd_att, &isnull);
		if (isnull)
			elog(ERROR, "null conbin for rel %s",
				 RelationGetRelationName(relation));

		check[found].ccbin = MemoryContextStrdup(CacheMemoryContext,
							 DatumGetCString(DirectFunctionCall1(textout,
																 val)));
		found++;
	}

	systable_endscan(conscan);
	heap_close(conrel, AccessShareLock);

	if (found != ncheck)
		elog(ERROR, "%d constraint record(s) missing for rel %s",
			 ncheck - found, RelationGetRelationName(relation));
}

/*
 * RelationGetIndexList -- get a list of OIDs of indexes on this relation
 *
 * The index list is created only if someone requests it.  We scan pg_index
 * to find relevant indexes, and add the list to the relcache entry so that
 * we won't have to compute it again.  Note that shared cache inval of a
 * relcache entry will delete the old list and set rd_indexvalid to 0,
 * so that we must recompute the index list on next request.  This handles
 * creation or deletion of an index.
 *
 * The returned list is guaranteed to be sorted in order by OID.  This is
 * needed by the executor, since for index types that we obtain exclusive
 * locks on when updating the index, all backends must lock the indexes in
 * the same order or we will get deadlocks (see ExecOpenIndices()).  Any
 * consistent ordering would do, but ordering by OID is easy.
 *
 * Since shared cache inval causes the relcache's copy of the list to go away,
 * we return a copy of the list palloc'd in the caller's context.  The caller
 * may freeList() the returned list after scanning it.	This is necessary
 * since the caller will typically be doing syscache lookups on the relevant
 * indexes, and syscache lookup could cause SI messages to be processed!
 */
List *
RelationGetIndexList(Relation relation)
{
	Relation	indrel;
	SysScanDesc indscan;
	ScanKeyData skey;
	HeapTuple	htup;
	List	   *result;
	MemoryContext oldcxt;

	/* Quick exit if we already computed the list. */
	if (relation->rd_indexvalid != 0)
		return listCopy(relation->rd_indexlist);

	/*
	 * We build the list we intend to return (in the caller's context)
	 * while doing the scan.  After successfully completing the scan, we
	 * copy that list into the relcache entry.	This avoids cache-context
	 * memory leakage if we get some sort of error partway through.
	 */
	result = NIL;

	/* Prepare to scan pg_index for entries having indrelid = this rel. */
	ScanKeyEntryInitialize(&skey,
						   (bits16) 0x0,
						   (AttrNumber) Anum_pg_index_indrelid,
						   (RegProcedure) F_OIDEQ,
						   ObjectIdGetDatum(RelationGetRelid(relation)));

	indrel = heap_openr(IndexRelationName, AccessShareLock);
	indscan = systable_beginscan(indrel, IndexIndrelidIndex, true,
								 SnapshotNow,
								 1, &skey);

	while (HeapTupleIsValid(htup = systable_getnext(indscan)))
	{
		Form_pg_index index = (Form_pg_index) GETSTRUCT(htup);

		result = insert_ordered_oid(result, index->indexrelid);
	}

	systable_endscan(indscan);
	heap_close(indrel, AccessShareLock);

	/* Now save a copy of the completed list in the relcache entry. */
	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);
	relation->rd_indexlist = listCopy(result);
	relation->rd_indexvalid = 1;
	MemoryContextSwitchTo(oldcxt);

	return result;
}

/*
 * insert_ordered_oid
 *		Insert a new Oid into a sorted list of Oids, preserving ordering
 *
 * Building the ordered list this way is O(N^2), but with a pretty small
 * constant, so for the number of entries we expect it will probably be
 * faster than trying to apply qsort().  Most tables don't have very many
 * indexes...
 */
static List *
insert_ordered_oid(List *list, Oid datum)
{
	List	   *l;

	/* Does the datum belong at the front? */
	if (list == NIL || datum < lfirsto(list))
		return lconso(datum, list);
	/* No, so find the entry it belongs after */
	l = list;
	for (;;)
	{
		List	   *n = lnext(l);

		if (n == NIL || datum < lfirsto(n))
			break;				/* it belongs before n */
		l = n;
	}
	/* Insert datum into list after item l */
	lnext(l) = lconso(datum, lnext(l));
	return list;
}

/*
 * RelationSetIndexList -- externally force the index list contents
 *
 * This is used to temporarily override what we think the set of valid
 * indexes is.  The forcing will be valid only until transaction commit
 * or abort.
 *
 * This should only be applied to nailed relations, because in a non-nailed
 * relation the hacked index list could be lost at any time due to SI
 * messages.  In practice it is only used on pg_class (see REINDEX).
 *
 * It is up to the caller to make sure the given list is correctly ordered.
 */
void
RelationSetIndexList(Relation relation, List *indexIds)
{
	MemoryContext oldcxt;

	Assert(relation->rd_isnailed == 1);
	/* Copy the list into the cache context (could fail for lack of mem) */
	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);
	indexIds = listCopy(indexIds);
	MemoryContextSwitchTo(oldcxt);
	/* Okay to replace old list */
	freeList(relation->rd_indexlist);
	relation->rd_indexlist = indexIds;
	relation->rd_indexvalid = 2;		/* mark list as forced */
}

/*
 * RelationGetIndexExpressions -- get the index expressions for an index
 *
 * We cache the result of transforming pg_index.indexprs into a node tree.
 * If the rel is not an index or has no expressional columns, we return NIL.
 * Otherwise, the returned tree is copied into the caller's memory context.
 * (We don't want to return a pointer to the relcache copy, since it could
 * disappear due to relcache invalidation.)
 */
List *
RelationGetIndexExpressions(Relation relation)
{
	List	   *result;
	Datum		exprsDatum;
	bool		isnull;
	char	   *exprsString;
	MemoryContext oldcxt;

	/* Quick exit if we already computed the result. */
	if (relation->rd_indexprs)
		return (List *) copyObject(relation->rd_indexprs);

	/* Quick exit if there is nothing to do. */
	if (relation->rd_indextuple == NULL ||
		heap_attisnull(relation->rd_indextuple, Anum_pg_index_indexprs))
		return NIL;

	/*
	 * We build the tree we intend to return in the caller's context.
	 * After successfully completing the work, we copy it into the
	 * relcache entry.	This avoids problems if we get some sort of error
	 * partway through.
	 *
	 * We make use of the syscache's copy of pg_index's tupledesc to access
	 * the non-fixed fields of the tuple.  We assume that the syscache
	 * will be initialized before any access of a partial index could
	 * occur.  (This would probably fail if we were to allow partial
	 * indexes on system catalogs.)
	 */
	exprsDatum = SysCacheGetAttr(INDEXRELID, relation->rd_indextuple,
								 Anum_pg_index_indexprs, &isnull);
	Assert(!isnull);
	exprsString = DatumGetCString(DirectFunctionCall1(textout, exprsDatum));
	result = (List *) stringToNode(exprsString);
	pfree(exprsString);

	/*
	 * Run the expressions through eval_const_expressions.	This is not
	 * just an optimization, but is necessary, because the planner will be
	 * comparing them to const-folded qual clauses, and may fail to detect
	 * valid matches without this.
	 */
	result = (List *) eval_const_expressions((Node *) result);

	/* May as well fix opfuncids too */
	fix_opfuncids((Node *) result);

	/* Now save a copy of the completed tree in the relcache entry. */
	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);
	relation->rd_indexprs = (List *) copyObject(result);
	MemoryContextSwitchTo(oldcxt);

	return result;
}

/*
 * RelationGetIndexPredicate -- get the index predicate for an index
 *
 * We cache the result of transforming pg_index.indpred into a node tree.
 * If the rel is not an index or has no predicate, we return NIL.
 * Otherwise, the returned tree is copied into the caller's memory context.
 * (We don't want to return a pointer to the relcache copy, since it could
 * disappear due to relcache invalidation.)
 */
List *
RelationGetIndexPredicate(Relation relation)
{
	List	   *result;
	Datum		predDatum;
	bool		isnull;
	char	   *predString;
	MemoryContext oldcxt;

	/* Quick exit if we already computed the result. */
	if (relation->rd_indpred)
		return (List *) copyObject(relation->rd_indpred);

	/* Quick exit if there is nothing to do. */
	if (relation->rd_indextuple == NULL ||
		heap_attisnull(relation->rd_indextuple, Anum_pg_index_indpred))
		return NIL;

	/*
	 * We build the tree we intend to return in the caller's context.
	 * After successfully completing the work, we copy it into the
	 * relcache entry.	This avoids problems if we get some sort of error
	 * partway through.
	 *
	 * We make use of the syscache's copy of pg_index's tupledesc to access
	 * the non-fixed fields of the tuple.  We assume that the syscache
	 * will be initialized before any access of a partial index could
	 * occur.  (This would probably fail if we were to allow partial
	 * indexes on system catalogs.)
	 */
	predDatum = SysCacheGetAttr(INDEXRELID, relation->rd_indextuple,
								Anum_pg_index_indpred, &isnull);
	Assert(!isnull);
	predString = DatumGetCString(DirectFunctionCall1(textout, predDatum));
	result = (List *) stringToNode(predString);
	pfree(predString);

	/*
	 * Run the expression through eval_const_expressions.  This is not
	 * just an optimization, but is necessary, because the planner will be
	 * comparing it to const-folded qual clauses, and may fail to detect
	 * valid matches without this.
	 */
	result = (List *) eval_const_expressions((Node *) result);

	/* May as well fix opfuncids too */
	fix_opfuncids((Node *) result);

	/* Now save a copy of the completed tree in the relcache entry. */
	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);
	relation->rd_indpred = (List *) copyObject(result);
	MemoryContextSwitchTo(oldcxt);

	return result;
}


/*
 *	load_relcache_init_file, write_relcache_init_file
 *
 *		In late 1992, we started regularly having databases with more than
 *		a thousand classes in them.  With this number of classes, it became
 *		critical to do indexed lookups on the system catalogs.
 *
 *		Bootstrapping these lookups is very hard.  We want to be able to
 *		use an index on pg_attribute, for example, but in order to do so,
 *		we must have read pg_attribute for the attributes in the index,
 *		which implies that we need to use the index.
 *
 *		In order to get around the problem, we do the following:
 *
 *		   +  When the database system is initialized (at initdb time), we
 *			  don't use indexes.  We do sequential scans.
 *
 *		   +  When the backend is started up in normal mode, we load an image
 *			  of the appropriate relation descriptors, in internal format,
 *			  from an initialization file in the data/base/... directory.
 *
 *		   +  If the initialization file isn't there, then we create the
 *			  relation descriptors using sequential scans and write 'em to
 *			  the initialization file for use by subsequent backends.
 *
 *		We could dispense with the initialization file and just build the
 *		critical reldescs the hard way on every backend startup, but that
 *		slows down backend startup noticeably.
 *
 *		We can in fact go further, and save more relcache entries than
 *		just the ones that are absolutely critical; this allows us to speed
 *		up backend startup by not having to build such entries the hard way.
 *		Presently, all the catalog and index entries that are referred to
 *		by catcaches are stored in the initialization file.
 *
 *		The same mechanism that detects when catcache and relcache entries
 *		need to be invalidated (due to catalog updates) also arranges to
 *		unlink the initialization file when its contents may be out of date.
 *		The file will then be rebuilt during the next backend startup.
 */

/*
 * load_relcache_init_file -- attempt to load cache from the init file
 *
 * If successful, return TRUE and set criticalRelcachesBuilt to true.
 * If not successful, return FALSE and set needNewCacheFile to true.
 *
 * NOTE: we assume we are already switched into CacheMemoryContext.
 */
static bool
load_relcache_init_file(void)
{
	FILE	   *fp;
	char		initfilename[MAXPGPATH];
	Relation   *rels;
	int			relno,
				num_rels,
				max_rels,
				nailed_rels,
				nailed_indexes;
	int			i;

	snprintf(initfilename, sizeof(initfilename), "%s/%s",
			 DatabasePath, RELCACHE_INIT_FILENAME);

	fp = AllocateFile(initfilename, PG_BINARY_R);
	if (fp == NULL)
	{
		needNewCacheFile = true;
		return false;
	}

	/*
	 * Read the index relcache entries from the file.  Note we will not
	 * enter any of them into the cache if the read fails partway through;
	 * this helps to guard against broken init files.
	 */
	max_rels = 100;
	rels = (Relation *) palloc(max_rels * sizeof(Relation));
	num_rels = 0;
	nailed_rels = nailed_indexes = 0;
	initFileRelationIds = NIL;

	for (relno = 0;; relno++)
	{
		Size		len;
		size_t		nread;
		Relation	rel;
		Form_pg_class relform;
		bool		has_not_null;

		/* first read the relation descriptor length */
		if ((nread = fread(&len, 1, sizeof(len), fp)) != sizeof(len))
		{
			if (nread == 0)
				break;			/* end of file */
			goto read_failed;
		}

		/* safety check for incompatible relcache layout */
		if (len != sizeof(RelationData))
			goto read_failed;

		/* allocate another relcache header */
		if (num_rels >= max_rels)
		{
			max_rels *= 2;
			rels = (Relation *) repalloc(rels, max_rels * sizeof(Relation));
		}

		rel = rels[num_rels++] = (Relation) palloc(len);

		/* then, read the Relation structure */
		if ((nread = fread(rel, 1, len, fp)) != len)
			goto read_failed;

		/* next read the relation tuple form */
		if ((nread = fread(&len, 1, sizeof(len), fp)) != sizeof(len))
			goto read_failed;

		relform = (Form_pg_class) palloc(len);
		if ((nread = fread(relform, 1, len, fp)) != len)
			goto read_failed;

		rel->rd_rel = relform;

		/* initialize attribute tuple forms */
		rel->rd_att = CreateTemplateTupleDesc(relform->relnatts,
											  relform->relhasoids);

		/* next read all the attribute tuple form data entries */
		has_not_null = false;
		for (i = 0; i < relform->relnatts; i++)
		{
			if ((nread = fread(&len, 1, sizeof(len), fp)) != sizeof(len))
				goto read_failed;

			rel->rd_att->attrs[i] = (Form_pg_attribute) palloc(len);

			if ((nread = fread(rel->rd_att->attrs[i], 1, len, fp)) != len)
				goto read_failed;

			has_not_null |= rel->rd_att->attrs[i]->attnotnull;
		}

		/* mark not-null status */
		if (has_not_null)
		{
			TupleConstr *constr = (TupleConstr *) palloc0(sizeof(TupleConstr));

			constr->has_not_null = true;
			rel->rd_att->constr = constr;
		}

		/* If it's an index, there's more to do */
		if (rel->rd_rel->relkind == RELKIND_INDEX)
		{
			Form_pg_am	am;
			MemoryContext indexcxt;
			IndexStrategy strat;
			Oid		   *operator;
			RegProcedure *support;
			int			nstrategies,
						nsupport;

			/* Count nailed indexes to ensure we have 'em all */
			if (rel->rd_isnailed)
				nailed_indexes++;

			/* next, read the pg_index tuple */
			if ((nread = fread(&len, 1, sizeof(len), fp)) != sizeof(len))
				goto read_failed;

			rel->rd_indextuple = (HeapTuple) palloc(len);
			if ((nread = fread(rel->rd_indextuple, 1, len, fp)) != len)
				goto read_failed;

			/* Fix up internal pointers in the tuple -- see heap_copytuple */
			rel->rd_indextuple->t_datamcxt = CurrentMemoryContext;
			rel->rd_indextuple->t_data = (HeapTupleHeader) ((char *) rel->rd_indextuple + HEAPTUPLESIZE);
			rel->rd_index = (Form_pg_index) GETSTRUCT(rel->rd_indextuple);

			/* next, read the access method tuple form */
			if ((nread = fread(&len, 1, sizeof(len), fp)) != sizeof(len))
				goto read_failed;

			am = (Form_pg_am) palloc(len);
			if ((nread = fread(am, 1, len, fp)) != len)
				goto read_failed;
			rel->rd_am = am;

			/*
			 * prepare index info context --- parameters should match
			 * RelationInitIndexAccessInfo
			 */
			indexcxt = AllocSetContextCreate(CacheMemoryContext,
											 RelationGetRelationName(rel),
											 ALLOCSET_SMALL_MINSIZE,
											 ALLOCSET_SMALL_INITSIZE,
											 ALLOCSET_SMALL_MAXSIZE);
			rel->rd_indexcxt = indexcxt;

			/* next, read the index strategy map */
			if ((nread = fread(&len, 1, sizeof(len), fp)) != sizeof(len))
				goto read_failed;

			strat = (IndexStrategy) MemoryContextAlloc(indexcxt, len);
			if ((nread = fread(strat, 1, len, fp)) != len)
				goto read_failed;

			/* have to invalidate any FmgrInfo data in the strategy maps */
			nstrategies = am->amstrategies * relform->relnatts;
			for (i = 0; i < nstrategies; i++)
				strat->strategyMapData[i].entry[0].sk_func.fn_oid = InvalidOid;

			rel->rd_istrat = strat;

			/* next, read the vector of operator OIDs */
			if ((nread = fread(&len, 1, sizeof(len), fp)) != sizeof(len))
				goto read_failed;

			operator = (Oid *) MemoryContextAlloc(indexcxt, len);
			if ((nread = fread(operator, 1, len, fp)) != len)
				goto read_failed;

			rel->rd_operator = operator;

			/* finally, read the vector of support procedures */
			if ((nread = fread(&len, 1, sizeof(len), fp)) != sizeof(len))
				goto read_failed;
			support = (RegProcedure *) MemoryContextAlloc(indexcxt, len);
			if ((nread = fread(support, 1, len, fp)) != len)
				goto read_failed;

			rel->rd_support = support;

			/* add a zeroed support-fmgr-info vector */
			nsupport = relform->relnatts * am->amsupport;
			rel->rd_supportinfo = (FmgrInfo *)
				MemoryContextAlloc(indexcxt, nsupport * sizeof(FmgrInfo));
			MemSet(rel->rd_supportinfo, 0, nsupport * sizeof(FmgrInfo));
		}
		else
		{
			/* Count nailed rels to ensure we have 'em all */
			if (rel->rd_isnailed)
				nailed_rels++;

			Assert(rel->rd_index == NULL);
			Assert(rel->rd_indextuple == NULL);
			Assert(rel->rd_am == NULL);
			Assert(rel->rd_indexcxt == NULL);
			Assert(rel->rd_istrat == NULL);
			Assert(rel->rd_operator == NULL);
			Assert(rel->rd_support == NULL);
			Assert(rel->rd_supportinfo == NULL);
		}

		/*
		 * Rules and triggers are not saved (mainly because the internal
		 * format is complex and subject to change).  They must be rebuilt
		 * if needed by RelationCacheInitializePhase2.	This is not
		 * expected to be a big performance hit since few system catalogs
		 * have such.  Ditto for index expressions and predicates.
		 */
		rel->rd_rules = NULL;
		rel->rd_rulescxt = NULL;
		rel->trigdesc = NULL;
		rel->rd_indexprs = NIL;
		rel->rd_indpred = NIL;

		/*
		 * Reset transient-state fields in the relcache entry
		 */
		rel->rd_fd = -1;
		rel->rd_targblock = InvalidBlockNumber;
		if (rel->rd_isnailed)
			RelationSetReferenceCount(rel, 1);
		else
			RelationSetReferenceCount(rel, 0);
		rel->rd_indexvalid = 0;
		rel->rd_indexlist = NIL;
		MemSet(&rel->pgstat_info, 0, sizeof(rel->pgstat_info));

		/*
		 * Make sure database ID is correct.  This is needed in case the
		 * pg_internal.init file was copied from some other database by
		 * CREATE DATABASE.
		 */
		if (rel->rd_rel->relisshared)
			rel->rd_node.tblNode = InvalidOid;
		else
			rel->rd_node.tblNode = MyDatabaseId;

		RelationInitLockInfo(rel);
	}

	/*
	 * We reached the end of the init file without apparent problem. Did
	 * we get the right number of nailed items?  (This is a useful
	 * crosscheck in case the set of critical rels or indexes changes.)
	 */
	if (nailed_rels != NUM_CRITICAL_RELS ||
		nailed_indexes != NUM_CRITICAL_INDEXES)
		goto read_failed;

	/*
	 * OK, all appears well.
	 *
	 * Now insert all the new relcache entries into the cache.
	 */
	for (relno = 0; relno < num_rels; relno++)
	{
		RelationCacheInsert(rels[relno]);
		/* also make a list of their OIDs, for RelationIdIsInInitFile */
		initFileRelationIds = lconso(RelationGetRelid(rels[relno]),
									 initFileRelationIds);
	}

	pfree(rels);
	FreeFile(fp);

	criticalRelcachesBuilt = true;
	return true;

	/*
	 * init file is broken, so do it the hard way.	We don't bother trying
	 * to free the clutter we just allocated; it's not in the relcache so
	 * it won't hurt.
	 */
read_failed:
	pfree(rels);
	FreeFile(fp);

	needNewCacheFile = true;
	return false;
}

/*
 * Write out a new initialization file with the current contents
 * of the relcache.
 */
static void
write_relcache_init_file(void)
{
	FILE	   *fp;
	char		tempfilename[MAXPGPATH];
	char		finalfilename[MAXPGPATH];
	HASH_SEQ_STATUS status;
	RelIdCacheEnt *idhentry;
	MemoryContext oldcxt;
	int			i;

	/*
	 * We must write a temporary file and rename it into place. Otherwise,
	 * another backend starting at about the same time might crash trying
	 * to read the partially-complete file.
	 */
	snprintf(tempfilename, sizeof(tempfilename), "%s/%s.%d",
			 DatabasePath, RELCACHE_INIT_FILENAME, MyProcPid);
	snprintf(finalfilename, sizeof(finalfilename), "%s/%s",
			 DatabasePath, RELCACHE_INIT_FILENAME);

	unlink(tempfilename);		/* in case it exists w/wrong permissions */

	fp = AllocateFile(tempfilename, PG_BINARY_W);
	if (fp == NULL)
	{
		/*
		 * We used to consider this a fatal error, but we might as well
		 * continue with backend startup ...
		 */
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not create relation-cache initialization file \"%s\": %m",
						tempfilename),
		  errdetail("Continuing anyway, but there's something wrong.")));
		return;
	}

	/*
	 * Write all the reldescs (in no particular order).
	 */
	hash_seq_init(&status, RelationIdCache);

	initFileRelationIds = NIL;

	while ((idhentry = (RelIdCacheEnt *) hash_seq_search(&status)) != NULL)
	{
		Relation	rel = idhentry->reldesc;
		Form_pg_class relform = rel->rd_rel;
		Size		len;

		/*
		 * first write the relcache entry proper
		 */
		len = sizeof(RelationData);

		/* first, write the relation descriptor length */
		if (fwrite(&len, 1, sizeof(len), fp) != sizeof(len))
			elog(FATAL, "could not write init file");

		/* next, write out the Relation structure */
		if (fwrite(rel, 1, len, fp) != len)
			elog(FATAL, "could not write init file");

		/* next write the relation tuple form */
		len = sizeof(FormData_pg_class);
		if (fwrite(&len, 1, sizeof(len), fp) != sizeof(len))
			elog(FATAL, "could not write init file");

		if (fwrite(relform, 1, len, fp) != len)
			elog(FATAL, "could not write init file");

		/* next, do all the attribute tuple form data entries */
		for (i = 0; i < relform->relnatts; i++)
		{
			len = ATTRIBUTE_TUPLE_SIZE;
			if (fwrite(&len, 1, sizeof(len), fp) != sizeof(len))
				elog(FATAL, "could not write init file");
			if (fwrite(rel->rd_att->attrs[i], 1, len, fp) != len)
				elog(FATAL, "could not write init file");
		}

		/* If it's an index, there's more to do */
		if (rel->rd_rel->relkind == RELKIND_INDEX)
		{
			Form_pg_am	am = rel->rd_am;

			/* write the pg_index tuple */
			/* we assume this was created by heap_copytuple! */
			len = HEAPTUPLESIZE + rel->rd_indextuple->t_len;
			if (fwrite(&len, 1, sizeof(len), fp) != sizeof(len))
				elog(FATAL, "could not write init file");

			if (fwrite(rel->rd_indextuple, 1, len, fp) != len)
				elog(FATAL, "could not write init file");

			/* next, write the access method tuple form */
			len = sizeof(FormData_pg_am);
			if (fwrite(&len, 1, sizeof(len), fp) != sizeof(len))
				elog(FATAL, "could not write init file");

			if (fwrite(am, 1, len, fp) != len)
				elog(FATAL, "could not write init file");

			/* next, write the index strategy map */
			len = AttributeNumberGetIndexStrategySize(relform->relnatts,
													  am->amstrategies);
			if (fwrite(&len, 1, sizeof(len), fp) != sizeof(len))
				elog(FATAL, "could not write init file");

			if (fwrite(rel->rd_istrat, 1, len, fp) != len)
				elog(FATAL, "could not write init file");

			/* next, write the vector of operator OIDs */
			len = relform->relnatts * (am->amstrategies * sizeof(Oid));
			if (fwrite(&len, 1, sizeof(len), fp) != sizeof(len))
				elog(FATAL, "could not write init file");

			if (fwrite(rel->rd_operator, 1, len, fp) != len)
				elog(FATAL, "could not write init file");

			/* finally, write the vector of support procedures */
			len = relform->relnatts * (am->amsupport * sizeof(RegProcedure));
			if (fwrite(&len, 1, sizeof(len), fp) != sizeof(len))
				elog(FATAL, "could not write init file");

			if (fwrite(rel->rd_support, 1, len, fp) != len)
				elog(FATAL, "could not write init file");
		}

		/* also make a list of their OIDs, for RelationIdIsInInitFile */
		oldcxt = MemoryContextSwitchTo(CacheMemoryContext);
		initFileRelationIds = lconso(RelationGetRelid(rel),
									 initFileRelationIds);
		MemoryContextSwitchTo(oldcxt);
	}

	FreeFile(fp);

	/*
	 * Now we have to check whether the data we've so painstakingly
	 * accumulated is already obsolete due to someone else's
	 * just-committed catalog changes.	If so, we just delete the temp
	 * file and leave it to the next backend to try again.	(Our own
	 * relcache entries will be updated by SI message processing, but we
	 * can't be sure whether what we wrote out was up-to-date.)
	 *
	 * This mustn't run concurrently with RelationCacheInitFileInvalidate, so
	 * grab a serialization lock for the duration.
	 */
	LWLockAcquire(RelCacheInitLock, LW_EXCLUSIVE);

	/* Make sure we have seen all incoming SI messages */
	AcceptInvalidationMessages();

	/*
	 * If we have received any SI relcache invals since backend start,
	 * assume we may have written out-of-date data.
	 */
	if (relcacheInvalsReceived == 0L)
	{
		/*
		 * OK, rename the temp file to its final name, deleting any
		 * previously-existing init file.
		 *
		 * Note: a failure here is possible under Cygwin, if some other
		 * backend is holding open an unlinked-but-not-yet-gone init file.
		 * So treat this as a noncritical failure.
		 */
		if (rename(tempfilename, finalfilename) < 0)
		{
			ereport(WARNING,
					(errcode_for_file_access(),
				errmsg("could not rename relation-cache initialization file \"%s\" to \"%s\": %m",
					   tempfilename, finalfilename),
					 errdetail("Continuing anyway, but there's something wrong.")));

			/*
			 * If we fail, try to clean up the useless temp file; don't
			 * bother to complain if this fails too.
			 */
			unlink(tempfilename);
		}
	}
	else
	{
		/* Delete the already-obsolete temp file */
		unlink(tempfilename);
	}

	LWLockRelease(RelCacheInitLock);
}

/*
 * Detect whether a given relation (identified by OID) is one of the ones
 * we store in the init file.
 *
 * Note that we effectively assume that all backends running in a database
 * would choose to store the same set of relations in the init file;
 * otherwise there are cases where we'd fail to detect the need for an init
 * file invalidation.  This does not seem likely to be a problem in practice.
 */
bool
RelationIdIsInInitFile(Oid relationId)
{
	return oidMember(relationId, initFileRelationIds);
}

/*
 * Invalidate (remove) the init file during commit of a transaction that
 * changed one or more of the relation cache entries that are kept in the
 * init file.
 *
 * We actually need to remove the init file twice: once just before sending
 * the SI messages that include relcache inval for such relations, and once
 * just after sending them.  The unlink before ensures that a backend that's
 * currently starting cannot read the now-obsolete init file and then miss
 * the SI messages that will force it to update its relcache entries.  (This
 * works because the backend startup sequence gets into the PROC array before
 * trying to load the init file.)  The unlink after is to synchronize with a
 * backend that may currently be trying to write an init file based on data
 * that we've just rendered invalid.  Such a backend will see the SI messages,
 * but we can't leave the init file sitting around to fool later backends.
 *
 * Ignore any failure to unlink the file, since it might not be there if
 * no backend has been started since the last removal.
 */
void
RelationCacheInitFileInvalidate(bool beforeSend)
{
	char		initfilename[MAXPGPATH];

	snprintf(initfilename, sizeof(initfilename), "%s/%s",
			 DatabasePath, RELCACHE_INIT_FILENAME);

	if (beforeSend)
	{
		/* no interlock needed here */
		unlink(initfilename);
	}
	else
	{
		/*
		 * We need to interlock this against write_relcache_init_file, to
		 * guard against possibility that someone renames a new-but-
		 * already-obsolete init file into place just after we unlink.
		 * With the interlock, it's certain that write_relcache_init_file
		 * will notice our SI inval message before renaming into place, or
		 * else that we will execute second and successfully unlink the
		 * file.
		 */
		LWLockAcquire(RelCacheInitLock, LW_EXCLUSIVE);
		unlink(initfilename);
		LWLockRelease(RelCacheInitLock);
	}
}

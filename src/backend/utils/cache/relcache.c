/*-------------------------------------------------------------------------
 *
 * relcache.c
 *	  POSTGRES relation descriptor cache code
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/cache/relcache.c,v 1.250.2.2 2008/04/16 18:23:19 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *		RelationCacheInitialize			- initialize relcache (to empty)
 *		RelationCacheInitializePhase2	- finish initializing relcache
 *		RelationIdGetRelation			- get a reldesc by relation id
 *		RelationClose					- close an open relation
 *
 * NOTES
 *		The following code contains many undocumented hacks.  Please be
 *		careful....
 */
#include "postgres.h"

#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "access/reloptions.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_attrdef.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_rewrite.h"
#include "catalog/pg_type.h"
#include "commands/trigger.h"
#include "miscadmin.h"
#include "optimizer/clauses.h"
#include "optimizer/planmain.h"
#include "optimizer/prep.h"
#include "rewrite/rewriteDefine.h"
#include "storage/fd.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/relcache.h"
#include "utils/resowner.h"
#include "utils/syscache.h"
#include "utils/typcache.h"


/*
 * name of relcache init file, used to speed up backend startup
 */
#define RELCACHE_INIT_FILENAME	"pg_internal.init"

#define RELCACHE_INIT_FILEMAGIC		0x573263	/* version ID value */

/*
 *		hardcoded tuple descriptors.  see include/catalog/pg_attribute.h
 */
static FormData_pg_attribute Desc_pg_class[Natts_pg_class] = {Schema_pg_class};
static FormData_pg_attribute Desc_pg_attribute[Natts_pg_attribute] = {Schema_pg_attribute};
static FormData_pg_attribute Desc_pg_proc[Natts_pg_proc] = {Schema_pg_proc};
static FormData_pg_attribute Desc_pg_type[Natts_pg_type] = {Schema_pg_type};
static FormData_pg_attribute Desc_pg_index[Natts_pg_index] = {Schema_pg_index};

/*
 *		Hash tables that index the relation cache
 *
 *		We used to index the cache by both name and OID, but now there
 *		is only an index by OID.
 */
typedef struct relidcacheent
{
	Oid			reloid;
	Relation	reldesc;
} RelIdCacheEnt;

static HTAB *RelationIdCache;

/*
 * This flag is false until we have prepared the critical relcache entries
 * that are needed to do indexscans on the tables read by relcache building.
 */
bool		criticalRelcachesBuilt = false;

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
 * This flag lets us optimize away work in AtEO(Sub)Xact_RelationCache().
 */
static bool need_eoxact_work = false;


/*
 *		macros to manipulate the lookup hashtables
 */
#define RelationCacheInsert(RELATION)	\
do { \
	RelIdCacheEnt *idhentry; bool found; \
	idhentry = (RelIdCacheEnt*)hash_search(RelationIdCache, \
										   (void *) &(RELATION->rd_id), \
										   HASH_ENTER, \
										   &found); \
	/* used to give notice if found -- now just keep quiet */ \
	idhentry->reldesc = RELATION; \
} while(0)

#define RelationIdCacheLookup(ID, RELATION) \
do { \
	RelIdCacheEnt *hentry; \
	hentry = (RelIdCacheEnt*)hash_search(RelationIdCache, \
										 (void *) &(ID), HASH_FIND,NULL); \
	if (hentry) \
		RELATION = hentry->reldesc; \
	else \
		RELATION = NULL; \
} while(0)

#define RelationCacheDelete(RELATION) \
do { \
	RelIdCacheEnt *idhentry; \
	idhentry = (RelIdCacheEnt*)hash_search(RelationIdCache, \
										   (void *) &(RELATION->rd_id), \
										   HASH_REMOVE, NULL); \
	if (idhentry == NULL) \
		elog(WARNING, "trying to delete a rd_id reldesc that does not exist"); \
} while(0)


/*
 * Special cache for opclass-related information
 *
 * Note: only default-subtype operators and support procs get cached
 */
typedef struct opclasscacheent
{
	Oid			opclassoid;		/* lookup key: OID of opclass */
	bool		valid;			/* set TRUE after successful fill-in */
	StrategyNumber numStrats;	/* max # of strategies (from pg_am) */
	StrategyNumber numSupport;	/* max # of support procs (from pg_am) */
	Oid		   *operatorOids;	/* strategy operators' OIDs */
	RegProcedure *supportProcs; /* support procs */
} OpClassCacheEnt;

static HTAB *OpClassCache = NULL;


/* non-export function prototypes */

static void RelationClearRelation(Relation relation, bool rebuild);

static void RelationReloadClassinfo(Relation relation);
static void RelationFlushRelation(Relation relation);
static bool load_relcache_init_file(void);
static void write_relcache_init_file(void);
static void write_item(const void *data, Size len, FILE *fp);

static void formrdesc(const char *relationName, Oid relationReltype,
		  bool hasoids, int natts, FormData_pg_attribute *att);

static HeapTuple ScanPgRelation(Oid targetRelId, bool indexOK);
static Relation AllocateRelationDesc(Relation relation, Form_pg_class relp);
static void RelationParseRelOptions(Relation relation, HeapTuple tuple);
static void RelationBuildTupleDesc(Relation relation);
static Relation RelationBuildDesc(Oid targetRelId, Relation oldrelation);
static void RelationInitPhysicalAddr(Relation relation);
static TupleDesc GetPgClassDescriptor(void);
static TupleDesc GetPgIndexDescriptor(void);
static void AttrDefaultFetch(Relation relation);
static void CheckConstraintFetch(Relation relation);
static List *insert_ordered_oid(List *list, Oid datum);
static void IndexSupportInitialize(oidvector *indclass,
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
 *		This is used by RelationBuildDesc to find a pg_class
 *		tuple matching targetRelId.  The caller must hold at least
 *		AccessShareLock on the target relid to prevent concurrent-update
 *		scenarios --- else our SnapshotNow scan might fail to find any
 *		version that it thinks is live.
 *
 *		NB: the returned tuple has been copied into palloc'd storage
 *		and must eventually be freed with heap_freetuple.
 */
static HeapTuple
ScanPgRelation(Oid targetRelId, bool indexOK)
{
	HeapTuple	pg_class_tuple;
	Relation	pg_class_desc;
	SysScanDesc pg_class_scan;
	ScanKeyData key[1];

	/*
	 * form a scan key
	 */
	ScanKeyInit(&key[0],
				ObjectIdAttributeNumber,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(targetRelId));

	/*
	 * Open pg_class and fetch a tuple.  Force heap scan if we haven't yet
	 * built the critical relcache entries (this includes initdb and startup
	 * without a pg_internal.init file).  The caller can also force a heap
	 * scan by setting indexOK == false.
	 */
	pg_class_desc = heap_open(RelationRelationId, AccessShareLock);
	pg_class_scan = systable_beginscan(pg_class_desc, ClassOidIndexId,
									   indexOK && criticalRelcachesBuilt,
									   SnapshotNow,
									   1, key);

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
	MemSet(relation, 0, sizeof(RelationData));
	relation->rd_targblock = InvalidBlockNumber;

	/* make sure relation is marked as having no open file yet */
	relation->rd_smgr = NULL;

	/*
	 * Copy the relation tuple form
	 *
	 * We only allocate space for the fixed fields, ie, CLASS_TUPLE_SIZE. The
	 * variable-length fields (relacl, reloptions) are NOT stored in the
	 * relcache --- there'd be little point in it, since we don't copy the
	 * tuple's nulls bitmap and hence wouldn't know if the values are valid.
	 * Bottom line is that relacl *cannot* be retrieved from the relcache. Get
	 * it from the syscache if you need it.  The same goes for the original
	 * form of reloptions (however, we do store the parsed form of reloptions
	 * in rd_options).
	 */
	relationForm = (Form_pg_class) palloc(CLASS_TUPLE_SIZE);

	memcpy(relationForm, relp, CLASS_TUPLE_SIZE);

	/* initialize relation tuple form */
	relation->rd_rel = relationForm;

	/* and allocate attribute tuple form storage */
	relation->rd_att = CreateTemplateTupleDesc(relationForm->relnatts,
											   relationForm->relhasoids);
	/* which we mark as a reference-counted tupdesc */
	relation->rd_att->tdrefcount = 1;

	MemoryContextSwitchTo(oldcxt);

	return relation;
}

/*
 * RelationParseRelOptions
 *		Convert pg_class.reloptions into pre-parsed rd_options
 *
 * tuple is the real pg_class tuple (not rd_rel!) for relation
 *
 * Note: rd_rel and (if an index) rd_am must be valid already
 */
static void
RelationParseRelOptions(Relation relation, HeapTuple tuple)
{
	Datum		datum;
	bool		isnull;
	bytea	   *options;

	relation->rd_options = NULL;

	/* Fall out if relkind should not have options */
	switch (relation->rd_rel->relkind)
	{
		case RELKIND_RELATION:
		case RELKIND_TOASTVALUE:
		case RELKIND_UNCATALOGED:
		case RELKIND_INDEX:
			break;
		default:
			return;
	}

	/*
	 * Fetch reloptions from tuple; have to use a hardwired descriptor because
	 * we might not have any other for pg_class yet (consider executing this
	 * code for pg_class itself)
	 */
	datum = fastgetattr(tuple,
						Anum_pg_class_reloptions,
						GetPgClassDescriptor(),
						&isnull);
	if (isnull)
		return;

	/* Parse into appropriate format; don't error out here */
	switch (relation->rd_rel->relkind)
	{
		case RELKIND_RELATION:
		case RELKIND_TOASTVALUE:
		case RELKIND_UNCATALOGED:
			options = heap_reloptions(relation->rd_rel->relkind, datum,
									  false);
			break;
		case RELKIND_INDEX:
			options = index_reloptions(relation->rd_am->amoptions, datum,
									   false);
			break;
		default:
			Assert(false);		/* can't get here */
			options = NULL;		/* keep compiler quiet */
			break;
	}

	/* Copy parsed data into CacheMemoryContext */
	if (options)
	{
		relation->rd_options = MemoryContextAlloc(CacheMemoryContext,
												  VARSIZE(options));
		memcpy(relation->rd_options, options, VARSIZE(options));
	}
}

/*
 *		RelationBuildTupleDesc
 *
 *		Form the relation's tuple descriptor from information in
 *		the pg_attribute, pg_attrdef & pg_constraint system catalogs.
 */
static void
RelationBuildTupleDesc(Relation relation)
{
	HeapTuple	pg_attribute_tuple;
	Relation	pg_attribute_desc;
	SysScanDesc pg_attribute_scan;
	ScanKeyData skey[2];
	int			need;
	TupleConstr *constr;
	AttrDefault *attrdef = NULL;
	int			ndef = 0;

	/* copy some fields from pg_class row to rd_att */
	relation->rd_att->tdtypeid = relation->rd_rel->reltype;
	relation->rd_att->tdtypmod = -1;	/* unnecessary, but... */
	relation->rd_att->tdhasoid = relation->rd_rel->relhasoids;

	constr = (TupleConstr *) MemoryContextAlloc(CacheMemoryContext,
												sizeof(TupleConstr));
	constr->has_not_null = false;

	/*
	 * Form a scan key that selects only user attributes (attnum > 0).
	 * (Eliminating system attribute rows at the index level is lots faster
	 * than fetching them.)
	 */
	ScanKeyInit(&skey[0],
				Anum_pg_attribute_attrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(relation)));
	ScanKeyInit(&skey[1],
				Anum_pg_attribute_attnum,
				BTGreaterStrategyNumber, F_INT2GT,
				Int16GetDatum(0));

	/*
	 * Open pg_attribute and begin a scan.	Force heap scan if we haven't yet
	 * built the critical relcache entries (this includes initdb and startup
	 * without a pg_internal.init file).
	 */
	pg_attribute_desc = heap_open(AttributeRelationId, AccessShareLock);
	pg_attribute_scan = systable_beginscan(pg_attribute_desc,
										   AttributeRelidNumIndexId,
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

		memcpy(relation->rd_att->attrs[attp->attnum - 1],
			   attp,
			   ATTRIBUTE_TUPLE_SIZE);

		/* Update constraint/default info */
		if (attp->attnotnull)
			constr->has_not_null = true;

		if (attp->atthasdef)
		{
			if (attrdef == NULL)
				attrdef = (AttrDefault *)
					MemoryContextAllocZero(CacheMemoryContext,
										   relation->rd_rel->relnatts *
										   sizeof(AttrDefault));
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
	 * attribute: it must be zero.	This eliminates the need for special cases
	 * for attnum=1 that used to exist in fastgetattr() and index_getattr().
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
				MemoryContextAllocZero(CacheMemoryContext,
									constr->num_check * sizeof(ConstrCheck));
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
	 * Make the private context.  Parameters are set on the assumption that
	 * it'll probably not contain much data.
	 */
	rulescxt = AllocSetContextCreate(CacheMemoryContext,
									 RelationGetRelationName(relation),
									 ALLOCSET_SMALL_MINSIZE,
									 ALLOCSET_SMALL_INITSIZE,
									 ALLOCSET_SMALL_MAXSIZE);
	relation->rd_rulescxt = rulescxt;

	/*
	 * allocate an array to hold the rewrite rules (the array is extended if
	 * necessary)
	 */
	maxlocks = 4;
	rules = (RewriteRule **)
		MemoryContextAlloc(rulescxt, sizeof(RewriteRule *) * maxlocks);
	numlocks = 0;

	/*
	 * form a scan key
	 */
	ScanKeyInit(&key,
				Anum_pg_rewrite_ev_class,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(relation)));

	/*
	 * open pg_rewrite and begin a scan
	 *
	 * Note: since we scan the rules using RewriteRelRulenameIndexId, we will
	 * be reading the rules in name order, except possibly during
	 * emergency-recovery operations (ie, IgnoreSystemIndexes). This in turn
	 * ensures that rules will be fired in name order.
	 */
	rewrite_desc = heap_open(RewriteRelationId, AccessShareLock);
	rewrite_tupdesc = RelationGetDescr(rewrite_desc);
	rewrite_scan = systable_beginscan(rewrite_desc,
									  RewriteRelRulenameIndexId,
									  true, SnapshotNow,
									  1, &key);

	while (HeapTupleIsValid(rewrite_tuple = systable_getnext(rewrite_scan)))
	{
		Form_pg_rewrite rewrite_form = (Form_pg_rewrite) GETSTRUCT(rewrite_tuple);
		bool		isnull;
		Datum		rule_datum;
		text	   *rule_text;
		char	   *rule_str;
		RewriteRule *rule;

		rule = (RewriteRule *) MemoryContextAlloc(rulescxt,
												  sizeof(RewriteRule));

		rule->ruleId = HeapTupleGetOid(rewrite_tuple);

		rule->event = rewrite_form->ev_type - '0';
		rule->attrno = rewrite_form->ev_attr;
		rule->isInstead = rewrite_form->is_instead;

		/*
		 * Must use heap_getattr to fetch ev_action and ev_qual.  Also, the
		 * rule strings are often large enough to be toasted.  To avoid
		 * leaking memory in the caller's context, do the detoasting here so
		 * we can free the detoasted version.
		 */
		rule_datum = heap_getattr(rewrite_tuple,
								  Anum_pg_rewrite_ev_action,
								  rewrite_tupdesc,
								  &isnull);
		Assert(!isnull);
		rule_text = DatumGetTextP(rule_datum);
		rule_str = DatumGetCString(DirectFunctionCall1(textout,
												PointerGetDatum(rule_text)));
		oldcxt = MemoryContextSwitchTo(rulescxt);
		rule->actions = (List *) stringToNode(rule_str);
		MemoryContextSwitchTo(oldcxt);
		pfree(rule_str);
		if ((Pointer) rule_text != DatumGetPointer(rule_datum))
			pfree(rule_text);

		rule_datum = heap_getattr(rewrite_tuple,
								  Anum_pg_rewrite_ev_qual,
								  rewrite_tupdesc,
								  &isnull);
		Assert(!isnull);
		rule_text = DatumGetTextP(rule_datum);
		rule_str = DatumGetCString(DirectFunctionCall1(textout,
												PointerGetDatum(rule_text)));
		oldcxt = MemoryContextSwitchTo(rulescxt);
		rule->qual = (Node *) stringToNode(rule_str);
		MemoryContextSwitchTo(oldcxt);
		pfree(rule_str);
		if ((Pointer) rule_text != DatumGetPointer(rule_datum))
			pfree(rule_text);

		/*
		 * We want the rule's table references to be checked as though by the
		 * table owner, not the user referencing the rule.	Therefore, scan
		 * through the rule's actions and set the checkAsUser field on all
		 * rtable entries.	We have to look at the qual as well, in case it
		 * contains sublinks.
		 *
		 * The reason for doing this when the rule is loaded, rather than when
		 * it is stored, is that otherwise ALTER TABLE OWNER would have to
		 * grovel through stored rules to update checkAsUser fields. Scanning
		 * the rule tree during load is relatively cheap (compared to
		 * constructing it in the first place), so we do it here.
		 */
		setRuleCheckAsUser((Node *) rule->actions, relation->rd_rel->relowner);
		setRuleCheckAsUser(rule->qual, relation->rd_rel->relowner);

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
	 * RelationBuildRuleLock should read 'em in a consistent order.  So just
	 * compare corresponding slots.
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


/*
 *		RelationBuildDesc
 *
 *		Build a relation descriptor --- either a new one, or by
 *		recycling the given old relation object.  The latter case
 *		supports rebuilding a relcache entry without invalidating
 *		pointers to it.  The caller must hold at least
 *		AccessShareLock on the target relid.
 *
 *		Returns NULL if no pg_class row could be found for the given relid
 *		(suggesting we are trying to access a just-deleted relation).
 *		Any other error is reported via elog.
 */
static Relation
RelationBuildDesc(Oid targetRelId, Relation oldrelation)
{
	Relation	relation;
	Oid			relid;
	HeapTuple	pg_class_tuple;
	Form_pg_class relp;
	MemoryContext oldcxt;

	/*
	 * find the tuple in pg_class corresponding to the given relation id
	 */
	pg_class_tuple = ScanPgRelation(targetRelId, true);

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
	 * allocate storage for the relation descriptor, and copy pg_class_tuple
	 * to relation->rd_rel.
	 */
	relation = AllocateRelationDesc(oldrelation, relp);

	/*
	 * initialize the relation's relation id (relation->rd_id)
	 */
	RelationGetRelid(relation) = relid;

	/*
	 * normal relations are not nailed into the cache; nor can a pre-existing
	 * relation be new.  It could be temp though.  (Actually, it could be new
	 * too, but it's okay to forget that fact if forced to flush the entry.)
	 */
	relation->rd_refcnt = 0;
	relation->rd_isnailed = false;
	relation->rd_createSubid = InvalidSubTransactionId;
	relation->rd_istemp = isTempNamespace(relation->rd_rel->relnamespace);

	/*
	 * initialize the tuple descriptor (relation->rd_att).
	 */
	RelationBuildTupleDesc(relation);

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

	/* extract reloptions if any */
	RelationParseRelOptions(relation, pg_class_tuple);

	/*
	 * initialize the relation lock manager information
	 */
	RelationInitLockInfo(relation);		/* see lmgr.c */

	/*
	 * initialize physical addressing information for the relation
	 */
	RelationInitPhysicalAddr(relation);

	/* make sure relation is marked as having no open file yet */
	relation->rd_smgr = NULL;

	/*
	 * now we can free the memory allocated for pg_class_tuple
	 */
	heap_freetuple(pg_class_tuple);

	/*
	 * Insert newly created relation into relcache hash tables.
	 */
	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);
	RelationCacheInsert(relation);
	MemoryContextSwitchTo(oldcxt);

	/* It's fully valid */
	relation->rd_isvalid = true;

	return relation;
}

/*
 * Initialize the physical addressing info (RelFileNode) for a relcache entry
 */
static void
RelationInitPhysicalAddr(Relation relation)
{
	if (relation->rd_rel->reltablespace)
		relation->rd_node.spcNode = relation->rd_rel->reltablespace;
	else
		relation->rd_node.spcNode = MyDatabaseTableSpace;
	if (relation->rd_rel->relisshared)
		relation->rd_node.dbNode = InvalidOid;
	else
		relation->rd_node.dbNode = MyDatabaseId;
	relation->rd_node.relNode = relation->rd_rel->relfilenode;
}

/*
 * Initialize index-access-method support data for an index relation
 */
void
RelationInitIndexAccessInfo(Relation relation)
{
	HeapTuple	tuple;
	Form_pg_am	aform;
	Datum		indclassDatum;
	bool		isnull;
	MemoryContext indexcxt;
	MemoryContext oldcontext;
	Oid		   *operator;
	RegProcedure *support;
	FmgrInfo   *supportinfo;
	int			natts;
	uint16		amstrategies;
	uint16		amsupport;

	/*
	 * Make a copy of the pg_index entry for the index.  Since pg_index
	 * contains variable-length and possibly-null fields, we have to do this
	 * honestly rather than just treating it as a Form_pg_index struct.
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
	 * indclass cannot be referenced directly through the C struct, because it
	 * is after the variable-width indkey field.  Therefore we extract the
	 * datum the hard way and provide a direct link in the relcache.
	 */
	indclassDatum = fastgetattr(relation->rd_indextuple,
								Anum_pg_index_indclass,
								GetPgIndexDescriptor(),
								&isnull);
	Assert(!isnull);
	relation->rd_indclass = (oidvector *) DatumGetPointer(indclassDatum);

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
	 * Make the private context to hold index access info.	The reason we need
	 * a context, and not just a couple of pallocs, is so that we won't leak
	 * any subsidiary info attached to fmgr lookup records.
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
	relation->rd_aminfo = (RelationAmInfo *)
		MemoryContextAllocZero(indexcxt, sizeof(RelationAmInfo));

	if (amstrategies > 0)
		operator = (Oid *)
			MemoryContextAllocZero(indexcxt,
								   natts * amstrategies * sizeof(Oid));
	else
		operator = NULL;

	if (amsupport > 0)
	{
		int			nsupport = natts * amsupport;

		support = (RegProcedure *)
			MemoryContextAllocZero(indexcxt, nsupport * sizeof(RegProcedure));
		supportinfo = (FmgrInfo *)
			MemoryContextAllocZero(indexcxt, nsupport * sizeof(FmgrInfo));
	}
	else
	{
		support = NULL;
		supportinfo = NULL;
	}

	relation->rd_operator = operator;
	relation->rd_support = support;
	relation->rd_supportinfo = supportinfo;

	/*
	 * Fill the operator and support procedure OID arrays.	(aminfo and
	 * supportinfo are left as zeroes, and are filled on-the-fly when used)
	 */
	IndexSupportInitialize(relation->rd_indclass,
						   operator, support,
						   amstrategies, amsupport, natts);

	/*
	 * expressions and predicate cache will be filled later
	 */
	relation->rd_indexprs = NIL;
	relation->rd_indpred = NIL;
	relation->rd_amcache = NULL;
}

/*
 * IndexSupportInitialize
 *		Initializes an index's cached opclass information,
 *		given the index's pg_index.indclass entry.
 *
 * Data is returned into *indexOperator and *indexSupport, which are arrays
 * allocated by the caller.
 *
 * The caller also passes maxStrategyNumber, maxSupportNumber, and
 * maxAttributeNumber, since these indicate the size of the arrays
 * it has allocated --- but in practice these numbers must always match
 * those obtainable from the system catalog entries for the index and
 * access method.
 */
static void
IndexSupportInitialize(oidvector *indclass,
					   Oid *indexOperator,
					   RegProcedure *indexSupport,
					   StrategyNumber maxStrategyNumber,
					   StrategyNumber maxSupportNumber,
					   AttrNumber maxAttributeNumber)
{
	int			attIndex;

	for (attIndex = 0; attIndex < maxAttributeNumber; attIndex++)
	{
		OpClassCacheEnt *opcentry;

		if (!OidIsValid(indclass->values[attIndex]))
			elog(ERROR, "bogus pg_index tuple");

		/* look up the info for this opclass, using a cache */
		opcentry = LookupOpclassInfo(indclass->values[attIndex],
									 maxStrategyNumber,
									 maxSupportNumber);

		/* copy cached data into relcache entry */
		if (maxStrategyNumber > 0)
			memcpy(&indexOperator[attIndex * maxStrategyNumber],
				   opcentry->operatorOids,
				   maxStrategyNumber * sizeof(Oid));
		if (maxSupportNumber > 0)
			memcpy(&indexSupport[attIndex * maxSupportNumber],
				   opcentry->supportProcs,
				   maxSupportNumber * sizeof(RegProcedure));
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
	Relation	rel;
	SysScanDesc scan;
	ScanKeyData skey[2];
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
		ctl.hash = oid_hash;
		OpClassCache = hash_create("Operator class cache", 64,
								   &ctl, HASH_ELEM | HASH_FUNCTION);
	}

	opcentry = (OpClassCacheEnt *) hash_search(OpClassCache,
											   (void *) &operatorClassOid,
											   HASH_ENTER, &found);

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
		opcentry->operatorOids = (Oid *)
			MemoryContextAllocZero(CacheMemoryContext,
								   numStrats * sizeof(Oid));
	else
		opcentry->operatorOids = NULL;

	if (numSupport > 0)
		opcentry->supportProcs = (RegProcedure *)
			MemoryContextAllocZero(CacheMemoryContext,
								   numSupport * sizeof(RegProcedure));
	else
		opcentry->supportProcs = NULL;

	/*
	 * To avoid infinite recursion during startup, force heap scans if we're
	 * looking up info for the opclasses used by the indexes we would like to
	 * reference here.
	 */
	indexOK = criticalRelcachesBuilt ||
		(operatorClassOid != OID_BTREE_OPS_OID &&
		 operatorClassOid != INT2_BTREE_OPS_OID);

	/*
	 * Scan pg_amop to obtain operators for the opclass.  We only fetch the
	 * default ones (those with subtype zero).
	 */
	if (numStrats > 0)
	{
		ScanKeyInit(&skey[0],
					Anum_pg_amop_amopclaid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(operatorClassOid));
		ScanKeyInit(&skey[1],
					Anum_pg_amop_amopsubtype,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(InvalidOid));
		rel = heap_open(AccessMethodOperatorRelationId, AccessShareLock);
		scan = systable_beginscan(rel, AccessMethodStrategyIndexId, indexOK,
								  SnapshotNow, 2, skey);

		while (HeapTupleIsValid(htup = systable_getnext(scan)))
		{
			Form_pg_amop amopform = (Form_pg_amop) GETSTRUCT(htup);

			if (amopform->amopstrategy <= 0 ||
				(StrategyNumber) amopform->amopstrategy > numStrats)
				elog(ERROR, "invalid amopstrategy number %d for opclass %u",
					 amopform->amopstrategy, operatorClassOid);
			opcentry->operatorOids[amopform->amopstrategy - 1] =
				amopform->amopopr;
		}

		systable_endscan(scan);
		heap_close(rel, AccessShareLock);
	}

	/*
	 * Scan pg_amproc to obtain support procs for the opclass.	We only fetch
	 * the default ones (those with subtype zero).
	 */
	if (numSupport > 0)
	{
		ScanKeyInit(&skey[0],
					Anum_pg_amproc_amopclaid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(operatorClassOid));
		ScanKeyInit(&skey[1],
					Anum_pg_amproc_amprocsubtype,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(InvalidOid));
		rel = heap_open(AccessMethodProcedureRelationId, AccessShareLock);
		scan = systable_beginscan(rel, AccessMethodProcedureIndexId, indexOK,
								  SnapshotNow, 2, skey);

		while (HeapTupleIsValid(htup = systable_getnext(scan)))
		{
			Form_pg_amproc amprocform = (Form_pg_amproc) GETSTRUCT(htup);

			if (amprocform->amprocnum <= 0 ||
				(StrategyNumber) amprocform->amprocnum > numSupport)
				elog(ERROR, "invalid amproc number %d for opclass %u",
					 amprocform->amprocnum, operatorClassOid);

			opcentry->supportProcs[amprocform->amprocnum - 1] =
				amprocform->amproc;
		}

		systable_endscan(scan);
		heap_close(rel, AccessShareLock);
	}

	opcentry->valid = true;
	return opcentry;
}


/*
 *		formrdesc
 *
 *		This is a special cut-down version of RelationBuildDesc()
 *		used by RelationCacheInitializePhase2() in initializing the relcache.
 *		The relation descriptor is built just from the supplied parameters,
 *		without actually looking at any system table entries.  We cheat
 *		quite a lot since we only need to work for a few basic system
 *		catalogs.
 *
 * formrdesc is currently used for: pg_class, pg_attribute, pg_proc,
 * and pg_type (see RelationCacheInitializePhase2).
 *
 * Note that these catalogs can't have constraints (except attnotnull),
 * default values, rules, or triggers, since we don't cope with any of that.
 *
 * NOTE: we assume we are already switched into CacheMemoryContext.
 */
static void
formrdesc(const char *relationName, Oid relationReltype,
		  bool hasoids, int natts, FormData_pg_attribute *att)
{
	Relation	relation;
	int			i;
	bool		has_not_null;

	/*
	 * allocate new relation desc, clear all fields of reldesc
	 */
	relation = (Relation) palloc0(sizeof(RelationData));
	relation->rd_targblock = InvalidBlockNumber;

	/* make sure relation is marked as having no open file yet */
	relation->rd_smgr = NULL;

	/*
	 * initialize reference count: 1 because it is nailed in cache
	 */
	relation->rd_refcnt = 1;

	/*
	 * all entries built with this routine are nailed-in-cache; none are for
	 * new or temp relations.
	 */
	relation->rd_isnailed = true;
	relation->rd_createSubid = InvalidSubTransactionId;
	relation->rd_istemp = false;

	/*
	 * initialize relation tuple form
	 *
	 * The data we insert here is pretty incomplete/bogus, but it'll serve to
	 * get us launched.  RelationCacheInitializePhase2() will read the real
	 * data from pg_class and replace what we've done here.
	 */
	relation->rd_rel = (Form_pg_class) palloc0(CLASS_TUPLE_SIZE);

	namestrcpy(&relation->rd_rel->relname, relationName);
	relation->rd_rel->relnamespace = PG_CATALOG_NAMESPACE;
	relation->rd_rel->reltype = relationReltype;

	/*
	 * It's important to distinguish between shared and non-shared relations,
	 * even at bootstrap time, to make sure we know where they are stored.	At
	 * present, all relations that formrdesc is used for are not shared.
	 */
	relation->rd_rel->relisshared = false;

	relation->rd_rel->relpages = 1;
	relation->rd_rel->reltuples = 1;
	relation->rd_rel->relkind = RELKIND_RELATION;
	relation->rd_rel->relhasoids = hasoids;
	relation->rd_rel->relnatts = (int16) natts;

	/*
	 * initialize attribute tuple form
	 *
	 * Unlike the case with the relation tuple, this data had better be right
	 * because it will never be replaced.  The input values must be correctly
	 * defined by macros in src/include/catalog/ headers.
	 */
	relation->rd_att = CreateTemplateTupleDesc(natts, hasoids);
	relation->rd_att->tdrefcount = 1;	/* mark as refcounted */

	relation->rd_att->tdtypeid = relationReltype;
	relation->rd_att->tdtypmod = -1;	/* unnecessary, but... */

	/*
	 * initialize tuple desc info
	 */
	has_not_null = false;
	for (i = 0; i < natts; i++)
	{
		memcpy(relation->rd_att->attrs[i],
			   &att[i],
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
	relation->rd_rel->relfilenode = RelationGetRelid(relation);

	/*
	 * initialize the relation lock manager information
	 */
	RelationInitLockInfo(relation);		/* see lmgr.c */

	/*
	 * initialize physical addressing information for the relation
	 */
	RelationInitPhysicalAddr(relation);

	/*
	 * initialize the rel-has-index flag, using hardwired knowledge
	 */
	if (IsBootstrapProcessingMode())
	{
		/* In bootstrap mode, we have no indexes */
		relation->rd_rel->relhasindex = false;
	}
	else
	{
		/* Otherwise, all the rels formrdesc is used for have indexes */
		relation->rd_rel->relhasindex = true;
	}

	/*
	 * add new reldesc to relcache
	 */
	RelationCacheInsert(relation);

	/* It's fully valid */
	relation->rd_isvalid = true;
}


/* ----------------------------------------------------------------
 *				 Relation Descriptor Lookup Interface
 * ----------------------------------------------------------------
 */

/*
 *		RelationIdGetRelation
 *
 *		Lookup a reldesc by OID; make one if not already in cache.
 *
 *		Returns NULL if no pg_class row could be found for the given relid
 *		(suggesting we are trying to access a just-deleted relation).
 *		Any other error is reported via elog.
 *
 *		NB: caller should already have at least AccessShareLock on the
 *		relation ID, else there are nasty race conditions.
 *
 *		NB: relation ref count is incremented, or set to 1 if new entry.
 *		Caller should eventually decrement count.  (Usually,
 *		that happens by calling RelationClose().)
 */
Relation
RelationIdGetRelation(Oid relationId)
{
	Relation	rd;

	/*
	 * first try to find reldesc in the cache
	 */
	RelationIdCacheLookup(relationId, rd);

	if (RelationIsValid(rd))
	{
		RelationIncrementReferenceCount(rd);
		/* revalidate nailed index if necessary */
		if (!rd->rd_isvalid)
			RelationReloadClassinfo(rd);
		return rd;
	}

	/*
	 * no reldesc in the cache, so have RelationBuildDesc() build one and add
	 * it.
	 */
	rd = RelationBuildDesc(relationId, NULL);
	if (RelationIsValid(rd))
		RelationIncrementReferenceCount(rd);
	return rd;
}

/* ----------------------------------------------------------------
 *				cache invalidation support routines
 * ----------------------------------------------------------------
 */

/*
 * RelationIncrementReferenceCount
 *		Increments relation reference count.
 *
 * Note: bootstrap mode has its own weird ideas about relation refcount
 * behavior; we ought to fix it someday, but for now, just disable
 * reference count ownership tracking in bootstrap mode.
 */
void
RelationIncrementReferenceCount(Relation rel)
{
	ResourceOwnerEnlargeRelationRefs(CurrentResourceOwner);
	rel->rd_refcnt += 1;
	if (!IsBootstrapProcessingMode())
		ResourceOwnerRememberRelationRef(CurrentResourceOwner, rel);
}

/*
 * RelationDecrementReferenceCount
 *		Decrements relation reference count.
 */
void
RelationDecrementReferenceCount(Relation rel)
{
	Assert(rel->rd_refcnt > 0);
	rel->rd_refcnt -= 1;
	if (!IsBootstrapProcessingMode())
		ResourceOwnerForgetRelationRef(CurrentResourceOwner, rel);
}

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
		relation->rd_createSubid == InvalidSubTransactionId)
		RelationClearRelation(relation, false);
#endif
}

/*
 * RelationReloadClassinfo - reload the pg_class row (only)
 *
 *	This function is used only for indexes.  We currently allow only the
 *	pg_class row of an existing index to change (to support changes of
 *	owner, tablespace, or relfilenode), not its pg_index row or other
 *	subsidiary index schema information.  Therefore it's sufficient to do
 *	this when we get an SI invalidation.  Furthermore, there are cases
 *	where it's necessary not to throw away the index information, especially
 *	for "nailed" indexes which we are unable to rebuild on-the-fly.
 *
 *	We can't necessarily reread the pg_class row right away; we might be
 *	in a failed transaction when we receive the SI notification.  If so,
 *	RelationClearRelation just marks the entry as invalid by setting
 *	rd_isvalid to false.  This routine is called to fix the entry when it
 *	is next needed.
 *
 *	We assume that at the time we are called, we have at least AccessShareLock
 *	on the target index.  (Note: in the calls from RelationClearRelation,
 *	this is legitimate because we know the rel has positive refcount.)
 */
static void
RelationReloadClassinfo(Relation relation)
{
	bool		indexOK;
	HeapTuple	pg_class_tuple;
	Form_pg_class relp;

	/* Should be called only for invalidated indexes */
	Assert(relation->rd_rel->relkind == RELKIND_INDEX &&
		   !relation->rd_isvalid);
	/* Should be closed at smgr level */
	Assert(relation->rd_smgr == NULL);

	/*
	 * Read the pg_class row
	 *
	 * Don't try to use an indexscan of pg_class_oid_index to reload the info
	 * for pg_class_oid_index ...
	 */
	indexOK = (RelationGetRelid(relation) != ClassOidIndexId);
	pg_class_tuple = ScanPgRelation(RelationGetRelid(relation), indexOK);
	if (!HeapTupleIsValid(pg_class_tuple))
		elog(ERROR, "could not find pg_class tuple for index %u",
			 RelationGetRelid(relation));
	relp = (Form_pg_class) GETSTRUCT(pg_class_tuple);
	memcpy(relation->rd_rel, relp, CLASS_TUPLE_SIZE);
	/* Reload reloptions in case they changed */
	if (relation->rd_options)
		pfree(relation->rd_options);
	RelationParseRelOptions(relation, pg_class_tuple);
	/* done with pg_class tuple */
	heap_freetuple(pg_class_tuple);
	/* We must recalculate physical address in case it changed */
	RelationInitPhysicalAddr(relation);
	/* Make sure targblock is reset in case rel was truncated */
	relation->rd_targblock = InvalidBlockNumber;
	/* Must free any AM cached data, too */
	if (relation->rd_amcache)
		pfree(relation->rd_amcache);
	relation->rd_amcache = NULL;
	/* Okay, now it's valid again */
	relation->rd_isvalid = true;
}

/*
 * RelationClearRelation
 *
 *	 Physically blow away a relation cache entry, or reset it and rebuild
 *	 it from scratch (that is, from catalog entries).  The latter path is
 *	 usually used when we are notified of a change to an open relation
 *	 (one with refcount > 0).  However, this routine just does whichever
 *	 it's told to do; callers must determine which they want.
 *
 *	 NB: when rebuilding, we'd better hold some lock on the relation.
 *	 In current usages this is presumed true because it has refcnt > 0.
 */
static void
RelationClearRelation(Relation relation, bool rebuild)
{
	Oid			old_reltype = relation->rd_rel->reltype;
	MemoryContext oldcxt;

	/*
	 * Make sure smgr and lower levels close the relation's files, if they
	 * weren't closed already.  If the relation is not getting deleted, the
	 * next smgr access should reopen the files automatically.	This ensures
	 * that the low-level file access state is updated after, say, a vacuum
	 * truncation.
	 */
	RelationCloseSmgr(relation);

	/*
	 * Never, never ever blow away a nailed-in system relation, because we'd
	 * be unable to recover.  However, we must reset rd_targblock, in case we
	 * got called because of a relation cache flush that was triggered by
	 * VACUUM.
	 *
	 * If it's a nailed index, then we need to re-read the pg_class row to see
	 * if its relfilenode changed.	We can't necessarily do that here, because
	 * we might be in a failed transaction.  We assume it's okay to do it if
	 * there are open references to the relcache entry (cf notes for
	 * AtEOXact_RelationCache).  Otherwise just mark the entry as possibly
	 * invalid, and it'll be fixed when next opened.
	 */
	if (relation->rd_isnailed)
	{
		relation->rd_targblock = InvalidBlockNumber;
		if (relation->rd_rel->relkind == RELKIND_INDEX)
		{
			relation->rd_isvalid = false;		/* needs to be revalidated */
			if (relation->rd_refcnt > 1)
				RelationReloadClassinfo(relation);
		}
		return;
	}

	/*
	 * Even non-system indexes should not be blown away if they are open and
	 * have valid index support information.  This avoids problems with active
	 * use of the index support information.  As with nailed indexes, we
	 * re-read the pg_class row to handle possible physical relocation of the
	 * index.
	 */
	if (relation->rd_rel->relkind == RELKIND_INDEX &&
		relation->rd_refcnt > 0 &&
		relation->rd_indexcxt != NULL)
	{
		relation->rd_isvalid = false;	/* needs to be revalidated */
		RelationReloadClassinfo(relation);
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
	 * manager might also have pointers into the rewrite rules. So to begin
	 * with, we can only get rid of these fields:
	 */
	FreeTriggerDesc(relation->trigdesc);
	if (relation->rd_indextuple)
		pfree(relation->rd_indextuple);
	if (relation->rd_am)
		pfree(relation->rd_am);
	if (relation->rd_rel)
		pfree(relation->rd_rel);
	if (relation->rd_options)
		pfree(relation->rd_options);
	list_free(relation->rd_indexlist);
	if (relation->rd_indexcxt)
		MemoryContextDelete(relation->rd_indexcxt);

	/*
	 * If we're really done with the relcache entry, blow it away. But if
	 * someone is still using it, reconstruct the whole deal without moving
	 * the physical RelationData record (so that the someone's pointer is
	 * still valid).
	 */
	if (!rebuild)
	{
		/* ok to zap remaining substructure */
		flush_rowtype_cache(old_reltype);
		/* can't use DecrTupleDescRefCount here */
		Assert(relation->rd_att->tdrefcount > 0);
		if (--relation->rd_att->tdrefcount == 0)
			FreeTupleDesc(relation->rd_att);
		if (relation->rd_rulescxt)
			MemoryContextDelete(relation->rd_rulescxt);
		pfree(relation);
	}
	else
	{
		/*
		 * When rebuilding an open relcache entry, must preserve ref count and
		 * rd_createSubid state.  Also attempt to preserve the tupledesc and
		 * rewrite-rule substructures in place.  (Note: the refcount mechanism
		 * for tupledescs may eventually ensure that we don't really need to
		 * preserve the tupledesc in-place, but for now there are still a lot
		 * of places that assume an open rel's tupledesc won't move.)
		 *
		 * Note that this process does not touch CurrentResourceOwner; which
		 * is good because whatever ref counts the entry may have do not
		 * necessarily belong to that resource owner.
		 */
		Oid			save_relid = RelationGetRelid(relation);
		int			old_refcnt = relation->rd_refcnt;
		SubTransactionId old_createSubid = relation->rd_createSubid;
		TupleDesc	old_att = relation->rd_att;
		RuleLock   *old_rules = relation->rd_rules;
		MemoryContext old_rulescxt = relation->rd_rulescxt;

		if (RelationBuildDesc(save_relid, relation) != relation)
		{
			/* Should only get here if relation was deleted */
			flush_rowtype_cache(old_reltype);
			Assert(old_att->tdrefcount > 0);
			if (--old_att->tdrefcount == 0)
				FreeTupleDesc(old_att);
			if (old_rulescxt)
				MemoryContextDelete(old_rulescxt);
			pfree(relation);
			elog(ERROR, "relation %u deleted while still in use", save_relid);
		}
		relation->rd_refcnt = old_refcnt;
		relation->rd_createSubid = old_createSubid;
		if (equalTupleDescs(old_att, relation->rd_att))
		{
			/* needn't flush typcache here */
			Assert(relation->rd_att->tdrefcount == 1);
			if (--relation->rd_att->tdrefcount == 0)
				FreeTupleDesc(relation->rd_att);
			relation->rd_att = old_att;
		}
		else
		{
			flush_rowtype_cache(old_reltype);
			Assert(old_att->tdrefcount > 0);
			if (--old_att->tdrefcount == 0)
				FreeTupleDesc(old_att);
		}
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

	if (relation->rd_createSubid != InvalidSubTransactionId)
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
 *		RelationCacheInvalidateEntry
 *
 *		This routine is invoked for SI cache flush messages.
 *
 * Any relcache entry matching the relid must be flushed.  (Note: caller has
 * already determined that the relid belongs to our database or is a shared
 * relation.)
 *
 * We used to skip local relations, on the grounds that they could
 * not be targets of cross-backend SI update messages; but it seems
 * safer to process them, so that our *own* SI update messages will
 * have the same effects during CommandCounterIncrement for both
 * local and nonlocal relations.
 */
void
RelationCacheInvalidateEntry(Oid relationId)
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
 *	 and rebuild those with positive reference counts.	Also reset the smgr
 *	 relation cache.
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
	ListCell   *l;

	/* Phase 1 */
	hash_seq_init(&status, RelationIdCache);

	while ((idhentry = (RelIdCacheEnt *) hash_seq_search(&status)) != NULL)
	{
		relation = idhentry->reldesc;

		/* Must close all smgr references to avoid leaving dangling ptrs */
		RelationCloseSmgr(relation);

		/* Ignore new relations, since they are never SI targets */
		if (relation->rd_createSubid != InvalidSubTransactionId)
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
			 * pg_class_oid_index goes on the front of rebuildFirstList, other
			 * nailed indexes on the back, and everything else into
			 * rebuildList (in no particular order).
			 */
			if (relation->rd_isnailed &&
				relation->rd_rel->relkind == RELKIND_INDEX)
			{
				if (RelationGetRelid(relation) == ClassOidIndexId)
					rebuildFirstList = lcons(relation, rebuildFirstList);
				else
					rebuildFirstList = lappend(rebuildFirstList, relation);
			}
			else
				rebuildList = lcons(relation, rebuildList);
		}
	}

	/*
	 * Now zap any remaining smgr cache entries.  This must happen before we
	 * start to rebuild entries, since that may involve catalog fetches which
	 * will re-open catalog files.
	 */
	smgrcloseall();

	/* Phase 2: rebuild the items found to need rebuild in phase 1 */
	foreach(l, rebuildFirstList)
	{
		relation = (Relation) lfirst(l);
		RelationClearRelation(relation, true);
	}
	list_free(rebuildFirstList);
	foreach(l, rebuildList)
	{
		relation = (Relation) lfirst(l);
		RelationClearRelation(relation, true);
	}
	list_free(rebuildList);
}

/*
 * AtEOXact_RelationCache
 *
 *	Clean up the relcache at main-transaction commit or abort.
 *
 * Note: this must be called *before* processing invalidation messages.
 * In the case of abort, we don't want to try to rebuild any invalidated
 * cache entries (since we can't safely do database accesses).  Therefore
 * we must reset refcnts before handling pending invalidations.
 *
 * As of PostgreSQL 8.1, relcache refcnts should get released by the
 * ResourceOwner mechanism.  This routine just does a debugging
 * cross-check that no pins remain.  However, we also need to do special
 * cleanup when the current transaction created any relations or made use
 * of forced index lists.
 */
void
AtEOXact_RelationCache(bool isCommit)
{
	HASH_SEQ_STATUS status;
	RelIdCacheEnt *idhentry;

	/*
	 * To speed up transaction exit, we want to avoid scanning the relcache
	 * unless there is actually something for this routine to do.  Other than
	 * the debug-only Assert checks, most transactions don't create any work
	 * for us to do here, so we keep a static flag that gets set if there is
	 * anything to do.	(Currently, this means either a relation is created in
	 * the current xact, or an index list is forced.)  For simplicity, the
	 * flag remains set till end of top-level transaction, even though we
	 * could clear it at subtransaction end in some cases.
	 */
	if (!need_eoxact_work
#ifdef USE_ASSERT_CHECKING
		&& !assert_enabled
#endif
		)
		return;

	hash_seq_init(&status, RelationIdCache);

	while ((idhentry = (RelIdCacheEnt *) hash_seq_search(&status)) != NULL)
	{
		Relation	relation = idhentry->reldesc;

		/*
		 * The relcache entry's ref count should be back to its normal
		 * not-in-a-transaction state: 0 unless it's nailed in cache.
		 *
		 * In bootstrap mode, this is NOT true, so don't check it --- the
		 * bootstrap code expects relations to stay open across start/commit
		 * transaction calls.  (That seems bogus, but it's not worth fixing.)
		 */
#ifdef USE_ASSERT_CHECKING
		if (!IsBootstrapProcessingMode())
		{
			int			expected_refcnt;

			expected_refcnt = relation->rd_isnailed ? 1 : 0;
			Assert(relation->rd_refcnt == expected_refcnt);
		}
#endif

		/*
		 * Is it a relation created in the current transaction?
		 *
		 * During commit, reset the flag to zero, since we are now out of the
		 * creating transaction.  During abort, simply delete the relcache
		 * entry --- it isn't interesting any longer.  (NOTE: if we have
		 * forgotten the new-ness of a new relation due to a forced cache
		 * flush, the entry will get deleted anyway by shared-cache-inval
		 * processing of the aborted pg_class insertion.)
		 */
		if (relation->rd_createSubid != InvalidSubTransactionId)
		{
			if (isCommit)
				relation->rd_createSubid = InvalidSubTransactionId;
			else
			{
				RelationClearRelation(relation, false);
				continue;
			}
		}

		/*
		 * Flush any temporary index list.
		 */
		if (relation->rd_indexvalid == 2)
		{
			list_free(relation->rd_indexlist);
			relation->rd_indexlist = NIL;
			relation->rd_oidindex = InvalidOid;
			relation->rd_indexvalid = 0;
		}
	}

	/* Once done with the transaction, we can reset need_eoxact_work */
	need_eoxact_work = false;
}

/*
 * AtEOSubXact_RelationCache
 *
 *	Clean up the relcache at sub-transaction commit or abort.
 *
 * Note: this must be called *before* processing invalidation messages.
 */
void
AtEOSubXact_RelationCache(bool isCommit, SubTransactionId mySubid,
						  SubTransactionId parentSubid)
{
	HASH_SEQ_STATUS status;
	RelIdCacheEnt *idhentry;

	/*
	 * Skip the relcache scan if nothing to do --- see notes for
	 * AtEOXact_RelationCache.
	 */
	if (!need_eoxact_work)
		return;

	hash_seq_init(&status, RelationIdCache);

	while ((idhentry = (RelIdCacheEnt *) hash_seq_search(&status)) != NULL)
	{
		Relation	relation = idhentry->reldesc;

		/*
		 * Is it a relation created in the current subtransaction?
		 *
		 * During subcommit, mark it as belonging to the parent, instead.
		 * During subabort, simply delete the relcache entry.
		 */
		if (relation->rd_createSubid == mySubid)
		{
			if (isCommit)
				relation->rd_createSubid = parentSubid;
			else
			{
				Assert(RelationHasReferenceCountZero(relation));
				RelationClearRelation(relation, false);
				continue;
			}
		}

		/*
		 * Flush any temporary index list.
		 */
		if (relation->rd_indexvalid == 2)
		{
			list_free(relation->rd_indexlist);
			relation->rd_indexlist = NIL;
			relation->rd_oidindex = InvalidOid;
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
						   Oid relid,
						   Oid reltablespace,
						   bool shared_relation)
{
	Relation	rel;
	MemoryContext oldcxt;
	int			natts = tupDesc->natts;
	int			i;
	bool		has_not_null;
	bool		nailit;

	AssertArg(natts >= 0);

	/*
	 * check for creation of a rel that must be nailed in cache.
	 *
	 * XXX this list had better match RelationCacheInitializePhase2's list.
	 */
	switch (relid)
	{
		case RelationRelationId:
		case AttributeRelationId:
		case ProcedureRelationId:
		case TypeRelationId:
			nailit = true;
			break;
		default:
			nailit = false;
			break;
	}

	/*
	 * check that hardwired list of shared rels matches what's in the
	 * bootstrap .bki file.  If you get a failure here during initdb, you
	 * probably need to fix IsSharedRelation() to match whatever you've done
	 * to the set of shared relations.
	 */
	if (shared_relation != IsSharedRelation(relid))
		elog(ERROR, "shared_relation flag for \"%s\" does not match IsSharedRelation(%u)",
			 relname, relid);

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
	rel->rd_smgr = NULL;

	/* mark it nailed if appropriate */
	rel->rd_isnailed = nailit;

	rel->rd_refcnt = nailit ? 1 : 0;

	/* it's being created in this transaction */
	rel->rd_createSubid = GetCurrentSubTransactionId();

	/* must flag that we have rels created in this transaction */
	need_eoxact_work = true;

	/* is it a temporary relation? */
	rel->rd_istemp = isTempNamespace(relnamespace);

	/*
	 * create a new tuple descriptor from the one passed in.  We do this
	 * partly to copy it into the cache context, and partly because the new
	 * relation can't have any defaults or constraints yet; they have to be
	 * added in later steps, because they require additions to multiple system
	 * catalogs.  We can copy attnotnull constraints here, however.
	 */
	rel->rd_att = CreateTupleDescCopy(tupDesc);
	rel->rd_att->tdrefcount = 1;	/* mark as refcounted */
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
	/* needed when bootstrapping: */
	rel->rd_rel->relowner = BOOTSTRAP_SUPERUSERID;

	/*
	 * Insert relation physical and logical identifiers (OIDs) into the right
	 * places.	Note that the physical ID (relfilenode) is initially the same
	 * as the logical ID (OID).
	 */
	rel->rd_rel->relisshared = shared_relation;

	RelationGetRelid(rel) = relid;

	for (i = 0; i < natts; i++)
		rel->rd_att->attrs[i]->attrelid = relid;

	rel->rd_rel->relfilenode = relid;
	rel->rd_rel->reltablespace = reltablespace;

	RelationInitLockInfo(rel);	/* see lmgr.c */

	RelationInitPhysicalAddr(rel);

	/*
	 * Okay to insert into the relcache hash tables.
	 */
	RelationCacheInsert(rel);

	/*
	 * done building relcache entry.
	 */
	MemoryContextSwitchTo(oldcxt);

	/* It's fully valid */
	rel->rd_isvalid = true;

	/*
	 * Caller expects us to pin the returned entry.
	 */
	RelationIncrementReferenceCount(rel);

	return rel;
}

/*
 *		RelationCacheInitialize
 *
 *		This initializes the relation descriptor cache.  At the time
 *		that this is invoked, we can't do database access yet (mainly
 *		because the transaction subsystem is not up); all we are doing
 *		is making an empty cache hashtable.  This must be done before
 *		starting the initialization transaction, because otherwise
 *		AtEOXact_RelationCache would crash if that transaction aborts
 *		before we can get the relcache set up.
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
	 * create hashtable that indexes the relcache
	 */
	MemSet(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(RelIdCacheEnt);
	ctl.hash = oid_hash;
	RelationIdCache = hash_create("Relcache by OID", INITRELCACHESIZE,
								  &ctl, HASH_ELEM | HASH_FUNCTION);

	MemoryContextSwitchTo(oldcxt);
}

/*
 *		RelationCacheInitializePhase2
 *
 *		This is called as soon as the catcache and transaction system
 *		are functional.  At this point we can actually read data from
 *		the system catalogs.  We first try to read pre-computed relcache
 *		entries from the pg_internal.init file.  If that's missing or
 *		broken, make phony entries for the minimum set of nailed-in-cache
 *		relations.	Then (unless bootstrapping) make sure we have entries
 *		for the critical system indexes.  Once we've done all this, we
 *		have enough infrastructure to open any system catalog or use any
 *		catcache.  The last step is to rewrite pg_internal.init if needed.
 */
void
RelationCacheInitializePhase2(void)
{
	HASH_SEQ_STATUS status;
	RelIdCacheEnt *idhentry;
	MemoryContext oldcxt;
	bool		needNewCacheFile = false;

	/*
	 * switch to cache memory context
	 */
	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);

	/*
	 * Try to load the relcache cache file.  If unsuccessful, bootstrap the
	 * cache with pre-made descriptors for the critical "nailed-in" system
	 * catalogs.
	 */
	if (IsBootstrapProcessingMode() ||
		!load_relcache_init_file())
	{
		needNewCacheFile = true;

		formrdesc("pg_class", PG_CLASS_RELTYPE_OID,
				  true, Natts_pg_class, Desc_pg_class);
		formrdesc("pg_attribute", PG_ATTRIBUTE_RELTYPE_OID,
				  false, Natts_pg_attribute, Desc_pg_attribute);
		formrdesc("pg_proc", PG_PROC_RELTYPE_OID,
				  true, Natts_pg_proc, Desc_pg_proc);
		formrdesc("pg_type", PG_TYPE_RELTYPE_OID,
				  true, Natts_pg_type, Desc_pg_type);

#define NUM_CRITICAL_RELS	4	/* fix if you change list above */
	}

	MemoryContextSwitchTo(oldcxt);

	/* In bootstrap mode, the faked-up formrdesc info is all we'll have */
	if (IsBootstrapProcessingMode())
		return;

	/*
	 * If we didn't get the critical system indexes loaded into relcache, do
	 * so now.	These are critical because the catcache depends on them for
	 * catcache fetches that are done during relcache load.  Thus, we have an
	 * infinite-recursion problem.	We can break the recursion by doing
	 * heapscans instead of indexscans at certain key spots. To avoid hobbling
	 * performance, we only want to do that until we have the critical indexes
	 * loaded into relcache.  Thus, the flag criticalRelcachesBuilt is used to
	 * decide whether to do heapscan or indexscan at the key spots, and we set
	 * it true after we've loaded the critical indexes.
	 *
	 * The critical indexes are marked as "nailed in cache", partly to make it
	 * easy for load_relcache_init_file to count them, but mainly because we
	 * cannot flush and rebuild them once we've set criticalRelcachesBuilt to
	 * true.  (NOTE: perhaps it would be possible to reload them by
	 * temporarily setting criticalRelcachesBuilt to false again.  For now,
	 * though, we just nail 'em in.)
	 *
	 * RewriteRelRulenameIndexId and TriggerRelidNameIndexId are not critical
	 * in the same way as the others, because the critical catalogs don't
	 * (currently) have any rules or triggers, and so these indexes can be
	 * rebuilt without inducing recursion.	However they are used during
	 * relcache load when a rel does have rules or triggers, so we choose to
	 * nail them for performance reasons.
	 */
	if (!criticalRelcachesBuilt)
	{
		Relation	ird;

#define LOAD_CRIT_INDEX(indexoid) \
		do { \
			LockRelationOid(indexoid, AccessShareLock); \
			ird = RelationBuildDesc(indexoid, NULL); \
			if (ird == NULL) \
				elog(PANIC, "could not open critical system index %u", \
					 indexoid); \
			ird->rd_isnailed = true; \
			ird->rd_refcnt = 1; \
			UnlockRelationOid(indexoid, AccessShareLock); \
		} while (0)

		LOAD_CRIT_INDEX(ClassOidIndexId);
		LOAD_CRIT_INDEX(AttributeRelidNumIndexId);
		LOAD_CRIT_INDEX(IndexRelidIndexId);
		LOAD_CRIT_INDEX(AccessMethodStrategyIndexId);
		LOAD_CRIT_INDEX(AccessMethodProcedureIndexId);
		LOAD_CRIT_INDEX(OperatorOidIndexId);
		LOAD_CRIT_INDEX(RewriteRelRulenameIndexId);
		LOAD_CRIT_INDEX(TriggerRelidNameIndexId);

#define NUM_CRITICAL_INDEXES	8		/* fix if you change list above */

		criticalRelcachesBuilt = true;
	}

	/*
	 * Now, scan all the relcache entries and update anything that might be
	 * wrong in the results from formrdesc or the relcache cache file. If we
	 * faked up relcache entries using formrdesc, then read the real pg_class
	 * rows and replace the fake entries with them. Also, if any of the
	 * relcache entries have rules or triggers, load that info the hard way
	 * since it isn't recorded in the cache file.
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

			/* Update rd_options while we have the tuple */
			if (relation->rd_options)
				pfree(relation->rd_options);
			RelationParseRelOptions(relation, htup);

			/*
			 * Also update the derived fields in rd_att.
			 */
			relation->rd_att->tdtypeid = relp->reltype;
			relation->rd_att->tdtypmod = -1;	/* unnecessary, but... */
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

	/*
	 * Lastly, write out a new relcache cache file if one is needed.
	 */
	if (needNewCacheFile)
	{
		/*
		 * Force all the catcaches to finish initializing and thereby open the
		 * catalogs and indexes they use.  This will preload the relcache with
		 * entries for all the most important system catalogs and indexes, so
		 * that the init file will be most useful for future backends.
		 */
		InitCatalogCachePhase2();

		/* now write the file */
		write_relcache_init_file();
	}
}

/*
 * GetPgClassDescriptor -- get a predefined tuple descriptor for pg_class
 * GetPgIndexDescriptor -- get a predefined tuple descriptor for pg_index
 *
 * We need this kluge because we have to be able to access non-fixed-width
 * fields of pg_class and pg_index before we have the standard catalog caches
 * available.  We use predefined data that's set up in just the same way as
 * the bootstrapped reldescs used by formrdesc().  The resulting tupdesc is
 * not 100% kosher: it does not have the correct rowtype OID in tdtypeid, nor
 * does it have a TupleConstr field.  But it's good enough for the purpose of
 * extracting fields.
 */
static TupleDesc
BuildHardcodedDescriptor(int natts, Form_pg_attribute attrs, bool hasoids)
{
	TupleDesc	result;
	MemoryContext oldcxt;
	int			i;

	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);

	result = CreateTemplateTupleDesc(natts, hasoids);
	result->tdtypeid = RECORDOID;		/* not right, but we don't care */
	result->tdtypmod = -1;

	for (i = 0; i < natts; i++)
	{
		memcpy(result->attrs[i], &attrs[i], ATTRIBUTE_TUPLE_SIZE);
		/* make sure attcacheoff is valid */
		result->attrs[i]->attcacheoff = -1;
	}

	/* initialize first attribute's attcacheoff, cf RelationBuildTupleDesc */
	result->attrs[0]->attcacheoff = 0;

	/* Note: we don't bother to set up a TupleConstr entry */

	MemoryContextSwitchTo(oldcxt);

	return result;
}

static TupleDesc
GetPgClassDescriptor(void)
{
	static TupleDesc pgclassdesc = NULL;

	/* Already done? */
	if (pgclassdesc == NULL)
		pgclassdesc = BuildHardcodedDescriptor(Natts_pg_class,
											   Desc_pg_class,
											   true);

	return pgclassdesc;
}

static TupleDesc
GetPgIndexDescriptor(void)
{
	static TupleDesc pgindexdesc = NULL;

	/* Already done? */
	if (pgindexdesc == NULL)
		pgindexdesc = BuildHardcodedDescriptor(Natts_pg_index,
											   Desc_pg_index,
											   false);

	return pgindexdesc;
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

	ScanKeyInit(&skey,
				Anum_pg_attrdef_adrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(relation)));

	adrel = heap_open(AttrDefaultRelationId, AccessShareLock);
	adscan = systable_beginscan(adrel, AttrDefaultIndexId, true,
								SnapshotNow, 1, &skey);
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

	ScanKeyInit(&skey[0],
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(relation)));

	conrel = heap_open(ConstraintRelationId, AccessShareLock);
	conscan = systable_beginscan(conrel, ConstraintRelidIndexId, true,
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
 * may list_free() the returned list after scanning it. This is necessary
 * since the caller will typically be doing syscache lookups on the relevant
 * indexes, and syscache lookup could cause SI messages to be processed!
 *
 * We also update rd_oidindex, which this module treats as effectively part
 * of the index list.  rd_oidindex is valid when rd_indexvalid isn't zero;
 * it is the pg_class OID of a unique index on OID when the relation has one,
 * and InvalidOid if there is no such index.
 */
List *
RelationGetIndexList(Relation relation)
{
	Relation	indrel;
	SysScanDesc indscan;
	ScanKeyData skey;
	HeapTuple	htup;
	List	   *result;
	Oid			oidIndex;
	MemoryContext oldcxt;

	/* Quick exit if we already computed the list. */
	if (relation->rd_indexvalid != 0)
		return list_copy(relation->rd_indexlist);

	/*
	 * We build the list we intend to return (in the caller's context) while
	 * doing the scan.	After successfully completing the scan, we copy that
	 * list into the relcache entry.  This avoids cache-context memory leakage
	 * if we get some sort of error partway through.
	 */
	result = NIL;
	oidIndex = InvalidOid;

	/* Prepare to scan pg_index for entries having indrelid = this rel. */
	ScanKeyInit(&skey,
				Anum_pg_index_indrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(relation)));

	indrel = heap_open(IndexRelationId, AccessShareLock);
	indscan = systable_beginscan(indrel, IndexIndrelidIndexId, true,
								 SnapshotNow, 1, &skey);

	while (HeapTupleIsValid(htup = systable_getnext(indscan)))
	{
		Form_pg_index index = (Form_pg_index) GETSTRUCT(htup);

		/* Add index's OID to result list in the proper order */
		result = insert_ordered_oid(result, index->indexrelid);

		/* Check to see if it is a unique, non-partial btree index on OID */
		if (index->indnatts == 1 &&
			index->indisunique &&
			index->indkey.values[0] == ObjectIdAttributeNumber &&
			index->indclass.values[0] == OID_BTREE_OPS_OID &&
			heap_attisnull(htup, Anum_pg_index_indpred))
			oidIndex = index->indexrelid;
	}

	systable_endscan(indscan);
	heap_close(indrel, AccessShareLock);

	/* Now save a copy of the completed list in the relcache entry. */
	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);
	relation->rd_indexlist = list_copy(result);
	relation->rd_oidindex = oidIndex;
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
	ListCell   *prev;

	/* Does the datum belong at the front? */
	if (list == NIL || datum < linitial_oid(list))
		return lcons_oid(datum, list);
	/* No, so find the entry it belongs after */
	prev = list_head(list);
	for (;;)
	{
		ListCell   *curr = lnext(prev);

		if (curr == NULL || datum < lfirst_oid(curr))
			break;				/* it belongs after 'prev', before 'curr' */

		prev = curr;
	}
	/* Insert datum into list after 'prev' */
	lappend_cell_oid(list, prev, datum);
	return list;
}

/*
 * RelationSetIndexList -- externally force the index list contents
 *
 * This is used to temporarily override what we think the set of valid
 * indexes is (including the presence or absence of an OID index).
 * The forcing will be valid only until transaction commit or abort.
 *
 * This should only be applied to nailed relations, because in a non-nailed
 * relation the hacked index list could be lost at any time due to SI
 * messages.  In practice it is only used on pg_class (see REINDEX).
 *
 * It is up to the caller to make sure the given list is correctly ordered.
 */
void
RelationSetIndexList(Relation relation, List *indexIds, Oid oidIndex)
{
	MemoryContext oldcxt;

	Assert(relation->rd_isnailed);
	/* Copy the list into the cache context (could fail for lack of mem) */
	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);
	indexIds = list_copy(indexIds);
	MemoryContextSwitchTo(oldcxt);
	/* Okay to replace old list */
	list_free(relation->rd_indexlist);
	relation->rd_indexlist = indexIds;
	relation->rd_oidindex = oidIndex;
	relation->rd_indexvalid = 2;	/* mark list as forced */
	/* must flag that we have a forced index list */
	need_eoxact_work = true;
}

/*
 * RelationGetOidIndex -- get the pg_class OID of the relation's OID index
 *
 * Returns InvalidOid if there is no such index.
 */
Oid
RelationGetOidIndex(Relation relation)
{
	List	   *ilist;

	/*
	 * If relation doesn't have OIDs at all, caller is probably confused. (We
	 * could just silently return InvalidOid, but it seems better to throw an
	 * assertion.)
	 */
	Assert(relation->rd_rel->relhasoids);

	if (relation->rd_indexvalid == 0)
	{
		/* RelationGetIndexList does the heavy lifting. */
		ilist = RelationGetIndexList(relation);
		list_free(ilist);
		Assert(relation->rd_indexvalid != 0);
	}

	return relation->rd_oidindex;
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
	 * We build the tree we intend to return in the caller's context. After
	 * successfully completing the work, we copy it into the relcache entry.
	 * This avoids problems if we get some sort of error partway through.
	 */
	exprsDatum = heap_getattr(relation->rd_indextuple,
							  Anum_pg_index_indexprs,
							  GetPgIndexDescriptor(),
							  &isnull);
	Assert(!isnull);
	exprsString = DatumGetCString(DirectFunctionCall1(textout, exprsDatum));
	result = (List *) stringToNode(exprsString);
	pfree(exprsString);

	/*
	 * Run the expressions through eval_const_expressions. This is not just an
	 * optimization, but is necessary, because the planner will be comparing
	 * them to similarly-processed qual clauses, and may fail to detect valid
	 * matches without this.  We don't bother with canonicalize_qual, however.
	 */
	result = (List *) eval_const_expressions((Node *) result);

	/*
	 * Also mark any coercion format fields as "don't care", so that the
	 * planner can match to both explicit and implicit coercions.
	 */
	set_coercionform_dontcare((Node *) result);

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
 * We cache the result of transforming pg_index.indpred into an implicit-AND
 * node tree (suitable for ExecQual).
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
	 * We build the tree we intend to return in the caller's context. After
	 * successfully completing the work, we copy it into the relcache entry.
	 * This avoids problems if we get some sort of error partway through.
	 */
	predDatum = heap_getattr(relation->rd_indextuple,
							 Anum_pg_index_indpred,
							 GetPgIndexDescriptor(),
							 &isnull);
	Assert(!isnull);
	predString = DatumGetCString(DirectFunctionCall1(textout, predDatum));
	result = (List *) stringToNode(predString);
	pfree(predString);

	/*
	 * Run the expression through const-simplification and canonicalization.
	 * This is not just an optimization, but is necessary, because the planner
	 * will be comparing it to similarly-processed qual clauses, and may fail
	 * to detect valid matches without this.  This must match the processing
	 * done to qual clauses in preprocess_expression()!  (We can skip the
	 * stuff involving subqueries, however, since we don't allow any in index
	 * predicates.)
	 */
	result = (List *) eval_const_expressions((Node *) result);

	result = (List *) canonicalize_qual((Expr *) result);

	/*
	 * Also mark any coercion format fields as "don't care", so that the
	 * planner can match to both explicit and implicit coercions.
	 */
	set_coercionform_dontcare((Node *) result);

	/* Also convert to implicit-AND format */
	result = make_ands_implicit((Expr *) result);

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
 * If not successful, return FALSE.
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
				nailed_indexes,
				magic;
	int			i;

	snprintf(initfilename, sizeof(initfilename), "%s/%s",
			 DatabasePath, RELCACHE_INIT_FILENAME);

	fp = AllocateFile(initfilename, PG_BINARY_R);
	if (fp == NULL)
		return false;

	/*
	 * Read the index relcache entries from the file.  Note we will not enter
	 * any of them into the cache if the read fails partway through; this
	 * helps to guard against broken init files.
	 */
	max_rels = 100;
	rels = (Relation *) palloc(max_rels * sizeof(Relation));
	num_rels = 0;
	nailed_rels = nailed_indexes = 0;
	initFileRelationIds = NIL;

	/* check for correct magic number (compatible version) */
	if (fread(&magic, 1, sizeof(magic), fp) != sizeof(magic))
		goto read_failed;
	if (magic != RELCACHE_INIT_FILEMAGIC)
		goto read_failed;

	for (relno = 0;; relno++)
	{
		Size		len;
		size_t		nread;
		Relation	rel;
		Form_pg_class relform;
		bool		has_not_null;
		Datum		indclassDatum;
		bool		isnull;

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
		rel->rd_att->tdrefcount = 1;	/* mark as refcounted */

		rel->rd_att->tdtypeid = relform->reltype;
		rel->rd_att->tdtypmod = -1;		/* unnecessary, but... */

		/* next read all the attribute tuple form data entries */
		has_not_null = false;
		for (i = 0; i < relform->relnatts; i++)
		{
			if ((nread = fread(&len, 1, sizeof(len), fp)) != sizeof(len))
				goto read_failed;
			if (len != ATTRIBUTE_TUPLE_SIZE)
				goto read_failed;
			if ((nread = fread(rel->rd_att->attrs[i], 1, len, fp)) != len)
				goto read_failed;

			has_not_null |= rel->rd_att->attrs[i]->attnotnull;
		}

		/* next read the access method specific field */
		if ((nread = fread(&len, 1, sizeof(len), fp)) != sizeof(len))
			goto read_failed;
		if (len > 0)
		{
			rel->rd_options = palloc(len);
			if ((nread = fread(rel->rd_options, 1, len, fp)) != len)
				goto read_failed;
			if (len != VARATT_SIZE(rel->rd_options))
				goto read_failed;		/* sanity check */
		}
		else
		{
			rel->rd_options = NULL;
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
			Oid		   *operator;
			RegProcedure *support;
			int			nsupport;

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
			rel->rd_indextuple->t_data = (HeapTupleHeader) ((char *) rel->rd_indextuple + HEAPTUPLESIZE);
			rel->rd_index = (Form_pg_index) GETSTRUCT(rel->rd_indextuple);

			/* fix up indclass pointer too */
			indclassDatum = fastgetattr(rel->rd_indextuple,
										Anum_pg_index_indclass,
										GetPgIndexDescriptor(),
										&isnull);
			Assert(!isnull);
			rel->rd_indclass = (oidvector *) DatumGetPointer(indclassDatum);

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

			/* set up zeroed fmgr-info vectors */
			rel->rd_aminfo = (RelationAmInfo *)
				MemoryContextAllocZero(indexcxt, sizeof(RelationAmInfo));
			nsupport = relform->relnatts * am->amsupport;
			rel->rd_supportinfo = (FmgrInfo *)
				MemoryContextAllocZero(indexcxt, nsupport * sizeof(FmgrInfo));
		}
		else
		{
			/* Count nailed rels to ensure we have 'em all */
			if (rel->rd_isnailed)
				nailed_rels++;

			Assert(rel->rd_index == NULL);
			Assert(rel->rd_indextuple == NULL);
			Assert(rel->rd_indclass == NULL);
			Assert(rel->rd_am == NULL);
			Assert(rel->rd_indexcxt == NULL);
			Assert(rel->rd_aminfo == NULL);
			Assert(rel->rd_operator == NULL);
			Assert(rel->rd_support == NULL);
			Assert(rel->rd_supportinfo == NULL);
		}

		/*
		 * Rules and triggers are not saved (mainly because the internal
		 * format is complex and subject to change).  They must be rebuilt if
		 * needed by RelationCacheInitializePhase2.  This is not expected to
		 * be a big performance hit since few system catalogs have such. Ditto
		 * for index expressions and predicates.
		 */
		rel->rd_rules = NULL;
		rel->rd_rulescxt = NULL;
		rel->trigdesc = NULL;
		rel->rd_indexprs = NIL;
		rel->rd_indpred = NIL;

		/*
		 * Reset transient-state fields in the relcache entry
		 */
		rel->rd_smgr = NULL;
		rel->rd_targblock = InvalidBlockNumber;
		if (rel->rd_isnailed)
			rel->rd_refcnt = 1;
		else
			rel->rd_refcnt = 0;
		rel->rd_indexvalid = 0;
		rel->rd_indexlist = NIL;
		rel->rd_oidindex = InvalidOid;
		rel->rd_createSubid = InvalidSubTransactionId;
		rel->rd_amcache = NULL;
		MemSet(&rel->pgstat_info, 0, sizeof(rel->pgstat_info));

		/*
		 * Recompute lock and physical addressing info.  This is needed in
		 * case the pg_internal.init file was copied from some other database
		 * by CREATE DATABASE.
		 */
		RelationInitLockInfo(rel);
		RelationInitPhysicalAddr(rel);
	}

	/*
	 * We reached the end of the init file without apparent problem. Did we
	 * get the right number of nailed items?  (This is a useful crosscheck in
	 * case the set of critical rels or indexes changes.)
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
		initFileRelationIds = lcons_oid(RelationGetRelid(rels[relno]),
										initFileRelationIds);
	}

	pfree(rels);
	FreeFile(fp);

	criticalRelcachesBuilt = true;
	return true;

	/*
	 * init file is broken, so do it the hard way.	We don't bother trying to
	 * free the clutter we just allocated; it's not in the relcache so it
	 * won't hurt.
	 */
read_failed:
	pfree(rels);
	FreeFile(fp);

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
	int			magic;
	HASH_SEQ_STATUS status;
	RelIdCacheEnt *idhentry;
	MemoryContext oldcxt;
	int			i;

	/*
	 * We must write a temporary file and rename it into place. Otherwise,
	 * another backend starting at about the same time might crash trying to
	 * read the partially-complete file.
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
	 * Write a magic number to serve as a file version identifier.	We can
	 * change the magic number whenever the relcache layout changes.
	 */
	magic = RELCACHE_INIT_FILEMAGIC;
	if (fwrite(&magic, 1, sizeof(magic), fp) != sizeof(magic))
		elog(FATAL, "could not write init file");

	/*
	 * Write all the reldescs (in no particular order).
	 */
	hash_seq_init(&status, RelationIdCache);

	initFileRelationIds = NIL;

	while ((idhentry = (RelIdCacheEnt *) hash_seq_search(&status)) != NULL)
	{
		Relation	rel = idhentry->reldesc;
		Form_pg_class relform = rel->rd_rel;

		/* first write the relcache entry proper */
		write_item(rel, sizeof(RelationData), fp);

		/* next write the relation tuple form */
		write_item(relform, CLASS_TUPLE_SIZE, fp);

		/* next, do all the attribute tuple form data entries */
		for (i = 0; i < relform->relnatts; i++)
		{
			write_item(rel->rd_att->attrs[i], ATTRIBUTE_TUPLE_SIZE, fp);
		}

		/* next, do the access method specific field */
		write_item(rel->rd_options,
				   (rel->rd_options ? VARATT_SIZE(rel->rd_options) : 0),
				   fp);

		/* If it's an index, there's more to do */
		if (rel->rd_rel->relkind == RELKIND_INDEX)
		{
			Form_pg_am	am = rel->rd_am;

			/* write the pg_index tuple */
			/* we assume this was created by heap_copytuple! */
			write_item(rel->rd_indextuple,
					   HEAPTUPLESIZE + rel->rd_indextuple->t_len,
					   fp);

			/* next, write the access method tuple form */
			write_item(am, sizeof(FormData_pg_am), fp);

			/* next, write the vector of operator OIDs */
			write_item(rel->rd_operator,
					   relform->relnatts * (am->amstrategies * sizeof(Oid)),
					   fp);

			/* finally, write the vector of support procedures */
			write_item(rel->rd_support,
				  relform->relnatts * (am->amsupport * sizeof(RegProcedure)),
					   fp);
		}

		/* also make a list of their OIDs, for RelationIdIsInInitFile */
		oldcxt = MemoryContextSwitchTo(CacheMemoryContext);
		initFileRelationIds = lcons_oid(RelationGetRelid(rel),
										initFileRelationIds);
		MemoryContextSwitchTo(oldcxt);
	}

	if (FreeFile(fp))
		elog(FATAL, "could not write init file");

	/*
	 * Now we have to check whether the data we've so painstakingly
	 * accumulated is already obsolete due to someone else's just-committed
	 * catalog changes.  If so, we just delete the temp file and leave it to
	 * the next backend to try again.  (Our own relcache entries will be
	 * updated by SI message processing, but we can't be sure whether what we
	 * wrote out was up-to-date.)
	 *
	 * This mustn't run concurrently with RelationCacheInitFileInvalidate, so
	 * grab a serialization lock for the duration.
	 */
	LWLockAcquire(RelCacheInitLock, LW_EXCLUSIVE);

	/* Make sure we have seen all incoming SI messages */
	AcceptInvalidationMessages();

	/*
	 * If we have received any SI relcache invals since backend start, assume
	 * we may have written out-of-date data.
	 */
	if (relcacheInvalsReceived == 0L)
	{
		/*
		 * OK, rename the temp file to its final name, deleting any
		 * previously-existing init file.
		 *
		 * Note: a failure here is possible under Cygwin, if some other
		 * backend is holding open an unlinked-but-not-yet-gone init file. So
		 * treat this as a noncritical failure; just remove the useless temp
		 * file on failure.
		 */
		if (rename(tempfilename, finalfilename) < 0)
			unlink(tempfilename);
	}
	else
	{
		/* Delete the already-obsolete temp file */
		unlink(tempfilename);
	}

	LWLockRelease(RelCacheInitLock);
}

/* write a chunk of data preceded by its length */
static void
write_item(const void *data, Size len, FILE *fp)
{
	if (fwrite(&len, 1, sizeof(len), fp) != sizeof(len))
		elog(FATAL, "could not write init file");
	if (fwrite(data, 1, len, fp) != len)
		elog(FATAL, "could not write init file");
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
	return list_member_oid(initFileRelationIds, relationId);
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
 * works because the backend startup sequence gets into the PGPROC array before
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
		 * already-obsolete init file into place just after we unlink. With
		 * the interlock, it's certain that write_relcache_init_file will
		 * notice our SI inval message before renaming into place, or else
		 * that we will execute second and successfully unlink the file.
		 */
		LWLockAcquire(RelCacheInitLock, LW_EXCLUSIVE);
		unlink(initfilename);
		LWLockRelease(RelCacheInitLock);
	}
}

/*
 * Remove the init file for a given database during postmaster startup.
 *
 * We used to keep the init file across restarts, but that is unsafe in PITR
 * scenarios, and even in simple crash-recovery cases there are windows for
 * the init file to become out-of-sync with the database.  So now we just
 * remove it during startup and expect the first backend launch to rebuild it.
 * Of course, this has to happen in each database of the cluster.  For
 * simplicity this is driven by flatfiles.c, which has to scan pg_database
 * anyway.
 */
void
RelationCacheInitFileRemove(const char *dbPath)
{
	char		initfilename[MAXPGPATH];

	snprintf(initfilename, sizeof(initfilename), "%s/%s",
			 dbPath, RELCACHE_INIT_FILENAME);
	unlink(initfilename);
	/* ignore any error, since it might not be there at all */
}

/*-------------------------------------------------------------------------
 *
 * index.c
 *	  code to create and destroy POSTGRES index relations
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/catalog/index.c,v 1.139 2001/01/24 19:42:51 momjian Exp $
 *
 *
 * INTERFACE ROUTINES
 *		index_create()			- Create a cataloged index relation
 *		index_drop()			- Removes index relation from catalogs
 *		BuildIndexInfo()		- Prepare to insert index tuples
 *		FormIndexDatum()		- Construct datum vector for one index tuple
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "access/istrat.h"
#include "bootstrap/bootstrap.h"
#include "catalog/catalog.h"
#include "catalog/catname.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/pg_index.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/comment.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "optimizer/clauses.h"
#include "optimizer/prep.h"
#include "parser/parse_func.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/relcache.h"
#include "utils/syscache.h"
#include "utils/temprel.h"

/*
 * macros used in guessing how many tuples are on a page.
 */
#define AVG_ATTR_SIZE 8
#define NTUPLES_PER_PAGE(natts) \
	((BLCKSZ - MAXALIGN(sizeof (PageHeaderData))) / \
	((natts) * AVG_ATTR_SIZE + MAXALIGN(sizeof(HeapTupleHeaderData))))

/* non-export function prototypes */
static Oid GetHeapRelationOid(char *heapRelationName, char *indexRelationName,
				   bool istemp);
static TupleDesc BuildFuncTupleDesc(Oid funcOid);
static TupleDesc ConstructTupleDescriptor(Relation heapRelation,
										  int numatts, AttrNumber *attNums);
static void ConstructIndexReldesc(Relation indexRelation, Oid amoid);
static Oid	UpdateRelationRelation(Relation indexRelation, char *temp_relname);
static void InitializeAttributeOids(Relation indexRelation,
						int numatts, Oid indexoid);
static void AppendAttributeTuples(Relation indexRelation, int numatts);
static void UpdateIndexRelation(Oid indexoid, Oid heapoid,
								IndexInfo *indexInfo,
								Oid *classOids,
								bool islossy, bool primary);
static void DefaultBuild(Relation heapRelation, Relation indexRelation,
						 IndexInfo *indexInfo, Node *oldPred,
						 IndexStrategy indexStrategy);
static Oid	IndexGetRelation(Oid indexId);
static bool activate_index(Oid indexId, bool activate, bool inplace);


static bool reindexing = false;


bool
SetReindexProcessing(bool reindexmode)
{
	bool		old = reindexing;

	reindexing = reindexmode;
	return old;
}

bool
IsReindexProcessing(void)
{
	return reindexing;
}

/* ----------------------------------------------------------------
 *	  sysatts is a structure containing attribute tuple forms
 *	  for system attributes (numbered -1, -2, ...).  This really
 *	  should be generated or eliminated or moved elsewhere. -cim 1/19/91
 *
 * typedef struct FormData_pg_attribute {
 *		Oid				attrelid;
 *		NameData		attname;
 *		Oid				atttypid;
 *		uint32			attnvals;
 *		int16			attlen;
 *		AttrNumber		attnum;
 *		uint32			attnelems;
 *		int32			attcacheoff;
 *		int32			atttypmod;
 *		bool			attbyval;
 *		bool			attisset;
 *		char			attalign;
 *		bool			attnotnull;
 *		bool			atthasdef;
 * } FormData_pg_attribute;
 *
 * ----------------------------------------------------------------
 */
static FormData_pg_attribute sysatts[] = {
	{0, {"ctid"}, TIDOID, 0, 6, -1, 0, -1, -1, '\0', 'p', '\0', 'i', '\0', '\0'},
	{0, {"oid"}, OIDOID, 0, 4, -2, 0, -1, -1, '\001', 'p', '\0', 'i', '\0', '\0'},
	{0, {"xmin"}, XIDOID, 0, 4, -3, 0, -1, -1, '\001', 'p', '\0', 'i', '\0', '\0'},
	{0, {"cmin"}, CIDOID, 0, 4, -4, 0, -1, -1, '\001', 'p', '\0', 'i', '\0', '\0'},
	{0, {"xmax"}, XIDOID, 0, 4, -5, 0, -1, -1, '\001', 'p', '\0', 'i', '\0', '\0'},
	{0, {"cmax"}, CIDOID, 0, 4, -6, 0, -1, -1, '\001', 'p', '\0', 'i', '\0', '\0'},
};

/* ----------------------------------------------------------------
 *		GetHeapRelationOid
 * ----------------------------------------------------------------
 */
static Oid
GetHeapRelationOid(char *heapRelationName, char *indexRelationName, bool istemp)
{
	Oid			indoid;
	Oid			heapoid;


	indoid = RelnameFindRelid(indexRelationName);

	if ((!istemp && OidIsValid(indoid)) ||
		(istemp && is_temp_rel_name(indexRelationName)))
		elog(ERROR, "Cannot create index: '%s' already exists",
			 indexRelationName);

	heapoid = RelnameFindRelid(heapRelationName);

	if (!OidIsValid(heapoid))
		elog(ERROR, "Cannot create index on '%s': relation does not exist",
			 heapRelationName);

	return heapoid;
}

static TupleDesc
BuildFuncTupleDesc(Oid funcOid)
{
	TupleDesc	funcTupDesc;
	HeapTuple	tuple;
	Oid			retType;

	/*
	 * Allocate and zero a tuple descriptor for a one-column tuple.
	 */
	funcTupDesc = CreateTemplateTupleDesc(1);
	funcTupDesc->attrs[0] = (Form_pg_attribute) palloc(ATTRIBUTE_TUPLE_SIZE);
	MemSet(funcTupDesc->attrs[0], 0, ATTRIBUTE_TUPLE_SIZE);

	/*
	 * Lookup the function to get its name and return type.
	 */
	tuple = SearchSysCache(PROCOID,
						   ObjectIdGetDatum(funcOid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "Function %u does not exist", funcOid);
	retType = ((Form_pg_proc) GETSTRUCT(tuple))->prorettype;

	/*
	 * make the attributes name the same as the functions
	 */
	namestrcpy(&funcTupDesc->attrs[0]->attname,
			   NameStr(((Form_pg_proc) GETSTRUCT(tuple))->proname));

	ReleaseSysCache(tuple);

	/*
	 * Lookup the return type in pg_type for the type length etc.
	 */
	tuple = SearchSysCache(TYPEOID,
						   ObjectIdGetDatum(retType),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "Type %u does not exist", retType);

	/*
	 * Assign some of the attributes values. Leave the rest as 0.
	 */
	funcTupDesc->attrs[0]->attlen = ((Form_pg_type) GETSTRUCT(tuple))->typlen;
	funcTupDesc->attrs[0]->atttypid = retType;
	funcTupDesc->attrs[0]->attnum = 1;
	funcTupDesc->attrs[0]->attbyval = ((Form_pg_type) GETSTRUCT(tuple))->typbyval;
	funcTupDesc->attrs[0]->attcacheoff = -1;
	funcTupDesc->attrs[0]->atttypmod = -1;
	funcTupDesc->attrs[0]->attstorage = ((Form_pg_type) GETSTRUCT(tuple))->typstorage;
	funcTupDesc->attrs[0]->attalign = ((Form_pg_type) GETSTRUCT(tuple))->typalign;

	ReleaseSysCache(tuple);

	return funcTupDesc;
}

/* ----------------------------------------------------------------
 *		ConstructTupleDescriptor
 *
 * Build an index tuple descriptor for a new index (plain not functional)
 * ----------------------------------------------------------------
 */
static TupleDesc
ConstructTupleDescriptor(Relation heapRelation,
						 int numatts,
						 AttrNumber *attNums)
{
	TupleDesc	heapTupDesc;
	TupleDesc	indexTupDesc;
	int			natts;			/* #atts in heap rel --- for error checks */
	int			i;

	heapTupDesc = RelationGetDescr(heapRelation);
	natts = RelationGetForm(heapRelation)->relnatts;

	/* ----------------
	 *	allocate the new tuple descriptor
	 * ----------------
	 */

	indexTupDesc = CreateTemplateTupleDesc(numatts);

	/* ----------------
	 *	  for each attribute we are indexing, obtain its attribute
	 *	  tuple form from either the static table of system attribute
	 *	  tuple forms or the relation tuple descriptor
	 * ----------------
	 */
	for (i = 0; i < numatts; i++)
	{
		AttrNumber	atnum;		/* attributeNumber[attributeOffset] */
		AttrNumber	atind;
		Form_pg_attribute from;
		Form_pg_attribute to;

		/* ----------------
		 *	 get the attribute number and make sure it's valid;
		 *	 determine which attribute descriptor to copy
		 * ----------------
		 */
		atnum = attNums[i];

		if (!AttrNumberIsForUserDefinedAttr(atnum))
		{
			/* ----------------
			 *	  here we are indexing on a system attribute (-1...-n)
			 *	  so we convert atnum into a usable index 0...n-1 so we can
			 *	  use it to dereference the array sysatts[] which stores
			 *	  tuple descriptor information for system attributes.
			 * ----------------
			 */
			if (atnum <= FirstLowInvalidHeapAttributeNumber || atnum >= 0)
				elog(ERROR, "Cannot create index on system attribute: attribute number out of range (%d)", atnum);
			atind = (-atnum) - 1;

			from = &sysatts[atind];
		}
		else
		{
			/* ----------------
			 *	  here we are indexing on a normal attribute (1...n)
			 * ----------------
			 */
			if (atnum > natts)
				elog(ERROR, "Cannot create index: attribute %d does not exist",
					 atnum);
			atind = AttrNumberGetAttrOffset(atnum);

			from = heapTupDesc->attrs[atind];
		}

		/* ----------------
		 *	 now that we've determined the "from", let's copy
		 *	 the tuple desc data...
		 * ----------------
		 */
		indexTupDesc->attrs[i] = to =
			(Form_pg_attribute) palloc(ATTRIBUTE_TUPLE_SIZE);
		memcpy(to, from, ATTRIBUTE_TUPLE_SIZE);

		/*
		 * Fix the stuff that should not be the same as the underlying attr
		 */
		to->attnum = i + 1;

		to->attdispersion = 0.0;
		to->attnotnull = false;
		to->atthasdef = false;
		to->attcacheoff = -1;

		/*
		 * We do not yet have the correct relation OID for the index,
		 * so just set it invalid for now.  InitializeAttributeOids()
		 * will fix it later.
		 */
		to->attrelid = InvalidOid;
	}

	return indexTupDesc;
}

/* ----------------------------------------------------------------
 * AccessMethodObjectIdGetForm
 *		Returns an access method tuple given its object identifier,
 *		or NULL if no such AM tuple can be found.
 *
 * Scanning is done using CurrentMemoryContext as working storage,
 * but the returned tuple will be allocated in resultCxt (which is
 * typically CacheMemoryContext).
 *
 * There was a note here about adding indexing, but I don't see a need
 * for it.  There are so few tuples in pg_am that an indexscan would
 * surely be slower.
 * ----------------------------------------------------------------
 */
Form_pg_am
AccessMethodObjectIdGetForm(Oid accessMethodObjectId,
							MemoryContext resultCxt)
{
	Relation	pg_am_desc;
	HeapScanDesc pg_am_scan;
	HeapTuple	pg_am_tuple;
	ScanKeyData key;
	Form_pg_am	aform;

	/* ----------------
	 *	form a scan key for the pg_am relation
	 * ----------------
	 */
	ScanKeyEntryInitialize(&key, 0, ObjectIdAttributeNumber,
						   F_OIDEQ,
						   ObjectIdGetDatum(accessMethodObjectId));

	/* ----------------
	 *	fetch the desired access method tuple
	 * ----------------
	 */
	pg_am_desc = heap_openr(AccessMethodRelationName, AccessShareLock);
	pg_am_scan = heap_beginscan(pg_am_desc, 0, SnapshotNow, 1, &key);

	pg_am_tuple = heap_getnext(pg_am_scan, 0);

	/* ----------------
	 *	return NULL if not found
	 * ----------------
	 */
	if (!HeapTupleIsValid(pg_am_tuple))
	{
		heap_endscan(pg_am_scan);
		heap_close(pg_am_desc, AccessShareLock);
		return NULL;
	}

	/* ----------------
	 *	if found AM tuple, then copy it into resultCxt and return the copy
	 * ----------------
	 */
	aform = (Form_pg_am) MemoryContextAlloc(resultCxt, sizeof *aform);
	memcpy(aform, GETSTRUCT(pg_am_tuple), sizeof *aform);

	heap_endscan(pg_am_scan);
	heap_close(pg_am_desc, AccessShareLock);

	return aform;
}

/* ----------------------------------------------------------------
 *		ConstructIndexReldesc
 * ----------------------------------------------------------------
 */
static void
ConstructIndexReldesc(Relation indexRelation, Oid amoid)
{
	indexRelation->rd_am = AccessMethodObjectIdGetForm(amoid,
													   CacheMemoryContext);

	/* ----------------
	 *	 XXX missing the initialization of some other fields
	 * ----------------
	 */

	indexRelation->rd_rel->relowner = GetUserId();

	indexRelation->rd_rel->relam = amoid;
	indexRelation->rd_rel->reltuples = 1;		/* XXX */
	indexRelation->rd_rel->relkind = RELKIND_INDEX;
}

/* ----------------------------------------------------------------
 *		UpdateRelationRelation
 * ----------------------------------------------------------------
 */
static Oid
UpdateRelationRelation(Relation indexRelation, char *temp_relname)
{
	Relation	pg_class;
	HeapTuple	tuple;
	Oid			tupleOid;
	Relation	idescs[Num_pg_class_indices];

	pg_class = heap_openr(RelationRelationName, RowExclusiveLock);

	/* XXX Natts_pg_class_fixed is a hack - see pg_class.h */
	tuple = heap_addheader(Natts_pg_class_fixed,
						   CLASS_TUPLE_SIZE,
						   (char *) indexRelation->rd_rel);

	/* ----------------
	 *	the new tuple must have the same oid as the relcache entry for the
	 *	index.	sure would be embarrassing to do this sort of thing in
	 *	polite company.
	 * ----------------
	 */
	tuple->t_data->t_oid = RelationGetRelid(indexRelation);
	heap_insert(pg_class, tuple);

	if (temp_relname)
		create_temp_relation(temp_relname, tuple);

	/*
	 * During normal processing, we need to make sure that the system
	 * catalog indices are correct.  Bootstrap (initdb) time doesn't
	 * require this, because we make sure that the indices are correct
	 * just before exiting.
	 */

	if (!IsIgnoringSystemIndexes())
	{
		CatalogOpenIndices(Num_pg_class_indices, Name_pg_class_indices, idescs);
		CatalogIndexInsert(idescs, Num_pg_class_indices, pg_class, tuple);
		CatalogCloseIndices(Num_pg_class_indices, idescs);
	}

	tupleOid = tuple->t_data->t_oid;
	heap_freetuple(tuple);
	heap_close(pg_class, RowExclusiveLock);

	return tupleOid;
}

/* ----------------------------------------------------------------
 *		InitializeAttributeOids
 * ----------------------------------------------------------------
 */
static void
InitializeAttributeOids(Relation indexRelation,
						int numatts,
						Oid indexoid)
{
	TupleDesc	tupleDescriptor;
	int			i;

	tupleDescriptor = RelationGetDescr(indexRelation);

	for (i = 0; i < numatts; i += 1)
		tupleDescriptor->attrs[i]->attrelid = indexoid;
}

/* ----------------------------------------------------------------
 *		AppendAttributeTuples
 *
 *		XXX For now, only change the ATTNUM attribute value
 * ----------------------------------------------------------------
 */
static void
AppendAttributeTuples(Relation indexRelation, int numatts)
{
	Relation	pg_attribute;
	HeapTuple	init_tuple,
				cur_tuple = NULL,
				new_tuple;
	bool		hasind;
	Relation	idescs[Num_pg_attr_indices];
	Datum		value[Natts_pg_attribute];
	char		nullv[Natts_pg_attribute];
	char		replace[Natts_pg_attribute];
	TupleDesc	indexTupDesc;
	int			i;

	/* ----------------
	 *	open the attribute relation
	 * ----------------
	 */
	pg_attribute = heap_openr(AttributeRelationName, RowExclusiveLock);

	/* ----------------
	 *	initialize *null, *replace and *value
	 * ----------------
	 */
	MemSet(nullv, ' ', Natts_pg_attribute);
	MemSet(replace, ' ', Natts_pg_attribute);

	/* ----------------
	 *	create the first attribute tuple.
	 *	XXX For now, only change the ATTNUM attribute value
	 * ----------------
	 */
	replace[Anum_pg_attribute_attnum - 1] = 'r';
	replace[Anum_pg_attribute_attcacheoff - 1] = 'r';

	value[Anum_pg_attribute_attnum - 1] = Int16GetDatum(1);
	value[Anum_pg_attribute_attcacheoff - 1] = Int32GetDatum(-1);

	init_tuple = heap_addheader(Natts_pg_attribute,
								ATTRIBUTE_TUPLE_SIZE,
							 (char *) (indexRelation->rd_att->attrs[0]));

	hasind = false;
	if (!IsIgnoringSystemIndexes() && pg_attribute->rd_rel->relhasindex)
	{
		hasind = true;
		CatalogOpenIndices(Num_pg_attr_indices, Name_pg_attr_indices, idescs);
	}

	/* ----------------
	 *	insert the first attribute tuple.
	 * ----------------
	 */
	cur_tuple = heap_modifytuple(init_tuple,
								 pg_attribute,
								 value,
								 nullv,
								 replace);
	heap_freetuple(init_tuple);

	heap_insert(pg_attribute, cur_tuple);
	if (hasind)
		CatalogIndexInsert(idescs, Num_pg_attr_indices, pg_attribute, cur_tuple);

	/* ----------------
	 *	now we use the information in the index cur_tuple
	 *	descriptor to form the remaining attribute tuples.
	 * ----------------
	 */
	indexTupDesc = RelationGetDescr(indexRelation);

	for (i = 1; i < numatts; i += 1)
	{
		/* ----------------
		 *	process the remaining attributes...
		 * ----------------
		 */
		memmove(GETSTRUCT(cur_tuple),
				(char *) indexTupDesc->attrs[i],
				ATTRIBUTE_TUPLE_SIZE);

		value[Anum_pg_attribute_attnum - 1] = Int16GetDatum(i + 1);

		new_tuple = heap_modifytuple(cur_tuple,
									 pg_attribute,
									 value,
									 nullv,
									 replace);
		heap_freetuple(cur_tuple);

		heap_insert(pg_attribute, new_tuple);
		if (hasind)
			CatalogIndexInsert(idescs, Num_pg_attr_indices, pg_attribute, new_tuple);

		/* ----------------
		 *	ModifyHeapTuple returns a new copy of a cur_tuple
		 *	so we free the original and use the copy..
		 * ----------------
		 */
		cur_tuple = new_tuple;
	}

	if (cur_tuple)
		heap_freetuple(cur_tuple);
	heap_close(pg_attribute, RowExclusiveLock);
	if (hasind)
		CatalogCloseIndices(Num_pg_attr_indices, idescs);
}

/* ----------------------------------------------------------------
 *		UpdateIndexRelation
 * ----------------------------------------------------------------
 */
static void
UpdateIndexRelation(Oid indexoid,
					Oid heapoid,
					IndexInfo *indexInfo,
					Oid *classOids,
					bool islossy,
					bool primary)
{
	Form_pg_index indexForm;
	char	   *predString;
	text	   *predText;
	int			predLen,
				itupLen;
	Relation	pg_index;
	HeapTuple	tuple;
	int			i;
	Relation	idescs[Num_pg_index_indices];

	/* ----------------
	 *	allocate a Form_pg_index big enough to hold the
	 *	index-predicate (if any) in string form
	 * ----------------
	 */
	if (indexInfo->ii_Predicate != NULL)
	{
		predString = nodeToString(indexInfo->ii_Predicate);
		predText = DatumGetTextP(DirectFunctionCall1(textin,
											CStringGetDatum(predString)));
		pfree(predString);
	}
	else
		predText = DatumGetTextP(DirectFunctionCall1(textin,
													 CStringGetDatum("")));

	predLen = VARSIZE(predText);
	itupLen = predLen + sizeof(FormData_pg_index);
	indexForm = (Form_pg_index) palloc(itupLen);
	MemSet(indexForm, 0, sizeof(FormData_pg_index));

	/* ----------------
	 *	store information into the index tuple form
	 * ----------------
	 */
	indexForm->indexrelid = indexoid;
	indexForm->indrelid = heapoid;
	indexForm->indproc = indexInfo->ii_FuncOid;
	indexForm->indisclustered = false;
	indexForm->indislossy = islossy;
	indexForm->indhaskeytype = true; /* not actually used anymore */
	indexForm->indisunique = indexInfo->ii_Unique;
	indexForm->indisprimary = primary;
	memcpy((char *) &indexForm->indpred, (char *) predText, predLen);

	/* ----------------
	 *	copy index key and op class information
	 *
	 *	We zeroed the extra slots (if any) above --- that's essential.
	 * ----------------
	 */
	for (i = 0; i < indexInfo->ii_NumKeyAttrs; i++)
		indexForm->indkey[i] = indexInfo->ii_KeyAttrNumbers[i];

	for (i = 0; i < indexInfo->ii_NumIndexAttrs; i++)
		indexForm->indclass[i] = classOids[i];

	/* ----------------
	 *	open the system catalog index relation
	 * ----------------
	 */
	pg_index = heap_openr(IndexRelationName, RowExclusiveLock);

	/* ----------------
	 *	form a tuple to insert into pg_index
	 * ----------------
	 */
	tuple = heap_addheader(Natts_pg_index,
						   itupLen,
						   (char *) indexForm);

	/* ----------------
	 *	insert the tuple into the pg_index
	 * ----------------
	 */
	heap_insert(pg_index, tuple);

	/* ----------------
	 *	add index tuples for it
	 * ----------------
	 */
	if (!IsIgnoringSystemIndexes())
	{
		CatalogOpenIndices(Num_pg_index_indices, Name_pg_index_indices, idescs);
		CatalogIndexInsert(idescs, Num_pg_index_indices, pg_index, tuple);
		CatalogCloseIndices(Num_pg_index_indices, idescs);
	}

	/* ----------------
	 *	close the relation and free the tuple
	 * ----------------
	 */
	heap_close(pg_index, RowExclusiveLock);
	pfree(predText);
	pfree(indexForm);
	heap_freetuple(tuple);
}

/* ----------------------------------------------------------------
 *		UpdateIndexPredicate
 * ----------------------------------------------------------------
 */
void
UpdateIndexPredicate(Oid indexoid, Node *oldPred, Node *predicate)
{
	Node	   *newPred;
	char	   *predString;
	text	   *predText;
	Relation	pg_index;
	HeapTuple	tuple;
	HeapTuple	newtup;
	int			i;
	Datum		values[Natts_pg_index];
	char		nulls[Natts_pg_index];
	char		replace[Natts_pg_index];

	/*
	 * Construct newPred as a CNF expression equivalent to the OR of the
	 * original partial-index predicate ("oldPred") and the extension
	 * predicate ("predicate").
	 *
	 * This should really try to process the result to change things like
	 * "a>2 OR a>1" to simply "a>1", but for now all it does is make sure
	 * that if the extension predicate is NULL (i.e., it is being extended
	 * to be a complete index), then newPred will be NULL - in effect,
	 * changing "a>2 OR TRUE" to "TRUE". --Nels, Jan '93
	 */
	newPred = NULL;
	if (predicate != NULL)
	{
		newPred = (Node *) make_orclause(lcons(make_andclause((List *) predicate),
								  lcons(make_andclause((List *) oldPred),
										NIL)));
		newPred = (Node *) cnfify((Expr *) newPred, true);
	}

	/* translate the index-predicate to string form */
	if (newPred != NULL)
	{
		predString = nodeToString(newPred);
		predText = DatumGetTextP(DirectFunctionCall1(textin,
											CStringGetDatum(predString)));
		pfree(predString);
	}
	else
		predText = DatumGetTextP(DirectFunctionCall1(textin,
													 CStringGetDatum("")));

	/* open the index system catalog relation */
	pg_index = heap_openr(IndexRelationName, RowExclusiveLock);

	tuple = SearchSysCache(INDEXRELID,
						   ObjectIdGetDatum(indexoid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "UpdateIndexPredicate: cache lookup failed for index %u",
			 indexoid);

	for (i = 0; i < Natts_pg_index; i++)
	{
		nulls[i] = heap_attisnull(tuple, i + 1) ? 'n' : ' ';
		replace[i] = ' ';
		values[i] = (Datum) NULL;
	}

	replace[Anum_pg_index_indpred - 1] = 'r';
	values[Anum_pg_index_indpred - 1] = (Datum) predText;

	newtup = heap_modifytuple(tuple, pg_index, values, nulls, replace);

	simple_heap_update(pg_index, &newtup->t_self, newtup);

	heap_freetuple(newtup);
	ReleaseSysCache(tuple);

	heap_close(pg_index, RowExclusiveLock);
	pfree(predText);
}

/* ----------------------------------------------------------------
 *		InitIndexStrategy
 * ----------------------------------------------------------------
 */
void
InitIndexStrategy(int numatts,
				  Relation indexRelation,
				  Oid accessMethodObjectId)
{
	IndexStrategy strategy;
	RegProcedure *support;
	uint16		amstrategies;
	uint16		amsupport;
	Oid			attrelid;
	Size		strsize;

	/* ----------------
	 *	get information from the index relation descriptor
	 * ----------------
	 */
	attrelid = indexRelation->rd_att->attrs[0]->attrelid;
	amstrategies = indexRelation->rd_am->amstrategies;
	amsupport = indexRelation->rd_am->amsupport;

	/* ----------------
	 *	get the size of the strategy
	 * ----------------
	 */
	strsize = AttributeNumberGetIndexStrategySize(numatts, amstrategies);

	/* ----------------
	 *	allocate the new index strategy structure
	 *
	 *	the index strategy has to be allocated in the same
	 *	context as the relation descriptor cache or else
	 *	it will be lost at the end of the transaction.
	 * ----------------
	 */
	if (!CacheMemoryContext)
		CreateCacheMemoryContext();

	strategy = (IndexStrategy) MemoryContextAlloc(CacheMemoryContext,
												  strsize);

	if (amsupport > 0)
	{
		strsize = numatts * (amsupport * sizeof(RegProcedure));
		support = (RegProcedure *) MemoryContextAlloc(CacheMemoryContext,
													  strsize);
	}
	else
		support = (RegProcedure *) NULL;

	/* ----------------
	 *	fill in the index strategy structure with information
	 *	from the catalogs.	First we must advance the command counter
	 *	so that we will see the newly-entered index catalog tuples.
	 * ----------------
	 */
	CommandCounterIncrement();

	IndexSupportInitialize(strategy, support,
						   &indexRelation->rd_uniqueindex,
						   attrelid, accessMethodObjectId,
						   amstrategies, amsupport, numatts);

	/* ----------------
	 *	store the strategy information in the index reldesc
	 * ----------------
	 */
	RelationSetIndexSupport(indexRelation, strategy, support);
}


/* ----------------------------------------------------------------
 *		index_create
 * ----------------------------------------------------------------
 */
void
index_create(char *heapRelationName,
			 char *indexRelationName,
			 IndexInfo *indexInfo,
			 Oid accessMethodObjectId,
			 Oid *classObjectId,
			 bool islossy,
			 bool primary,
			 bool allow_system_table_mods)
{
	Relation	heapRelation;
	Relation	indexRelation;
	TupleDesc	indexTupDesc;
	Oid			heapoid;
	Oid			indexoid;
	bool		istemp = is_temp_rel_name(heapRelationName);
	char	   *temp_relname = NULL;

	SetReindexProcessing(false);

	/* ----------------
	 *	check parameters
	 * ----------------
	 */
	if (indexInfo->ii_NumIndexAttrs < 1 ||
		indexInfo->ii_NumKeyAttrs < 1)
		elog(ERROR, "must index at least one attribute");

	/* ----------------
	 *	  get heap relation oid and open the heap relation
	 * ----------------
	 */
	heapoid = GetHeapRelationOid(heapRelationName, indexRelationName, istemp);

	/*
	 * Only SELECT ... FOR UPDATE are allowed while doing this
	 */
	heapRelation = heap_open(heapoid, ShareLock);

	/* ----------------
	 *	  construct new tuple descriptor
	 * ----------------
	 */
	if (OidIsValid(indexInfo->ii_FuncOid))
		indexTupDesc = BuildFuncTupleDesc(indexInfo->ii_FuncOid);
	else
		indexTupDesc = ConstructTupleDescriptor(heapRelation,
												indexInfo->ii_NumKeyAttrs,
												indexInfo->ii_KeyAttrNumbers);

	if (istemp)
	{
		/* save user relation name because heap_create changes it */
		temp_relname = pstrdup(indexRelationName);	/* save original value */
		indexRelationName = palloc(NAMEDATALEN);
		strcpy(indexRelationName, temp_relname);	/* heap_create will
													 * change this */
	}

	/* ----------------
	 *	create the index relation
	 * ----------------
	 */
	indexRelation = heap_create(indexRelationName, indexTupDesc,
								istemp, false, allow_system_table_mods);

	/*
	 * Obtain exclusive lock on it.  Although no other backends can see it
	 * until we commit, this prevents deadlock-risk complaints from lock
	 * manager in cases such as CLUSTER.
	 */
	LockRelation(indexRelation, AccessExclusiveLock);

	/* ----------------
	 *	  construct the index relation descriptor
	 *
	 *	  XXX should have a proper way to create cataloged relations
	 * ----------------
	 */
	ConstructIndexReldesc(indexRelation, accessMethodObjectId);

	/* ----------------
	 *	  add index to catalogs
	 *	  (append RELATION tuple)
	 * ----------------
	 */
	indexoid = UpdateRelationRelation(indexRelation, temp_relname);

	/*
	 * We create the disk file for this relation here
	 */
	heap_storage_create(indexRelation);

	/* ----------------
	 *	now update the object id's of all the attribute
	 *	tuple forms in the index relation's tuple descriptor
	 * ----------------
	 */
	InitializeAttributeOids(indexRelation,
							indexInfo->ii_NumIndexAttrs,
							indexoid);

	/* ----------------
	 *	  append ATTRIBUTE tuples for the index
	 * ----------------
	 */
	AppendAttributeTuples(indexRelation, indexInfo->ii_NumIndexAttrs);

	/* ----------------
	 *	  update pg_index
	 *	  (append INDEX tuple)
	 *
	 *	  Note that this stows away a representation of "predicate".
	 *	  (Or, could define a rule to maintain the predicate) --Nels, Feb '92
	 * ----------------
	 */
	UpdateIndexRelation(indexoid, heapoid, indexInfo,
						classObjectId, islossy, primary);

	/* ----------------
	 *	  initialize the index strategy
	 * ----------------
	 */
	InitIndexStrategy(indexInfo->ii_NumIndexAttrs,
					  indexRelation,
					  accessMethodObjectId);

	/*
	 * If this is bootstrap (initdb) time, then we don't actually fill in
	 * the index yet.  We'll be creating more indices and classes later,
	 * so we delay filling them in until just before we're done with
	 * bootstrapping.  Otherwise, we call the routine that constructs the
	 * index.
	 *
	 * In normal processing mode, the heap and index relations are closed by
	 * index_build() --- but we continue to hold the ShareLock on the heap
	 * and the exclusive lock on the index that we acquired above, until
	 * end of transaction.
	 */
	if (IsBootstrapProcessingMode())
	{
		index_register(heapRelationName, indexRelationName, indexInfo);
		/* XXX shouldn't we close the heap and index rels here? */
	}
	else
	{
		index_build(heapRelation, indexRelation, indexInfo, NULL);
	}
}

/* ----------------------------------------------------------------
 *
 *		index_drop
 *
 * ----------------------------------------------------------------
 */
void
index_drop(Oid indexId)
{
	Oid			heapId;
	Relation	userHeapRelation;
	Relation	userIndexRelation;
	Relation	indexRelation;
	Relation	relationRelation;
	Relation	attributeRelation;
	HeapTuple	tuple;
	int16		attnum;
	int			i;

	Assert(OidIsValid(indexId));

	/* ----------------
	 *	To drop an index safely, we must grab exclusive lock on its parent
	 *	table; otherwise there could be other backends using the index!
	 *	Exclusive lock on the index alone is insufficient because the index
	 *	access routines are a little slipshod about obtaining adequate locking
	 *	(see ExecOpenIndices()).  We do grab exclusive lock on the index too,
	 *	just to be safe.  Both locks must be held till end of transaction,
	 *	else other backends will still see this index in pg_index.
	 * ----------------
	 */
	heapId = IndexGetRelation(indexId);
	userHeapRelation = heap_open(heapId, AccessExclusiveLock);

	userIndexRelation = index_open(indexId);
	LockRelation(userIndexRelation, AccessExclusiveLock);

	/* ----------------
	 *	Note: unlike heap_drop_with_catalog, we do not need to prevent
	 *	deletion of system indexes here; that's checked for upstream.
	 *	If we did check it here, deletion of TOAST tables would fail...
	 * ----------------
	 */

	/* ----------------
	 * fix DESCRIPTION relation
	 * ----------------
	 */
	DeleteComments(indexId);

	/* ----------------
	 * fix RELATION relation
	 * ----------------
	 */
	relationRelation = heap_openr(RelationRelationName, RowExclusiveLock);

	/* Remove the pg_class tuple for the index itself */
	tuple = SearchSysCacheCopy(RELOID,
							   ObjectIdGetDatum(indexId),
							   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "index_drop: cache lookup failed for index %u",
			 indexId);

	simple_heap_delete(relationRelation, &tuple->t_self);
	heap_freetuple(tuple);

	/*
	 * Update the pg_class tuple for the owning relation.  We are presently
	 * too lazy to attempt to compute the new correct value of relhasindex
	 * (the next VACUUM will fix it if necessary).  But we must send out a
	 * shared-cache-inval notice on the owning relation to ensure other
	 * backends update their relcache lists of indexes.  So, unconditionally
	 * do setRelhasindex(true).
	 */
	setRelhasindex(heapId, true);

	heap_close(relationRelation, RowExclusiveLock);

	/* ----------------
	 * fix ATTRIBUTE relation
	 * ----------------
	 */
	attributeRelation = heap_openr(AttributeRelationName, RowExclusiveLock);

	attnum = 1;					/* indexes start at 1 */

	while (HeapTupleIsValid(tuple = SearchSysCacheCopy(ATTNUM,
											   ObjectIdGetDatum(indexId),
												   Int16GetDatum(attnum),
													   0, 0)))
	{
		simple_heap_delete(attributeRelation, &tuple->t_self);
		heap_freetuple(tuple);
		attnum++;
	}
	heap_close(attributeRelation, RowExclusiveLock);

	/* ----------------
	 * fix INDEX relation
	 * ----------------
	 */
	indexRelation = heap_openr(IndexRelationName, RowExclusiveLock);

	tuple = SearchSysCacheCopy(INDEXRELID,
							   ObjectIdGetDatum(indexId),
							   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "index_drop: cache lookup failed for index %u",
			 indexId);

	simple_heap_delete(indexRelation, &tuple->t_self);
	heap_freetuple(tuple);
	heap_close(indexRelation, RowExclusiveLock);

	/*
	 * flush buffer cache and physically remove the file
	 */
	i = FlushRelationBuffers(userIndexRelation, (BlockNumber) 0);
	if (i < 0)
		elog(ERROR, "index_drop: FlushRelationBuffers returned %d", i);

	smgrunlink(DEFAULT_SMGR, userIndexRelation);

	/*
	 * Close rels, but keep locks
	 */
	index_close(userIndexRelation);
	heap_close(userHeapRelation, NoLock);

	RelationForgetRelation(indexId);

	/* if it's a temp index, clear the temp mapping table entry */
	remove_temp_rel_by_relid(indexId);
}

/* ----------------------------------------------------------------
 *						index_build support
 * ----------------------------------------------------------------
 */

/* ----------------
 *		BuildIndexInfo
 *			Construct an IndexInfo record given the index's pg_index tuple
 *
 * IndexInfo stores the information about the index that's needed by
 * FormIndexDatum, which is used for both index_build() and later insertion
 * of individual index tuples.  Normally we build an IndexInfo for an index
 * just once per command, and then use it for (potentially) many tuples.
 * ----------------
 */
IndexInfo *
BuildIndexInfo(HeapTuple indexTuple)
{
	Form_pg_index indexStruct = (Form_pg_index) GETSTRUCT(indexTuple);
	IndexInfo  *ii = makeNode(IndexInfo);
	int			i;
	int			numKeys;

	/* ----------------
	 *	count the number of keys, and copy them into the IndexInfo
	 * ----------------
	 */
	numKeys = 0;
	for (i = 0; i < INDEX_MAX_KEYS &&
		 indexStruct->indkey[i] != InvalidAttrNumber; i++)
	{
		ii->ii_KeyAttrNumbers[i] = indexStruct->indkey[i];
		numKeys++;
	}
	ii->ii_NumKeyAttrs = numKeys;

	/* ----------------
	 *	Handle functional index.
	 *
	 *	If we have a functional index then the number of
	 *	attributes defined in the index must be 1 (the function's
	 *	single return value).  Otherwise it's same as number of keys.
	 * ----------------
	 */
	ii->ii_FuncOid = indexStruct->indproc;

	if (OidIsValid(indexStruct->indproc))
	{
		ii->ii_NumIndexAttrs = 1;
		/* Do a lookup on the function, too */
		fmgr_info(indexStruct->indproc, & ii->ii_FuncInfo);
	}
	else
		ii->ii_NumIndexAttrs = numKeys;

	/* ----------------
	 *	If partial index, convert predicate into expression nodetree
	 * ----------------
	 */
	if (VARSIZE(&indexStruct->indpred) != 0)
	{
		char	   *predString;

		predString = DatumGetCString(DirectFunctionCall1(textout,
									 PointerGetDatum(&indexStruct->indpred)));
		ii->ii_Predicate = stringToNode(predString);
		pfree(predString);
	}
	else
		ii->ii_Predicate = NULL;

	/* Other info */
	ii->ii_Unique = indexStruct->indisunique;

	return ii;
}

/* ----------------
 *		FormIndexDatum
 *			Construct Datum[] and nullv[] arrays for a new index tuple.
 *
 *	indexInfo		Info about the index
 *	heapTuple		Heap tuple for which we must prepare an index entry
 *	heapDescriptor	tupledesc for heap tuple
 *	resultCxt		Temporary memory context for any palloc'd datums created
 *	datum			Array of index Datums (output area)
 *	nullv			Array of is-null indicators (output area)
 *
 * For largely historical reasons, we don't actually call index_formtuple()
 * here, we just prepare its input arrays datum[] and nullv[].
 * ----------------
 */
void
FormIndexDatum(IndexInfo *indexInfo,
			   HeapTuple heapTuple,
			   TupleDesc heapDescriptor,
			   MemoryContext resultCxt,
			   Datum *datum,
			   char *nullv)
{
	MemoryContext oldContext;
	int			i;
	Datum		iDatum;
	bool		isNull;

	oldContext = MemoryContextSwitchTo(resultCxt);

	if (OidIsValid(indexInfo->ii_FuncOid))
	{
		/* ----------------
		 *	Functional index --- compute the single index attribute
		 * ----------------
		 */
		FunctionCallInfoData	fcinfo;
		bool					anynull = false;

		MemSet(&fcinfo, 0, sizeof(fcinfo));
		fcinfo.flinfo = &indexInfo->ii_FuncInfo;
		fcinfo.nargs = indexInfo->ii_NumKeyAttrs;

		for (i = 0; i < indexInfo->ii_NumKeyAttrs; i++)
		{
			fcinfo.arg[i] = heap_getattr(heapTuple,
										 indexInfo->ii_KeyAttrNumbers[i],
										 heapDescriptor,
										 &fcinfo.argnull[i]);
			anynull |= fcinfo.argnull[i];
		}
		if (indexInfo->ii_FuncInfo.fn_strict && anynull)
		{
			/* force a null result for strict function */
			iDatum = (Datum) 0;
			isNull = true;
		}
		else
		{
			iDatum = FunctionCallInvoke(&fcinfo);
			isNull = fcinfo.isnull;
		}
		datum[0] = iDatum;
		nullv[0] = (isNull) ? 'n' : ' ';
	}
	else
	{
		/* ----------------
		 *	Plain index --- for each attribute we need from the heap tuple,
		 *	get the attribute and stick it into the datum and nullv arrays.
		 * ----------------
		 */
		for (i = 0; i < indexInfo->ii_NumIndexAttrs; i++)
		{
			iDatum = heap_getattr(heapTuple,
								  indexInfo->ii_KeyAttrNumbers[i],
								  heapDescriptor,
								  &isNull);
			datum[i] = iDatum;
			nullv[i] = (isNull) ? 'n' : ' ';
		}
	}

	MemoryContextSwitchTo(oldContext);
}


/* --------------------------------------------
 *		Lock class info for update
 * --------------------------------------------
 */
static bool
LockClassinfoForUpdate(Oid relid, HeapTuple rtup,
					   Buffer *buffer, bool confirmCommitted)
{
	HeapTuple	classTuple;
	bool		test;
	Relation	relationRelation;

	/*
	 * NOTE: get and hold RowExclusiveLock on pg_class, because caller will
	 * probably modify the rel's pg_class tuple later on.
	 */
	relationRelation = heap_openr(RelationRelationName, RowExclusiveLock);
	classTuple = SearchSysCache(RELOID, PointerGetDatum(relid),
								0, 0, 0);
	if (!HeapTupleIsValid(classTuple))
	{
		heap_close(relationRelation, NoLock);
		return false;
	}
	rtup->t_self = classTuple->t_self;
	ReleaseSysCache(classTuple);

	while (1)
	{
		ItemPointerData	tidsave;

		ItemPointerCopy(&(rtup->t_self), &tidsave);
		test = heap_mark4update(relationRelation, rtup, buffer);
		switch (test)
		{
			case HeapTupleSelfUpdated:
			case HeapTupleMayBeUpdated:
				break;
			case HeapTupleUpdated:
				ReleaseBuffer(*buffer);
				if (!ItemPointerEquals(&(rtup->t_self), &tidsave))
					continue;
			default:
				elog(ERROR, "LockClassinfoForUpdate couldn't lock relid %u", relid);
				return false;
		}
		break;
	}
	RelationInvalidateHeapTuple(relationRelation, rtup);
	if (confirmCommitted)
	{
		HeapTupleHeader th = rtup->t_data;

		if (!(th->t_infomask & HEAP_XMIN_COMMITTED))
			elog(ERROR, "The tuple isn't committed");
		if (th->t_infomask & HEAP_XMAX_COMMITTED)
			if (!(th->t_infomask & HEAP_MARKED_FOR_UPDATE))
				elog(ERROR, "The tuple is already deleted");
	}
	heap_close(relationRelation, NoLock);
	return true;
}

/* ---------------------------------------------
 *		Indexes of the relation active ?
 * ---------------------------------------------
 */
bool
IndexesAreActive(Oid relid, bool confirmCommitted)
{
	HeapTupleData tuple;
	Relation	indexRelation;
	Buffer		buffer;
	HeapScanDesc scan;
	ScanKeyData entry;
	bool		isactive;

	if (!LockClassinfoForUpdate(relid, &tuple, &buffer, confirmCommitted))
		elog(ERROR, "IndexesAreActive couldn't lock %u", relid);
	if (((Form_pg_class) GETSTRUCT(&tuple))->relkind != RELKIND_RELATION &&
		((Form_pg_class) GETSTRUCT(&tuple))->relkind != RELKIND_TOASTVALUE)
		elog(ERROR, "relation %u isn't an indexable relation", relid);
	isactive = ((Form_pg_class) GETSTRUCT(&tuple))->relhasindex;
	ReleaseBuffer(buffer);
	if (isactive)
		return isactive;
	indexRelation = heap_openr(IndexRelationName, AccessShareLock);
	ScanKeyEntryInitialize(&entry, 0, Anum_pg_index_indrelid,
						   F_OIDEQ, ObjectIdGetDatum(relid));
	scan = heap_beginscan(indexRelation, false, SnapshotNow,
						  1, &entry);
	if (!heap_getnext(scan, 0))
		isactive = true;
	heap_endscan(scan);
	heap_close(indexRelation, AccessShareLock);
	return isactive;
}

/* ----------------
 *		set relhasindex of relation's pg_class entry
 *
 * NOTE: an important side-effect of this operation is that an SI invalidation
 * message is sent out to all backends --- including me --- causing relcache
 * entries to be flushed or updated with the new hasindex data.
 * Therefore, we execute the update even if relhasindex has the right value
 * already.  Possible future improvement: skip the disk update and just send
 * an SI message in that case.
 * ----------------
 */
void
setRelhasindex(Oid relid, bool hasindex)
{
	Relation	pg_class;
	HeapTuple	tuple;
	HeapScanDesc pg_class_scan = NULL;

	/*
	 * Find the tuple to update in pg_class.
	 */
	pg_class = heap_openr(RelationRelationName, RowExclusiveLock);

#ifdef	OLD_FILE_NAMING
	if (!IsIgnoringSystemIndexes())
#else
	if (!IsIgnoringSystemIndexes() && (!IsReindexProcessing() || pg_class->rd_rel->relhasindex))
#endif /* OLD_FILE_NAMING */
	{
		tuple = SearchSysCacheCopy(RELOID,
								   ObjectIdGetDatum(relid),
								   0, 0, 0);
	}
	else
	{
		ScanKeyData key[1];

		ScanKeyEntryInitialize(&key[0], 0,
							   ObjectIdAttributeNumber,
							   F_OIDEQ,
							   ObjectIdGetDatum(relid));

		pg_class_scan = heap_beginscan(pg_class, 0, SnapshotNow, 1, key);
		tuple = heap_getnext(pg_class_scan, 0);
	}

	if (!HeapTupleIsValid(tuple))
	{
		if (pg_class_scan)
			heap_endscan(pg_class_scan);
		heap_close(pg_class, RowExclusiveLock);
		elog(ERROR, "setRelhasindex: cannot find relation %u in pg_class",
			 relid);
	}

	/* ----------------
	 *	Update hasindex in pg_class.
	 * ----------------
	 */
	if (pg_class_scan)
		LockBuffer(pg_class_scan->rs_cbuf, BUFFER_LOCK_EXCLUSIVE);
	((Form_pg_class) GETSTRUCT(tuple))->relhasindex = hasindex;
	if (pg_class_scan)
		LockBuffer(pg_class_scan->rs_cbuf, BUFFER_LOCK_UNLOCK);

	if (pg_class_scan)
	{
		/* Write the modified tuple in-place */
		WriteNoReleaseBuffer(pg_class_scan->rs_cbuf);
		/* Send out shared cache inval if necessary */
		if (!IsBootstrapProcessingMode())
			RelationInvalidateHeapTuple(pg_class, tuple);
		BufferSync();
	}
	else
	{
		simple_heap_update(pg_class, &tuple->t_self, tuple);

		/* Keep the catalog indices up to date */
		if (!IsIgnoringSystemIndexes())
		{
			Relation	idescs[Num_pg_class_indices];

			CatalogOpenIndices(Num_pg_class_indices, Name_pg_class_indices,
							   idescs);
			CatalogIndexInsert(idescs, Num_pg_class_indices, pg_class, tuple);
			CatalogCloseIndices(Num_pg_class_indices, idescs);
		}
	}

	if (!pg_class_scan)
		heap_freetuple(tuple);
	else
		heap_endscan(pg_class_scan);

	heap_close(pg_class, RowExclusiveLock);
}

#ifndef OLD_FILE_NAMING
void
setNewRelfilenode(Relation relation)
{
	Relation	pg_class, idescs[Num_pg_class_indices];
	Oid		newrelfilenode;
	bool		in_place_update = false;
	HeapTupleData 	lockTupleData;
	HeapTuple 	classTuple = NULL;
	Buffer		buffer;
	RelationData	workrel;
	
	Assert(!IsSystemRelationName(NameStr(relation->rd_rel->relname)) || relation->rd_rel->relkind == RELKIND_INDEX);

	pg_class = heap_openr(RelationRelationName, RowExclusiveLock);
	 /* Fetch and lock the classTuple associated with this relation */
	if (!LockClassinfoForUpdate(relation->rd_id, &lockTupleData, &buffer, true))
		elog(ERROR, "setNewRelfilenode impossible to lock class tuple");
	if (IsIgnoringSystemIndexes())
		in_place_update = true;
	/* Allocate a new relfilenode */
	newrelfilenode = newoid();
	/* update pg_class tuple with new relfilenode */
	if (!in_place_update)
	{
		classTuple = heap_copytuple(&lockTupleData);
		ReleaseBuffer(buffer);
		((Form_pg_class) GETSTRUCT(classTuple))->relfilenode = newrelfilenode;
		simple_heap_update(pg_class, &classTuple->t_self, classTuple);
	}
	/* schedule unlinking old relfilenode */
	smgrunlink(DEFAULT_SMGR, relation);
	/* cleanup pg_internal.init if necessary */
	if (relation->rd_isnailed)
		unlink(RELCACHE_INIT_FILENAME);
	/* create another storage file. Is it a little ugly ? */
	memcpy((char *) &workrel, relation, sizeof(RelationData));
	workrel.rd_node.relNode = newrelfilenode;
	heap_storage_create(&workrel);
	smgrclose(DEFAULT_SMGR, &workrel);
	/* update pg_class tuple with new relfilenode in place */
	if (in_place_update)
	{
		classTuple = &lockTupleData;
		/* Send out shared cache inval if necessary */
		if (!IsBootstrapProcessingMode())
			RelationInvalidateHeapTuple(pg_class, classTuple);
		/* Update the buffer in-place */
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		((Form_pg_class) GETSTRUCT(classTuple))->relfilenode = newrelfilenode;
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		WriteBuffer(buffer);
		BufferSync();
	}
	/* Keep the catalog indices up to date */
	if (!in_place_update && pg_class->rd_rel->relhasindex)
	{
		CatalogOpenIndices(Num_pg_class_indices, Name_pg_class_indices,
							   idescs);
		CatalogIndexInsert(idescs, Num_pg_class_indices, pg_class, classTuple);
		CatalogCloseIndices(Num_pg_class_indices, idescs);
	}
	heap_close(pg_class, NoLock);
	if (!in_place_update)
		heap_freetuple(classTuple);
	/* Make sure the relfilenode change */
	CommandCounterIncrement();
}
#endif /* OLD_FILE_NAMING */

/* ----------------
 *		UpdateStats
 * ----------------
 */
void
UpdateStats(Oid relid, long reltuples)
{
	Relation	whichRel;
	Relation	pg_class;
	HeapTuple	tuple;
	HeapTuple	newtup;
	long		relpages;
	int			i;
	Form_pg_class rd_rel;
	Relation	idescs[Num_pg_class_indices];
	Datum		values[Natts_pg_class];
	char		nulls[Natts_pg_class];
	char		replace[Natts_pg_class];
	HeapScanDesc pg_class_scan = NULL;
	bool		in_place_upd;

	/* ----------------
	 * This routine handles updates for both the heap and index relation
	 * statistics.	In order to guarantee that we're able to *see* the index
	 * relation tuple, we bump the command counter id here.  The index
	 * relation tuple was created in the current transaction.
	 * ----------------
	 */
	CommandCounterIncrement();

	/* ----------------
	 * CommandCounterIncrement() flushes invalid cache entries, including
	 * those for the heap and index relations for which we're updating
	 * statistics.	Now that the cache is flushed, it's safe to open the
	 * relation again.	We need the relation open in order to figure out
	 * how many blocks it contains.
	 * ----------------
	 */

	/*
	 * Can't use heap_open here since we don't know if it's an index...
	 */
	whichRel = RelationIdGetRelation(relid);

	if (!RelationIsValid(whichRel))
		elog(ERROR, "UpdateStats: cannot open relation id %u", relid);

	/* Grab lock to be held till end of xact (probably redundant...) */
	LockRelation(whichRel, ShareLock);

	/* ----------------
	 * Find the RELATION relation tuple for the given relation.
	 * ----------------
	 */
	pg_class = heap_openr(RelationRelationName, RowExclusiveLock);

#ifdef	OLD_FILE_NAMING
	in_place_upd = (IsReindexProcessing() || IsBootstrapProcessingMode());
#else
	in_place_upd = (IsIgnoringSystemIndexes() || IsReindexProcessing());
#endif /* OLD_FILE_NAMING */

	if (!in_place_upd)
	{
		tuple = SearchSysCacheCopy(RELOID,
								   ObjectIdGetDatum(relid),
								   0, 0, 0);
	}
	else
	{
		ScanKeyData key[1];

		ScanKeyEntryInitialize(&key[0], 0,
							   ObjectIdAttributeNumber,
							   F_OIDEQ,
							   ObjectIdGetDatum(relid));

		pg_class_scan = heap_beginscan(pg_class, 0, SnapshotNow, 1, key);
		tuple = heap_getnext(pg_class_scan, 0);
	}

	if (!HeapTupleIsValid(tuple))
	{
		if (pg_class_scan)
			heap_endscan(pg_class_scan);
		heap_close(pg_class, RowExclusiveLock);
		elog(ERROR, "UpdateStats: cannot find relation %u in pg_class",
			 relid);
	}

	/* ----------------
	 * Figure values to insert.
	 *
	 * If we found zero tuples in the scan, do NOT believe it; instead put
	 * a bogus estimate into the statistics fields.  Otherwise, the common
	 * pattern "CREATE TABLE; CREATE INDEX; insert data" leaves the table
	 * with zero size statistics until a VACUUM is done.  The optimizer will
	 * generate very bad plans if the stats claim the table is empty when
	 * it is actually sizable.	See also CREATE TABLE in heap.c.
	 * ----------------
	 */
	relpages = RelationGetNumberOfBlocks(whichRel);

	if (reltuples == 0)
	{
		if (relpages == 0)
		{
			/* Bogus defaults for a virgin table, same as heap.c */
			reltuples = 1000;
			relpages = 10;
		}
		else if (whichRel->rd_rel->relkind == RELKIND_INDEX && relpages <= 2)
		{
			/* Empty index, leave bogus defaults in place */
			reltuples = 1000;
		}
		else
			reltuples = relpages * NTUPLES_PER_PAGE(whichRel->rd_rel->relnatts);
	}

	/*
	 * We shouldn't have to do this, but we do...  Modify the reldesc in
	 * place with the new values so that the cache contains the latest
	 * copy.
	 */
	whichRel->rd_rel->relpages = relpages;
	whichRel->rd_rel->reltuples = reltuples;

	/* ----------------
	 *	Update statistics in pg_class.
	 * ----------------
	 */
	if (in_place_upd)
	{
		/*
		 * At bootstrap time, we don't need to worry about concurrency or
		 * visibility of changes, so we cheat.  Also cheat if REINDEX.
		 */
		rd_rel = (Form_pg_class) GETSTRUCT(tuple);
		LockBuffer(pg_class_scan->rs_cbuf, BUFFER_LOCK_EXCLUSIVE);
		rd_rel->relpages = relpages;
		rd_rel->reltuples = reltuples;
		LockBuffer(pg_class_scan->rs_cbuf, BUFFER_LOCK_UNLOCK);
		WriteNoReleaseBuffer(pg_class_scan->rs_cbuf);
		if (!IsBootstrapProcessingMode())
			RelationInvalidateHeapTuple(pg_class, tuple);
	}
	else
	{
		/* During normal processing, must work harder. */

		for (i = 0; i < Natts_pg_class; i++)
		{
			nulls[i] = heap_attisnull(tuple, i + 1) ? 'n' : ' ';
			replace[i] = ' ';
			values[i] = (Datum) NULL;
		}

		replace[Anum_pg_class_relpages - 1] = 'r';
		values[Anum_pg_class_relpages - 1] = (Datum) relpages;
		replace[Anum_pg_class_reltuples - 1] = 'r';
		values[Anum_pg_class_reltuples - 1] = (Datum) reltuples;
		newtup = heap_modifytuple(tuple, pg_class, values, nulls, replace);
		simple_heap_update(pg_class, &tuple->t_self, newtup);
		if (!IsIgnoringSystemIndexes())
		{
			CatalogOpenIndices(Num_pg_class_indices, Name_pg_class_indices, idescs);
			CatalogIndexInsert(idescs, Num_pg_class_indices, pg_class, newtup);
			CatalogCloseIndices(Num_pg_class_indices, idescs);
		}
		heap_freetuple(newtup);
	}

	if (!pg_class_scan)
		heap_freetuple(tuple);
	else
		heap_endscan(pg_class_scan);

	heap_close(pg_class, RowExclusiveLock);
	/* Cheating a little bit since we didn't open it with heap_open... */
	heap_close(whichRel, NoLock);
}


/* ----------------
 *		DefaultBuild
 *
 * NB: this routine is dead code, and likely always has been, because
 * there are no access methods that don't supply their own ambuild procedure.
 *
 * Anyone want to wager whether it would actually work if executed?
 * ----------------
 */
static void
DefaultBuild(Relation heapRelation,
			 Relation indexRelation,
			 IndexInfo *indexInfo,
			 Node *oldPred,
			 IndexStrategy indexStrategy) /* not used */
{
	HeapScanDesc scan;
	HeapTuple	heapTuple;
	TupleDesc	heapDescriptor;
	Datum		datum[INDEX_MAX_KEYS];
	char		nullv[INDEX_MAX_KEYS];
	long		reltuples,
				indtuples;
	Node	   *predicate = indexInfo->ii_Predicate;
#ifndef OMIT_PARTIAL_INDEX
	TupleTable	tupleTable;
	TupleTableSlot *slot;
#endif
	ExprContext *econtext;
	InsertIndexResult insertResult;

	/* ----------------
	 *	more & better checking is needed
	 * ----------------
	 */
	Assert(OidIsValid(indexRelation->rd_rel->relam));	/* XXX */

	heapDescriptor = RelationGetDescr(heapRelation);

	/*
	 * If this is a predicate (partial) index, we will need to evaluate
	 * the predicate using ExecQual, which requires the current tuple to
	 * be in a slot of a TupleTable.  In addition, ExecQual must have an
	 * ExprContext referring to that slot.	Here, we initialize dummy
	 * TupleTable and ExprContext objects for this purpose. --Nels, Feb 92
	 *
	 * We construct the ExprContext anyway since we need a per-tuple
	 * temporary memory context for function evaluation -- tgl July 00
	 */
#ifndef OMIT_PARTIAL_INDEX
	if (predicate != NULL || oldPred != NULL)
	{
		tupleTable = ExecCreateTupleTable(1);
		slot = ExecAllocTableSlot(tupleTable);
		ExecSetSlotDescriptor(slot, heapDescriptor);
	}
	else
	{
		tupleTable = NULL;
		slot = NULL;
	}
	econtext = MakeExprContext(slot, TransactionCommandContext);
#else
	econtext = MakeExprContext(NULL, TransactionCommandContext);
#endif	 /* OMIT_PARTIAL_INDEX */

	/* ----------------
	 *	Ok, begin our scan of the base relation.
	 * ----------------
	 */
	scan = heap_beginscan(heapRelation, /* relation */
						  0,	/* start at end */
						  SnapshotNow,	/* seeself */
						  0,	/* number of keys */
						  (ScanKey) NULL);		/* scan key */

	reltuples = indtuples = 0;

	/* ----------------
	 *	for each tuple in the base relation, we create an index
	 *	tuple and add it to the index relation.  We keep a running
	 *	count of the number of tuples so that we can update pg_class
	 *	with correct statistics when we're done building the index.
	 * ----------------
	 */
	while (HeapTupleIsValid(heapTuple = heap_getnext(scan, 0)))
	{
		MemoryContextReset(econtext->ecxt_per_tuple_memory);

		reltuples++;

#ifndef OMIT_PARTIAL_INDEX
		/*
		 * If oldPred != NULL, this is an EXTEND INDEX command, so skip
		 * this tuple if it was already in the existing partial index
		 */
		if (oldPred != NULL)
		{
			slot->val = heapTuple;
			if (ExecQual((List *) oldPred, econtext, false))
			{
				indtuples++;
				continue;
			}
		}

		/*
		 * Skip this tuple if it doesn't satisfy the partial-index
		 * predicate
		 */
		if (predicate != NULL)
		{
			slot->val = heapTuple;
			if (!ExecQual((List *) predicate, econtext, false))
				continue;
		}
#endif	 /* OMIT_PARTIAL_INDEX */

		indtuples++;

		/* ----------------
		 *	FormIndexDatum fills in its datum and null parameters
		 *	with attribute information taken from the given heap tuple.
		 * ----------------
		 */
		FormIndexDatum(indexInfo,
					   heapTuple,
					   heapDescriptor,
					   econtext->ecxt_per_tuple_memory,
					   datum,
					   nullv);

		insertResult = index_insert(indexRelation, datum, nullv,
									&(heapTuple->t_self), heapRelation);

		if (insertResult)
			pfree(insertResult);
	}

	heap_endscan(scan);

#ifndef OMIT_PARTIAL_INDEX
	if (predicate != NULL || oldPred != NULL)
	{
		ExecDropTupleTable(tupleTable, true);
	}
#endif	 /* OMIT_PARTIAL_INDEX */
	FreeExprContext(econtext);

	/*
	 * Since we just counted the tuples in the heap, we update its stats
	 * in pg_class to guarantee that the planner takes advantage of the
	 * index we just created.  But, only update statistics during normal
	 * index definitions, not for indices on system catalogs created
	 * during bootstrap processing.  We must close the relations before
	 * updating statistics to guarantee that the relcache entries are
	 * flushed when we increment the command counter in UpdateStats(). But
	 * we do not release any locks on the relations; those will be held
	 * until end of transaction.
	 */
	if (IsNormalProcessingMode())
	{
		Oid			hrelid = RelationGetRelid(heapRelation);
		Oid			irelid = RelationGetRelid(indexRelation);

		heap_close(heapRelation, NoLock);
		index_close(indexRelation);
		UpdateStats(hrelid, reltuples);
		UpdateStats(irelid, indtuples);
		if (oldPred != NULL)
		{
			if (indtuples == reltuples)
				predicate = NULL;
			UpdateIndexPredicate(irelid, oldPred, predicate);
		}
	}
}

/* ----------------
 *		index_build
 * ----------------
 */
void
index_build(Relation heapRelation,
			Relation indexRelation,
			IndexInfo *indexInfo,
			Node *oldPred)
{
	RegProcedure procedure;

	/* ----------------
	 *	sanity checks
	 * ----------------
	 */
	Assert(RelationIsValid(indexRelation));
	Assert(PointerIsValid(indexRelation->rd_am));

	procedure = indexRelation->rd_am->ambuild;

	/* ----------------
	 *	use the access method build procedure if supplied, else default.
	 * ----------------
	 */
	if (RegProcedureIsValid(procedure))
		OidFunctionCall5(procedure,
						 PointerGetDatum(heapRelation),
						 PointerGetDatum(indexRelation),
						 PointerGetDatum(indexInfo),
						 PointerGetDatum(oldPred),
						 PointerGetDatum(RelationGetIndexStrategy(indexRelation)));
	else
		DefaultBuild(heapRelation,
					 indexRelation,
					 indexInfo,
					 oldPred,
					 RelationGetIndexStrategy(indexRelation));
}

/*
 * IndexGetRelation: given an index's relation OID, get the OID of the
 * relation it is an index on.	Uses the system cache.
 */
static Oid
IndexGetRelation(Oid indexId)
{
	HeapTuple	tuple;
	Form_pg_index index;
	Oid			result;

	tuple = SearchSysCache(INDEXRELID,
						   ObjectIdGetDatum(indexId),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "IndexGetRelation: can't find index id %u",
			 indexId);
	index = (Form_pg_index) GETSTRUCT(tuple);
	Assert(index->indexrelid == indexId);

	result = index->indrelid;
	ReleaseSysCache(tuple);
	return result;
}

/* ---------------------------------
 * activate_index -- activate/deactivate the specified index.
 *		Note that currently PostgreSQL doesn't hold the
 *		status per index
 * ---------------------------------
 */
static bool
activate_index(Oid indexId, bool activate, bool inplace)
{
	if (!activate)				/* Currently does nothing */
		return true;
	return reindex_index(indexId, false, inplace);
}

/* --------------------------------
 * reindex_index - This routine is used to recreate an index
 * --------------------------------
 */
bool
reindex_index(Oid indexId, bool force, bool inplace)
{
	Relation	iRel,
				indexRelation,
				heapRelation;
	ScanKeyData entry;
	HeapScanDesc scan;
	HeapTuple	indexTuple,
				classTuple;
	IndexInfo  *indexInfo;
	Oid			heapId,
				accessMethodId;
	bool		old;

	/* ----------------
	 *	REINDEX within a transaction block is dangerous, because
	 *	if the transaction is later rolled back we have no way to
	 *	undo truncation of the index's physical file.  Disallow it.
	 * ----------------
	 */
	if (IsTransactionBlock())
		elog(ERROR, "REINDEX cannot run inside a BEGIN/END block");

	old = SetReindexProcessing(true);

	/* Scan pg_index to find the index's pg_index entry */
	indexRelation = heap_openr(IndexRelationName, AccessShareLock);
	ScanKeyEntryInitialize(&entry, 0, Anum_pg_index_indexrelid, F_OIDEQ,
						   ObjectIdGetDatum(indexId));
	scan = heap_beginscan(indexRelation, false, SnapshotNow, 1, &entry);
	indexTuple = heap_getnext(scan, 0);
	if (!HeapTupleIsValid(indexTuple))
		elog(ERROR, "reindex_index: index %u not found in pg_index", indexId);

	/* Get OID of index's parent table */
	heapId = ((Form_pg_index) GETSTRUCT(indexTuple))->indrelid;
	/* Fetch info needed for index_build */
	indexInfo = BuildIndexInfo(indexTuple);

	/* Complete the scan and close pg_index */
	heap_endscan(scan);
	heap_close(indexRelation, AccessShareLock);

	/* Fetch the classTuple associated with this index */
	classTuple = SearchSysCache(RELOID,
								ObjectIdGetDatum(indexId),
								0, 0, 0);
	if (!HeapTupleIsValid(classTuple))
		elog(ERROR, "reindex_index: index %u not found in pg_class", indexId);
	accessMethodId = ((Form_pg_class) GETSTRUCT(classTuple))->relam;
	ReleaseSysCache(classTuple);

	/* Open our index relation */
	heapRelation = heap_open(heapId, ExclusiveLock);
	if (heapRelation == NULL)
		elog(ERROR, "reindex_index: can't open heap relation");
	iRel = index_open(indexId);
	if (iRel == NULL)
		elog(ERROR, "reindex_index: can't open index relation");

#ifndef OLD_FILE_NAMING
	if (!inplace)
		setNewRelfilenode(iRel);
#endif /* OLD_FILE_NAMING */
	/* Obtain exclusive lock on it, just to be sure */
	LockRelation(iRel, AccessExclusiveLock);

	if (inplace)
	{
		/*
	 	 * Release any buffers associated with this index.	If they're dirty,
	 	 * they're just dropped without bothering to flush to disk.
	 	 */
		DropRelationBuffers(iRel);

		/* Now truncate the actual data and set blocks to zero */
		smgrtruncate(DEFAULT_SMGR, iRel, 0);
		iRel->rd_nblocks = 0;
	}

	/* Initialize the index and rebuild */
	InitIndexStrategy(indexInfo->ii_NumIndexAttrs, iRel, accessMethodId);
	index_build(heapRelation, iRel, indexInfo, NULL);

	/*
	 * index_build will close both the heap and index relations (but not
	 * give up the locks we hold on them).  So we're done.
	 */

	SetReindexProcessing(old);

	return true;
}

/*
 * ----------------------------
 * activate_indexes_of_a_table
 *	activate/deactivate indexes of the specified table.
 * ----------------------------
 */
bool
activate_indexes_of_a_table(Oid relid, bool activate)
{
	if (IndexesAreActive(relid, true))
	{
		if (!activate)
			setRelhasindex(relid, false);
		else
			return false;
	}
	else
	{
		if (activate)
			reindex_relation(relid, false);
		else
			return false;
	}
	return true;
}

/* --------------------------------
 * reindex_relation - This routine is used to recreate indexes
 * of a relation.
 * --------------------------------
 */
bool
reindex_relation(Oid relid, bool force)
{
	Relation	indexRelation;
	ScanKeyData entry;
	HeapScanDesc scan;
	HeapTuple	indexTuple;
	bool		old,
				reindexed;

	bool	deactivate_needed, overwrite, upd_pg_class_inplace;
#ifdef OLD_FILE_NAMING
	overwrite = upd_pg_class_inplace = deactivate_needed = true;	
#else
	Relation rel;
	overwrite = upd_pg_class_inplace = deactivate_needed = false;	
	/*
 	 * avoid heap_update() pg_class tuples while processing
 	 * reindex for pg_class. 
 	 */
	if (IsIgnoringSystemIndexes())
		upd_pg_class_inplace = true;
	/*
	 * ignore the indexes of the target system relation while processing
	 * reindex.
	 */ 
	rel = RelationIdGetRelation(relid);
	if (!IsIgnoringSystemIndexes() && IsSystemRelationName(NameStr(rel->rd_rel->relname)))
		deactivate_needed = true;
#ifndef	ENABLE_REINDEX_NAILED_RELATIONS
	/* 
 	 * nailed relations are never updated.
 	 * We couldn't keep the consistency between the relation
 	 * descriptors and pg_class tuples.
 	 */
	if (rel->rd_isnailed)
	{
		if (IsIgnoringSystemIndexes())
		{
			overwrite = true;
			deactivate_needed = true;
		}
		else
			elog(ERROR, "the target relation %u is nailed", relid);
	}
#endif /* ENABLE_REINDEX_NAILED_RELATIONS */
	RelationClose(rel);
#endif /* OLD_FILE_NAMING */
	old = SetReindexProcessing(true);
	if (deactivate_needed)
	{
		if (IndexesAreActive(relid, upd_pg_class_inplace))
		{
			if (!force)
			{
				SetReindexProcessing(old);
				return false;
			}
			activate_indexes_of_a_table(relid, false);
			CommandCounterIncrement();
		}
	}

	indexRelation = heap_openr(IndexRelationName, AccessShareLock);
	ScanKeyEntryInitialize(&entry, 0, Anum_pg_index_indrelid,
						   F_OIDEQ, ObjectIdGetDatum(relid));
	scan = heap_beginscan(indexRelation, false, SnapshotNow,
						  1, &entry);
	reindexed = false;
	while (HeapTupleIsValid(indexTuple = heap_getnext(scan, 0)))
	{
		Form_pg_index index = (Form_pg_index) GETSTRUCT(indexTuple);

		if (activate_index(index->indexrelid, true, overwrite))
			reindexed = true;
		else
		{
			reindexed = false;
			break;
		}
	}
	heap_endscan(scan);
	heap_close(indexRelation, AccessShareLock);
	if (reindexed)
	/*
	 * Ok,we could use the reindexed indexes of the target
	 * system relation now.
	 */
	{ 
		if (deactivate_needed)
		{
			if (!overwrite && relid == RelOid_pg_class)
			{
				/* 
				 * For pg_class, relhasindex should be set
				 * to true here in place.
				 */
				setRelhasindex(relid, true);
				CommandCounterIncrement();
				/* 
				 * However the following setRelhasindex()
				 * is needed to keep consistency with WAL.
				 */
			}
			setRelhasindex(relid, true);
		}
	}
	SetReindexProcessing(old);

	return reindexed;
}

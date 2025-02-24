#include "postgres.h"

#include "access/heapam.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/pg_class_d.h"
#include "catalog/pg_zstd_dictionaries.h"
#include "catalog/pg_zstd_dictionaries_d.h"
#include "catalog/pg_type.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/hsearch.h"
#include "access/toast_compression.h"
#include "utils/array.h"
#include "funcapi.h"
#include "utils/attoptcache.h"
#include "parser/analyze.h"
#include "utils/jsonb.h"
#include "common/hashfn.h"
#include "nodes/makefuncs.h"
#include "access/reloptions.h"
#include "miscadmin.h"
#include "access/genam.h"
#include "executor/tuptable.h"
#include "access/htup_details.h"
#include "access/sdir.h"
#include "utils/lsyscache.h"
#include <stdbool.h>

#include <zstd.h>
#include <zstd_errors.h>
#include <zdict.h>

/* Configuration Macros */
#define TARG_ROWS 30000
#define INITIAL_HASH_CAPACITY 4096
#define INITIAL_SAMPLE_CAPACITY 1024
#define SAMPLE_GROWTH_FACTOR 2

typedef struct SampleEntry SampleEntry;
typedef struct SampleCollector SampleCollector;

/* Structure to store a sample entry */
struct SampleEntry
{
    void   *data;   /* Pointer to sample data */
    size_t  size;   /* Size of the sample */
    int     count;  /* Frequency count */
};

/* Structure to collect samples along with a hash table for deduplication */
struct SampleCollector
{
    SampleEntry **samples;     /* Dynamic array of pointers to SampleEntry */
    int          sample_count; /* Number of collected samples */
    int          capacity;     /* Allocated capacity for samples */
    HTAB        *hash;         /* Hash table for deduplication & frequency tracking */
};

/* Function prototypes */
static SampleCollector *create_sample_collector(void);
static inline uint32 sample_hash_fn(const void *key, Size keysize);
static int sample_match(const void *key1, const void *key2, Size keysize);
static void add_sample(SampleCollector *collector, void *data, size_t size);
static void extract_samples_from_jsonb(Jsonb *jsonb, SampleCollector *collector);
static void filter_samples_under_size_limit(SampleCollector *collector,
                                             SampleEntry **filtered_samples,
                                             int *filtered_count);
static int compare_sample_entry(const void *a, const void *b);
static Oid GetNewDictId(Relation relation, Oid indexId, AttrNumber dictIdColumn);
static void process_datum_for_samples(Datum value, Oid typid, SampleCollector *collector);
/* ----------------------------------------------------------------
 * Zstandard Dictionary Catalog and Training Functions
 * ----------------------------------------------------------------
 */

/*
 * build_zstd_dictionary
 *   1) Validate that the given (relid, attno) can have a Zstd dictionary
 *   2) Call the type-specific dictionary builder (returns sample data in memory)
 *   3) Train a dictionary via ZDICT_trainFromBuffer()
 *   4) Insert dictionary into pg_zstd_dictionaries
 *   5) Update pg_attribute.attoptions with dictid
 */
bool
build_zstd_dictionary(Oid relid, AttrNumber attno)
{
    /* ----
     * 1) Open user relation just to verify it's a normal table and has Zstd compression
     * ----
     */
    Relation    rel = table_open(relid, AccessShareLock);
    if (rel->rd_rel->relkind != RELKIND_RELATION)
    {
        table_close(rel, AccessShareLock);
        return false; /* not a regular table */
    }

    /* If the column doesn't use Zstd, nothing to do */
    TupleDesc       tupleDesc = RelationGetDescr(rel);
    Form_pg_attribute att = TupleDescAttr(tupleDesc, attno - 1);
    if (att->attcompression != TOAST_ZSTD_COMPRESSION)
    {
        table_close(rel, AccessShareLock);
        return false;
    }

    /* Check attoptions for user-requested dictionary size, etc. */
    AttributeOpts  *attopt = get_attribute_options(relid, attno);
    if (attopt && attopt->zstd_dict_size == 0)
    {
        /* user explicitly says "no dictionary needed" */
        table_close(rel, AccessShareLock);
        return false;
    }

    /*
     * 2) Look up the type's custom dictionary builder function
     *    We'll call it to get sample data. Then we can close 'rel'
     *    because we don't need it open to do the actual Zdict training.
     */
    HeapTuple typeTup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(att->atttypid));
    if (!HeapTupleIsValid(typeTup))
    {
        table_close(rel, AccessShareLock);
        elog(ERROR, "cache lookup failed for type %u", att->atttypid);
    }
    Form_pg_type typeForm = (Form_pg_type) GETSTRUCT(typeTup);

    /* Get the base type */
    Oid baseTypeOid = get_element_type(typeForm->oid);
    Oid train_func = InvalidOid;

    if (OidIsValid(baseTypeOid))
    {
        /* It's an array type: get the base type's training function */
        HeapTuple baseTypeTup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(baseTypeOid));
        if (!HeapTupleIsValid(baseTypeTup))
            ereport(ERROR,
                    (errmsg("Cache lookup failed for base type %u", baseTypeOid)));

        Form_pg_type baseTypeForm = (Form_pg_type) GETSTRUCT(baseTypeTup);
        train_func = baseTypeForm->typebuildzstddictionary;
        ReleaseSysCache(baseTypeTup);
    }
    else
        train_func = typeForm->typebuildzstddictionary;

    /* If the type does not supply a builder, skip */
    if (!OidIsValid(train_func))
    {
        ReleaseSysCache(typeTup);
        table_close(rel, AccessShareLock);
        return false;
    }

    /* Call the type-specific builder. It should return ZstdTrainingData */
    Datum dictDatum = OidFunctionCall2(train_func,
                                       PointerGetDatum(rel),  /* pass relation ref */
                                       Int16GetDatum(attno));
    ReleaseSysCache(typeTup);

    /* We no longer need the user relation open */
    table_close(rel, AccessShareLock);

    ZstdTrainingData *dict = (ZstdTrainingData *) DatumGetPointer(dictDatum);
    if (!dict || dict->nitems == 0)
        return false;

    /*
     * 3) Train a Zstd dictionary in-memory.
     */
    char   *samples_buffer      = dict->sample_buffer;
    size_t *sample_sizes        = dict->sample_sizes;
    int     nitems              = dict->nitems;

    uint32  dictionary_size = (!attopt ? DEFAULT_ZSTD_DICTIONARY_SIZE
                                       : (uint32) attopt->zstd_dict_size);

    /* Allocate buffer for dictionary training result */
    void  *dict_data = palloc(dictionary_size);
    size_t dict_size = ZDICT_trainFromBuffer(dict_data,
                                             dictionary_size,
                                             samples_buffer,
                                             sample_sizes,
                                             nitems);
    if (ZDICT_isError(dict_size))
    {
        elog(LOG, "Zstd dictionary training failed: %s",
                  ZDICT_getErrorName(dict_size));
        pfree(dict_data);
        return false;
    }

    /*
     * Finalize dictionary to embed a custom dictID.
     * E.g. We can get a new Oid from pg_zstd_dictionaries here *before*
     * we build the bytea. But for brevity, let's do it after opening
     * pg_zstd_dictionaries (so we can do the dictionary insertion + ID assignment
     * in one place).
     */

    /*
     * 4) Insert dictionary into pg_zstd_dictionaries
     *    We do that by opening the ZstdDictionariesRelation, generating
     *    a new dictid, forming a tuple, and inserting it.
     */

    /* Open the catalog relation with ShareRowExclusiveLock */
    Relation catalogRel = table_open(ZstdDictionariesRelationId, ShareRowExclusiveLock);
    TupleDesc  catTupDesc = RelationGetDescr(catalogRel);

    Oid dictid = GetNewDictId(catalogRel, ZstdDictidIndexId, Anum_pg_zstd_dictionaries_dictid);
    
    /*
     * finalize to embed that 'dictid' in the dictionary itself
     */
    {
        ZDICT_params_t fParams;
        memset(&fParams, 0, sizeof(fParams));
        fParams.dictID = dictid;   /* embed the newly allocated Oid as the dictID */

        size_t final_dict_size = ZDICT_finalizeDictionary(
                                     dict_data,          /* output buffer (reuse) */
                                     dictionary_size,    /* capacity */
                                     dict_data,          /* input dictionary from train step */
                                     dict_size,          /* size from train step */
                                     samples_buffer,
                                     sample_sizes,
                                     nitems,
                                     fParams);
        if (ZDICT_isError(final_dict_size))
        {
            elog(LOG, "Zstd dictionary finalization failed: %s",
                      ZDICT_getErrorName(final_dict_size));
            pfree(dict_data);
            table_close(catalogRel, ShareRowExclusiveLock);
            return false;
        }

        /* Now copy that finalized dictionary into a bytea. */
        {
            bytea *dict_bytea = (bytea *) palloc(VARHDRSZ + final_dict_size);
            SET_VARSIZE(dict_bytea, VARHDRSZ + final_dict_size);
            memcpy(VARDATA(dict_bytea), dict_data, final_dict_size);

            /* We’ll store this bytea in pg_zstd_dictionaries. */
            Datum       values[Natts_pg_zstd_dictionaries];
            bool        nulls[Natts_pg_zstd_dictionaries];
            HeapTuple   tup;

            MemSet(values, 0, sizeof(values));
            MemSet(nulls, false, sizeof(nulls));

            values[Anum_pg_zstd_dictionaries_dictid - 1] = ObjectIdGetDatum(dictid);
            values[Anum_pg_zstd_dictionaries_dict   - 1] = PointerGetDatum(dict_bytea);

            tup = heap_form_tuple(catTupDesc, values, nulls);
            CatalogTupleInsert(catalogRel, tup);
            heap_freetuple(tup);

            pfree(dict_bytea);
        }

        pfree(dict_data);
    }

    /*
     * 5) Update pg_attribute.attoptions with "zstd_dictid" => dictid
     *    so the column knows which dictionary to use at compression time.
     */
    {
        Relation    attRel = table_open(AttributeRelationId, RowExclusiveLock);
        HeapTuple   atttup, newtuple;
        Datum       attoptionsDatum, newOptions;
        bool        isnull;
        Datum       repl_val[Natts_pg_attribute];
        bool        repl_null[Natts_pg_attribute];
        bool        repl_repl[Natts_pg_attribute];

        atttup = SearchSysCacheAttNum(relid, attno);
        if (!HeapTupleIsValid(atttup))
            ereport(ERROR,
                    (errcode(ERRCODE_UNDEFINED_COLUMN),
                     errmsg("column number %d of relation \"%u\" does not exist",
                            attno, relid)));

        /* Build new attoptions with zstd_dictid=... */
        DefElem *def = makeDefElem("zstd_dictid",
                                   (Node *) makeString(psprintf("%u", dictid)),
                                   -1);
        List *optlist = list_make1(def);

        attoptionsDatum = SysCacheGetAttr(ATTNUM, atttup,
                                          Anum_pg_attribute_attoptions,
                                          &isnull);
        newOptions = transformRelOptions(isnull ? (Datum)0 : attoptionsDatum,
                                         optlist,
                                         NULL, NULL,
                                         false, false);
        /* Validate them (throws error if invalid) */
        (void) attribute_reloptions(newOptions, true);

        MemSet(repl_null, false, sizeof(repl_null));
        MemSet(repl_repl, false, sizeof(repl_repl));

        if (newOptions != (Datum) 0)
            repl_val[Anum_pg_attribute_attoptions - 1] = newOptions;
        else
            repl_null[Anum_pg_attribute_attoptions - 1] = true;

        repl_repl[Anum_pg_attribute_attoptions - 1] = true;

        newtuple = heap_modify_tuple(atttup,
                                     RelationGetDescr(attRel),
                                     repl_val,
                                     repl_null,
                                     repl_repl);

        CatalogTupleUpdate(attRel, &newtuple->t_self, newtuple);
        heap_freetuple(newtuple);

        ReleaseSysCache(atttup);

        table_close(attRel, NoLock);
    }

    /**
     * Done inserting dictionary and updating attribute.
     * Unlock the table (locks remain held until transaction commit)
     */
    table_close(catalogRel, NoLock);

    return true;
}

Datum
build_zstd_dict_relation_column(PG_FUNCTION_ARGS)
{
    Oid         relid = PG_GETARG_OID(0);
    AttrNumber  attno = PG_GETARG_INT32(1);

    bool success = build_zstd_dictionary(relid, attno);
    PG_RETURN_BOOL(success);
}

/* -------------------------- Main Function -------------------------- */

/*
 * Generate a Zstandard dictionary from table sample data.
 * This version only processes JSONB values (and arrays of JSONB).
 * Any non-JSONB values (or arrays whose element type is not a base JSONB)
 * are ignored.
 */
Datum
jsonb_generate_zstd_dictionary(PG_FUNCTION_ARGS)
{
    ZstdTrainingData *dict;
    double totalrows, totaldeadrows;
    int num_sampled;
    Relation rel = (Relation) PG_GETARG_POINTER(0);
    AttrNumber attnum = PG_GETARG_INT16(1);
    TupleDesc tupleDesc = RelationGetDescr(rel);
    HeapTuple *sample_rows = palloc(TARG_ROWS * sizeof(HeapTuple));

    /* Acquire sample rows from the table */
    num_sampled = acquire_sample_rows(rel, 0, sample_rows,
                                      TARG_ROWS, &totalrows, &totaldeadrows);

    /* Create a sample collector to accumulate JSONB string samples */
    SampleCollector *collector = create_sample_collector();

    /* Get the type OID of the target column */
    Oid colType = TupleDescAttr(tupleDesc, attnum - 1)->atttypid;

    for (int i = 0; i < num_sampled; i++)
    {
        CHECK_FOR_INTERRUPTS();
        Datum value;
        bool isnull;
        value = heap_getattr(sample_rows[i], attnum, tupleDesc, &isnull);
        if (!isnull)
        {
            /* Process the datum recursively.
             * Only JSONB (and arrays whose element type is JSONB base) will be sampled.
             */
            process_datum_for_samples(value, colType, collector);
        }
    }

    /* Filter samples and build the training dictionary */
    SampleEntry *filtered_samples;
    int filtered_sample_count;
    filter_samples_under_size_limit(collector, &filtered_samples, &filtered_sample_count);

    size_t total_samples_size = 0;
    for (int i = 0; i < filtered_sample_count; i++)
        total_samples_size += filtered_samples[i].size;

    char *samples_buffer = palloc(total_samples_size);
    size_t *sample_sizes = palloc(filtered_sample_count * sizeof(size_t));
    size_t current_offset = 0;
    for (int i = 0; i < filtered_sample_count; i++)
    {
        memcpy(samples_buffer + current_offset, filtered_samples[i].data, filtered_samples[i].size);
        sample_sizes[i] = filtered_samples[i].size;
        current_offset += filtered_samples[i].size;
    }

    dict = palloc(sizeof(ZstdTrainingData));
    dict->sample_buffer = samples_buffer;
    dict->sample_sizes  = sample_sizes;
    dict->nitems        = filtered_sample_count;

    PG_RETURN_POINTER(dict);
}

/*
 * Recursive helper to process a Datum value.
 *
 * This function handles two cases:
 *
 * 1. If the type is an array type, it deconstructs the array and
 *    recursively processes each element. It only recurses if the array’s
 *    element type is a base type and is JSONB.
 *
 * 2. Otherwise, if the type is a base type and equals JSONBOID,
 *    it calls extract_samples_from_jsonb() to extract the sample.
 *
 * Any value not meeting these conditions is ignored.
 */
static void
process_datum_for_samples(Datum value, Oid typid, SampleCollector *collector)
{
    /* Check if typid is an array type */
    Oid elementType = get_element_type(typid);
    if (OidIsValid(elementType))
    {
        /* Look up the element type to ensure it is a base type and JSONB */
        HeapTuple eltTup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(elementType));
        if (!HeapTupleIsValid(eltTup))
            ereport(ERROR,
                    (errmsg("cache lookup failed for type %u", elementType)));
        Form_pg_type eltForm = (Form_pg_type) GETSTRUCT(eltTup);
        bool isBase = (eltForm->typtype == TYPTYPE_BASE);
        ReleaseSysCache(eltTup);

        if (!isBase || elementType != JSONBOID)
        {
            /* Not a base JSONB; ignore the array */
            return;
        }

        /* Process the array of JSONB values */
        ArrayType *arr = DatumGetArrayTypeP(value);
        int16 elmlen;
        bool elmbyval;
        char elmalign;
        Datum   *elem_values;
        bool    *elem_nulls;
        int      nelems;
        get_typlenbyvalalign(elementType, &elmlen, &elmbyval, &elmalign);

        deconstruct_array(arr,
                          elementType,
                          elmlen, elmbyval, elmalign,
                          &elem_values, &elem_nulls, &nelems);

        for (int i = 0; i < nelems; i++)
        {
            if (!elem_nulls[i])
                process_datum_for_samples(elem_values[i], elementType, collector);
        }
    }
    else
    {
        /* Not an array type; check if the type is a base JSONB */
        HeapTuple typeTup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
        if (!HeapTupleIsValid(typeTup))
            ereport(ERROR,
                    (errmsg("cache lookup failed for type %u", typid)));
        Form_pg_type typeForm = (Form_pg_type) GETSTRUCT(typeTup);
        bool isBase = (typeForm->typtype == TYPTYPE_BASE);
        ReleaseSysCache(typeTup);

        if (isBase && typid == JSONBOID)
        {
            Jsonb *jsonb = DatumGetJsonbP(value);
            extract_samples_from_jsonb(jsonb, collector);
        }
        /* Otherwise, ignore this value */
    }
}

/* -------------------------- Utility Functions -------------------------- */

/*
 * Hash function for SampleEntry using the sample's data.
 */
static inline uint32
sample_hash_fn(const void *key, Size keysize)
{
    const SampleEntry *entry = (const SampleEntry *) key;
    return DatumGetUInt32(hash_any((unsigned char *) entry->data, entry->size));
}

/*
 * Match function for SampleEntry.
 * Returns 0 if the two entries have the same size and identical data.
 */
static int
sample_match(const void *key1, const void *key2, Size keysize)
{
    const SampleEntry *entry1 = (const SampleEntry *) key1;
    const SampleEntry *entry2 = (const SampleEntry *) key2;
    if (entry1->size != entry2->size)
        return 1;  /* Different sizes: not a match */
    return memcmp(entry1->data, entry2->data, entry1->size);
}

/*
 * Create and initialize a SampleCollector.
 */
static SampleCollector *
create_sample_collector(void)
{
    HASHCTL hash_ctl = {0};
    SampleCollector *collector = palloc(sizeof(SampleCollector));

    collector->samples = NULL;
    collector->sample_count = 0;
    collector->capacity = 0;

    hash_ctl.keysize = sizeof(SampleEntry);
    hash_ctl.entrysize = sizeof(SampleEntry);
    hash_ctl.hash = sample_hash_fn;
    hash_ctl.match = sample_match;
    hash_ctl.hcxt = CurrentMemoryContext;

    collector->hash = hash_create("SampleEntry Hash", INITIAL_HASH_CAPACITY,
                                  &hash_ctl,
                                  HASH_ELEM | HASH_FUNCTION | HASH_COMPARE | HASH_CONTEXT);
    return collector;
}

/*
 * Add a sample to the collector.
 * If an identical sample already exists (via the hash table), increment its count
 * and free the new data; otherwise, store the hash table pointer.
 */
static void
add_sample(SampleCollector *collector, void *data, size_t size)
{
    SampleEntry temp_entry = { .data = data, .size = size, .count = 1 };
    bool found;
    SampleEntry *hash_entry = hash_search(collector->hash, &temp_entry, HASH_ENTER, &found);

    if (found)
    {
        /* Duplicate found: increment count and free the new allocation */
        hash_entry->count++;
        pfree(data);
    }
    else
    {
        /* Initialize the new hash table entry */
        hash_entry->data = data;
        hash_entry->size = size;
        hash_entry->count = 1;

        /* Ensure the samples array is allocated */
        if (collector->samples == NULL)
        {
            collector->capacity = INITIAL_SAMPLE_CAPACITY;
            collector->samples = palloc(collector->capacity * sizeof(SampleEntry *));
        }
        else if (collector->sample_count >= collector->capacity)
        {
            collector->capacity *= SAMPLE_GROWTH_FACTOR;
            collector->samples = repalloc(collector->samples,
                                          collector->capacity * sizeof(SampleEntry *));
        }
        /* Store the pointer from the hash table */
        collector->samples[collector->sample_count++] = hash_entry;
    }
}

/*
 * Extract string samples from a JSONB value.
 * Iterates through the JSONB structure and, for each string value, extracts
 * the data and adds it to the collector.
 */
static void
extract_samples_from_jsonb(Jsonb *jsonb, SampleCollector *collector)
{
    JsonbIterator *it = JsonbIteratorInit(&jsonb->root);
    JsonbValue value;
    JsonbIteratorToken token;

    while ((token = JsonbIteratorNext(&it, &value, false)) != WJB_DONE)
    {
        if (token == WJB_KEY || token == WJB_VALUE || token == WJB_ELEM)
        {
            switch(value.type)
            {
                case jbvString:
                {
                    size_t size = value.val.string.len;
                    void *data = palloc(size);
                    memcpy(data, value.val.string.val, size);
                    add_sample(collector, data, size);
                    break;
                }
                default:
                    break;
            }
        }
    }
}

/*
 * Comparison function for sorting SampleEntry pointers.
 * Sorts primarily by descending frequency (count) and then by descending size.
 */
static int
compare_sample_entry(const void *a, const void *b)
{
    const SampleEntry *entry1 = *(const SampleEntry **) a;
    const SampleEntry *entry2 = *(const SampleEntry **) b;

    if (entry2->count != entry1->count)
        return (int)(entry2->count - entry1->count);
    if (entry1->size < entry2->size)
        return 1;
    if (entry1->size > entry2->size)
        return -1;
    return 0;
}

/*
 * Filter collected samples without exceeding MaxAllocSize.
 * Sorts the samples in place and then selects as many as possible.
 * The function returns an array of SampleEntry (by value) and the count.
 */
static void
filter_samples_under_size_limit(SampleCollector *collector,
                                SampleEntry **filtered_samples,
                                int *filtered_count)
{
    size_t cumulative_size = 0;
    int count = 0;

    /* Sort the array of pointers in place */
    qsort(collector->samples, collector->sample_count, sizeof(SampleEntry *), compare_sample_entry);

    /* Allocate output array for filtered samples */
    *filtered_samples = palloc(collector->sample_count * sizeof(SampleEntry));
    for (int i = 0; i < collector->sample_count; i++)
    {
        CHECK_FOR_INTERRUPTS();
        SampleEntry *entry = collector->samples[i];
        if (cumulative_size + entry->size > MaxAllocSize)
            break;
        (*filtered_samples)[count++] = *entry;
        cumulative_size += entry->size;
    }
    *filtered_count = count;
}

/*
 * Acquire a new unique DictId for a relation.
 *
 * Assumes the relation is already locked with ShareRowExclusiveLock,
 * ensuring that concurrent transactions cannot generate duplicate DictIds.
 */
static Oid
GetNewDictId(Relation relation, Oid indexId, AttrNumber dictIdColumn)
{
    Relation indexRel = index_open(indexId, AccessShareLock);
    Oid maxDictId = InvalidDictId;
    SysScanDesc scan;
    HeapTuple tuple;

    /* Retrieve the maximum existing DictId by scanning in reverse order */
    scan = systable_beginscan_ordered(relation, indexRel, SnapshotAny, 0, NULL);
    tuple = systable_getnext_ordered(scan, BackwardScanDirection);
    if (HeapTupleIsValid(tuple))
    {
        Datum value;
        bool isNull;

        value = heap_getattr(tuple, dictIdColumn, RelationGetDescr(relation), &isNull);
        if (!isNull)
            maxDictId = DatumGetObjectId(value);
    }
    systable_endscan(scan);

    Oid newDictId = maxDictId + 1;
    Assert(newDictId != InvalidDictId);

    /* Check that the new DictId is indeed unique */
    ScanKeyData key;
    ScanKeyInit(&key,
                dictIdColumn,
                BTEqualStrategyNumber, F_OIDEQ,
                ObjectIdGetDatum(newDictId));

    scan = systable_beginscan(relation, indexRel->rd_id, true,
                              SnapshotAny, 1, &key);
    bool collision = HeapTupleIsValid(systable_getnext(scan));
    systable_endscan(scan);

    if (collision)
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("unexpected collision for new DictId %ld", newDictId)));

    return newDictId;
}

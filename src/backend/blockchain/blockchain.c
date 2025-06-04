#include "postgres.h"
#include "blockchain.h"

#include "access/htup_details.h"
#include "access/tableam.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "common/sha2.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "parser/parser.h"
#include "parser/parse_type.h"
#include "storage/lock.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"

static const BlockchainColumnDef system_cols[BLOCKCHAIN_COLS] = {
    {"prev_hash", BYTEAOID, -1, false},
    {"curr_hash", BYTEAOID, -1, true},
    {"timestamp", TIMESTAMPOID, -1, true}
};

void
AddBlockchainSystemColumns(CreateStmt *stmt)
{
    ListCell *lc;
    elog(NOTICE, "Injecting blockchain system columns");

    
    /* Check if system columns already exist */
    foreach(lc, stmt->tableElts)
    {
        ColumnDef *col = (ColumnDef *) lfirst(lc);
        if (IsA(col, ColumnDef))
        {
            for (int i = 0; i < BLOCKCHAIN_COLS; i++)
            {
                if (strcmp(col->colname, system_cols[i].colname) == 0)
                {
                    ereport(ERROR,
                            (errcode(ERRCODE_DUPLICATE_COLUMN),
                             errmsg("column \"%s\" is reserved for blockchain system use",
                                    col->colname)));
                }
            }
        }
    }

    /* Add system columns */
    for (int i = 0; i < BLOCKCHAIN_COLS; i++)
    {
        ColumnDef *col = makeNode(ColumnDef);
        col->colname = pstrdup(system_cols[i].colname);
        col->typeName = makeTypeNameFromOid(system_cols[i].coltype, 
                                          system_cols[i].typmod);
        col->is_not_null = system_cols[i].not_null;
        col->storage = 'p'; /* plain storage */
        col->collClause = NULL;
        col->constraints = NIL;

        stmt->tableElts = lappend(stmt->tableElts, col);
    }
}

bool
IsBlockchainTable(Oid relid)
{
    HeapTuple   tuple;
    Form_pg_class relform;
    bool        is_blockchain = false;

    tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
    if (!HeapTupleIsValid(tuple))
        return false;

    relform = (Form_pg_class) GETSTRUCT(tuple);
    is_blockchain = (relform->relkind == RELKIND_BLOCKCHAIN_TABLE);

    ReleaseSysCache(tuple);
    return is_blockchain;
}

void
ProcessBlockchainInsert(TupleTableSlot *slot, Relation rel)
{
    bytea      *prev_hash = NULL;
    bytea      *curr_hash;
    HeapTuple   prev_tuple;

    /* Get previous row's hash */
    prev_tuple = get_last_row(rel);
    if (HeapTupleIsValid(prev_tuple))
    {
        bool        isnull;
        Datum       hash_datum;
        int         attnum = GetBlockchainColumnAttnum(rel, BLOCKCHAIN_CURR_HASH);

        hash_datum = heap_getattr(prev_tuple, attnum,
                                 RelationGetDescr(rel), &isnull);
        if (!isnull)
            prev_hash = DatumGetByteaPCopy(hash_datum);
        
        heap_freetuple(prev_tuple);
    }

    /* Compute new hash */
    curr_hash = ComputeBlockchainHash(slot, prev_hash);

    /* Inject system values into the slot */
    inject_system_values(slot, rel, prev_hash, curr_hash);

    /* Cleanup */
    if (prev_hash)
        pfree(prev_hash);
    pfree(curr_hash);
}

bytea *
ComputeBlockchainHash(TupleTableSlot *slot, bytea *prev_hash)
{
    StringInfoData buf;
    bytea       *result;
    int         i;
    bool        is_system_col = false;

    initStringInfo(&buf);

    /* Ensure slot is materialized */
    slot_getallattrs(slot);

    /* Serialize all user columns (skip system columns) */
    for (i = 0; i < slot->tts_tupleDescriptor->natts; i++)
    {
        Form_pg_attribute attr = TupleDescAttr(slot->tts_tupleDescriptor, i);
        
        /* Skip dropped columns and system columns */
        if (attr->attisdropped || attr->attnum < 0)
            continue;

        /* Skip blockchain system columns */
        
        for (int j = 0; j < BLOCKCHAIN_COLS; j++)
        {
            if (strcmp(NameStr(attr->attname), system_cols[j].colname) == 0)
            {
                is_system_col = true;
                break;
            }
        }
        if (is_system_col)
            continue;

        /* Serialize column value */
        if (!slot->tts_isnull[i])
        {
            Oid         typoutput;
            bool        typisvarlena;
            char       *outputstr;

            getTypeOutputInfo(attr->atttypid, &typoutput, &typisvarlena);
            outputstr = OidOutputFunctionCall(typoutput, slot->tts_values[i]);
            appendStringInfo(&buf, "%s", outputstr);
            appendStringInfoChar(&buf, '\0'); /* Null-terminate for consistency */
            pfree(outputstr);   
        }
        else
        {
            appendStringInfo(&buf, "\\N");
            appendStringInfoChar(&buf, '\0'); 
        }
    }

    /* Include previous hash if exists */
    if (prev_hash)
        appendBinaryStringInfo(&buf, VARDATA_ANY(prev_hash), VARSIZE_ANY_EXHDR(prev_hash));

    /* Compute SHA-256 hash */
    result = (bytea *) palloc(VARHDRSZ + PG_SHA256_DIGEST_LENGTH);
    SET_VARSIZE(result, VARHDRSZ + PG_SHA256_DIGEST_LENGTH);

    if (!pg_sha256_buf(buf.data, buf.len, VARDATA(result)))
        elog(ERROR, "failed to compute SHA-256 hash");

    pfree(buf.data);
    return result;
}

int
GetBlockchainColumnAttnum(Relation rel, const char *colname)
{
    int attnum = get_attnum(RelationGetRelid(rel), colname);
    if (attnum == InvalidAttrNumber)
        elog(ERROR, "blockchain system column \"%s\" not found", colname);
    return attnum;
}

bool
ValidateBlockchainTuple(HeapTuple tuple, TupleDesc tupdesc)
{
    bool        isnull;
    Datum       curr_hash;
    int         attnum;
    Relation    rel = NULL;
    
    if(tuple->t_tableOid == InvalidOid)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("invalid tuple: missing table OID")));

    /* Check current hash exists and is valid */
    attnum = GetBlockchainColumnAttnum(rel, BLOCKCHAIN_CURR_HASH);
    curr_hash = heap_getattr(tuple, attnum, tupdesc, &isnull);

    RelationClose(rel);

    if (isnull)
        return false;

    /* Verify hash length */
    if (VARSIZE_ANY_EXHDR(DatumGetByteaP(curr_hash)) != PG_SHA256_DIGEST_LENGTH)
        return false;

    return true;
}

void
CheckBlockchainPermissions(Relation rel)
{
    AclResult aclresult = pg_class_aclcheck(RelationGetRelid(rel), GetUserId(), ACL_INSERT);
    if (aclresult != ACLCHECK_OK)
        aclcheck_error(aclresult, OBJECT_TABLE, RelationGetRelationName(rel));
}

void
BlockUnauthorizedColumnUpdate(Relation rel, Bitmapset *modified_attrs)
{
    for (int i = 0; i < BLOCKCHAIN_COLS; i++)
    {
        int attnum = GetBlockchainColumnAttnum(rel, system_cols[i].colname);
        if (bms_is_member(attnum, modified_attrs))
            ereport(ERROR,
                    (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                     errmsg("cannot modify system column \"%s\"",
                            system_cols[i].colname)));
    }
}

/* Local helper functions */
HeapTuple
get_last_row(Relation rel)
{
    TableScanDesc scan;
    TupleTableSlot *slot;
    HeapTuple tuple = NULL;
    
    slot = table_slot_create(rel, NULL);
    scan = table_beginscan(rel, GetActiveSnapshot(), 0, NULL);
    
    if (table_scan_getnextslot(scan, ForwardScanDirection, slot))
    {
        tuple = ExecCopySlotHeapTuple(slot);
    }
    
    table_endscan(scan);
    ExecDropSingleTupleTableSlot(slot);
    
    return tuple;
}

void
inject_system_values(TupleTableSlot *slot, Relation rel,
                   bytea *prev_hash, bytea *curr_hash)
{
    Datum       timestamp;
    int         prev_attnum, curr_attnum, ts_attnum;

    /* Get attribute numbers */
    prev_attnum = GetBlockchainColumnAttnum(rel, BLOCKCHAIN_PREV_HASH);
    curr_attnum = GetBlockchainColumnAttnum(rel, BLOCKCHAIN_CURR_HASH);
    ts_attnum = GetBlockchainColumnAttnum(rel, BLOCKCHAIN_TIMESTAMP);

    /* Prepare timestamp */
    timestamp = TimestampGetDatum(GetCurrentTimestamp());

    /* Ensure slot is materialized */
    slot_getallattrs(slot);

    /* Inject values (convert to 0-based indexing) */
    slot->tts_values[prev_attnum - 1] = prev_hash ? PointerGetDatum(prev_hash) : (Datum) 0;
    slot->tts_isnull[prev_attnum - 1] = (prev_hash == NULL);

    slot->tts_values[curr_attnum - 1] = PointerGetDatum(curr_hash);
    slot->tts_isnull[curr_attnum - 1] = false;

    slot->tts_values[ts_attnum - 1] = timestamp;
    slot->tts_isnull[ts_attnum - 1] = false;

    /* Mark slot as modified */
    ExecStoreVirtualTuple(slot);
}


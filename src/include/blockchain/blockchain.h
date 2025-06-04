#ifndef BLOCKCHAIN_H
#define BLOCKCHAIN_H

#include "postgres.h"
#include "nodes/parsenodes.h"
#include "utils/rel.h"
#include "executor/tuptable.h"
#include "access/htup.h"

/* Blockchain system column names */
#define BLOCKCHAIN_PREV_HASH    "prev_hash"
#define BLOCKCHAIN_CURR_HASH    "curr_hash"
#define BLOCKCHAIN_TIMESTAMP    "timestamp"
#define BLOCKCHAIN_COLS         3

/* Blockchain column definition */
typedef struct BlockchainColumnDef
{
    const char *colname;
    Oid         coltype;
    int32       typmod;
    bool        not_null;
    bool        is_system;
} BlockchainColumnDef;

/* Function prototypes */

/* DDL functions */
extern void AddBlockchainSystemColumns(CreateStmt *stmt);
extern bool IsBlockchainTable(Oid relid);

/* DML functions */
extern void ProcessBlockchainInsert(TupleTableSlot *slot, Relation rel);
extern bytea *ComputeBlockchainHash(TupleTableSlot *slot, bytea *prev_hash);

/* Utility functions */
extern int GetBlockchainColumnAttnum(Relation rel, const char *colname);
extern bool ValidateBlockchainTuple(HeapTuple tuple, TupleDesc tupdesc);
extern void CheckBlockchainPermissions(Relation rel);
extern void BlockUnauthorizedColumnUpdate(Relation rel, Bitmapset *modified_attrs);

/* Local helper functions - declare as extern since they're used across files */
extern HeapTuple get_last_row(Relation rel);
extern void inject_system_values(TupleTableSlot *slot, Relation rel,
                               bytea *prev_hash, bytea *curr_hash);

#endif /* BLOCKCHAIN_H */
/*
 * This PostgreSQL extension allows you to read the raw data pages of a table.
 * It is designed for educational and testing purposes to understand how PostgreSQL
 * handles dirty reads and internal storage mechanisms. 
 *
 * **DISCLAIMER:** This code is not intended for production use. It bypasses PostgreSQL's
 * MVCC (Multi-Version Concurrency Control) mechanisms and can read "dirty" data that is
 * not yet committed or has been rolled back. Use this code only in a low-critical environment.
 *
 * To compile and install this extension:
 * 1. Ensure you have PostgreSQL server development packages installed.
 * 2. Place this code in a file named `page_reader.c`.
 * 3. Compile the code using the following commands:
 *      $ gcc -fPIC -I/usr/local/pgsql/include/server -c page_reader.c
 *      $ gcc -shared -o page_reader.so page_reader.o
 * 4. Copy the compiled `page_reader.so` to the PostgreSQL library directory.
 * 5. Load the extension in PostgreSQL using the following command:
 *      $ CREATE FUNCTION read_data_pages(schema_name TEXT, table_name TEXT) RETURNS TEXT AS 'path/to/page_reader.so', 'read_data_pages' LANGUAGE C;
 * 
 * **Usage:**
 * To use this function, call it with the schema and table name as arguments:
 *      $ SELECT read_data_pages('public', 'your_table_name');
 *
 * Note: Ensure you have the necessary permissions to read the table and access the underlying files.
 */

#include "postgres.h"
#include "fmgr.h"
#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "utils/builtins.h"
#include "storage/fd.h" // For file operations
#include "access/xlog.h" // For BLCKSZ
#include "storage/bufmgr.h"
#include "access/heapam.h"
#include "miscadmin.h" // For MyDatabaseId
#include "access/htup.h"
#include "utils/rel.h"
#include "catalog/pg_class.h"
#include "catalog/namespace.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(read_data_pages);

Datum read_data_pages(PG_FUNCTION_ARGS);

Datum
read_data_pages(PG_FUNCTION_ARGS)
{
    // Extract schema and table name from the arguments
    text *schema_name = PG_GETARG_TEXT_P(0);
    text *table_name = PG_GETARG_TEXT_P(1);
    char *schema = text_to_cstring(schema_name);
    char *table = text_to_cstring(table_name);
    
    // Variables for table details
    char path[MAXPGPATH];
    int fd;
    char buffer[BLCKSZ];
    int blockNum = 0;
    StringInfoData result;
    Oid relid;
    
    // Initialize result string
    initStringInfo(&result);

    elog(INFO, "Starting read_data_pages");

    PG_TRY();
    {
        // Create RangeVar for schema and table
        elog(INFO, "Creating RangeVar for schema: %s, table: %s", schema, table);
        RangeVar *relrv = makeRangeVar(schema, table, -1);

        elog(INFO, "Checking if RangeVar is NULL");
        if (relrv == NULL)
        {
            elog(ERROR, "Failed to create RangeVar");
            PG_RETURN_NULL();
        }

        elog(INFO, "Getting relid for RangeVar");
        // Get the relation Oid
        relid = RangeVarGetRelid(relrv, AccessShareLock, false);

        elog(INFO, "Checking if relid is valid");
        if (!OidIsValid(relid))
        {
            elog(ERROR, "Relation \"%s.%s\" does not exist", schema, table);
            PG_RETURN_NULL();
        }

        // Construct the file path using the determined relid
        snprintf(path, sizeof(path), "base/%u/%u", MyDatabaseId, relid);

        elog(INFO, "Reading table file: %s", path);

        // Open the table file
        fd = OpenTransientFile(path, O_RDONLY | PG_BINARY);
        if (fd < 0)
        {
            elog(ERROR, "Could not open file \"%s\": %m", path);
            PG_RETURN_NULL();
        }

        // Read each block from the file
        while (read(fd, buffer, BLCKSZ) == BLCKSZ)
        {
            Page page = (Page) buffer;
            OffsetNumber offnum, maxoff;

            maxoff = PageGetMaxOffsetNumber(page);

            // Iterate through each tuple in the block
            for (offnum = FirstOffsetNumber; offnum <= maxoff; offnum = OffsetNumberNext(offnum))
            {
                ItemId itemid = PageGetItemId(page, offnum);
                if (ItemIdIsUsed(itemid) && !ItemIdIsDead(itemid))
                {
                    HeapTupleData tuple;
                    tuple.t_data = (HeapTupleHeader) PageGetItem(page, itemid);
                    tuple.t_len = ItemIdGetLength(itemid);
                    ItemPointerSet(&(tuple.t_self), blockNum, offnum);

                    // Append the tuple data to the result
                    appendStringInfo(&result, "Tuple (%u,%u): ", blockNum, offnum);

                    int natts = HeapTupleHeaderGetNatts(tuple.t_data);
                    for (int i = 0; i < natts; i++)
                    {
                        bool isnull;
                        Datum d = fastgetattr(&tuple, i + 1, tuple.t_data->t_infomask2, &isnull);
                        if (isnull)
                        {
                            appendStringInfo(&result, "NULL");
                        }
                        else
                        {
                            char *value = TextDatumGetCString(d);
                            appendStringInfo(&result, "%s", value);
                            pfree(value);
                        }
                        if (i < natts - 1)
                            appendStringInfo(&result, ", ");
                    }
                    appendStringInfo(&result, "\n");
                }
            }
            blockNum++;
        }

        // Close the table file
        CloseTransientFile(fd);
    }
    PG_CATCH();
    {
        // Handle the error
        ErrorData *errdata;
        MemoryContext oldcontext;

        oldcontext = MemoryContextSwitchTo(CurrentMemoryContext);
        errdata = CopyErrorData();
        elog(WARNING, "Error in read_data_pages: %s", errdata->message);
        FlushErrorState();
        MemoryContextSwitchTo(oldcontext);
        PG_RE_THROW();
    }
    PG_END_TRY();

    PG_RETURN_TEXT_P(cstring_to_text(result.data));
}

/*-------------------------------------------------------------------------
 *
 * pg_tde_utils.c
 *      Utility functions.
 *
 * IDENTIFICATION
 *    contrib/pg_tde/src/pg_tde_utils.c
 *
 *-------------------------------------------------------------------------
 */

#define MAX_CONFIG_FILE_DATA_LENGTH 1024

#include "postgres.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "catalog/pg_class.h"
#include "catalog/pg_am.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "commands/defrem.h"
#include "common/pg_tde_utils.h"
#include "fmgr.h"
#include "keyring/keyring_curl.h"
#include "utils/builtins.h"
#include "unistd.h"

Oid
get_tde_table_am_oid(void)
{
    return get_table_am_oid("pg_tde", false);
}

/*
 * Returns the list of OIDs for all TDE tables in a database
 */
List*
get_all_tde_tables(void)
{
    Relation pg_class;
    SysScanDesc scan;
    HeapTuple tuple;
    List* tde_tables = NIL;
    Oid am_oid = get_tde_table_am_oid();

    /* Open the pg_class table */
    pg_class = table_open(RelationRelationId, AccessShareLock);

    /* Start a scan */
    scan = systable_beginscan(pg_class, ClassOidIndexId, true,
                              SnapshotSelf, 0, NULL);

    /* Iterate over all tuples in the table */
    while ((tuple = systable_getnext(scan)) != NULL)
    {
        Form_pg_class classForm = (Form_pg_class)GETSTRUCT(tuple);

        /* Check if the table uses the specified access method */
        if (classForm->relam == am_oid)
        {
            /* Print the name of the table */
            tde_tables = lappend_oid(tde_tables, classForm->oid);
            elog(DEBUG2, "Table %s uses the TDE access method.", NameStr(classForm->relname));
        }
    }

    /* End the scan */
    systable_endscan(scan);

    /* Close the pg_class table */
    table_close(pg_class, AccessShareLock);
    return tde_tables;
}

int
get_tde_tables_count(void)
{
    List* tde_tables = get_all_tde_tables();
    int count = list_length(tde_tables);
    list_free(tde_tables);
    return count;
}


const char *
extract_json_cstr(Datum json, const char* field_name)
{
	Datum field = DirectFunctionCall2(json_object_field_text, json, CStringGetTextDatum(field_name));
	const char* cstr = TextDatumGetCString(field);

	return cstr;
}

const char *
extract_json_option_value(Datum top_json, const char* field_name)
{
	Datum field;
	Datum field_type;
	const char* field_type_cstr;

	field = DirectFunctionCall2(json_object_field, top_json, CStringGetTextDatum(field_name));

	field_type = DirectFunctionCall1(json_typeof, field);
	field_type_cstr = TextDatumGetCString(field_type);

	if(field_type_cstr == NULL)
	{
		return NULL;
	}

	if(strcmp(field_type_cstr, "string") == 0)
	{
		return extract_json_cstr(top_json, field_name);
	}

	if(strcmp(field_type_cstr, "object") != 0)
	{
		elog(ERROR, "Unsupported type for object %s: %s", field_name, field_type_cstr);
		return NULL;
	}

	/* Now it is definitely an object */

	{
		const char* type_cstr = extract_json_cstr(field, "type");

		if(type_cstr == NULL)
		{
			elog(ERROR, "Missing type property for remote object %s", field_name);
			return NULL;
		}

		if(strncmp("remote", type_cstr, 7) == 0)
		{
			const char* url_cstr = extract_json_cstr(field, "url");

			long httpCode;
			CurlString outStr;

			if(url_cstr == NULL)
			{
				elog(ERROR, "Missing url property for remote object %s", field_name);
				return NULL;
			}

            /* Http request should return the value of the option */
			outStr.ptr = palloc0(1);
			outStr.len = 0;
			if(!curlSetupSession(url_cstr, NULL, &outStr)) 
			{
				elog(ERROR, "CURL error for remote object %s", field_name);
				return NULL;
			}
			if(curl_easy_perform(keyringCurl) != CURLE_OK)
			{
				elog(ERROR, "HTTP request error for remote object %s", field_name);
				return NULL;
			}
			if(curl_easy_getinfo(keyringCurl, CURLINFO_RESPONSE_CODE, &httpCode) != CURLE_OK)
			{
				elog(ERROR, "HTTP error for remote object %s, HTTP code %li", field_name, httpCode);
				return NULL;
			}
#if KEYRING_DEBUG
	elog(DEBUG2, "HTTP response for config [%s] '%s'", field_name, outStr->ptr != NULL ? outStr->ptr : "");
#endif
			return outStr.ptr;
		}

		if(strncmp("file", type_cstr, 5) == 0)
		{
			const char* path_cstr = extract_json_cstr(field, "path");
			FILE* f;
			char* out;

			if(path_cstr == NULL)
			{
				elog(ERROR, "Missing path property for file object %s", field_name);
				return NULL;
			}

			if(access(path_cstr, R_OK) != 0)
			{
				elog(ERROR, "The file referenced by %s doesn't exists, or is not readable to postgres: %s", field_name, path_cstr);
				return NULL;
			}

			f = fopen(path_cstr, "r");

			if(!f)
			{
				elog(ERROR, "The file referenced by %s doesn't exists, or is not readable to postgres: %s", field_name, path_cstr);
				return NULL;
			}

            /* File should contain the value for the option */
			out = palloc(MAX_CONFIG_FILE_DATA_LENGTH);
			fgets(out, MAX_CONFIG_FILE_DATA_LENGTH, f);
			out[strcspn(out, "\r\n")] = 0;

			fclose(f);

			return out;
		}

		elog(ERROR, "Unknown type for object %s: %s", field_name, type_cstr);
		return NULL;
	}
}
/*-------------------------------------------------------------------------
 *
 * json.h
 *	  Declarations for JSON data type support.
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/json.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef JSON_H
#define JSON_H

#include "lib/stringinfo.h"

typedef enum					/* type categories for datum_to_json */
{
	JSONTYPE_NULL,				/* null, so we didn't bother to identify */
	JSONTYPE_BOOL,				/* boolean (built-in types only) */
	JSONTYPE_NUMERIC,			/* numeric (ditto) */
	JSONTYPE_DATE,				/* we use special formatting for datetimes */
	JSONTYPE_TIMESTAMP,
	JSONTYPE_TIMESTAMPTZ,
	JSONTYPE_JSON,				/* JSON itself (and JSONB) */
	JSONTYPE_ARRAY,				/* array */
	JSONTYPE_COMPOSITE,			/* composite */
	JSONTYPE_CAST,				/* something with an explicit cast to JSON */
	JSONTYPE_OTHER				/* all else */
} JsonTypeCategory;

/* functions in json.c */
extern void escape_json(StringInfo buf, const char *str);
extern char *JsonEncodeDateTime(char *buf, Datum value, Oid typid,
								const int *tzp);
extern bool to_json_is_immutable(Oid typoid);
extern void json_categorize_type(Oid typoid, JsonTypeCategory *tcategory,
								 Oid *outfuncoid);
extern Datum to_json_worker(Datum val, JsonTypeCategory tcategory,
							Oid outfuncoid);
extern Datum json_build_object_worker(int nargs, Datum *args, bool *nulls,
									  Oid *types, bool absent_on_null,
									  bool unique_keys);
extern Datum json_build_array_worker(int nargs, Datum *args, bool *nulls,
									 Oid *types, bool absent_on_null);
extern bool json_validate(text *json, bool check_unique_keys, bool throw_error);

#endif							/* JSON_H */

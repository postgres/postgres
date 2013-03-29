/*-------------------------------------------------------------------------
 *
 * json.h
 *	  Declarations for JSON data type support.
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/json.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef JSON_H
#define JSON_H

#include "fmgr.h"
#include "lib/stringinfo.h"

/* functions in json.c */
extern Datum json_in(PG_FUNCTION_ARGS);
extern Datum json_out(PG_FUNCTION_ARGS);
extern Datum json_recv(PG_FUNCTION_ARGS);
extern Datum json_send(PG_FUNCTION_ARGS);
extern Datum array_to_json(PG_FUNCTION_ARGS);
extern Datum array_to_json_pretty(PG_FUNCTION_ARGS);
extern Datum row_to_json(PG_FUNCTION_ARGS);
extern Datum row_to_json_pretty(PG_FUNCTION_ARGS);
extern Datum to_json(PG_FUNCTION_ARGS);

extern Datum json_agg_transfn(PG_FUNCTION_ARGS);
extern Datum json_agg_finalfn(PG_FUNCTION_ARGS);

extern void escape_json(StringInfo buf, const char *str);

/* functions in jsonfuncs.c */
extern Datum json_object_field(PG_FUNCTION_ARGS);
extern Datum json_object_field_text(PG_FUNCTION_ARGS);
extern Datum json_array_element(PG_FUNCTION_ARGS);
extern Datum json_array_element_text(PG_FUNCTION_ARGS);
extern Datum json_extract_path(PG_FUNCTION_ARGS);
extern Datum json_extract_path_text(PG_FUNCTION_ARGS);
extern Datum json_object_keys(PG_FUNCTION_ARGS);
extern Datum json_array_length(PG_FUNCTION_ARGS);
extern Datum json_each(PG_FUNCTION_ARGS);
extern Datum json_each_text(PG_FUNCTION_ARGS);
extern Datum json_array_elements(PG_FUNCTION_ARGS);
extern Datum json_populate_record(PG_FUNCTION_ARGS);
extern Datum json_populate_recordset(PG_FUNCTION_ARGS);

#endif   /* JSON_H */

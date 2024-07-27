/*-------------------------------------------------------------------------
 *
 * json.h
 *	  Declarations for JSON data type support.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/json.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef JSON_H
#define JSON_H

#include "lib/stringinfo.h"

/* functions in json.c */
extern void escape_json(StringInfo buf, const char *str);
extern void escape_json_with_len(StringInfo buf, const char *str, int len);
extern void escape_json_text(StringInfo buf, const text *txt);
extern char *JsonEncodeDateTime(char *buf, Datum value, Oid typid,
								const int *tzp);
extern bool to_json_is_immutable(Oid typoid);
extern Datum json_build_object_worker(int nargs, const Datum *args, const bool *nulls,
									  const Oid *types, bool absent_on_null,
									  bool unique_keys);
extern Datum json_build_array_worker(int nargs, const Datum *args, const bool *nulls,
									 const Oid *types, bool absent_on_null);
extern bool json_validate(text *json, bool check_unique_keys, bool throw_error);

#endif							/* JSON_H */

/*-------------------------------------------------------------------------
 *
 * json.h
 *	  Declarations for JSON data type support.
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/json.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef JSON_H
#define JSON_H

#include "fmgr.h"

extern Datum json_in(PG_FUNCTION_ARGS);
extern Datum json_out(PG_FUNCTION_ARGS);
extern Datum json_recv(PG_FUNCTION_ARGS);
extern Datum json_send(PG_FUNCTION_ARGS);

#endif   /* XML_H */

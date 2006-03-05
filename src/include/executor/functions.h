/*-------------------------------------------------------------------------
 *
 * functions.h
 *		Declarations for execution of SQL-language functions.
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/functions.h,v 1.27 2006/03/05 15:58:56 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef FUNCTIONS_H
#define FUNCTIONS_H

#include "fmgr.h"
#include "nodes/execnodes.h"


extern Datum fmgr_sql(PG_FUNCTION_ARGS);

extern bool check_sql_fn_retval(Oid func_id, Oid rettype,
					List *queryTreeList,
					JunkFilter **junkFilter);

#endif   /* FUNCTIONS_H */

/*-------------------------------------------------------------------------
 *
 * functions.h
 *		Declarations for execution of SQL-language functions.
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/functions.h,v 1.31 2008/03/18 22:04:14 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef FUNCTIONS_H
#define FUNCTIONS_H

#include "nodes/execnodes.h"


extern Datum fmgr_sql(PG_FUNCTION_ARGS);

extern bool check_sql_fn_retval(Oid func_id, Oid rettype,
					List *queryTreeList,
					bool insertRelabels,
					JunkFilter **junkFilter);

#endif   /* FUNCTIONS_H */

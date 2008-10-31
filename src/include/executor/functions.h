/*-------------------------------------------------------------------------
 *
 * functions.h
 *		Declarations for execution of SQL-language functions.
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/functions.h,v 1.32 2008/10/31 19:37:56 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef FUNCTIONS_H
#define FUNCTIONS_H

#include "nodes/execnodes.h"
#include "tcop/dest.h"


extern Datum fmgr_sql(PG_FUNCTION_ARGS);

extern bool check_sql_fn_retval(Oid func_id, Oid rettype,
					List *queryTreeList,
					bool insertRelabels,
					JunkFilter **junkFilter);

extern DestReceiver *CreateSQLFunctionDestReceiver(void);

#endif   /* FUNCTIONS_H */

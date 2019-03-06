/*----------------------------------------------------------------------
 *
 * tableam.c
 *		Table access method routines too big to be inline functions.
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/access/table/tableam.c
 *----------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/tableam.h"


/* GUC variables */
char	   *default_table_access_method = DEFAULT_TABLE_ACCESS_METHOD;

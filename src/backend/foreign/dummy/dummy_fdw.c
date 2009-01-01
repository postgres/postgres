/*-------------------------------------------------------------------------
 *
 * dummy_fdw.c
 *        "dummy" foreign-data wrapper
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *        $PostgreSQL: pgsql/src/backend/foreign/dummy/dummy_fdw.c,v 1.2 2009/01/01 17:23:42 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"
#include "foreign/foreign.h"

PG_MODULE_MAGIC;

/*
 * This looks like a complete waste right now, but it is useful for
 * testing, and will become more interesting as more parts of the
 * interface are implemented.
 */

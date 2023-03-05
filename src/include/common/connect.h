/*-------------------------------------------------------------------------
 *
 * Interfaces in support of FE/BE connections.
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/common/connect.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef CONNECT_H
#define CONNECT_H

/*
 * This SQL statement installs an always-secure search path, so malicious
 * users can't take control.  CREATE of an unqualified name will fail, because
 * this selects no creation schema.  This does not demote pg_temp, so it is
 * suitable where we control the entire FE/BE connection but not suitable in
 * SECURITY DEFINER functions.  This is portable to PostgreSQL 7.3, which
 * introduced schemas.  When connected to an older version from code that
 * might work with the old server, skip this.
 */
#define ALWAYS_SECURE_SEARCH_PATH_SQL \
	"SELECT pg_catalog.set_config('search_path', '', false);"

#endif							/* CONNECT_H */

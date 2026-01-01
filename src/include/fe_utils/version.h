/*-------------------------------------------------------------------------
 *
 * Routines to retrieve information of PG_VERSION
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/fe_utils/version.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_VERSION_H
#define PG_VERSION_H

/*
 * Retrieve the version major number, useful for major version checks
 * based on PG_MAJORVERSION_NUM.
 */
#define GET_PG_MAJORVERSION_NUM(v)	((v) / 10000)

extern uint32 get_pg_version(const char *datadir, char **version_str);

#endif							/* PG_VERSION_H */

/*-------------------------------------------------------------------------
 *
 * Utility routines for SQL dumping
 *
 * Basically this is stuff that is useful in both pg_dump and pg_dumpall.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pg_dump/dumputils.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef DUMPUTILS_H
#define DUMPUTILS_H

#include "libpq-fe.h"
#include "pqexpbuffer.h"

/*
 * Preferred strftime(3) format specifier for printing timestamps in pg_dump
 * and friends.
 *
 * We don't print the timezone on Windows, because the names are long and
 * localized, which means they may contain characters in various random
 * encodings; this has been seen to cause encoding errors when reading the
 * dump script.  Think not to get around that by using %z, because
 * (1) %z is not portable to pre-C99 systems, and
 * (2) %z doesn't actually act differently from %Z on Windows anyway.
 */
#ifndef WIN32
#define PGDUMP_STRFTIME_FMT  "%Y-%m-%d %H:%M:%S %Z"
#else
#define PGDUMP_STRFTIME_FMT  "%Y-%m-%d %H:%M:%S"
#endif


extern bool buildACLCommands(const char *name, const char *subname, const char *nspname,
							 const char *type, const char *acls, const char *baseacls,
							 const char *owner, const char *prefix, int remoteVersion,
							 PQExpBuffer sql);
extern bool buildDefaultACLCommands(const char *type, const char *nspname,
									const char *acls, const char *acldefault,
									const char *owner,
									int remoteVersion,
									PQExpBuffer sql);

extern void quoteAclUserName(PQExpBuffer output, const char *input);

extern void buildShSecLabelQuery(const char *catalog_name,
								 Oid objectId, PQExpBuffer sql);
extern void emitShSecLabels(PGconn *conn, PGresult *res,
							PQExpBuffer buffer, const char *objtype, const char *objname);

extern bool variable_is_guc_list_quote(const char *name);

extern bool SplitGUCList(char *rawstring, char separator,
						 char ***namelist);

extern void makeAlterConfigCommand(PGconn *conn, const char *configitem,
								   const char *type, const char *name,
								   const char *type2, const char *name2,
								   PQExpBuffer buf);

#endif							/* DUMPUTILS_H */

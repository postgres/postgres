/*-------------------------------------------------------------------------
 *
 * Utility routines for SQL dumping
 *	Basically this is stuff that is useful in both pg_dump and pg_dumpall.
 *	Lately it's also being used by psql and bin/scripts/ ...
 *
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
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

typedef enum					/* bits returned by set_dump_section */
{
	DUMP_PRE_DATA = 0x01,
	DUMP_DATA = 0x02,
	DUMP_POST_DATA = 0x04,
	DUMP_UNSECTIONED = 0xff
} DumpSections;

typedef void (*on_exit_nicely_callback) (int code, void *arg);

extern int	quote_all_identifiers;
extern const char *progname;

extern void init_parallel_dump_utils(void);
extern const char *fmtId(const char *identifier);
extern void appendStringLiteral(PQExpBuffer buf, const char *str,
					int encoding, bool std_strings);
extern void appendStringLiteralConn(PQExpBuffer buf, const char *str,
						PGconn *conn);
extern void appendStringLiteralDQ(PQExpBuffer buf, const char *str,
					  const char *dqprefix);
extern void appendByteaLiteral(PQExpBuffer buf,
				   const unsigned char *str, size_t length,
				   bool std_strings);
extern int	parse_version(const char *versionString);
extern bool parsePGArray(const char *atext, char ***itemarray, int *nitems);
extern bool buildACLCommands(const char *name, const char *subname,
				 const char *type, const char *acls, const char *owner,
				 const char *prefix, int remoteVersion,
				 PQExpBuffer sql);
extern bool buildDefaultACLCommands(const char *type, const char *nspname,
						const char *acls, const char *owner,
						int remoteVersion,
						PQExpBuffer sql);
extern bool processSQLNamePattern(PGconn *conn, PQExpBuffer buf,
					  const char *pattern,
					  bool have_where, bool force_escape,
					  const char *schemavar, const char *namevar,
					  const char *altnamevar, const char *visibilityrule);
extern void buildShSecLabelQuery(PGconn *conn, const char *catalog_name,
					 uint32 objectId, PQExpBuffer sql);
extern void emitShSecLabels(PGconn *conn, PGresult *res,
				PQExpBuffer buffer, const char *target, const char *objname);
extern void set_dump_section(const char *arg, int *dumpSections);
extern void
write_msg(const char *modulename, const char *fmt,...)
__attribute__((format(PG_PRINTF_ATTRIBUTE, 2, 3)));
extern void
vwrite_msg(const char *modulename, const char *fmt, va_list ap)
__attribute__((format(PG_PRINTF_ATTRIBUTE, 2, 0)));
extern void
exit_horribly(const char *modulename, const char *fmt,...)
__attribute__((format(PG_PRINTF_ATTRIBUTE, 2, 3), noreturn));
extern void on_exit_nicely(on_exit_nicely_callback function, void *arg);
extern void exit_nicely(int code) __attribute__((noreturn));

#endif   /* DUMPUTILS_H */

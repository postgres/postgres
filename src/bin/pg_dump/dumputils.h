/*-------------------------------------------------------------------------
 *
 * Utility routines for SQL dumping
 *	Basically this is stuff that is useful in both pg_dump and pg_dumpall.
 *	Lately it's also being used by psql and bin/scripts/ ...
 *
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
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

typedef struct SimpleStringListCell
{
	struct SimpleStringListCell *next;
	char		val[1];			/* VARIABLE LENGTH FIELD */
} SimpleStringListCell;

typedef struct SimpleStringList
{
	SimpleStringListCell *head;
	SimpleStringListCell *tail;
} SimpleStringList;


extern int	quote_all_identifiers;
extern PQExpBuffer (*getLocalPQExpBuffer) (void);

extern const char *fmtId(const char *identifier);
extern const char *fmtQualifiedId(int remoteVersion,
			   const char *schema, const char *id);
extern void appendStringLiteral(PQExpBuffer buf, const char *str,
					int encoding, bool std_strings);
extern void appendStringLiteralConn(PQExpBuffer buf, const char *str,
						PGconn *conn);
extern void appendStringLiteralDQ(PQExpBuffer buf, const char *str,
					  const char *dqprefix);
extern void appendByteaLiteral(PQExpBuffer buf,
				   const unsigned char *str, size_t length,
				   bool std_strings);
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

extern void simple_string_list_append(SimpleStringList *list, const char *val);
extern bool simple_string_list_member(SimpleStringList *list, const char *val);

#endif   /* DUMPUTILS_H */

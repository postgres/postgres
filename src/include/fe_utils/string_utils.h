/*-------------------------------------------------------------------------
 *
 * String-processing utility routines for frontend code
 *
 * Assorted utility functions that are useful in constructing SQL queries
 * and interpreting backend output.
 *
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/fe_utils/string_utils.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef STRING_UTILS_H
#define STRING_UTILS_H

#include "libpq-fe.h"
#include "pqexpbuffer.h"

#define atooid(x)  ((Oid) strtoul((x), NULL, 10))

/* Global variables controlling behavior of fmtId() and fmtQualifiedId() */
extern int	quote_all_identifiers;
extern PQExpBuffer (*getLocalPQExpBuffer) (void);

/* Functions */
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

extern bool processSQLNamePattern(PGconn *conn, PQExpBuffer buf,
					  const char *pattern,
					  bool have_where, bool force_escape,
					  const char *schemavar, const char *namevar,
					  const char *altnamevar, const char *visibilityrule);

#endif   /* STRING_UTILS_H */

/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2003, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/bin/psql/describe.h,v 1.26 2004/07/15 03:56:06 momjian Exp $
 */
#ifndef DESCRIBE_H
#define DESCRIBE_H

#include "settings.h"

/* \da */
bool		describeAggregates(const char *pattern, bool verbose);

/* \db */
bool		describeTablespaces(const char *pattern, bool verbose);

/* \df */
bool		describeFunctions(const char *pattern, bool verbose);

/* \dT */
bool		describeTypes(const char *pattern, bool verbose);

/* \do */
bool		describeOperators(const char *pattern);

/* \du */
bool		describeUsers(const char *pattern);

/* \dg */
bool		describeGroups(const char *pattern);

/* \z (or \dp) */
bool		permissionsList(const char *pattern);

/* \dd */
bool		objectDescription(const char *pattern);

/* \d foo */
bool		describeTableDetails(const char *pattern, bool verbose);

/* \l */
bool		listAllDbs(bool verbose);

/* \dt, \di, \ds, \dS, etc. */
bool		listTables(const char *tabtypes, const char *pattern, bool verbose);

/* \dD */
bool		listDomains(const char *pattern);

/* \dc */
bool		listConversions(const char *pattern);

/* \dC */
bool		listCasts(const char *pattern);

/* \dn */
bool		listSchemas(const char *pattern, bool verbose);


#endif   /* DESCRIBE_H */

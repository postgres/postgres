/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2005, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/bin/psql/describe.h,v 1.29 2005/08/14 18:49:30 tgl Exp $
 */
#ifndef DESCRIBE_H
#define DESCRIBE_H

#include "settings.h"

/* \da */
extern bool describeAggregates(const char *pattern, bool verbose);

/* \db */
extern bool describeTablespaces(const char *pattern, bool verbose);

/* \df */
extern bool describeFunctions(const char *pattern, bool verbose);

/* \dT */
extern bool describeTypes(const char *pattern, bool verbose);

/* \do */
extern bool describeOperators(const char *pattern);

/* \du, \dg */
extern bool describeRoles(const char *pattern);

/* \z (or \dp) */
extern bool permissionsList(const char *pattern);

/* \dd */
extern bool objectDescription(const char *pattern);

/* \d foo */
extern bool describeTableDetails(const char *pattern, bool verbose);

/* \l */
extern bool listAllDbs(bool verbose);

/* \dt, \di, \ds, \dS, etc. */
extern bool listTables(const char *tabtypes, const char *pattern, bool verbose);

/* \dD */
extern bool listDomains(const char *pattern);

/* \dc */
extern bool listConversions(const char *pattern);

/* \dC */
extern bool listCasts(const char *pattern);

/* \dn */
extern bool listSchemas(const char *pattern, bool verbose);


#endif   /* DESCRIBE_H */

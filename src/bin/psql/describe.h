/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2003, PostgreSQL Global Development Group
 *
 * $Header: /cvsroot/pgsql/src/bin/psql/describe.h,v 1.21 2003/08/04 23:59:40 tgl Exp $
 */
#ifndef DESCRIBE_H
#define DESCRIBE_H

#include "settings.h"

/* \da */
bool		describeAggregates(const char *pattern, bool verbose);

/* \df */
bool		describeFunctions(const char *pattern, bool verbose);

/* \dT */
bool		describeTypes(const char *pattern, bool verbose);

/* \do */
bool		describeOperators(const char *pattern);

/* \du */
bool		describeUsers(const char *pattern);

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
bool		listSchemas(const char *pattern);


#endif   /* DESCRIBE_H */

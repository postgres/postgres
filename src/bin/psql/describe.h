/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright 2000 by PostgreSQL Global Development Group
 *
 * $Header: /cvsroot/pgsql/src/bin/psql/describe.h,v 1.16 2002/03/19 02:32:21 momjian Exp $
 */
#ifndef DESCRIBE_H
#define DESCRIBE_H

#include "settings.h"

/* \da */
bool		describeAggregates(const char *name);

/* \df */
bool		describeFunctions(const char *name, bool verbose);

/* \dT */
bool		describeTypes(const char *name, bool verbose);

/* \do */
bool		describeOperators(const char *name);

/* \du */
bool		describeUsers(const char *name);

/* \z (or \dp) */
bool		permissionsList(const char *name);

/* \dd */
bool		objectDescription(const char *object);

/* \d foo */
bool		describeTableDetails(const char *name, bool desc);

/* \l */
bool		listAllDbs(bool desc);

/* \dt, \di, \ds, \dS, etc. */
bool		listTables(const char *infotype, const char *name, bool desc);

/* \dD */
bool		listDomains(const char *name);

#endif   /* DESCRIBE_H */

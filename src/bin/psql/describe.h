/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2009, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/bin/psql/describe.h,v 1.38 2009/01/06 21:10:30 momjian Exp $
 */
#ifndef DESCRIBE_H
#define DESCRIBE_H


/* \da */
extern bool describeAggregates(const char *pattern, bool verbose, bool showSystem);

/* \db */
extern bool describeTablespaces(const char *pattern, bool verbose);

/* \df */
extern bool describeFunctions(const char *pattern, bool verbose, bool showSystem);

/* \dT */
extern bool describeTypes(const char *pattern, bool verbose, bool showSystem);

/* \do */
extern bool describeOperators(const char *pattern, bool showSystem);

/* \du, \dg */
extern bool describeRoles(const char *pattern, bool verbose);

/* \z (or \dp) */
extern bool permissionsList(const char *pattern);

/* \dd */
extern bool objectDescription(const char *pattern, bool showSystem);

/* \d foo */
extern bool describeTableDetails(const char *pattern, bool verbose);

/* \dF */
extern bool listTSConfigs(const char *pattern, bool verbose);

/* \dFp */
extern bool listTSParsers(const char *pattern, bool verbose);

/* \dFd */
extern bool listTSDictionaries(const char *pattern, bool verbose);

/* \dFt */
extern bool listTSTemplates(const char *pattern, bool verbose);

/* \l */
extern bool listAllDbs(bool verbose);

/* \dt, \di, \ds, \dS, etc. */
extern bool listTables(const char *tabtypes, const char *pattern, bool verbose, bool showSystem);

/* \dD */
extern bool listDomains(const char *pattern, bool showSystem);

/* \dc */
extern bool listConversions(const char *pattern, bool showSystem);

/* \dC */
extern bool listCasts(const char *pattern);

/* \dn */
extern bool listSchemas(const char *pattern, bool verbose);

/* \dew */
extern bool listForeignDataWrappers(const char *pattern, bool verbose);

/* \des */
extern bool listForeignServers(const char *pattern, bool verbose);

/* \deu */
extern bool listUserMappings(const char *pattern, bool verbose);


#endif   /* DESCRIBE_H */

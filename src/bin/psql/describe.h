#ifndef DESCRIBE_H
#define DESCRIBE_H

#include "settings.h"

/* \da */
bool
describeAggregates(const char * name, PsqlSettings * pset);

/* \df */
bool
describeFunctions(const char * name, PsqlSettings * pset);

/* \dT */
bool
describeTypes(const char * name, PsqlSettings * pset);

/* \do */
bool
describeOperators(const char * name, PsqlSettings * pset);

/* \dp (formerly \z) */
bool
permissionsList(const char * name, PsqlSettings *pset);

/* \dd */
bool
objectDescription(const char * object, PsqlSettings *pset);

/* \d foo */
bool
describeTableDetails(const char * name, PsqlSettings * pset);

/* \l */
bool
listAllDbs(PsqlSettings *pset);

/* \dt, \di, \dS, etc. */
bool
listTables(const char * infotype, const char * name, PsqlSettings * pset);

#endif /* DESCRIBE_H */

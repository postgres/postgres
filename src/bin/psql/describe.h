#ifndef DESCRIBE_H
#define DESCRIBE_H

#include <c.h>
#include "settings.h"

/* \da */
bool describeAggregates(const char *name, PsqlSettings *pset, bool verbose, bool desc);

/* \df */
bool describeFunctions(const char *name, PsqlSettings *pset, bool verbose, bool desc);

/* \dT */
bool describeTypes(const char *name, PsqlSettings *pset, bool verbose, bool desc);

/* \do */
bool describeOperators(const char *name, PsqlSettings *pset, bool verbose, bool desc);

/* \z (or \dp) */
bool permissionsList(const char *name, PsqlSettings *pset);

/* \dd */
bool objectDescription(const char *object, PsqlSettings *pset);

/* \d foo */
bool describeTableDetails(const char *name, PsqlSettings *pset, bool desc);

/* \l */
bool listAllDbs(PsqlSettings *pset, bool desc);

/* \dt, \di, \ds, \dS, etc. */
bool listTables(const char *infotype, const char *name, PsqlSettings *pset, bool desc);

#endif	 /* DESCRIBE_H */

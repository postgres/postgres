#ifndef COMMON_H
#define COMMON_H

#include <c.h>
#include "settings.h"

char *
xstrdup(const char * string);

bool
setQFout(const char *fname, PsqlSettings *pset);

char *
simple_prompt(const char *prompt, int maxlen, bool echo);

const char *
interpolate_var(const char * name, PsqlSettings * pset);

PGresult *
PSQLexec(PsqlSettings *pset, const char *query);

bool
SendQuery(PsqlSettings *pset, const char *query);

#endif /* COMMON_H */

#ifndef COMMON_H
#define COMMON_H

#include <c.h>
#include "settings.h"

char *
			xstrdup(const char *string);

bool
			setQFout(const char *fname);

char *
			simple_prompt(const char *prompt, int maxlen, bool echo);

PGresult   *
			PSQLexec(const char *query);

bool
			SendQuery(const char *query);

#endif	 /* COMMON_H */

#ifndef HELP_H
#define HELP_H

#include "settings.h"

void usage(void);

void slashUsage(PsqlSettings *pset);

void helpSQL(const char *topic);

void print_copyright(void);


#endif


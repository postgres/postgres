#ifndef LARGE_OBJ_H
#define LARGE_OBJ_H

#include <c.h>
#include "settings.h"

bool		do_lo_export(PsqlSettings *pset, const char *loid_arg, const char *filename_arg);
bool		do_lo_import(PsqlSettings *pset, const char *filename_arg, const char *comment_arg);
bool		do_lo_unlink(PsqlSettings *pset, const char *loid_arg);
bool		do_lo_list(PsqlSettings *pset);

#endif	 /* LARGE_OBJ_H */

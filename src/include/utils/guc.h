/*
 * guc.h
 *
 * External declarations pertaining to backend/utils/misc/guc.c and
 * backend/utils/misc/guc-file.l
 *
 * $Header: /cvsroot/pgsql/src/include/utils/guc.h,v 1.1 2000/05/31 00:28:40 petere Exp $
 */
#ifndef GUC_H
#define GUC_H

#include "postgres.h"

/*
 * This is sort of a permission list. Those contexts with a higher
 * number can also be set via the lower numbered ways.
 */
typedef enum {
	PGC_POSTMASTER = 1,  /* static postmaster option */
	PGC_BACKEND    = 2,  /* per backend startup option */
	PGC_SIGHUP     = 4,  /* can change this option via SIGHUP */
	PGC_SUSET      = 8,  /* can change this option via SET if superuser */
	PGC_USERSET    = 16, /* everyone can change this option via SET */
} GucContext;


void         SetConfigOption(const char * name, const char * value, GucContext context);
const char * GetConfigOption(const char * name, bool issuper);
void         ProcessConfigFile(GucContext context);
void         ResetAllOptions(void);

bool         set_config_option(const char * name, const char * value, GucContext context, bool DoIt);


extern bool Debug_print_query;
extern bool Debug_print_plan;
extern bool Debug_print_parse;
extern bool Debug_print_rewritten;
extern bool Debug_pretty_print;

extern bool Show_parser_stats;
extern bool Show_planner_stats;
extern bool Show_executor_stats;
extern bool Show_query_stats;
extern bool Show_btree_build_stats;

#endif /*GUC_H*/

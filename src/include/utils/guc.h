/*
 * guc.h
 *
 * External declarations pertaining to backend/utils/misc/guc.c and
 * backend/utils/misc/guc-file.l
 *
 * $Header: /cvsroot/pgsql/src/include/utils/guc.h,v 1.2 2000/06/22 22:31:24 petere Exp $
 */
#ifndef GUC_H
#define GUC_H

#include "postgres.h"

/*
 * Certain options can only be set at certain times. The rules are
 * like this:
 *
 * POSTMASTER options can only be set when the postmaster starts,
 * either from the configuration file or the command line.
 *
 * SIGHUP options can only be set at postmaster startup or by changing
 * the configuration file and sending the HUP signal to the postmaster
 * or a backend process. (Notice that the signal receipt will not be
 * evaluated immediately. The postmaster and the backend block at a
 * certain point in their main loop. It's safer to wait than to read a
 * file asynchronously.)
 *
 * BACKEND options can only be set at postmaster startup or with the
 * PGOPTIONS variable from the client when the connection is
 * initiated. Note that you cannot change this kind of option using
 * the SIGHUP mechanism, that would defeat the purpose of this being
 * fixed for a given backend once started.
 *
 * SUSET options can be set at postmaster startup, with the SIGHUP
 * mechanism, or from SQL if you're a superuser. These options cannot
 * be set using the PGOPTIONS mechanism, because there is not check as
 * to who does this.
 *
 * USERSET options can be set by anyone any time.
 */
typedef enum {
	PGC_POSTMASTER,
	PGC_SIGHUP,
	PGC_BACKEND,
	PGC_SUSET,
	PGC_USERSET,
} GucContext;


void         SetConfigOption(const char * name, const char * value, GucContext context);
const char * GetConfigOption(const char * name);
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

extern bool SQL_inheritance;

#endif /*GUC_H*/

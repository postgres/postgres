/*
 * guc.h
 *
 * External declarations pertaining to backend/utils/misc/guc.c and
 * backend/utils/misc/guc-file.l
 *
 * $Id: guc.h,v 1.12 2001/10/28 06:26:09 momjian Exp $
 */
#ifndef GUC_H
#define GUC_H

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
 * BACKEND options can only be set at postmaster startup, from the
 * configuration file, or with the PGOPTIONS variable from the client
 * when the connection is initiated.  Furthermore, an already-started
 * backend will ignore changes to such an option in the configuration
 * file.  The idea is that these options are fixed for a given backend
 * once it's started, but they can vary across backends.
 *
 * SUSET options can be set at postmaster startup, with the SIGHUP
 * mechanism, or from SQL if you're a superuser. These options cannot
 * be set using the PGOPTIONS mechanism, because there is not check as
 * to who does this.
 *
 * USERSET options can be set by anyone any time.
 */
typedef enum
{
	PGC_POSTMASTER,
	PGC_SIGHUP,
	PGC_BACKEND,
	PGC_SUSET,
	PGC_USERSET
} GucContext;


extern void SetConfigOption(const char *name, const char *value,
				GucContext context, bool makeDefault);
extern const char *GetConfigOption(const char *name);
extern void ProcessConfigFile(GucContext context);
extern void ResetAllOptions(bool isStartup);
extern void ParseLongOption(const char *string, char **name, char **value);
extern bool set_config_option(const char *name, const char *value,
				  GucContext context, bool DoIt, bool makeDefault);
extern void ShowAllGUCConfig(void);


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
extern bool Australian_timezones;

#endif	 /* GUC_H */

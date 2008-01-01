/*-------------------------------------------------------------------------
 * pg_regress.h --- regression test driver
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/test/regress/pg_regress.h,v 1.3 2008/01/01 19:46:00 momjian Exp $
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"
#include <unistd.h>

#ifndef WIN32
#define PID_TYPE pid_t
#define INVALID_PID (-1)
#else
#define PID_TYPE HANDLE
#define INVALID_PID INVALID_HANDLE_VALUE
#endif

/* simple list of strings */
typedef struct _stringlist
{
	char	   *str;
	struct _stringlist *next;
}	_stringlist;

typedef PID_TYPE(*test_function) (const char *,
						  _stringlist **,
						  _stringlist **,
						  _stringlist **);
typedef void (*init_function) (void);

extern char *bindir;
extern char *libdir;
extern char *datadir;
extern char *host_platform;

extern _stringlist *dblist;
extern bool debug;
extern char *inputdir;
extern char *outputdir;

/*
 * This should not be global but every module should be able to read command
 * line parameters.
 */
extern char *psqldir;

extern const char *basic_diff_opts;
extern const char *pretty_diff_opts;

int regression_main(int argc, char *argv[],
				init_function ifunc, test_function tfunc);
void		add_stringlist_item(_stringlist ** listhead, const char *str);
PID_TYPE	spawn_process(const char *cmdline);
void		exit_nicely(int code);
void		replace_string(char *string, char *replace, char *replacement);

/*-------------------------------------------------------------------------
 * pg_regress.h --- regression test driver
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/test/regress/pg_regress.h
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
} _stringlist;

typedef PID_TYPE(*test_function) (const char *,
						  _stringlist **,
						  _stringlist **,
						  _stringlist **);
typedef void (*init_function) (int argc, char **argv);

extern char *bindir;
extern char *libdir;
extern char *datadir;
extern char *host_platform;

extern _stringlist *dblist;
extern bool debug;
extern char *inputdir;
extern char *outputdir;
extern char *launcher;

/*
 * This should not be global but every module should be able to read command
 * line parameters.
 */
extern char *psqldir;

extern const char *basic_diff_opts;
extern const char *pretty_diff_opts;

int regression_main(int argc, char *argv[],
				init_function ifunc, test_function tfunc);
void		add_stringlist_item(_stringlist **listhead, const char *str);
PID_TYPE	spawn_process(const char *cmdline);
void		replace_string(char *string, char *replace, char *replacement);
bool		file_exists(const char *file);

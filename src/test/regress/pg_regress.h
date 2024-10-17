/*-------------------------------------------------------------------------
 * pg_regress.h --- regression test driver
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/test/regress/pg_regress.h
 *-------------------------------------------------------------------------
 */

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

/*
 * Callback function signatures for test programs that use regression_main()
 */

/* Initialize at program start */
typedef void (*init_function) (int argc, char **argv);

/* Launch one test case */
typedef PID_TYPE(*test_start_function) (const char *testname,
										_stringlist **resultfiles,
										_stringlist **expectfiles,
										_stringlist **tags);

/* Postprocess one result file (optional) */
typedef void (*postprocess_result_function) (const char *filename);


extern char *bindir;
extern char *libdir;
extern char *datadir;
extern char *host_platform;

extern _stringlist *dblist;
extern bool debug;
extern char *inputdir;
extern char *outputdir;
extern char *expecteddir;
extern char *launcher;

extern const char *basic_diff_opts;
extern const char *pretty_diff_opts;

int			regression_main(int argc, char *argv[],
							init_function ifunc,
							test_start_function startfunc,
							postprocess_result_function postfunc);

void		add_stringlist_item(_stringlist **listhead, const char *str);
PID_TYPE	spawn_process(const char *cmdline);
bool		file_exists(const char *file);

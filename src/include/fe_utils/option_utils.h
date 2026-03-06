/*-------------------------------------------------------------------------
 *
 * Command line option processing facilities for frontend code
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/fe_utils/option_utils.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef OPTION_UTILS_H
#define OPTION_UTILS_H

#include "common/file_utils.h"

typedef void (*help_handler) (const char *progname);

extern void handle_help_version_opts(int argc, char *argv[],
									 const char *fixed_progname,
									 help_handler hlp);
extern bool option_parse_int(const char *optarg, const char *optname,
							 int min_range, int max_range,
							 int *result);
extern bool parse_sync_method(const char *optarg,
							  DataDirSyncMethod *sync_method);
extern void check_mut_excl_opts_internal(int n,...);

/* see comment for check_mut_excl_opts_internal() in option_utils.c for info */
#define check_mut_excl_opts(set, opt, ...) \
	check_mut_excl_opts_internal(VA_ARGS_NARGS(__VA_ARGS__) + 2, \
								 set, opt, __VA_ARGS__)

#endif							/* OPTION_UTILS_H */

/*-------------------------------------------------------------------------
 *
 * Command line option processing facilities for frontend code
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/fe_utils/option_utils.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef OPTION_UTILS_H
#define OPTION_UTILS_H

#include "postgres_fe.h"

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

#endif							/* OPTION_UTILS_H */

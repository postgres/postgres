/*--------------------------------------------------------------------
 * conffiles.h
 *
 * Utilities related to configuration files.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/conffiles.h
 *
 *--------------------------------------------------------------------
 */
#ifndef CONFFILES_H
#define CONFFILES_H

/* recursion nesting depth for configuration files */
#define CONF_FILE_START_DEPTH	0
#define CONF_FILE_MAX_DEPTH		10

extern char *AbsoluteConfigLocation(const char *location,
									const char *calling_file);
extern char **GetConfFilesInDir(const char *includedir,
								const char *calling_file,
								int elevel, int *num_filenames,
								char **err_msg);

#endif							/* CONFFILES_H */

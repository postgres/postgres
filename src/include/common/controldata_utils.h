/*
 * controldata_utils.h
 *		Common code for pg_controldata output
 *
 *	Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *	Portions Copyright (c) 1994, Regents of the University of California
 *
 *	src/include/common/controldata_utils.h
 */
#ifndef COMMON_CONTROLDATA_UTILS_H
#define COMMON_CONTROLDATA_UTILS_H

#include "catalog/pg_control.h"

extern ControlFileData *get_controlfile(const char *DataDir, bool *crc_ok_p);
extern ControlFileData *get_controlfile_by_exact_path(const char *ControlFilePath,
													  bool *crc_ok_p);
extern void update_controlfile(const char *DataDir,
							   ControlFileData *ControlFile, bool do_sync);

#endif							/* COMMON_CONTROLDATA_UTILS_H */

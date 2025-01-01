/*-------------------------------------------------------------------------
 *
 * astreamer_inject.h
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/bin/pg_basebackup/astreamer_inject.h
 *-------------------------------------------------------------------------
 */

#ifndef ASTREAMER_INJECT_H
#define ASTREAMER_INJECT_H

#include "fe_utils/astreamer.h"
#include "pqexpbuffer.h"

extern astreamer *astreamer_recovery_injector_new(astreamer *next,
												  bool is_recovery_guc_supported,
												  PQExpBuffer recoveryconfcontents);
extern void astreamer_inject_file(astreamer *streamer, char *pathname,
								  char *data, int len);

#endif

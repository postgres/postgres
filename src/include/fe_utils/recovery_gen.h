/*-------------------------------------------------------------------------
 *
 * Generator for recovery configuration
 *
 * Portions Copyright (c) 2011-2022, PostgreSQL Global Development Group
 *
 * src/include/fe_utils/recovery_gen.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef RECOVERY_GEN_H
#define RECOVERY_GEN_H

#include "libpq-fe.h"
#include "pqexpbuffer.h"

/*
 * recovery configuration is part of postgresql.conf in version 12 and up, and
 * in recovery.conf before that.
 */
#define MINIMUM_VERSION_FOR_RECOVERY_GUC 120000

extern PQExpBuffer GenerateRecoveryConfig(PGconn *pgconn,
										  char *pg_replication_slot);
extern void WriteRecoveryConfig(PGconn *pgconn, char *target_dir,
								PQExpBuffer contents);

#endif							/* RECOVERY_GEN_H */

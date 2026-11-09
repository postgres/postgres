/*-------------------------------------------------------------------------
 *
 * pgstattuple.h
 *	  Common functions for pgstattuple.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/pgstattuple/pgstattuple.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _PGSTATTUPLE_H_
#define _PGSTATTUPLE_H_

#include "utils/relcache.h"

/* in pgstattuple.c */
extern void check_relation_supported(Relation rel);

#endif							/* _PGSTATTUPLE_H_ */

/*-------------------------------------------------------------------------
 *
 * pg_tde_tdemap.h
 *	  TDE relation fork manapulation.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_TDE_MAP_H
#define PG_TDE_MAP_H

#include "utils/rel.h"
#include "storage/relfilelocator.h"

extern void pg_tde_create_key_fork(const RelFileLocator *newrlocator, Relation rel);

#endif                            /* PG_TDE_MAP_H */
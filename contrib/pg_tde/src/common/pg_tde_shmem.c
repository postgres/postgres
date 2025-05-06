/*-------------------------------------------------------------------------
 *
 * pg_tde_shmem.c
 *      Shared memory area to manage cache and locks.
 *
 * IDENTIFICATION
 *    contrib/pg_tde/src/pg_tde_shmem.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "common/pg_tde_shmem.h"

int
TdeRequiredLocksCount(void)
{
	return TDE_LWLOCK_COUNT;
}

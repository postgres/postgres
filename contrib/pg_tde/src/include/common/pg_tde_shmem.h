/*-------------------------------------------------------------------------
 *
 * pg_tde_shmem.h
 * src/include/common/pg_tde_shmem.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_TDE_SHMEM_H
#define PG_TDE_SHMEM_H

#define TDE_TRANCHE_NAME "pg_tde_tranche"

typedef enum
{
	TDE_LWLOCK_ENC_KEY,
	TDE_LWLOCK_PI_FILES,

	/* Must be the last entry in the enum */
	TDE_LWLOCK_COUNT
}			TDELockTypes;

extern int	TdeRequiredLocksCount(void);

#endif							/* PG_TDE_SHMEM_H */

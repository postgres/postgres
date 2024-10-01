/*-------------------------------------------------------------------------
 *
 * pg_tde_shmem.h
 * src/include/common/pg_tde_shmem.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_TDE_SHMEM_H
#define PG_TDE_SHMEM_H

#include "postgres.h"
#include "storage/shmem.h"
#include "storage/lwlock.h"
#include "lib/dshash.h"
#include "utils/dsa.h"

#define TDE_TRANCHE_NAME "pg_tde_tranche"

typedef enum
{
    TDE_LWLOCK_ENC_KEY,
    TDE_LWLOCK_PI_FILES,

    /* Must be the last entry in the enum */
    TDE_LWLOCK_COUNT
} TDELockTypes;

typedef struct TDEShmemSetupRoutine
{
    /* init_shared_state gets called at the time of extension load
     * you can initialize the data structures required to be placed in
     * shared memory in this callback
     * The callback must return the size of the shared memory area acquired.
     * The argument to the function is the start of the shared memory address
     * that can be used to store the shared data structures.
     */
    Size (*init_shared_state)(void *raw_dsa_area);
    /*
     * shmem_startup gets called at the time of postmaster shutdown
     */
    void (*shmem_kill)(int code, Datum arg);
    /*
     * The callback must return the size of the shared memory acquired.
     */
    Size (*required_shared_mem_size)(void);
    /*
     * Gets called after all shared memory structures are initialized and
     * here you can create shared memory hash tables or any other shared
     * objects that needs to live in DSA area.
     */
    void (*init_dsa_area_objects)(dsa_area *dsa, void *raw_dsa_area);
} TDEShmemSetupRoutine;

/* Interface to register the shared memory requests */
extern void RegisterShmemRequest(const TDEShmemSetupRoutine *routine);
extern void TdeShmemInit(void);
extern Size TdeRequiredSharedMemorySize(void);
extern int TdeRequiredLocksCount(void);

#endif /*PG_TDE_SHMEM_H*/
/*-------------------------------------------------------------------------
 *
 * pg_tde_smgr.h
 * src/include/smgr/pg_tde_smgr.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef PG_TDE_SMGR_H
#define PG_TDE_SMGR_H

#include "storage/relfilelocator.h"
#include "storage/smgr.h"

extern void RegisterStorageMgr(void);
extern void tde_smgr_create_key_redo(const RelFileLocator *rlocator);
extern void tde_smgr_delete_key_redo(const RelFileLocator *rlocator);
extern bool tde_smgr_rel_is_encrypted(SMgrRelation reln);

#endif							/* PG_TDE_SMGR_H */

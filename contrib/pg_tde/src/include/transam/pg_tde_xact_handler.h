/*-------------------------------------------------------------------------
 *
 * pg_tde_xact_handler.h
 *	  TDE transaction handling.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_TDE_XACT_HANDLER_H
#define PG_TDE_XACT_HANDLER_H

#include "postgres.h"
#include "storage/relfilelocator.h"

extern void RegisterTdeXactCallbacks(void);
extern void RegisterEntryForDeletion(const RelFileLocator *rlocator, off_t map_entry_offset, bool atCommit);


#endif							/* PG_TDE_XACT_HANDLER_H */

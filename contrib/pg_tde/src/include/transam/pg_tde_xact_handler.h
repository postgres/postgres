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
#include "access/xact.h"

extern void pg_tde_xact_callback(XactEvent event, void *arg);
extern void pg_tde_subxact_callback(SubXactEvent event, SubTransactionId mySubid,
									SubTransactionId parentSubid, void *arg);

extern void RegisterEntryForDeletion(const RelFileLocator *rlocator, off_t map_entry_offset, bool atCommit);


#endif /* PG_TDE_XACT_HANDLER_H */

/*-------------------------------------------------------------------------
 *
 * flatfiles.h
 *	  Routines for maintaining "flat file" images of the shared catalogs.
 *
 *
 * $PostgreSQL: pgsql/src/include/utils/flatfiles.h,v 1.1 2005/02/20 02:22:07 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef FLATFILES_H
#define FLATFILES_H

#include "fmgr.h"

extern void database_file_update_needed(void);
extern void group_file_update_needed(void);
extern void user_file_update_needed(void);

extern char *database_getflatfilename(void);
extern char *group_getflatfilename(void);
extern char *user_getflatfilename(void);

extern void BuildFlatFiles(bool database_only);

extern void AtEOXact_UpdateFlatFiles(bool isCommit);
extern void AtEOSubXact_UpdateFlatFiles(bool isCommit,
										SubTransactionId mySubid,
										SubTransactionId parentSubid);

extern Datum flatfile_update_trigger(PG_FUNCTION_ARGS);

#endif   /* FLATFILES_H */

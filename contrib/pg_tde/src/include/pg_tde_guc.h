/*-------------------------------------------------------------------------
 *
 * pg_tde_guc.h
 *	  GUC variables for pg_tde
 *
 * src/include/pg_tde_guc.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef TDE_GUC_H
#define TDE_GUC_H


#include "postgres.h"

#ifndef FRONTEND

extern bool AllowInheritGlobalProviders;
extern bool EncryptXLog;
extern bool EnforceEncryption;

extern void TdeGucInit(void);

#endif
#endif							/* TDE_GUC_H */

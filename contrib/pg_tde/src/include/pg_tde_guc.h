/*
 * GUC variables for pg_tde
 */

#ifndef TDE_GUC_H
#define TDE_GUC_H

#include "c.h"

#ifndef FRONTEND

extern bool AllowInheritGlobalProviders;
extern bool EncryptXLog;
extern bool EnforceEncryption;

extern void TdeGucInit(void);

#endif
#endif							/* TDE_GUC_H */

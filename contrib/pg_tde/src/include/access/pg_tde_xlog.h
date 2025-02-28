/*-------------------------------------------------------------------------
 *
 * tdeheap_xlog.h
 *	  TDE XLog resource manager
 *
 *-------------------------------------------------------------------------
 */

#ifndef PG_TDE_XLOG_H
#define PG_TDE_XLOG_H

#ifndef FRONTEND

#include "postgres.h"

/* TDE XLOG resource manager */
#define XLOG_TDE_ADD_RELATION_KEY		0x00
#define XLOG_TDE_ADD_PRINCIPAL_KEY		0x10
#define XLOG_TDE_EXTENSION_INSTALL_KEY	0x20
#define XLOG_TDE_ROTATE_KEY				0x30
#define XLOG_TDE_ADD_KEY_PROVIDER_KEY 	0x40
#define XLOG_TDE_FREE_MAP_ENTRY		 	0x50
#define XLOG_TDE_UPDATE_PRINCIPAL_KEY	0x60

/* ID 140 is registered for Percona TDE extension: https://wiki.postgresql.org/wiki/CustomWALResourceManagers */
#define RM_TDERMGR_ID	140

extern void RegisterTdeRmgr(void);

#endif							/* !FRONTEND */
#endif							/* PG_TDE_XLOG_H */

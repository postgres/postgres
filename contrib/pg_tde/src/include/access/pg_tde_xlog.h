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

/* TDE XLOG record types */
#define XLOG_TDE_ADD_RELATION_KEY		0x00
#define XLOG_TDE_ADD_PRINCIPAL_KEY		0x10
#define XLOG_TDE_ROTATE_PRINCIPAL_KEY	0x20
#define XLOG_TDE_WRITE_KEY_PROVIDER 	0x30
#define XLOG_TDE_INSTALL_EXTENSION		0x40
#define XLOG_TDE_REMOVE_RELATION_KEY	0x50
#define XLOG_TDE_DELETE_PRINCIPAL_KEY	0x60

/* ID 140 is registered for Percona TDE extension: https://wiki.postgresql.org/wiki/CustomWALResourceManagers */
#define RM_TDERMGR_ID	140

extern void RegisterTdeRmgr(void);

#endif							/* !FRONTEND */
#endif							/* PG_TDE_XLOG_H */

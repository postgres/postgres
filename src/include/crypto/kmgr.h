/*-------------------------------------------------------------------------
 *
 * kmgr.h
 *
 * Portions Copyright (c) 2020, PostgreSQL Global Development Group
 *
 * src/include/crypto/kmgr.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef KMGR_H
#define KMGR_H

#include "common/cipher.h"
#include "common/kmgr_utils.h"
#include "storage/relfilenode.h"
#include "storage/bufpage.h"

/* GUC parameters */
extern int file_encryption_keylen;
extern char *cluster_key_command;

extern Size KmgrShmemSize(void);
extern void KmgrShmemInit(void);
extern void BootStrapKmgr(void);
extern void InitializeKmgr(void);
extern const CryptoKey *KmgrGetKey(int id);

#endif							/* KMGR_H */

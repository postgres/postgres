#ifndef PG_TDE_MAP_H
#define PG_TDE_MAP_H

#include "storage/relfilelocator.h"

#include "access/pg_tde_keys_common.h"
#include "encryption/enc_tde.h"

extern void pg_tde_save_smgr_key(RelFileLocator rel, const InternalKey *key);
extern bool pg_tde_has_smgr_key(RelFileLocator rel);
extern InternalKey *pg_tde_get_smgr_key(RelFileLocator rel);
extern void pg_tde_free_key_map_entry(RelFileLocator rel);

extern int	pg_tde_count_encryption_keys(Oid dbOid);

extern void pg_tde_delete_tde_files(Oid dbOid);

extern TDESignedPrincipalKeyInfo *pg_tde_get_principal_key_info(Oid dbOid);
extern bool pg_tde_verify_principal_key_info(TDESignedPrincipalKeyInfo *signed_key_info, const KeyData *principal_key_data);
extern void pg_tde_save_principal_key(const TDEPrincipalKey *principal_key, bool write_xlog);
extern void pg_tde_save_principal_key_redo(const TDESignedPrincipalKeyInfo *signed_key_info);
extern void pg_tde_perform_rotate_key(const TDEPrincipalKey *principal_key, const TDEPrincipalKey *new_principal_key, bool write_xlog);
extern void pg_tde_delete_principal_key(Oid dbOid);
extern void pg_tde_delete_principal_key_redo(Oid dbOid);

extern void pg_tde_sign_principal_key_info(TDESignedPrincipalKeyInfo *signed_key_info, const TDEPrincipalKey *principal_key);

const char *tde_sprint_key(InternalKey *k);

#endif							/* PG_TDE_MAP_H */

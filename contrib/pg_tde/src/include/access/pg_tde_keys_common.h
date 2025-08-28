#ifndef PG_TDE_KEYS_COMMON_H
#define PG_TDE_KEYS_COMMON_H

#include "catalog/tde_principal_key.h"

#define MAP_ENTRY_IV_SIZE 16
#define MAP_ENTRY_AEAD_TAG_SIZE 16

typedef struct
{
	TDEPrincipalKeyInfo data;
	unsigned char sign_iv[MAP_ENTRY_IV_SIZE];
	unsigned char aead_tag[MAP_ENTRY_AEAD_TAG_SIZE];
} TDESignedPrincipalKeyInfo;

#endif							/* PG_TDE_KEYS_COMMON_H */

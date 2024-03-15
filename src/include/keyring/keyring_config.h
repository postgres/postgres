
#ifndef KEYRING_CONFIG_H
#define KEYRING_CONFIG_H

#include "postgres.h"

enum KeyringProvider
{
	PROVIDER_UNKNOWN,
	PROVIDER_FILE,
	PROVIDER_VAULT_V2,
} ;

extern enum KeyringProvider keyringProvider;

void keyringRegisterVariables(void);

#endif // KEYRING_CONFIG_H

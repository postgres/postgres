
#include "keyring/keyring_config.h"
#include "keyring/keyring_file.h"
#include "keyring/keyring_vault.h"

#include <stdio.h>
#include <assert.h>
#include <unistd.h>

#include "utils/guc.h"

enum KeyringProvider keyringProvider = PROVIDER_UNKNOWN;

void keyringRegisterVariables(void)
{
	// nop for now
}

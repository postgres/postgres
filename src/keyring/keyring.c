
#include "keyring/keyring.h"
#include "keyring/keyring_config.h"
#include "keyring/keyring_api.h"

void keyringInitialize(void)
{
	keyringRegisterVariables();
	keyringInitializeCache();
}

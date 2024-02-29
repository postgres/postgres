
#include "keyring/keyring_config.h"
#include "keyring/keyring_file.h"
#include "keyring/keyring_vault.h"

#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <json.h>

#include "utils/guc.h"

enum KeyringProvider keyringProvider = PROVIDER_UNKNOWN;

void keyringRegisterVariables(void)
{
	// nop for now
}

const char* keyringParseStringParam(json_object* object)
{
	if(json_object_get_type(object) == json_type_object)
	{
		elog(WARNING, "Remote parameters are not yet implemented");
	}

	return json_object_get_string(object);
}

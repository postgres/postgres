
#include "keyring/keyring_config.h"
#include "keyring/keyring_file.h"

#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <json.h>

#include "utils/guc.h"

char* keyringConfigFile = "";

static bool keyringCheckConfigFile(char **newval, void **extra, GucSource source)
{
	if(*newval == NULL || strlen(*newval) == 0)
	{
		return 1; // empty
	}

	if(access(*newval, R_OK) != 0)
	{
		elog(ERROR, "The file referenced by pg_tde.keyringConfigFile doesn't exists, or is not readable to postgres");
		return 0;
	}

	if(access(*newval, W_OK) != 0)
	{
		elog(WARNING, "The file referenced by pg_tde.keyringConfigFile is writable to the database process");
		return 0;
	}

	return 1;
}

static void keyringAssignConfigFile(const char *newval, void *extra)
{
	// TODO: make sure we only load the configuration once...
	if(newval == NULL || strlen(newval) == 0)
	{
		//elog(WARNING, "pg_tde.keyringConfigFile is empty. Encryption features will not be available.");
		return;
	} 
	keyringLoadConfiguration(newval);
}

void keyringRegisterVariables(void)
{
	DefineCustomStringVariable("pg_tde.keyringConfigFile", /* name */
							"Location of the configuration file for the keyring", /* short_desc */
							NULL,	/* long_desc */
							&keyringConfigFile,	/* value address */
							"",	/* boot value */
							PGC_POSTMASTER, /* context */
							0,	/* flags */
							&keyringCheckConfigFile,	/* check_hook */
							&keyringAssignConfigFile,	/* assign_hook */
							NULL	/* show_hook */
		);
}

bool keyringLoadConfiguration(const char* configFileName)
{
	int ret = 0;
	json_object *providerO;
	const char* provider;

	struct json_object *root = json_object_from_file(configFileName);

	if(root == NULL)
	{
		elog(ERROR, "pg_tde.keyringConfigFile is not a valid JSON file. Keyring is not available.");
		return 0;
	}

	if(!json_object_object_get_ex(root, "provider", &providerO))
	{
		elog(ERROR, "Invalid pg_tde.keyringConfigFile: Missing 'provider'. Keyring is not available.");
		goto cleanup;
	}

	provider = json_object_get_string(providerO);

	if(provider == NULL || strlen(provider) == 0)
	{
		elog(ERROR, "Invalid pg_tde.keyringConfigFile: Empty 'provider'. Keyring is not available.");
		goto cleanup;
	}
	
	if(strncmp("file", provider, 5) != 0)
	{
		elog(ERROR, "Invalid pg_tde.keyringConfigFile: Unknown 'provider': %s. Currently only 'file' provider is supported. Keyring is not available.", provider);
	}

	ret = keyringFileParseConfiguration(root);

	if (!ret)
	{
		elog(ERROR, "Failed to initialize keyring provider. Keyring is not available.");
	}

cleanup:
	json_object_put(root);

	return ret;
}

const char* keyringParseStringParam(json_object* object)
{
	if(json_object_get_type(object) == json_type_object)
	{
		elog(WARNING, "Remote parameters are not yet implementeed");
	}

	return json_object_get_string(object);
}

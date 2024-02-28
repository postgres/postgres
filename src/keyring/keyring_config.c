
#include "keyring/keyring_config.h"
#include "keyring/keyring_file.h"
#include "keyring/keyring_vault.h"

#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <json.h>

#include "utils/guc.h"

char* keyringConfigFile = "";
char* keyringKeyPrefix = "";
enum KeyringProvider keyringProvider = PROVIDER_UNKNOWN;

static bool keyringCheckKeyPrefix(char **newval, void **extra, GucSource source)
{
	if(*newval == NULL || strlen(*newval) == 0)
	{
		return 1; // empty
	}

	if(strlen(*newval) > 32)
	{
		elog(ERROR, "The maximum length of pg_tde.keyringKeyPrefix is 32 characters.");
		return 0;
	}

	return 1;
}

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

	if(access(*newval, W_OK) == 0)
	{
		elog(WARNING, "The file referenced by pg_tde.keyringConfigFile is writable for the database process");
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

	DefineCustomStringVariable("pg_tde.keyringKeyPrefix", /* name */
							"Location of the configuration file for the keyring", /* short_desc */
							NULL,	/* long_desc */
							&keyringKeyPrefix,	/* value address */
							"",	/* boot value */
							PGC_POSTMASTER, /* context */
							0,	/* flags */
							&keyringCheckKeyPrefix,	/* check_hook */
							NULL,	/* assign_hook */
							NULL	/* show_hook */
		);
}

const char* keyringParseStringParam(json_object* object)
{
	if(json_object_get_type(object) == json_type_object)
	{
		elog(WARNING, "Remote parameters are not yet implemented");
	}

	return json_object_get_string(object);
}

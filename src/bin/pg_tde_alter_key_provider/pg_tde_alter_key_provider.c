
#include "postgres_fe.h"

#include "common/logging.h"
#include "catalog/tde_keyring.h"
#include "catalog/tde_global_space.h"

#include <stdarg.h>
#include <stdio.h>

/* version string we expect back from pg_tde_modify_key_provider */
#define PROGNAME "pg_tde_modify_key_provider (PostgreSQL) " PG_VERSION "\n"

static void
help(void)
{
	printf("pg_tde_modify_key_provider changes the configuration of a pg_tde key provider");
	puts("");
	printf("Usage:");
	printf("pg_tde_modify_key_provider [-D <datadir>] <dbOid> <provider_name> file <filename>");
	printf("pg_tde_modify_key_provider [-D <datadir>] <dbOid> <provider_name> vault <token> <url> <mount_path> [<ca_path>]");
	printf("pg_tde_modify_key_provider [-D <datadir>] <dbOid> <provider_name> kmip <host> <port> <cert_path> [<ca_path>]");
	printf("\nWARNING:");
	printf("This tool only changes the values, without properly XLogging the changes. Only use it in case the database is inaccessible and can't be started.");
}

#define BUFFER_SIZE 1024

static bool
build_json(char *buffer, int count,...)
{
	va_list		args;
	char	   *ptr;

	va_start(args, count);

	ptr = strcat(buffer, "{");

	for (int i = 0; i < count; ++i)
	{
		/* TODO: no validation about the paramters at all... */
		/* not much we can do without a proper JSON library */
		/* If the JSON is incorrect, it will fail a bit later when */
		/* we try to backread it */
		const char *key = va_arg(args, const char *);
		const char *value = va_arg(args, const char *);

		bool		addQuotes = value == NULL || (value[0] != '{' && value[0] != '"');

		if (i != 0)
		{
			ptr = strcat(ptr, ",");
		}


		ptr = strcat(ptr, "\"");
		ptr = strcat(ptr, key);
		ptr = strcat(ptr, "\":");

		if (addQuotes)
		{
			ptr = strcat(ptr, "\"");
		}
		if (value != NULL)
		{
			ptr = strcat(ptr, value);
		}
		if (addQuotes)
		{
			ptr = strcat(ptr, "\"");
		}
		if (ptr - buffer > BUFFER_SIZE)
		{
			printf("Error: Configuration too long.");
			return false;
		}
	}
	va_end(args);

	ptr = strcat(ptr, "}");

	if (ptr - buffer > BUFFER_SIZE)
	{
		printf("Error: Configuration too long.");
		return false;
	}

	return true;
}

int
main(int argc, char *argv[])
{
	char	   *provider_name;
	char	   *new_provider_type;
	char	   *datadir = getenv("PGDATA");

	int			argstart = 0;

	char		json[BUFFER_SIZE * 2] = {0,};
	char		tdedir[1024] = {0,};
	char	   *cptr = tdedir;
	bool		provider_found = false;
	GenericKeyring *keyring = NULL;
	KeyringProvideRecord provider;

	Oid			db_oid;

	pg_logging_init(argv[0]);
	pg_logging_set_level(PG_LOG_WARNING);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pg_tde_alter_key_provider"));

	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			help();
			exit(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			puts("pg_tde_alter_key_provider (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

	if (argc > 3)
	{
		if (strcmp(argv[1], "-D") == 0)
		{
			datadir = argv[2];
		}
		argstart += 2;
	}


	if (datadir == NULL || strlen(datadir) == 0)
	{
		help();
		puts("\n");
		printf("Error: Data directory missing");
		exit(1);
	}

	if (argc - argstart < 3)
	{
		help();
		exit(1);
	}

	db_oid = atoi(argv[1 + argstart]);
	provider_name = argv[2 + argstart];
	new_provider_type = argv[3 + argstart];

	if (strcmp("file", new_provider_type) == 0)
	{
		provider_found = true;

		if (argc - argstart != 5)
		{
			help();
			puts("\n");
			printf("Error: wrong number of arguments");
			exit(1);
		}

		if (!build_json(json, 2, "type", "file", "path", argv[4 + argstart]))
		{
			exit(1);
		}
	}

	if (strcmp("vault-v2", new_provider_type) == 0)
	{
		provider_found = true;

		if (argc - argstart != 7 && argc - argstart != 8)
		{
			help();
			puts("\n");
			printf("Error: wrong number of arguments");
			exit(1);
		}

		if (!build_json(json, 5, "type", "vault-v2", "url", argv[4 + argstart], "token", argv[5 + argstart], "mountPath", argv[6 + argstart], "caPath", (argc - argstart > 7 ? argv[7 + argstart] : "")))
		{
			exit(1);
		}
	}

	if (strcmp("kmip", new_provider_type) == 0)
	{
		provider_found = true;

		if (argc - argstart != 7 && argc - argstart != 8)
		{
			help();
			puts("\n");
			printf("Error: wrong number of arguments");
			exit(1);
		}

		if (!build_json(json, 5, "type", "kmip", "host", argv[4 + argstart], "port", argv[5 + argstart], "caPath", (argc - argstart > 7 ? argv[7 + argstart] : ""), "certPath", argv[6 + argstart]))
		{
			exit(1);
		}
	}

	if (!provider_found)
	{
		help();
		puts("\n");
		printf("Error: Unknown provider type: %s", new_provider_type);
		exit(1);
	}

	cptr = strcat(cptr, datadir);
	cptr = strcat(cptr, "/pg_tde");
	pg_tde_set_data_dir(tdedir);

	/* reports error if not found */
	keyring = GetKeyProviderByName(provider_name, db_oid);

	if (keyring == NULL)
	{
		printf("Error: provider not found");
		exit(1);
	}

	strncpy(provider.options, json, sizeof(provider.options));
	strncpy(provider.provider_name, provider_name, sizeof(provider.provider_name));
	provider.provider_type = get_keyring_provider_from_typename(new_provider_type);
	modify_key_provider_info(&provider, db_oid, false);

	printf("Key provider updated successfully!");

	return 0;
}

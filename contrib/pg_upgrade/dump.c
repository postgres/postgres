/*
 *	dump.c
 *
 *	dump functions
 *
 *	Copyright (c) 2010, PostgreSQL Global Development Group
 *	$PostgreSQL: pgsql/contrib/pg_upgrade/dump.c,v 1.7 2010/07/06 19:18:55 momjian Exp $
 */

#include "pg_upgrade.h"



void
generate_old_dump(migratorContext *ctx)
{
	/* run new pg_dumpall binary */
	prep_status(ctx, "Creating catalog dump");

	/*
	 * --binary-upgrade records the width of dropped columns in pg_class, and
	 * restores the frozenid's for databases and relations.
	 */
	exec_prog(ctx, true,
			  SYSTEMQUOTE "\"%s/pg_dumpall\" --port %d --username \"%s\" "
			  "--schema-only --binary-upgrade -f \"%s/" ALL_DUMP_FILE "\""
		   SYSTEMQUOTE, ctx->new.bindir, ctx->old.port, ctx->user, ctx->cwd);
	check_ok(ctx);
}


/*
 *	split_old_dump
 *
 *	This function splits pg_dumpall output into global values and
 *	database creation, and per-db schemas.  This allows us to create
 *	the toast place holders between restoring these two parts of the
 *	dump.  We split on the first "\connect " after a CREATE ROLE
 *	username match;  this is where the per-db restore starts.
 *
 *	We suppress recreation of our own username so we don't generate
 *	an error during restore
 */
void
split_old_dump(migratorContext *ctx)
{
	FILE	   *all_dump,
			   *globals_dump,
			   *db_dump;
	FILE	   *current_output;
	char		line[LINE_ALLOC];
	bool		start_of_line = true;
	char		create_role_str[MAX_STRING];
	char		create_role_str_quote[MAX_STRING];
	char		filename[MAXPGPATH];
	bool		suppressed_username = false;

	/* 
	 * Open all files in binary mode to avoid line end translation on Windows,
	 * both for input and output.
	 */

	snprintf(filename, sizeof(filename), "%s/%s", ctx->cwd, ALL_DUMP_FILE);
	if ((all_dump = fopen(filename, PG_BINARY_R)) == NULL)
		pg_log(ctx, PG_FATAL, "Cannot open dump file %s\n", filename);
	snprintf(filename, sizeof(filename), "%s/%s", ctx->cwd, GLOBALS_DUMP_FILE);
	if ((globals_dump = fopen(filename, PG_BINARY_W)) == NULL)
		pg_log(ctx, PG_FATAL, "Cannot write to dump file %s\n", filename);
	snprintf(filename, sizeof(filename), "%s/%s", ctx->cwd, DB_DUMP_FILE);
	if ((db_dump = fopen(filename, PG_BINARY_W)) == NULL)
		pg_log(ctx, PG_FATAL, "Cannot write to dump file %s\n", filename);
	current_output = globals_dump;

	/* patterns used to prevent our own username from being recreated */
	snprintf(create_role_str, sizeof(create_role_str),
			 "CREATE ROLE %s;", ctx->user);
	snprintf(create_role_str_quote, sizeof(create_role_str_quote),
			 "CREATE ROLE %s;", quote_identifier(ctx, ctx->user));

	while (fgets(line, sizeof(line), all_dump) != NULL)
	{
		/* switch to db_dump file output? */
		if (current_output == globals_dump && start_of_line &&
			suppressed_username &&
			strncmp(line, "\\connect ", strlen("\\connect ")) == 0)
			current_output = db_dump;

		/* output unless we are recreating our own username */
		if (current_output != globals_dump || !start_of_line ||
			(strncmp(line, create_role_str, strlen(create_role_str)) != 0 &&
			 strncmp(line, create_role_str_quote, strlen(create_role_str_quote)) != 0))
			fputs(line, current_output);
		else
			suppressed_username = true;

		if (strlen(line) > 0 && line[strlen(line) - 1] == '\n')
			start_of_line = true;
		else
			start_of_line = false;
	}

	fclose(all_dump);
	fclose(globals_dump);
	fclose(db_dump);
}

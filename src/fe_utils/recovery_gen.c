/*-------------------------------------------------------------------------
 *
 * recovery_gen.c
 *		Generator for recovery configuration
 *
 * Portions Copyright (c) 2011-2024, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include "common/logging.h"
#include "fe_utils/recovery_gen.h"
#include "fe_utils/string_utils.h"

static char *escape_quotes(const char *src);

/*
 * Write recovery configuration contents into a fresh PQExpBuffer, and
 * return it.
 *
 * This accepts the dbname which will be appended to the primary_conninfo.
 * The dbname will be ignored by walreceiver process but slotsync worker uses
 * it to connect to the primary server.
 */
PQExpBuffer
GenerateRecoveryConfig(PGconn *pgconn, const char *replication_slot,
					   char *dbname)
{
	PQconninfoOption *connOptions;
	PQExpBufferData conninfo_buf;
	char	   *escaped;
	PQExpBuffer contents;

	Assert(pgconn != NULL);

	contents = createPQExpBuffer();
	if (!contents)
		pg_fatal("out of memory");

	/*
	 * In PostgreSQL 12 and newer versions, standby_mode is gone, replaced by
	 * standby.signal to trigger a standby state at recovery.
	 */
	if (PQserverVersion(pgconn) < MINIMUM_VERSION_FOR_RECOVERY_GUC)
		appendPQExpBufferStr(contents, "standby_mode = 'on'\n");

	connOptions = PQconninfo(pgconn);
	if (connOptions == NULL)
		pg_fatal("out of memory");

	initPQExpBuffer(&conninfo_buf);
	for (PQconninfoOption *opt = connOptions; opt && opt->keyword; opt++)
	{
		/* Omit empty settings and those libpqwalreceiver overrides. */
		if (strcmp(opt->keyword, "replication") == 0 ||
			strcmp(opt->keyword, "dbname") == 0 ||
			strcmp(opt->keyword, "fallback_application_name") == 0 ||
			(opt->val == NULL) ||
			(opt->val != NULL && opt->val[0] == '\0'))
			continue;

		/* Separate key-value pairs with spaces */
		if (conninfo_buf.len != 0)
			appendPQExpBufferChar(&conninfo_buf, ' ');

		/*
		 * Write "keyword=value" pieces, the value string is escaped and/or
		 * quoted if necessary.
		 */
		appendPQExpBuffer(&conninfo_buf, "%s=", opt->keyword);
		appendConnStrVal(&conninfo_buf, opt->val);
	}

	if (dbname)
	{
		/*
		 * If dbname is specified in the connection, append the dbname. This
		 * will be used later for logical replication slot synchronization.
		 */
		if (conninfo_buf.len != 0)
			appendPQExpBufferChar(&conninfo_buf, ' ');

		appendPQExpBuffer(&conninfo_buf, "%s=", "dbname");
		appendConnStrVal(&conninfo_buf, dbname);
	}

	if (PQExpBufferDataBroken(conninfo_buf))
		pg_fatal("out of memory");

	/*
	 * Escape the connection string, so that it can be put in the config file.
	 * Note that this is different from the escaping of individual connection
	 * options above!
	 */
	escaped = escape_quotes(conninfo_buf.data);
	termPQExpBuffer(&conninfo_buf);
	appendPQExpBuffer(contents, "primary_conninfo = '%s'\n", escaped);
	free(escaped);

	if (replication_slot)
	{
		/* unescaped: ReplicationSlotValidateName allows [a-z0-9_] only */
		appendPQExpBuffer(contents, "primary_slot_name = '%s'\n",
						  replication_slot);
	}

	if (PQExpBufferBroken(contents))
		pg_fatal("out of memory");

	PQconninfoFree(connOptions);

	return contents;
}

/*
 * Write the configuration file in the directory specified in target_dir,
 * with the contents already collected in memory appended.  Then write
 * the signal file into the target_dir.  If the server does not support
 * recovery parameters as GUCs, the signal file is not necessary, and
 * configuration is written to recovery.conf.
 */
void
WriteRecoveryConfig(PGconn *pgconn, const char *target_dir, PQExpBuffer contents)
{
	char		filename[MAXPGPATH];
	FILE	   *cf;
	bool		use_recovery_conf;

	Assert(pgconn != NULL);

	use_recovery_conf =
		PQserverVersion(pgconn) < MINIMUM_VERSION_FOR_RECOVERY_GUC;

	snprintf(filename, MAXPGPATH, "%s/%s", target_dir,
			 use_recovery_conf ? "recovery.conf" : "postgresql.auto.conf");

	cf = fopen(filename, use_recovery_conf ? "w" : "a");
	if (cf == NULL)
		pg_fatal("could not open file \"%s\": %m", filename);

	if (fwrite(contents->data, contents->len, 1, cf) != 1)
		pg_fatal("could not write to file \"%s\": %m", filename);

	fclose(cf);

	if (!use_recovery_conf)
	{
		snprintf(filename, MAXPGPATH, "%s/%s", target_dir, "standby.signal");
		cf = fopen(filename, "w");
		if (cf == NULL)
			pg_fatal("could not create file \"%s\": %m", filename);

		fclose(cf);
	}
}

/*
 * Escape a string so that it can be used as a value in a key-value pair
 * a configuration file.
 */
static char *
escape_quotes(const char *src)
{
	char	   *result = escape_single_quotes_ascii(src);

	if (!result)
		pg_fatal("out of memory");
	return result;
}

/*
 *	check.c
 *
 *	server checks and output routines
 *
 *	Copyright (c) 2010-2025, PostgreSQL Global Development Group
 *	src/bin/pg_upgrade/check.c
 */

#include "postgres_fe.h"

#include "catalog/pg_authid_d.h"
#include "catalog/pg_class_d.h"
#include "fe_utils/string_utils.h"
#include "pg_upgrade.h"
#include "common/unicode_version.h"

static void check_new_cluster_is_empty(void);
static void check_is_install_user(ClusterInfo *cluster);
static void check_for_connection_status(ClusterInfo *cluster);
static void check_for_prepared_transactions(ClusterInfo *cluster);
static void check_for_isn_and_int8_passing_mismatch(ClusterInfo *cluster);
static void check_for_user_defined_postfix_ops(ClusterInfo *cluster);
static void check_for_incompatible_polymorphics(ClusterInfo *cluster);
static void check_for_tables_with_oids(ClusterInfo *cluster);
static void check_for_pg_role_prefix(ClusterInfo *cluster);
static void check_for_new_tablespace_dir(void);
static void check_for_user_defined_encoding_conversions(ClusterInfo *cluster);
static void check_for_unicode_update(ClusterInfo *cluster);
static void check_new_cluster_logical_replication_slots(void);
static void check_new_cluster_subscription_configuration(void);
static void check_old_cluster_for_valid_slots(void);
static void check_old_cluster_subscription_state(void);

/*
 * DataTypesUsageChecks - definitions of data type checks for the old cluster
 * in order to determine if an upgrade can be performed.  See the comment on
 * data_types_usage_checks below for a more detailed description.
 */
typedef struct
{
	/* Status line to print to the user */
	const char *status;
	/* Filename to store report to */
	const char *report_filename;
	/* Query to extract the oid of the datatype */
	const char *base_query;
	/* Text to store to report in case of error */
	const char *report_text;
	/* The latest version where the check applies */
	int			threshold_version;
	/* A function pointer for determining if the check applies */
	DataTypesUsageVersionCheck version_hook;
} DataTypesUsageChecks;

/*
 * Special values for threshold_version for indicating that a check applies to
 * all versions, or that a custom function needs to be invoked to determine
 * if the check applies.
 */
#define MANUAL_CHECK 1
#define ALL_VERSIONS -1

/*--
 * Data type usage checks. Each check for problematic data type usage is
 * defined in this array with metadata, SQL query for finding the data type
 * and functionality for deciding if the check is applicable to the version
 * of the old cluster. The struct members are described in detail below:
 *
 * status				A oneline string which can be printed to the user to
 *						inform about progress. Should not end with newline.
 * report_filename		The filename in which the list of problems detected by
 *						the check will be printed.
 * base_query			A query which extracts the Oid of the datatype checked
 *						for.
 * report_text			The text which will be printed to the user to explain
 *						what the check did, and why it failed. The text should
 *						end with a newline, and does not need to refer to the
 *						report_filename as that is automatically appended to
 *						the report with the path to the log folder.
 * threshold_version	The major version of PostgreSQL for which to run the
 *						check. Iff the old cluster is less than, or equal to,
 *						the threshold version then the check will be executed.
 *						If the old version is greater than the threshold then
 *						the check is skipped. If the threshold_version is set
 *						to ALL_VERSIONS then it will be run unconditionally,
 *						if set to MANUAL_CHECK then the version_hook function
 *						will be executed in order to determine whether or not
 *						to run.
 * version_hook			A function pointer to a version check function of type
 *						DataTypesUsageVersionCheck which is used to determine
 *						if the check is applicable to the old cluster. If the
 *						version_hook returns true then the check will be run,
 *						else it will be skipped. The function will only be
 *						executed iff threshold_version is set to MANUAL_CHECK.
 */
static DataTypesUsageChecks data_types_usage_checks[] =
{
	/*
	 * Look for composite types that were made during initdb *or* belong to
	 * information_schema; that's important in case information_schema was
	 * dropped and reloaded.
	 *
	 * The cutoff OID here should match the source cluster's value of
	 * FirstNormalObjectId.  We hardcode it rather than using that C #define
	 * because, if that #define is ever changed, our own version's value is
	 * NOT what to use.  Eventually we may need a test on the source cluster's
	 * version to select the correct value.
	 */
	{
		.status = gettext_noop("Checking for system-defined composite types in user tables"),
		.report_filename = "tables_using_composite.txt",
		.base_query =
		"SELECT t.oid FROM pg_catalog.pg_type t "
		"LEFT JOIN pg_catalog.pg_namespace n ON t.typnamespace = n.oid "
		" WHERE typtype = 'c' AND (t.oid < 16384 OR nspname = 'information_schema')",
		.report_text =
		gettext_noop("Your installation contains system-defined composite types in user tables.\n"
					 "These type OIDs are not stable across PostgreSQL versions,\n"
					 "so this cluster cannot currently be upgraded.  You can drop the\n"
					 "problem columns and restart the upgrade.\n"),
		.threshold_version = ALL_VERSIONS
	},

	/*
	 * 9.3 -> 9.4 Fully implement the 'line' data type in 9.4, which
	 * previously returned "not enabled" by default and was only functionally
	 * enabled with a compile-time switch; as of 9.4 "line" has a different
	 * on-disk representation format.
	 */
	{
		.status = gettext_noop("Checking for incompatible \"line\" data type"),
		.report_filename = "tables_using_line.txt",
		.base_query =
		"SELECT 'pg_catalog.line'::pg_catalog.regtype AS oid",
		.report_text =
		gettext_noop("Your installation contains the \"line\" data type in user tables.\n"
					 "This data type changed its internal and input/output format\n"
					 "between your old and new versions so this\n"
					 "cluster cannot currently be upgraded.  You can\n"
					 "drop the problem columns and restart the upgrade.\n"),
		.threshold_version = 903
	},

	/*
	 * pg_upgrade only preserves these system values: pg_class.oid pg_type.oid
	 * pg_enum.oid
	 *
	 * Many of the reg* data types reference system catalog info that is not
	 * preserved, and hence these data types cannot be used in user tables
	 * upgraded by pg_upgrade.
	 */
	{
		.status = gettext_noop("Checking for reg* data types in user tables"),
		.report_filename = "tables_using_reg.txt",

		/*
		 * Note: older servers will not have all of these reg* types, so we
		 * have to write the query like this rather than depending on casts to
		 * regtype.
		 */
		.base_query =
		"SELECT oid FROM pg_catalog.pg_type t "
		"WHERE t.typnamespace = "
		"        (SELECT oid FROM pg_catalog.pg_namespace "
		"         WHERE nspname = 'pg_catalog') "
		"  AND t.typname IN ( "
		/* pg_class.oid is preserved, so 'regclass' is OK */
		"           'regcollation', "
		"           'regconfig', "
		"           'regdictionary', "
		"           'regnamespace', "
		"           'regoper', "
		"           'regoperator', "
		"           'regproc', "
		"           'regprocedure' "
		/* pg_authid.oid is preserved, so 'regrole' is OK */
		/* pg_type.oid is (mostly) preserved, so 'regtype' is OK */
		"         )",
		.report_text =
		gettext_noop("Your installation contains one of the reg* data types in user tables.\n"
					 "These data types reference system OIDs that are not preserved by\n"
					 "pg_upgrade, so this cluster cannot currently be upgraded.  You can\n"
					 "drop the problem columns and restart the upgrade.\n"),
		.threshold_version = ALL_VERSIONS
	},

	/*
	 * PG 16 increased the size of the 'aclitem' type, which breaks the
	 * on-disk format for existing data.
	 */
	{
		.status = gettext_noop("Checking for incompatible \"aclitem\" data type"),
		.report_filename = "tables_using_aclitem.txt",
		.base_query =
		"SELECT 'pg_catalog.aclitem'::pg_catalog.regtype AS oid",
		.report_text =
		gettext_noop("Your installation contains the \"aclitem\" data type in user tables.\n"
					 "The internal format of \"aclitem\" changed in PostgreSQL version 16\n"
					 "so this cluster cannot currently be upgraded.  You can drop the\n"
					 "problem columns and restart the upgrade.\n"),
		.threshold_version = 1500
	},

	/*
	 * It's no longer allowed to create tables or views with "unknown"-type
	 * columns.  We do not complain about views with such columns, because
	 * they should get silently converted to "text" columns during the DDL
	 * dump and reload; it seems unlikely to be worth making users do that by
	 * hand.  However, if there's a table with such a column, the DDL reload
	 * will fail, so we should pre-detect that rather than failing
	 * mid-upgrade.  Worse, if there's a matview with such a column, the DDL
	 * reload will silently change it to "text" which won't match the on-disk
	 * storage (which is like "cstring").  So we *must* reject that.
	 */
	{
		.status = gettext_noop("Checking for invalid \"unknown\" user columns"),
		.report_filename = "tables_using_unknown.txt",
		.base_query =
		"SELECT 'pg_catalog.unknown'::pg_catalog.regtype AS oid",
		.report_text =
		gettext_noop("Your installation contains the \"unknown\" data type in user tables.\n"
					 "This data type is no longer allowed in tables, so this cluster\n"
					 "cannot currently be upgraded.  You can drop the problem columns\n"
					 "and restart the upgrade.\n"),
		.threshold_version = 906
	},

	/*
	 * PG 12 changed the 'sql_identifier' type storage to be based on name,
	 * not varchar, which breaks on-disk format for existing data. So we need
	 * to prevent upgrade when used in user objects (tables, indexes, ...). In
	 * 12, the sql_identifier data type was switched from name to varchar,
	 * which does affect the storage (name is by-ref, but not varlena). This
	 * means user tables using sql_identifier for columns are broken because
	 * the on-disk format is different.
	 */
	{
		.status = gettext_noop("Checking for invalid \"sql_identifier\" user columns"),
		.report_filename = "tables_using_sql_identifier.txt",
		.base_query =
		"SELECT 'information_schema.sql_identifier'::pg_catalog.regtype AS oid",
		.report_text =
		gettext_noop("Your installation contains the \"sql_identifier\" data type in user tables.\n"
					 "The on-disk format for this data type has changed, so this\n"
					 "cluster cannot currently be upgraded.  You can drop the problem\n"
					 "columns and restart the upgrade.\n"),
		.threshold_version = 1100
	},

	/*
	 * JSONB changed its storage format during 9.4 beta, so check for it.
	 */
	{
		.status = gettext_noop("Checking for incompatible \"jsonb\" data type in user tables"),
		.report_filename = "tables_using_jsonb.txt",
		.base_query =
		"SELECT 'pg_catalog.jsonb'::pg_catalog.regtype AS oid",
		.report_text =
		gettext_noop("Your installation contains the \"jsonb\" data type in user tables.\n"
					 "The internal format of \"jsonb\" changed during 9.4 beta so this\n"
					 "cluster cannot currently be upgraded.  You can drop the problem \n"
					 "columns and restart the upgrade.\n"),
		.threshold_version = MANUAL_CHECK,
		.version_hook = jsonb_9_4_check_applicable
	},

	/*
	 * PG 12 removed types abstime, reltime, tinterval.
	 */
	{
		.status = gettext_noop("Checking for removed \"abstime\" data type in user tables"),
		.report_filename = "tables_using_abstime.txt",
		.base_query =
		"SELECT 'pg_catalog.abstime'::pg_catalog.regtype AS oid",
		.report_text =
		gettext_noop("Your installation contains the \"abstime\" data type in user tables.\n"
					 "The \"abstime\" type has been removed in PostgreSQL version 12,\n"
					 "so this cluster cannot currently be upgraded.  You can drop the\n"
					 "problem columns, or change them to another data type, and restart\n"
					 "the upgrade.\n"),
		.threshold_version = 1100
	},
	{
		.status = gettext_noop("Checking for removed \"reltime\" data type in user tables"),
		.report_filename = "tables_using_reltime.txt",
		.base_query =
		"SELECT 'pg_catalog.reltime'::pg_catalog.regtype AS oid",
		.report_text =
		gettext_noop("Your installation contains the \"reltime\" data type in user tables.\n"
					 "The \"reltime\" type has been removed in PostgreSQL version 12,\n"
					 "so this cluster cannot currently be upgraded.  You can drop the\n"
					 "problem columns, or change them to another data type, and restart\n"
					 "the upgrade.\n"),
		.threshold_version = 1100
	},
	{
		.status = gettext_noop("Checking for removed \"tinterval\" data type in user tables"),
		.report_filename = "tables_using_tinterval.txt",
		.base_query =
		"SELECT 'pg_catalog.tinterval'::pg_catalog.regtype AS oid",
		.report_text =
		gettext_noop("Your installation contains the \"tinterval\" data type in user tables.\n"
					 "The \"tinterval\" type has been removed in PostgreSQL version 12,\n"
					 "so this cluster cannot currently be upgraded.  You can drop the\n"
					 "problem columns, or change them to another data type, and restart\n"
					 "the upgrade.\n"),
		.threshold_version = 1100
	},

	/* End of checks marker, must remain last */
	{
		NULL, NULL, NULL, NULL, 0, NULL
	}
};

/*
 * Private state for check_for_data_types_usage()'s UpgradeTask.
 */
struct data_type_check_state
{
	DataTypesUsageChecks *check;	/* the check for this step */
	bool		result;			/* true if check failed for any database */
	PQExpBuffer *report;		/* buffer for report on failed checks */
};

/*
 * Returns a palloc'd query string for the data type check, for use by
 * check_for_data_types_usage()'s UpgradeTask.
 */
static char *
data_type_check_query(int checknum)
{
	DataTypesUsageChecks *check = &data_types_usage_checks[checknum];

	return psprintf("WITH RECURSIVE oids AS ( "
	/* start with the type(s) returned by base_query */
					"	%s "
					"	UNION ALL "
					"	SELECT * FROM ( "
	/* inner WITH because we can only reference the CTE once */
					"		WITH x AS (SELECT oid FROM oids) "
	/* domains on any type selected so far */
					"			SELECT t.oid FROM pg_catalog.pg_type t, x WHERE typbasetype = x.oid AND typtype = 'd' "
					"			UNION ALL "
	/* arrays over any type selected so far */
					"			SELECT t.oid FROM pg_catalog.pg_type t, x WHERE typelem = x.oid AND typtype = 'b' "
					"			UNION ALL "
	/* composite types containing any type selected so far */
					"			SELECT t.oid FROM pg_catalog.pg_type t, pg_catalog.pg_class c, pg_catalog.pg_attribute a, x "
					"			WHERE t.typtype = 'c' AND "
					"				  t.oid = c.reltype AND "
					"				  c.oid = a.attrelid AND "
					"				  NOT a.attisdropped AND "
					"				  a.atttypid = x.oid "
					"			UNION ALL "
	/* ranges containing any type selected so far */
					"			SELECT t.oid FROM pg_catalog.pg_type t, pg_catalog.pg_range r, x "
					"			WHERE t.typtype = 'r' AND r.rngtypid = t.oid AND r.rngsubtype = x.oid"
					"	) foo "
					") "
	/* now look for stored columns of any such type */
					"SELECT n.nspname, c.relname, a.attname "
					"FROM	pg_catalog.pg_class c, "
					"		pg_catalog.pg_namespace n, "
					"		pg_catalog.pg_attribute a "
					"WHERE	c.oid = a.attrelid AND "
					"		NOT a.attisdropped AND "
					"		a.atttypid IN (SELECT oid FROM oids) AND "
					"		c.relkind IN ("
					CppAsString2(RELKIND_RELATION) ", "
					CppAsString2(RELKIND_MATVIEW) ", "
					CppAsString2(RELKIND_INDEX) ") AND "
					"		c.relnamespace = n.oid AND "
	/* exclude possible orphaned temp tables */
					"		n.nspname !~ '^pg_temp_' AND "
					"		n.nspname !~ '^pg_toast_temp_' AND "
	/* exclude system catalogs, too */
					"		n.nspname NOT IN ('pg_catalog', 'information_schema')",
					check->base_query);
}

/*
 * Callback function for processing results of queries for
 * check_for_data_types_usage()'s UpgradeTask.  If the query returned any rows
 * (i.e., the check failed), write the details to the report file.
 */
static void
process_data_type_check(DbInfo *dbinfo, PGresult *res, void *arg)
{
	struct data_type_check_state *state = (struct data_type_check_state *) arg;
	int			ntups = PQntuples(res);
	char		output_path[MAXPGPATH];
	int			i_nspname = PQfnumber(res, "nspname");
	int			i_relname = PQfnumber(res, "relname");
	int			i_attname = PQfnumber(res, "attname");
	FILE	   *script = NULL;

	AssertVariableIsOfType(&process_data_type_check, UpgradeTaskProcessCB);

	if (ntups == 0)
		return;

	snprintf(output_path, sizeof(output_path), "%s/%s",
			 log_opts.basedir,
			 state->check->report_filename);

	/*
	 * Make sure we have a buffer to save reports to now that we found a first
	 * failing check.
	 */
	if (*state->report == NULL)
		*state->report = createPQExpBuffer();

	/*
	 * If this is the first time we see an error for the check in question
	 * then print a status message of the failure.
	 */
	if (!state->result)
	{
		pg_log(PG_REPORT, "failed check: %s", _(state->check->status));
		appendPQExpBuffer(*state->report, "\n%s\n%s    %s\n",
						  _(state->check->report_text),
						  _("A list of the problem columns is in the file:"),
						  output_path);
	}
	state->result = true;

	if ((script = fopen_priv(output_path, "a")) == NULL)
		pg_fatal("could not open file \"%s\": %m", output_path);

	fprintf(script, "In database: %s\n", dbinfo->db_name);

	for (int rowno = 0; rowno < ntups; rowno++)
		fprintf(script, "  %s.%s.%s\n",
				PQgetvalue(res, rowno, i_nspname),
				PQgetvalue(res, rowno, i_relname),
				PQgetvalue(res, rowno, i_attname));

	fclose(script);
}

/*
 * check_for_data_types_usage()
 *	Detect whether there are any stored columns depending on given type(s)
 *
 * If so, write a report to the given file name and signal a failure to the
 * user.
 *
 * The checks to run are defined in a DataTypesUsageChecks structure where
 * each check has a metadata for explaining errors to the user, a base_query,
 * a report filename and a function pointer hook for validating if the check
 * should be executed given the cluster at hand.
 *
 * base_query should be a SELECT yielding a single column named "oid",
 * containing the pg_type OIDs of one or more types that are known to have
 * inconsistent on-disk representations across server versions.
 *
 * We check for the type(s) in tables, matviews, and indexes, but not views;
 * there's no storage involved in a view.
 */
static void
check_for_data_types_usage(ClusterInfo *cluster)
{
	PQExpBuffer report = NULL;
	DataTypesUsageChecks *tmp = data_types_usage_checks;
	int			n_data_types_usage_checks = 0;
	UpgradeTask *task = upgrade_task_create();
	char	  **queries = NULL;
	struct data_type_check_state *states;

	prep_status("Checking data type usage");

	/* Gather number of checks to perform */
	while (tmp->status != NULL)
	{
		n_data_types_usage_checks++;
		tmp++;
	}

	/* Allocate memory for queries and for task states */
	queries = pg_malloc0(sizeof(char *) * n_data_types_usage_checks);
	states = pg_malloc0(sizeof(struct data_type_check_state) * n_data_types_usage_checks);

	for (int i = 0; i < n_data_types_usage_checks; i++)
	{
		DataTypesUsageChecks *check = &data_types_usage_checks[i];

		if (check->threshold_version == MANUAL_CHECK)
		{
			Assert(check->version_hook);

			/*
			 * Make sure that the check applies to the current cluster version
			 * and skip it if not.
			 */
			if (!check->version_hook(cluster))
				continue;
		}
		else if (check->threshold_version != ALL_VERSIONS)
		{
			if (GET_MAJOR_VERSION(cluster->major_version) > check->threshold_version)
				continue;
		}
		else
			Assert(check->threshold_version == ALL_VERSIONS);

		queries[i] = data_type_check_query(i);

		states[i].check = check;
		states[i].report = &report;

		upgrade_task_add_step(task, queries[i], process_data_type_check,
							  true, &states[i]);
	}

	/*
	 * Connect to each database in the cluster and run all defined checks
	 * against that database before trying the next one.
	 */
	upgrade_task_run(task, cluster);
	upgrade_task_free(task);

	if (report)
	{
		pg_fatal("Data type checks failed: %s", report->data);
		destroyPQExpBuffer(report);
	}

	for (int i = 0; i < n_data_types_usage_checks; i++)
	{
		if (queries[i])
			pg_free(queries[i]);
	}
	pg_free(queries);
	pg_free(states);

	check_ok();
}

/*
 * fix_path_separator
 * For non-Windows, just return the argument.
 * For Windows convert any forward slash to a backslash
 * such as is suitable for arguments to builtin commands
 * like RMDIR and DEL.
 */
static char *
fix_path_separator(char *path)
{
#ifdef WIN32

	char	   *result;
	char	   *c;

	result = pg_strdup(path);

	for (c = result; *c != '\0'; c++)
		if (*c == '/')
			*c = '\\';

	return result;
#else

	return path;
#endif
}

void
output_check_banner(void)
{
	if (user_opts.live_check)
	{
		pg_log(PG_REPORT,
			   "Performing Consistency Checks on Old Live Server\n"
			   "------------------------------------------------");
	}
	else
	{
		pg_log(PG_REPORT,
			   "Performing Consistency Checks\n"
			   "-----------------------------");
	}
}


void
check_and_dump_old_cluster(void)
{
	/* -- OLD -- */

	if (!user_opts.live_check)
		start_postmaster(&old_cluster, true);

	/*
	 * First check that all databases allow connections since we'll otherwise
	 * fail in later stages.
	 */
	check_for_connection_status(&old_cluster);

	/*
	 * Extract a list of databases, tables, and logical replication slots from
	 * the old cluster.
	 */
	get_db_rel_and_slot_infos(&old_cluster);

	init_tablespaces();

	get_loadable_libraries();


	/*
	 * Check for various failure cases
	 */
	check_is_install_user(&old_cluster);
	check_for_prepared_transactions(&old_cluster);
	check_for_isn_and_int8_passing_mismatch(&old_cluster);

	if (GET_MAJOR_VERSION(old_cluster.major_version) >= 1700)
	{
		/*
		 * Logical replication slots can be migrated since PG17. See comments
		 * atop get_old_cluster_logical_slot_infos().
		 */
		check_old_cluster_for_valid_slots();

		/*
		 * Subscriptions and their dependencies can be migrated since PG17.
		 * Before that the logical slots are not upgraded, so we will not be
		 * able to upgrade the logical replication clusters completely.
		 */
		get_subscription_count(&old_cluster);
		check_old_cluster_subscription_state();
	}

	check_for_data_types_usage(&old_cluster);

	/*
	 * Unicode updates can affect some objects that use expressions with
	 * functions dependent on Unicode.
	 */
	check_for_unicode_update(&old_cluster);

	/*
	 * PG 14 changed the function signature of encoding conversion functions.
	 * Conversions from older versions cannot be upgraded automatically
	 * because the user-defined functions used by the encoding conversions
	 * need to be changed to match the new signature.
	 */
	if (GET_MAJOR_VERSION(old_cluster.major_version) <= 1300)
		check_for_user_defined_encoding_conversions(&old_cluster);

	/*
	 * Pre-PG 14 allowed user defined postfix operators, which are not
	 * supported anymore.  Verify there are none, iff applicable.
	 */
	if (GET_MAJOR_VERSION(old_cluster.major_version) <= 1300)
		check_for_user_defined_postfix_ops(&old_cluster);

	/*
	 * PG 14 changed polymorphic functions from anyarray to
	 * anycompatiblearray.
	 */
	if (GET_MAJOR_VERSION(old_cluster.major_version) <= 1300)
		check_for_incompatible_polymorphics(&old_cluster);

	/*
	 * Pre-PG 12 allowed tables to be declared WITH OIDS, which is not
	 * supported anymore. Verify there are none, iff applicable.
	 */
	if (GET_MAJOR_VERSION(old_cluster.major_version) <= 1100)
		check_for_tables_with_oids(&old_cluster);

	/*
	 * Pre-PG 10 allowed tables with 'unknown' type columns and non WAL logged
	 * hash indexes
	 */
	if (GET_MAJOR_VERSION(old_cluster.major_version) <= 906)
	{
		if (user_opts.check)
			old_9_6_invalidate_hash_indexes(&old_cluster, true);
	}

	/* 9.5 and below should not have roles starting with pg_ */
	if (GET_MAJOR_VERSION(old_cluster.major_version) <= 905)
		check_for_pg_role_prefix(&old_cluster);

	/*
	 * While not a check option, we do this now because this is the only time
	 * the old server is running.
	 */
	if (!user_opts.check)
		generate_old_dump();

	if (!user_opts.live_check)
		stop_postmaster(false);
}


void
check_new_cluster(void)
{
	get_db_rel_and_slot_infos(&new_cluster);

	check_new_cluster_is_empty();

	check_loadable_libraries();

	switch (user_opts.transfer_mode)
	{
		case TRANSFER_MODE_CLONE:
			check_file_clone();
			break;
		case TRANSFER_MODE_COPY:
			break;
		case TRANSFER_MODE_COPY_FILE_RANGE:
			check_copy_file_range();
			break;
		case TRANSFER_MODE_LINK:
			check_hard_link(TRANSFER_MODE_LINK);
			break;
		case TRANSFER_MODE_SWAP:

			/*
			 * We do the hard link check for --swap, too, since it's an easy
			 * way to verify the clusters are in the same file system.  This
			 * allows us to take some shortcuts in the file synchronization
			 * step.  With some more effort, we could probably support the
			 * separate-file-system use case, but this mode is unlikely to
			 * offer much benefit if we have to copy the files across file
			 * system boundaries.
			 */
			check_hard_link(TRANSFER_MODE_SWAP);

			/*
			 * There are a few known issues with using --swap to upgrade from
			 * versions older than 10.  For example, the sequence tuple format
			 * changed in v10, and the visibility map format changed in 9.6.
			 * While such problems are not insurmountable (and we may have to
			 * deal with similar problems in the future, anyway), it doesn't
			 * seem worth the effort to support swap mode for upgrades from
			 * long-unsupported versions.
			 */
			if (GET_MAJOR_VERSION(old_cluster.major_version) < 1000)
				pg_fatal("Swap mode can only upgrade clusters from PostgreSQL version %s and later.",
						 "10");

			break;
	}

	check_is_install_user(&new_cluster);

	check_for_prepared_transactions(&new_cluster);

	check_for_new_tablespace_dir();

	check_new_cluster_logical_replication_slots();

	check_new_cluster_subscription_configuration();
}


void
report_clusters_compatible(void)
{
	if (user_opts.check)
	{
		pg_log(PG_REPORT, "\n*Clusters are compatible*");
		/* stops new cluster */
		stop_postmaster(false);

		cleanup_output_dirs();
		exit(0);
	}

	pg_log(PG_REPORT, "\n"
		   "If pg_upgrade fails after this point, you must re-initdb the\n"
		   "new cluster before continuing.");
}


void
issue_warnings_and_set_wal_level(void)
{
	/*
	 * We unconditionally start/stop the new server because pg_resetwal -o set
	 * wal_level to 'minimum'.  If the user is upgrading standby servers using
	 * the rsync instructions, they will need pg_upgrade to write its final
	 * WAL record showing wal_level as 'replica'.
	 */
	start_postmaster(&new_cluster, true);

	/* Reindex hash indexes for old < 10.0 */
	if (GET_MAJOR_VERSION(old_cluster.major_version) <= 906)
		old_9_6_invalidate_hash_indexes(&new_cluster, false);

	report_extension_updates(&new_cluster);

	stop_postmaster(false);
}


void
output_completion_banner(char *deletion_script_file_name)
{
	PQExpBufferData user_specification;

	initPQExpBuffer(&user_specification);
	if (os_info.user_specified)
	{
		appendPQExpBufferStr(&user_specification, "-U ");
		appendShellString(&user_specification, os_info.user);
		appendPQExpBufferChar(&user_specification, ' ');
	}

	pg_log(PG_REPORT,
		   "Some optimizer statistics may not have been transferred by pg_upgrade.\n"
		   "Once you start the new server, consider running:\n"
		   "    %s/vacuumdb %s--all --analyze-in-stages --missing-stats-only", new_cluster.bindir, user_specification.data);

	if (deletion_script_file_name)
		pg_log(PG_REPORT,
			   "Running this script will delete the old cluster's data files:\n"
			   "    %s",
			   deletion_script_file_name);
	else
		pg_log(PG_REPORT,
			   "Could not create a script to delete the old cluster's data files\n"
			   "because user-defined tablespaces or the new cluster's data directory\n"
			   "exist in the old cluster directory.  The old cluster's contents must\n"
			   "be deleted manually.");

	termPQExpBuffer(&user_specification);
}


void
check_cluster_versions(void)
{
	prep_status("Checking cluster versions");

	/* cluster versions should already have been obtained */
	Assert(old_cluster.major_version != 0);
	Assert(new_cluster.major_version != 0);

	/*
	 * We allow upgrades from/to the same major version for alpha/beta
	 * upgrades
	 */

	if (GET_MAJOR_VERSION(old_cluster.major_version) < 902)
		pg_fatal("This utility can only upgrade from PostgreSQL version %s and later.",
				 "9.2");

	/* Only current PG version is supported as a target */
	if (GET_MAJOR_VERSION(new_cluster.major_version) != GET_MAJOR_VERSION(PG_VERSION_NUM))
		pg_fatal("This utility can only upgrade to PostgreSQL version %s.",
				 PG_MAJORVERSION);

	/*
	 * We can't allow downgrading because we use the target pg_dump, and
	 * pg_dump cannot operate on newer database versions, only current and
	 * older versions.
	 */
	if (old_cluster.major_version > new_cluster.major_version)
		pg_fatal("This utility cannot be used to downgrade to older major PostgreSQL versions.");

	/* Ensure binaries match the designated data directories */
	if (GET_MAJOR_VERSION(old_cluster.major_version) !=
		GET_MAJOR_VERSION(old_cluster.bin_version))
		pg_fatal("Old cluster data and binary directories are from different major versions.");
	if (GET_MAJOR_VERSION(new_cluster.major_version) !=
		GET_MAJOR_VERSION(new_cluster.bin_version))
		pg_fatal("New cluster data and binary directories are from different major versions.");

	/*
	 * Since from version 18, newly created database clusters always have
	 * 'signed' default char-signedness, it makes less sense to use
	 * --set-char-signedness option for upgrading from version 18 or later.
	 * Users who want to change the default char signedness of the new
	 * cluster, they can use pg_resetwal manually before the upgrade.
	 */
	if (GET_MAJOR_VERSION(old_cluster.major_version) >= 1800 &&
		user_opts.char_signedness != -1)
		pg_fatal("%s option cannot be used to upgrade from PostgreSQL %s and later.",
				 "--set-char-signedness", "18");

	check_ok();
}


void
check_cluster_compatibility(void)
{
	/* get/check pg_control data of servers */
	get_control_data(&old_cluster);
	get_control_data(&new_cluster);
	check_control_data(&old_cluster.controldata, &new_cluster.controldata);

	if (user_opts.live_check && old_cluster.port == new_cluster.port)
		pg_fatal("When checking a live server, "
				 "the old and new port numbers must be different.");
}


static void
check_new_cluster_is_empty(void)
{
	int			dbnum;

	for (dbnum = 0; dbnum < new_cluster.dbarr.ndbs; dbnum++)
	{
		int			relnum;
		RelInfoArr *rel_arr = &new_cluster.dbarr.dbs[dbnum].rel_arr;

		for (relnum = 0; relnum < rel_arr->nrels;
			 relnum++)
		{
			/* pg_largeobject and its index should be skipped */
			if (strcmp(rel_arr->rels[relnum].nspname, "pg_catalog") != 0)
				pg_fatal("New cluster database \"%s\" is not empty: found relation \"%s.%s\"",
						 new_cluster.dbarr.dbs[dbnum].db_name,
						 rel_arr->rels[relnum].nspname,
						 rel_arr->rels[relnum].relname);
		}
	}
}

/*
 * A previous run of pg_upgrade might have failed and the new cluster
 * directory recreated, but they might have forgotten to remove
 * the new cluster's tablespace directories.  Therefore, check that
 * new cluster tablespace directories do not already exist.  If
 * they do, it would cause an error while restoring global objects.
 * This allows the failure to be detected at check time, rather than
 * during schema restore.
 */
static void
check_for_new_tablespace_dir(void)
{
	int			tblnum;
	char		new_tablespace_dir[MAXPGPATH];

	prep_status("Checking for new cluster tablespace directories");

	for (tblnum = 0; tblnum < os_info.num_old_tablespaces; tblnum++)
	{
		struct stat statbuf;

		snprintf(new_tablespace_dir, MAXPGPATH, "%s%s",
				 os_info.old_tablespaces[tblnum],
				 new_cluster.tablespace_suffix);

		if (stat(new_tablespace_dir, &statbuf) == 0 || errno != ENOENT)
			pg_fatal("new cluster tablespace directory already exists: \"%s\"",
					 new_tablespace_dir);
	}

	check_ok();
}

/*
 * create_script_for_old_cluster_deletion()
 *
 *	This is particularly useful for tablespace deletion.
 */
void
create_script_for_old_cluster_deletion(char **deletion_script_file_name)
{
	FILE	   *script = NULL;
	int			tblnum;
	char		old_cluster_pgdata[MAXPGPATH],
				new_cluster_pgdata[MAXPGPATH];
	char	   *old_tblspc_suffix;

	*deletion_script_file_name = psprintf("%sdelete_old_cluster.%s",
										  SCRIPT_PREFIX, SCRIPT_EXT);

	strlcpy(old_cluster_pgdata, old_cluster.pgdata, MAXPGPATH);
	canonicalize_path(old_cluster_pgdata);

	strlcpy(new_cluster_pgdata, new_cluster.pgdata, MAXPGPATH);
	canonicalize_path(new_cluster_pgdata);

	/* Some people put the new data directory inside the old one. */
	if (path_is_prefix_of_path(old_cluster_pgdata, new_cluster_pgdata))
	{
		pg_log(PG_WARNING,
			   "\nWARNING:  new data directory should not be inside the old data directory, i.e. %s", old_cluster_pgdata);

		/* Unlink file in case it is left over from a previous run. */
		unlink(*deletion_script_file_name);
		pg_free(*deletion_script_file_name);
		*deletion_script_file_name = NULL;
		return;
	}

	/*
	 * Some users (oddly) create tablespaces inside the cluster data
	 * directory.  We can't create a proper old cluster delete script in that
	 * case.
	 */
	for (tblnum = 0; tblnum < os_info.num_old_tablespaces; tblnum++)
	{
		char		old_tablespace_dir[MAXPGPATH];

		strlcpy(old_tablespace_dir, os_info.old_tablespaces[tblnum], MAXPGPATH);
		canonicalize_path(old_tablespace_dir);
		if (path_is_prefix_of_path(old_cluster_pgdata, old_tablespace_dir))
		{
			/* reproduce warning from CREATE TABLESPACE that is in the log */
			pg_log(PG_WARNING,
				   "\nWARNING:  user-defined tablespace locations should not be inside the data directory, i.e. %s", old_tablespace_dir);

			/* Unlink file in case it is left over from a previous run. */
			unlink(*deletion_script_file_name);
			pg_free(*deletion_script_file_name);
			*deletion_script_file_name = NULL;
			return;
		}
	}

	prep_status("Creating script to delete old cluster");

	if ((script = fopen_priv(*deletion_script_file_name, "w")) == NULL)
		pg_fatal("could not open file \"%s\": %m",
				 *deletion_script_file_name);

#ifndef WIN32
	/* add shebang header */
	fprintf(script, "#!/bin/sh\n\n");
#endif

	/* delete old cluster's default tablespace */
	fprintf(script, RMDIR_CMD " %c%s%c\n", PATH_QUOTE,
			fix_path_separator(old_cluster.pgdata), PATH_QUOTE);

	/* delete old cluster's alternate tablespaces */
	old_tblspc_suffix = pg_strdup(old_cluster.tablespace_suffix);
	fix_path_separator(old_tblspc_suffix);
	for (tblnum = 0; tblnum < os_info.num_old_tablespaces; tblnum++)
		fprintf(script, RMDIR_CMD " %c%s%s%c\n", PATH_QUOTE,
				fix_path_separator(os_info.old_tablespaces[tblnum]),
				old_tblspc_suffix, PATH_QUOTE);
	pfree(old_tblspc_suffix);

	fclose(script);

#ifndef WIN32
	if (chmod(*deletion_script_file_name, S_IRWXU) != 0)
		pg_fatal("could not add execute permission to file \"%s\": %m",
				 *deletion_script_file_name);
#endif

	check_ok();
}


/*
 *	check_is_install_user()
 *
 *	Check we are the install user, and that the new cluster
 *	has no other users.
 */
static void
check_is_install_user(ClusterInfo *cluster)
{
	PGresult   *res;
	PGconn	   *conn = connectToServer(cluster, "template1");

	prep_status("Checking database user is the install user");

	/* Can't use pg_authid because only superusers can view it. */
	res = executeQueryOrDie(conn,
							"SELECT rolsuper, oid "
							"FROM pg_catalog.pg_roles "
							"WHERE rolname = current_user "
							"AND rolname !~ '^pg_'");

	/*
	 * We only allow the install user in the new cluster (see comment below)
	 * and we preserve pg_authid.oid, so this must be the install user in the
	 * old cluster too.
	 */
	if (PQntuples(res) != 1 ||
		atooid(PQgetvalue(res, 0, 1)) != BOOTSTRAP_SUPERUSERID)
		pg_fatal("database user \"%s\" is not the install user",
				 os_info.user);

	PQclear(res);

	res = executeQueryOrDie(conn,
							"SELECT COUNT(*) "
							"FROM pg_catalog.pg_roles "
							"WHERE rolname !~ '^pg_'");

	if (PQntuples(res) != 1)
		pg_fatal("could not determine the number of users");

	/*
	 * We only allow the install user in the new cluster because other defined
	 * users might match users defined in the old cluster and generate an
	 * error during pg_dump restore.
	 */
	if (cluster == &new_cluster && strcmp(PQgetvalue(res, 0, 0), "1") != 0)
		pg_fatal("Only the install user can be defined in the new cluster.");

	PQclear(res);

	PQfinish(conn);

	check_ok();
}


/*
 *	check_for_connection_status
 *
 *	Ensure that all non-template0 databases allow connections since they
 *	otherwise won't be restored; and that template0 explicitly doesn't allow
 *	connections since it would make pg_dumpall --globals restore fail.
 */
static void
check_for_connection_status(ClusterInfo *cluster)
{
	int			dbnum;
	PGconn	   *conn_template1;
	PGresult   *dbres;
	int			ntups;
	int			i_datname;
	int			i_datallowconn;
	int			i_datconnlimit;
	FILE	   *script = NULL;
	char		output_path[MAXPGPATH];

	prep_status("Checking database connection settings");

	snprintf(output_path, sizeof(output_path), "%s/%s",
			 log_opts.basedir,
			 "databases_cannot_connect_to.txt");

	conn_template1 = connectToServer(cluster, "template1");

	/* get database names */
	dbres = executeQueryOrDie(conn_template1,
							  "SELECT	datname, datallowconn, datconnlimit "
							  "FROM	pg_catalog.pg_database");

	i_datname = PQfnumber(dbres, "datname");
	i_datallowconn = PQfnumber(dbres, "datallowconn");
	i_datconnlimit = PQfnumber(dbres, "datconnlimit");

	ntups = PQntuples(dbres);
	for (dbnum = 0; dbnum < ntups; dbnum++)
	{
		char	   *datname = PQgetvalue(dbres, dbnum, i_datname);
		char	   *datallowconn = PQgetvalue(dbres, dbnum, i_datallowconn);
		char	   *datconnlimit = PQgetvalue(dbres, dbnum, i_datconnlimit);

		if (strcmp(datname, "template0") == 0)
		{
			/* avoid restore failure when pg_dumpall tries to create template0 */
			if (strcmp(datallowconn, "t") == 0)
				pg_fatal("template0 must not allow connections, "
						 "i.e. its pg_database.datallowconn must be false");
		}
		else
		{
			/*
			 * Avoid datallowconn == false databases from being skipped on
			 * restore, and ensure that no databases are marked invalid with
			 * datconnlimit == -2.
			 */
			if ((strcmp(datallowconn, "f") == 0) || strcmp(datconnlimit, "-2") == 0)
			{
				if (script == NULL && (script = fopen_priv(output_path, "w")) == NULL)
					pg_fatal("could not open file \"%s\": %m", output_path);

				fprintf(script, "%s\n", datname);
			}
		}
	}

	PQclear(dbres);

	PQfinish(conn_template1);

	if (script)
	{
		fclose(script);
		pg_log(PG_REPORT, "fatal");
		pg_fatal("All non-template0 databases must allow connections, i.e. their\n"
				 "pg_database.datallowconn must be true and pg_database.datconnlimit\n"
				 "must not be -2.  Your installation contains non-template0 databases\n"
				 "which cannot be connected to.  Consider allowing connection for all\n"
				 "non-template0 databases or drop the databases which do not allow\n"
				 "connections.  A list of databases with the problem is in the file:\n"
				 "    %s", output_path);
	}
	else
		check_ok();
}


/*
 *	check_for_prepared_transactions()
 *
 *	Make sure there are no prepared transactions because the storage format
 *	might have changed.
 */
static void
check_for_prepared_transactions(ClusterInfo *cluster)
{
	PGresult   *res;
	PGconn	   *conn = connectToServer(cluster, "template1");

	prep_status("Checking for prepared transactions");

	res = executeQueryOrDie(conn,
							"SELECT * "
							"FROM pg_catalog.pg_prepared_xacts");

	if (PQntuples(res) != 0)
	{
		if (cluster == &old_cluster)
			pg_fatal("The source cluster contains prepared transactions");
		else
			pg_fatal("The target cluster contains prepared transactions");
	}

	PQclear(res);

	PQfinish(conn);

	check_ok();
}

/*
 * Callback function for processing result of query for
 * check_for_isn_and_int8_passing_mismatch()'s UpgradeTask.  If the query
 * returned any rows (i.e., the check failed), write the details to the report
 * file.
 */
static void
process_isn_and_int8_passing_mismatch(DbInfo *dbinfo, PGresult *res, void *arg)
{
	int			ntups = PQntuples(res);
	int			i_nspname = PQfnumber(res, "nspname");
	int			i_proname = PQfnumber(res, "proname");
	UpgradeTaskReport *report = (UpgradeTaskReport *) arg;

	AssertVariableIsOfType(&process_isn_and_int8_passing_mismatch,
						   UpgradeTaskProcessCB);

	if (ntups == 0)
		return;

	if (report->file == NULL &&
		(report->file = fopen_priv(report->path, "w")) == NULL)
		pg_fatal("could not open file \"%s\": %m", report->path);

	fprintf(report->file, "In database: %s\n", dbinfo->db_name);

	for (int rowno = 0; rowno < ntups; rowno++)
		fprintf(report->file, "  %s.%s\n",
				PQgetvalue(res, rowno, i_nspname),
				PQgetvalue(res, rowno, i_proname));
}

/*
 *	check_for_isn_and_int8_passing_mismatch()
 *
 *	contrib/isn relies on data type int8, and in 8.4 int8 can now be passed
 *	by value.  The schema dumps the CREATE TYPE PASSEDBYVALUE setting so
 *	it must match for the old and new servers.
 */
static void
check_for_isn_and_int8_passing_mismatch(ClusterInfo *cluster)
{
	UpgradeTask *task;
	UpgradeTaskReport report;
	const char *query = "SELECT n.nspname, p.proname "
		"FROM   pg_catalog.pg_proc p, "
		"       pg_catalog.pg_namespace n "
		"WHERE  p.pronamespace = n.oid AND "
		"       p.probin = '$libdir/isn'";

	prep_status("Checking for contrib/isn with bigint-passing mismatch");

	if (old_cluster.controldata.float8_pass_by_value ==
		new_cluster.controldata.float8_pass_by_value)
	{
		/* no mismatch */
		check_ok();
		return;
	}

	report.file = NULL;
	snprintf(report.path, sizeof(report.path), "%s/%s",
			 log_opts.basedir,
			 "contrib_isn_and_int8_pass_by_value.txt");

	task = upgrade_task_create();
	upgrade_task_add_step(task, query, process_isn_and_int8_passing_mismatch,
						  true, &report);
	upgrade_task_run(task, cluster);
	upgrade_task_free(task);

	if (report.file)
	{
		fclose(report.file);
		pg_log(PG_REPORT, "fatal");
		pg_fatal("Your installation contains \"contrib/isn\" functions which rely on the\n"
				 "bigint data type.  Your old and new clusters pass bigint values\n"
				 "differently so this cluster cannot currently be upgraded.  You can\n"
				 "manually dump databases in the old cluster that use \"contrib/isn\"\n"
				 "facilities, drop them, perform the upgrade, and then restore them.  A\n"
				 "list of the problem functions is in the file:\n"
				 "    %s", report.path);
	}
	else
		check_ok();
}

/*
 * Callback function for processing result of query for
 * check_for_user_defined_postfix_ops()'s UpgradeTask.  If the query returned
 * any rows (i.e., the check failed), write the details to the report file.
 */
static void
process_user_defined_postfix_ops(DbInfo *dbinfo, PGresult *res, void *arg)
{
	UpgradeTaskReport *report = (UpgradeTaskReport *) arg;
	int			ntups = PQntuples(res);
	int			i_oproid = PQfnumber(res, "oproid");
	int			i_oprnsp = PQfnumber(res, "oprnsp");
	int			i_oprname = PQfnumber(res, "oprname");
	int			i_typnsp = PQfnumber(res, "typnsp");
	int			i_typname = PQfnumber(res, "typname");

	AssertVariableIsOfType(&process_user_defined_postfix_ops,
						   UpgradeTaskProcessCB);

	if (ntups == 0)
		return;

	if (report->file == NULL &&
		(report->file = fopen_priv(report->path, "w")) == NULL)
		pg_fatal("could not open file \"%s\": %m", report->path);

	fprintf(report->file, "In database: %s\n", dbinfo->db_name);

	for (int rowno = 0; rowno < ntups; rowno++)
		fprintf(report->file, "  (oid=%s) %s.%s (%s.%s, NONE)\n",
				PQgetvalue(res, rowno, i_oproid),
				PQgetvalue(res, rowno, i_oprnsp),
				PQgetvalue(res, rowno, i_oprname),
				PQgetvalue(res, rowno, i_typnsp),
				PQgetvalue(res, rowno, i_typname));
}

/*
 * Verify that no user defined postfix operators exist.
 */
static void
check_for_user_defined_postfix_ops(ClusterInfo *cluster)
{
	UpgradeTaskReport report;
	UpgradeTask *task = upgrade_task_create();
	const char *query;

	/*
	 * The query below hardcodes FirstNormalObjectId as 16384 rather than
	 * interpolating that C #define into the query because, if that #define is
	 * ever changed, the cutoff we want to use is the value used by
	 * pre-version 14 servers, not that of some future version.
	 */
	query = "SELECT o.oid AS oproid, "
		"       n.nspname AS oprnsp, "
		"       o.oprname, "
		"       tn.nspname AS typnsp, "
		"       t.typname "
		"FROM pg_catalog.pg_operator o, "
		"     pg_catalog.pg_namespace n, "
		"     pg_catalog.pg_type t, "
		"     pg_catalog.pg_namespace tn "
		"WHERE o.oprnamespace = n.oid AND "
		"      o.oprleft = t.oid AND "
		"      t.typnamespace = tn.oid AND "
		"      o.oprright = 0 AND "
		"      o.oid >= 16384";

	prep_status("Checking for user-defined postfix operators");

	report.file = NULL;
	snprintf(report.path, sizeof(report.path), "%s/%s",
			 log_opts.basedir,
			 "postfix_ops.txt");

	upgrade_task_add_step(task, query, process_user_defined_postfix_ops,
						  true, &report);
	upgrade_task_run(task, cluster);
	upgrade_task_free(task);

	if (report.file)
	{
		fclose(report.file);
		pg_log(PG_REPORT, "fatal");
		pg_fatal("Your installation contains user-defined postfix operators, which are not\n"
				 "supported anymore.  Consider dropping the postfix operators and replacing\n"
				 "them with prefix operators or function calls.\n"
				 "A list of user-defined postfix operators is in the file:\n"
				 "    %s", report.path);
	}
	else
		check_ok();
}

/*
 * Callback function for processing results of query for
 * check_for_incompatible_polymorphics()'s UpgradeTask.  If the query returned
 * any rows (i.e., the check failed), write the details to the report file.
 */
static void
process_incompat_polymorphics(DbInfo *dbinfo, PGresult *res, void *arg)
{
	UpgradeTaskReport *report = (UpgradeTaskReport *) arg;
	int			ntups = PQntuples(res);
	int			i_objkind = PQfnumber(res, "objkind");
	int			i_objname = PQfnumber(res, "objname");

	AssertVariableIsOfType(&process_incompat_polymorphics,
						   UpgradeTaskProcessCB);

	if (ntups == 0)
		return;

	if (report->file == NULL &&
		(report->file = fopen_priv(report->path, "w")) == NULL)
		pg_fatal("could not open file \"%s\": %m", report->path);

	fprintf(report->file, "In database: %s\n", dbinfo->db_name);

	for (int rowno = 0; rowno < ntups; rowno++)
		fprintf(report->file, "  %s: %s\n",
				PQgetvalue(res, rowno, i_objkind),
				PQgetvalue(res, rowno, i_objname));
}

/*
 *	check_for_incompatible_polymorphics()
 *
 *	Make sure nothing is using old polymorphic functions with
 *	anyarray/anyelement rather than the new anycompatible variants.
 */
static void
check_for_incompatible_polymorphics(ClusterInfo *cluster)
{
	PQExpBufferData old_polymorphics;
	UpgradeTask *task = upgrade_task_create();
	UpgradeTaskReport report;
	char	   *query;

	prep_status("Checking for incompatible polymorphic functions");

	report.file = NULL;
	snprintf(report.path, sizeof(report.path), "%s/%s",
			 log_opts.basedir,
			 "incompatible_polymorphics.txt");

	/* The set of problematic functions varies a bit in different versions */
	initPQExpBuffer(&old_polymorphics);

	appendPQExpBufferStr(&old_polymorphics,
						 "'array_append(anyarray,anyelement)'"
						 ", 'array_cat(anyarray,anyarray)'"
						 ", 'array_prepend(anyelement,anyarray)'");

	if (GET_MAJOR_VERSION(cluster->major_version) >= 903)
		appendPQExpBufferStr(&old_polymorphics,
							 ", 'array_remove(anyarray,anyelement)'"
							 ", 'array_replace(anyarray,anyelement,anyelement)'");

	if (GET_MAJOR_VERSION(cluster->major_version) >= 905)
		appendPQExpBufferStr(&old_polymorphics,
							 ", 'array_position(anyarray,anyelement)'"
							 ", 'array_position(anyarray,anyelement,integer)'"
							 ", 'array_positions(anyarray,anyelement)'"
							 ", 'width_bucket(anyelement,anyarray)'");

	/*
	 * The query below hardcodes FirstNormalObjectId as 16384 rather than
	 * interpolating that C #define into the query because, if that #define is
	 * ever changed, the cutoff we want to use is the value used by
	 * pre-version 14 servers, not that of some future version.
	 */

	/* Aggregate transition functions */
	query = psprintf("SELECT 'aggregate' AS objkind, p.oid::regprocedure::text AS objname "
					 "FROM pg_proc AS p "
					 "JOIN pg_aggregate AS a ON a.aggfnoid=p.oid "
					 "JOIN pg_proc AS transfn ON transfn.oid=a.aggtransfn "
					 "WHERE p.oid >= 16384 "
					 "AND a.aggtransfn = ANY(ARRAY[%s]::regprocedure[]) "
					 "AND a.aggtranstype = ANY(ARRAY['anyarray', 'anyelement']::regtype[]) "

	/* Aggregate final functions */
					 "UNION ALL "
					 "SELECT 'aggregate' AS objkind, p.oid::regprocedure::text AS objname "
					 "FROM pg_proc AS p "
					 "JOIN pg_aggregate AS a ON a.aggfnoid=p.oid "
					 "JOIN pg_proc AS finalfn ON finalfn.oid=a.aggfinalfn "
					 "WHERE p.oid >= 16384 "
					 "AND a.aggfinalfn = ANY(ARRAY[%s]::regprocedure[]) "
					 "AND a.aggtranstype = ANY(ARRAY['anyarray', 'anyelement']::regtype[]) "

	/* Operators */
					 "UNION ALL "
					 "SELECT 'operator' AS objkind, op.oid::regoperator::text AS objname "
					 "FROM pg_operator AS op "
					 "WHERE op.oid >= 16384 "
					 "AND oprcode = ANY(ARRAY[%s]::regprocedure[]) "
					 "AND oprleft = ANY(ARRAY['anyarray', 'anyelement']::regtype[])",
					 old_polymorphics.data,
					 old_polymorphics.data,
					 old_polymorphics.data);

	upgrade_task_add_step(task, query, process_incompat_polymorphics,
						  true, &report);
	upgrade_task_run(task, cluster);
	upgrade_task_free(task);

	if (report.file)
	{
		fclose(report.file);
		pg_log(PG_REPORT, "fatal");
		pg_fatal("Your installation contains user-defined objects that refer to internal\n"
				 "polymorphic functions with arguments of type \"anyarray\" or \"anyelement\".\n"
				 "These user-defined objects must be dropped before upgrading and restored\n"
				 "afterwards, changing them to refer to the new corresponding functions with\n"
				 "arguments of type \"anycompatiblearray\" and \"anycompatible\".\n"
				 "A list of the problematic objects is in the file:\n"
				 "    %s", report.path);
	}
	else
		check_ok();

	termPQExpBuffer(&old_polymorphics);
	pg_free(query);
}

/*
 * Callback function for processing results of query for
 * check_for_tables_with_oids()'s UpgradeTask.  If the query returned any rows
 * (i.e., the check failed), write the details to the report file.
 */
static void
process_with_oids_check(DbInfo *dbinfo, PGresult *res, void *arg)
{
	UpgradeTaskReport *report = (UpgradeTaskReport *) arg;
	int			ntups = PQntuples(res);
	int			i_nspname = PQfnumber(res, "nspname");
	int			i_relname = PQfnumber(res, "relname");

	AssertVariableIsOfType(&process_with_oids_check, UpgradeTaskProcessCB);

	if (ntups == 0)
		return;

	if (report->file == NULL &&
		(report->file = fopen_priv(report->path, "w")) == NULL)
		pg_fatal("could not open file \"%s\": %m", report->path);

	fprintf(report->file, "In database: %s\n", dbinfo->db_name);

	for (int rowno = 0; rowno < ntups; rowno++)
		fprintf(report->file, "  %s.%s\n",
				PQgetvalue(res, rowno, i_nspname),
				PQgetvalue(res, rowno, i_relname));
}

/*
 * Verify that no tables are declared WITH OIDS.
 */
static void
check_for_tables_with_oids(ClusterInfo *cluster)
{
	UpgradeTaskReport report;
	UpgradeTask *task = upgrade_task_create();
	const char *query = "SELECT n.nspname, c.relname "
		"FROM   pg_catalog.pg_class c, "
		"       pg_catalog.pg_namespace n "
		"WHERE  c.relnamespace = n.oid AND "
		"       c.relhasoids AND"
		"       n.nspname NOT IN ('pg_catalog')";

	prep_status("Checking for tables WITH OIDS");

	report.file = NULL;
	snprintf(report.path, sizeof(report.path), "%s/%s",
			 log_opts.basedir,
			 "tables_with_oids.txt");

	upgrade_task_add_step(task, query, process_with_oids_check,
						  true, &report);
	upgrade_task_run(task, cluster);
	upgrade_task_free(task);

	if (report.file)
	{
		fclose(report.file);
		pg_log(PG_REPORT, "fatal");
		pg_fatal("Your installation contains tables declared WITH OIDS, which is not\n"
				 "supported anymore.  Consider removing the oid column using\n"
				 "    ALTER TABLE ... SET WITHOUT OIDS;\n"
				 "A list of tables with the problem is in the file:\n"
				 "    %s", report.path);
	}
	else
		check_ok();
}


/*
 * check_for_pg_role_prefix()
 *
 *	Versions older than 9.6 should not have any pg_* roles
 */
static void
check_for_pg_role_prefix(ClusterInfo *cluster)
{
	PGresult   *res;
	PGconn	   *conn = connectToServer(cluster, "template1");
	int			ntups;
	int			i_roloid;
	int			i_rolname;
	FILE	   *script = NULL;
	char		output_path[MAXPGPATH];

	prep_status("Checking for roles starting with \"pg_\"");

	snprintf(output_path, sizeof(output_path), "%s/%s",
			 log_opts.basedir,
			 "pg_role_prefix.txt");

	res = executeQueryOrDie(conn,
							"SELECT oid AS roloid, rolname "
							"FROM pg_catalog.pg_roles "
							"WHERE rolname ~ '^pg_'");

	ntups = PQntuples(res);
	i_roloid = PQfnumber(res, "roloid");
	i_rolname = PQfnumber(res, "rolname");
	for (int rowno = 0; rowno < ntups; rowno++)
	{
		if (script == NULL && (script = fopen_priv(output_path, "w")) == NULL)
			pg_fatal("could not open file \"%s\": %m", output_path);
		fprintf(script, "%s (oid=%s)\n",
				PQgetvalue(res, rowno, i_rolname),
				PQgetvalue(res, rowno, i_roloid));
	}

	PQclear(res);

	PQfinish(conn);

	if (script)
	{
		fclose(script);
		pg_log(PG_REPORT, "fatal");
		pg_fatal("Your installation contains roles starting with \"pg_\".\n"
				 "\"pg_\" is a reserved prefix for system roles.  The cluster\n"
				 "cannot be upgraded until these roles are renamed.\n"
				 "A list of roles starting with \"pg_\" is in the file:\n"
				 "    %s", output_path);
	}
	else
		check_ok();
}

/*
 * Callback function for processing results of query for
 * check_for_user_defined_encoding_conversions()'s UpgradeTask.  If the query
 * returned any rows (i.e., the check failed), write the details to the report
 * file.
 */
static void
process_user_defined_encoding_conversions(DbInfo *dbinfo, PGresult *res, void *arg)
{
	UpgradeTaskReport *report = (UpgradeTaskReport *) arg;
	int			ntups = PQntuples(res);
	int			i_conoid = PQfnumber(res, "conoid");
	int			i_conname = PQfnumber(res, "conname");
	int			i_nspname = PQfnumber(res, "nspname");

	AssertVariableIsOfType(&process_user_defined_encoding_conversions,
						   UpgradeTaskProcessCB);

	if (ntups == 0)
		return;

	if (report->file == NULL &&
		(report->file = fopen_priv(report->path, "w")) == NULL)
		pg_fatal("could not open file \"%s\": %m", report->path);

	fprintf(report->file, "In database: %s\n", dbinfo->db_name);

	for (int rowno = 0; rowno < ntups; rowno++)
		fprintf(report->file, "  (oid=%s) %s.%s\n",
				PQgetvalue(res, rowno, i_conoid),
				PQgetvalue(res, rowno, i_nspname),
				PQgetvalue(res, rowno, i_conname));
}

/*
 * Verify that no user-defined encoding conversions exist.
 */
static void
check_for_user_defined_encoding_conversions(ClusterInfo *cluster)
{
	UpgradeTaskReport report;
	UpgradeTask *task = upgrade_task_create();
	const char *query;

	prep_status("Checking for user-defined encoding conversions");

	report.file = NULL;
	snprintf(report.path, sizeof(report.path), "%s/%s",
			 log_opts.basedir,
			 "encoding_conversions.txt");

	/*
	 * The query below hardcodes FirstNormalObjectId as 16384 rather than
	 * interpolating that C #define into the query because, if that #define is
	 * ever changed, the cutoff we want to use is the value used by
	 * pre-version 14 servers, not that of some future version.
	 */
	query = "SELECT c.oid as conoid, c.conname, n.nspname "
		"FROM pg_catalog.pg_conversion c, "
		"     pg_catalog.pg_namespace n "
		"WHERE c.connamespace = n.oid AND "
		"      c.oid >= 16384";

	upgrade_task_add_step(task, query,
						  process_user_defined_encoding_conversions,
						  true, &report);
	upgrade_task_run(task, cluster);
	upgrade_task_free(task);

	if (report.file)
	{
		fclose(report.file);
		pg_log(PG_REPORT, "fatal");
		pg_fatal("Your installation contains user-defined encoding conversions.\n"
				 "The conversion function parameters changed in PostgreSQL version 14\n"
				 "so this cluster cannot currently be upgraded.  You can remove the\n"
				 "encoding conversions in the old cluster and restart the upgrade.\n"
				 "A list of user-defined encoding conversions is in the file:\n"
				 "    %s", report.path);
	}
	else
		check_ok();
}

/*
 * Callback function for processing results of query for
 * check_for_unicode_update()'s UpgradeTask.  If the query returned any rows
 * (i.e., the check failed), write the details to the report file.
 */
static void
process_unicode_update(DbInfo *dbinfo, PGresult *res, void *arg)
{
	UpgradeTaskReport *report = (UpgradeTaskReport *) arg;
	int			ntups = PQntuples(res);
	int			i_reloid = PQfnumber(res, "reloid");
	int			i_nspname = PQfnumber(res, "nspname");
	int			i_relname = PQfnumber(res, "relname");

	if (ntups == 0)
		return;

	if (report->file == NULL &&
		(report->file = fopen_priv(report->path, "w")) == NULL)
		pg_fatal("could not open file \"%s\": %m", report->path);

	fprintf(report->file, "In database: %s\n", dbinfo->db_name);

	for (int rowno = 0; rowno < ntups; rowno++)
		fprintf(report->file, "  (oid=%s) %s.%s\n",
				PQgetvalue(res, rowno, i_reloid),
				PQgetvalue(res, rowno, i_nspname),
				PQgetvalue(res, rowno, i_relname));
}

/*
 * Check if the Unicode version built into Postgres changed between the old
 * cluster and the new cluster.
 */
static bool
unicode_version_changed(ClusterInfo *cluster)
{
	PGconn	   *conn_template1 = connectToServer(cluster, "template1");
	PGresult   *res;
	char	   *old_unicode_version;
	bool		unicode_updated;

	res = executeQueryOrDie(conn_template1, "SELECT unicode_version()");
	old_unicode_version = PQgetvalue(res, 0, 0);
	unicode_updated = (strcmp(old_unicode_version, PG_UNICODE_VERSION) != 0);

	PQclear(res);
	PQfinish(conn_template1);

	return unicode_updated;
}

/*
 * check_for_unicode_update()
 *
 * Check if the version of Unicode in the old server and the new server
 * differ. If so, check for indexes, partitioned tables, or constraints that
 * use expressions with functions dependent on Unicode behavior.
 */
static void
check_for_unicode_update(ClusterInfo *cluster)
{
	UpgradeTaskReport report;
	UpgradeTask *task;
	const char *query;

	/*
	 * The builtin provider did not exist prior to version 17. While there are
	 * still problems that could potentially be caught from earlier versions,
	 * such as an index on NORMALIZE(), we don't check for that here.
	 */
	if (GET_MAJOR_VERSION(cluster->major_version) < 1700)
		return;

	prep_status("Checking for objects affected by Unicode update");

	if (!unicode_version_changed(cluster))
	{
		check_ok();
		return;
	}

	report.file = NULL;
	snprintf(report.path, sizeof(report.path), "%s/%s",
			 log_opts.basedir,
			 "unicode_dependent_rels.txt");

	query =
	/* collations that use built-in Unicode for character semantics */
		"WITH collations(collid) AS ( "
		"  SELECT oid FROM pg_collation "
		"  WHERE collprovider='b' AND colllocale IN ('C.UTF-8','PG_UNICODE_FAST') "
	/* include default collation, if appropriate */
		"  UNION "
		"  SELECT 'pg_catalog.default'::regcollation FROM pg_database "
		"  WHERE datname = current_database() AND "
		"  datlocprovider='b' AND datlocale IN ('C.UTF-8','PG_UNICODE_FAST') "
		"), "
	/* functions that use built-in Unicode */
		"functions(procid) AS ( "
		"  SELECT proc.oid FROM pg_proc proc "
		"  WHERE proname IN ('normalize','unicode_assigned','unicode_version','is_normalized') AND "
		"        pronamespace='pg_catalog'::regnamespace "
		"), "
	/* operators that use the input collation for character semantics */
		"coll_operators(operid, procid, collid) AS ( "
		"  SELECT oper.oid, oper.oprcode, collid FROM pg_operator oper, collations "
		"  WHERE oprname IN ('~', '~*', '!~', '!~*', '~~*', '!~~*') AND "
		"        oprnamespace='pg_catalog'::regnamespace AND "
		"        oprright='text'::regtype "
		"), "
	/* functions that use the input collation for character semantics */
		"coll_functions(procid, collid) AS ( "
		"  SELECT proc.oid, collid FROM pg_proc proc, collations "
		"  WHERE proname IN ('lower','initcap','upper') AND "
		"        pronamespace='pg_catalog'::regnamespace AND "
		"        proargtypes[0] = 'text'::regtype "
	/* include functions behind the operators listed above */
		"  UNION "
		"  SELECT procid, collid FROM coll_operators "
		"), "

	/*
	 * Generate patterns to search a pg_node_tree for the above functions and
	 * operators.
	 */
		"patterns(p) AS ( "
		"  SELECT '{FUNCEXPR :funcid ' || procid::text || '[ }]' FROM functions "
		"  UNION "
		"  SELECT '{OPEXPR :opno ' || operid::text || ' (:\\w+ \\w+ )*' || "
		"         ':inputcollid ' || collid::text || '[ }]' FROM coll_operators "
		"  UNION "
		"  SELECT '{FUNCEXPR :funcid ' || procid::text || ' (:\\w+ \\w+ )*' || "
		"         ':inputcollid ' || collid::text || '[ }]' FROM coll_functions "
		") "

	/*
	 * Match the patterns against expressions used for relation contents.
	 */
		"SELECT reloid, relkind, nspname, relname "
		"  FROM ( "
		"    SELECT conrelid "
		"    FROM pg_constraint, patterns WHERE conbin::text ~ p "
		"  UNION "
		"    SELECT indexrelid "
		"    FROM pg_index, patterns WHERE indexprs::text ~ p OR indpred::text ~ p "
		"  UNION "
		"    SELECT partrelid "
		"    FROM pg_partitioned_table, patterns WHERE partexprs::text ~ p "
		"  UNION "
		"    SELECT ev_class "
		"    FROM pg_rewrite, pg_class, patterns "
		"    WHERE ev_class = pg_class.oid AND relkind = 'm' AND ev_action::text ~ p"
		"  ) s(reloid), pg_class c, pg_namespace n, pg_database d "
		"  WHERE s.reloid = c.oid AND c.relnamespace = n.oid AND "
		"        d.datname = current_database() AND "
		"        d.encoding = pg_char_to_encoding('UTF8');";

	task = upgrade_task_create();
	upgrade_task_add_step(task, query,
						  process_unicode_update,
						  true, &report);
	upgrade_task_run(task, cluster);
	upgrade_task_free(task);

	if (report.file)
	{
		fclose(report.file);
		report_status(PG_WARNING, "warning");
		pg_log(PG_WARNING, "Your installation contains relations that may be affected by a new version of Unicode.\n"
			   "A list of potentially-affected relations is in the file:\n"
			   "    %s", report.path);
	}
	else
		check_ok();
}

/*
 * check_new_cluster_logical_replication_slots()
 *
 * Verify that there are no logical replication slots on the new cluster and
 * that the parameter settings necessary for creating slots are sufficient.
 */
static void
check_new_cluster_logical_replication_slots(void)
{
	PGresult   *res;
	PGconn	   *conn;
	int			nslots_on_old;
	int			nslots_on_new;
	int			max_replication_slots;
	char	   *wal_level;

	/* Logical slots can be migrated since PG17. */
	if (GET_MAJOR_VERSION(old_cluster.major_version) <= 1600)
		return;

	nslots_on_old = count_old_cluster_logical_slots();

	/* Quick return if there are no logical slots to be migrated. */
	if (nslots_on_old == 0)
		return;

	conn = connectToServer(&new_cluster, "template1");

	prep_status("Checking for new cluster logical replication slots");

	res = executeQueryOrDie(conn, "SELECT count(*) "
							"FROM pg_catalog.pg_replication_slots "
							"WHERE slot_type = 'logical' AND "
							"temporary IS FALSE;");

	if (PQntuples(res) != 1)
		pg_fatal("could not count the number of logical replication slots");

	nslots_on_new = atoi(PQgetvalue(res, 0, 0));

	if (nslots_on_new)
		pg_fatal("expected 0 logical replication slots but found %d",
				 nslots_on_new);

	PQclear(res);

	res = executeQueryOrDie(conn, "SELECT setting FROM pg_settings "
							"WHERE name IN ('wal_level', 'max_replication_slots') "
							"ORDER BY name DESC;");

	if (PQntuples(res) != 2)
		pg_fatal("could not determine parameter settings on new cluster");

	wal_level = PQgetvalue(res, 0, 0);

	if (strcmp(wal_level, "logical") != 0)
		pg_fatal("\"wal_level\" must be \"logical\" but is set to \"%s\"",
				 wal_level);

	max_replication_slots = atoi(PQgetvalue(res, 1, 0));

	if (nslots_on_old > max_replication_slots)
		pg_fatal("\"max_replication_slots\" (%d) must be greater than or equal to the number of "
				 "logical replication slots (%d) on the old cluster",
				 max_replication_slots, nslots_on_old);

	PQclear(res);
	PQfinish(conn);

	check_ok();
}

/*
 * check_new_cluster_subscription_configuration()
 *
 * Verify that the max_active_replication_origins configuration specified is
 * enough for creating the subscriptions. This is required to create the
 * replication origin for each subscription.
 */
static void
check_new_cluster_subscription_configuration(void)
{
	PGresult   *res;
	PGconn	   *conn;
	int			max_active_replication_origins;

	/* Subscriptions and their dependencies can be migrated since PG17. */
	if (GET_MAJOR_VERSION(old_cluster.major_version) < 1700)
		return;

	/* Quick return if there are no subscriptions to be migrated. */
	if (old_cluster.nsubs == 0)
		return;

	prep_status("Checking for new cluster configuration for subscriptions");

	conn = connectToServer(&new_cluster, "template1");

	res = executeQueryOrDie(conn, "SELECT setting FROM pg_settings "
							"WHERE name = 'max_active_replication_origins';");

	if (PQntuples(res) != 1)
		pg_fatal("could not determine parameter settings on new cluster");

	max_active_replication_origins = atoi(PQgetvalue(res, 0, 0));
	if (old_cluster.nsubs > max_active_replication_origins)
		pg_fatal("\"max_active_replication_origins\" (%d) must be greater than or equal to the number of "
				 "subscriptions (%d) on the old cluster",
				 max_active_replication_origins, old_cluster.nsubs);

	PQclear(res);
	PQfinish(conn);

	check_ok();
}

/*
 * check_old_cluster_for_valid_slots()
 *
 * Verify that all the logical slots are valid and have consumed all the WAL
 * before shutdown.
 */
static void
check_old_cluster_for_valid_slots(void)
{
	char		output_path[MAXPGPATH];
	FILE	   *script = NULL;

	prep_status("Checking for valid logical replication slots");

	snprintf(output_path, sizeof(output_path), "%s/%s",
			 log_opts.basedir,
			 "invalid_logical_slots.txt");

	for (int dbnum = 0; dbnum < old_cluster.dbarr.ndbs; dbnum++)
	{
		LogicalSlotInfoArr *slot_arr = &old_cluster.dbarr.dbs[dbnum].slot_arr;

		for (int slotnum = 0; slotnum < slot_arr->nslots; slotnum++)
		{
			LogicalSlotInfo *slot = &slot_arr->slots[slotnum];

			/* Is the slot usable? */
			if (slot->invalid)
			{
				if (script == NULL &&
					(script = fopen_priv(output_path, "w")) == NULL)
					pg_fatal("could not open file \"%s\": %m", output_path);

				fprintf(script, "The slot \"%s\" is invalid\n",
						slot->slotname);

				continue;
			}

			/*
			 * Do additional check to ensure that all logical replication
			 * slots have consumed all the WAL before shutdown.
			 *
			 * Note: This can be satisfied only when the old cluster has been
			 * shut down, so we skip this for live checks.
			 */
			if (!user_opts.live_check && !slot->caught_up)
			{
				if (script == NULL &&
					(script = fopen_priv(output_path, "w")) == NULL)
					pg_fatal("could not open file \"%s\": %m", output_path);

				fprintf(script,
						"The slot \"%s\" has not consumed the WAL yet\n",
						slot->slotname);
			}
		}
	}

	if (script)
	{
		fclose(script);

		pg_log(PG_REPORT, "fatal");
		pg_fatal("Your installation contains logical replication slots that cannot be upgraded.\n"
				 "You can remove invalid slots and/or consume the pending WAL for other slots,\n"
				 "and then restart the upgrade.\n"
				 "A list of the problematic slots is in the file:\n"
				 "    %s", output_path);
	}

	check_ok();
}

/*
 * Callback function for processing results of query for
 * check_old_cluster_subscription_state()'s UpgradeTask.  If the query returned
 * any rows (i.e., the check failed), write the details to the report file.
 */
static void
process_old_sub_state_check(DbInfo *dbinfo, PGresult *res, void *arg)
{
	UpgradeTaskReport *report = (UpgradeTaskReport *) arg;
	int			ntups = PQntuples(res);
	int			i_srsubstate = PQfnumber(res, "srsubstate");
	int			i_subname = PQfnumber(res, "subname");
	int			i_nspname = PQfnumber(res, "nspname");
	int			i_relname = PQfnumber(res, "relname");

	AssertVariableIsOfType(&process_old_sub_state_check, UpgradeTaskProcessCB);

	if (ntups == 0)
		return;

	if (report->file == NULL &&
		(report->file = fopen_priv(report->path, "w")) == NULL)
		pg_fatal("could not open file \"%s\": %m", report->path);

	for (int i = 0; i < ntups; i++)
		fprintf(report->file, "The table sync state \"%s\" is not allowed for database:\"%s\" subscription:\"%s\" schema:\"%s\" relation:\"%s\"\n",
				PQgetvalue(res, i, i_srsubstate),
				dbinfo->db_name,
				PQgetvalue(res, i, i_subname),
				PQgetvalue(res, i, i_nspname),
				PQgetvalue(res, i, i_relname));
}

/*
 * check_old_cluster_subscription_state()
 *
 * Verify that the replication origin corresponding to each of the
 * subscriptions are present and each of the subscribed tables is in
 * 'i' (initialize) or 'r' (ready) state.
 */
static void
check_old_cluster_subscription_state(void)
{
	UpgradeTask *task = upgrade_task_create();
	UpgradeTaskReport report;
	const char *query;
	PGresult   *res;
	PGconn	   *conn;
	int			ntup;

	prep_status("Checking for subscription state");

	report.file = NULL;
	snprintf(report.path, sizeof(report.path), "%s/%s",
			 log_opts.basedir,
			 "subs_invalid.txt");

	/*
	 * Check that all the subscriptions have their respective replication
	 * origin.  This check only needs to run once.
	 */
	conn = connectToServer(&old_cluster, old_cluster.dbarr.dbs[0].db_name);
	res = executeQueryOrDie(conn,
							"SELECT d.datname, s.subname "
							"FROM pg_catalog.pg_subscription s "
							"LEFT OUTER JOIN pg_catalog.pg_replication_origin o "
							"	ON o.roname = 'pg_' || s.oid "
							"INNER JOIN pg_catalog.pg_database d "
							"	ON d.oid = s.subdbid "
							"WHERE o.roname IS NULL;");
	ntup = PQntuples(res);
	for (int i = 0; i < ntup; i++)
	{
		if (report.file == NULL &&
			(report.file = fopen_priv(report.path, "w")) == NULL)
			pg_fatal("could not open file \"%s\": %m", report.path);
		fprintf(report.file, "The replication origin is missing for database:\"%s\" subscription:\"%s\"\n",
				PQgetvalue(res, i, 0),
				PQgetvalue(res, i, 1));
	}
	PQclear(res);
	PQfinish(conn);

	/*
	 * We don't allow upgrade if there is a risk of dangling slot or origin
	 * corresponding to initial sync after upgrade.
	 *
	 * A slot/origin not created yet refers to the 'i' (initialize) state,
	 * while 'r' (ready) state refers to a slot/origin created previously but
	 * already dropped. These states are supported for pg_upgrade. The other
	 * states listed below are not supported:
	 *
	 * a) SUBREL_STATE_DATASYNC: A relation upgraded while in this state would
	 * retain a replication slot, which could not be dropped by the sync
	 * worker spawned after the upgrade because the subscription ID used for
	 * the slot name won't match anymore.
	 *
	 * b) SUBREL_STATE_SYNCDONE: A relation upgraded while in this state would
	 * retain the replication origin when there is a failure in tablesync
	 * worker immediately after dropping the replication slot in the
	 * publisher.
	 *
	 * c) SUBREL_STATE_FINISHEDCOPY: A tablesync worker spawned to work on a
	 * relation upgraded while in this state would expect an origin ID with
	 * the OID of the subscription used before the upgrade, causing it to
	 * fail.
	 *
	 * d) SUBREL_STATE_SYNCWAIT, SUBREL_STATE_CATCHUP and
	 * SUBREL_STATE_UNKNOWN: These states are not stored in the catalog, so we
	 * need not allow these states.
	 */
	query = "SELECT r.srsubstate, s.subname, n.nspname, c.relname "
		"FROM pg_catalog.pg_subscription_rel r "
		"LEFT JOIN pg_catalog.pg_subscription s"
		"   ON r.srsubid = s.oid "
		"LEFT JOIN pg_catalog.pg_class c"
		"   ON r.srrelid = c.oid "
		"LEFT JOIN pg_catalog.pg_namespace n"
		"   ON c.relnamespace = n.oid "
		"WHERE r.srsubstate NOT IN ('i', 'r') "
		"ORDER BY s.subname";

	upgrade_task_add_step(task, query, process_old_sub_state_check,
						  true, &report);

	upgrade_task_run(task, &old_cluster);
	upgrade_task_free(task);

	if (report.file)
	{
		fclose(report.file);
		pg_log(PG_REPORT, "fatal");
		pg_fatal("Your installation contains subscriptions without origin or having relations not in i (initialize) or r (ready) state.\n"
				 "You can allow the initial sync to finish for all relations and then restart the upgrade.\n"
				 "A list of the problematic subscriptions is in the file:\n"
				 "    %s", report.path);
	}
	else
		check_ok();
}

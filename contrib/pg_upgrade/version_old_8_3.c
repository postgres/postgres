/*
 *	version.c
 *
 *	Postgres-version-specific routines
 *
 *	Copyright (c) 2010-2013, PostgreSQL Global Development Group
 *	contrib/pg_upgrade/version_old_8_3.c
 */

#include "postgres_fe.h"

#include "pg_upgrade.h"

#include "access/transam.h"


/*
 * old_8_3_check_for_name_data_type_usage()
 *	8.3 -> 8.4
 *	Alignment for the 'name' data type changed to 'char' in 8.4;
 *	checks tables and indexes.
 */
void
old_8_3_check_for_name_data_type_usage(ClusterInfo *cluster)
{
	int			dbnum;
	FILE	   *script = NULL;
	bool		found = false;
	char		output_path[MAXPGPATH];

	prep_status("Checking for invalid \"name\" user columns");

	snprintf(output_path, sizeof(output_path), "tables_using_name.txt");

	for (dbnum = 0; dbnum < cluster->dbarr.ndbs; dbnum++)
	{
		PGresult   *res;
		bool		db_used = false;
		int			ntups;
		int			rowno;
		int			i_nspname,
					i_relname,
					i_attname;
		DbInfo	   *active_db = &cluster->dbarr.dbs[dbnum];
		PGconn	   *conn = connectToServer(cluster, active_db->db_name);

		/*
		 * With a smaller alignment in 8.4, 'name' cannot be used in a
		 * non-pg_catalog table, except as the first column. (We could tighten
		 * that condition with enough analysis, but it seems not worth the
		 * trouble.)
		 */
		res = executeQueryOrDie(conn,
								"SELECT n.nspname, c.relname, a.attname "
								"FROM	pg_catalog.pg_class c, "
								"		pg_catalog.pg_namespace n, "
								"		pg_catalog.pg_attribute a "
								"WHERE	c.oid = a.attrelid AND "
								"		a.attnum > 1 AND "
								"		NOT a.attisdropped AND "
								"		a.atttypid = 'pg_catalog.name'::pg_catalog.regtype AND "
								"		c.relnamespace = n.oid AND "
		/* exclude possible orphaned temp tables */
								"  		n.nspname !~ '^pg_temp_' AND "
						 "		n.nspname !~ '^pg_toast_temp_' AND "
								"		n.nspname NOT IN ('pg_catalog', 'information_schema')");

		ntups = PQntuples(res);
		i_nspname = PQfnumber(res, "nspname");
		i_relname = PQfnumber(res, "relname");
		i_attname = PQfnumber(res, "attname");
		for (rowno = 0; rowno < ntups; rowno++)
		{
			found = true;
			if (script == NULL && (script = fopen_priv(output_path, "w")) == NULL)
				pg_log(PG_FATAL, "could not open file \"%s\": %s\n", output_path, getErrorText(errno));
			if (!db_used)
			{
				fprintf(script, "Database: %s\n", active_db->db_name);
				db_used = true;
			}
			fprintf(script, "  %s.%s.%s\n",
					PQgetvalue(res, rowno, i_nspname),
					PQgetvalue(res, rowno, i_relname),
					PQgetvalue(res, rowno, i_attname));
		}

		PQclear(res);

		PQfinish(conn);
	}

	if (script)
		fclose(script);

	if (found)
	{
		pg_log(PG_REPORT, "fatal\n");
		pg_log(PG_FATAL,
			   "Your installation contains the \"name\" data type in user tables.  This\n"
		"data type changed its internal alignment between your old and new\n"
			   "clusters so this cluster cannot currently be upgraded.  You can remove\n"
		"the problem tables and restart the upgrade.  A list of the problem\n"
			   "columns is in the file:\n"
			   "    %s\n\n", output_path);
	}
	else
		check_ok();
}


/*
 * old_8_3_check_for_tsquery_usage()
 *	8.3 -> 8.4
 *	A new 'prefix' field was added to the 'tsquery' data type in 8.4
 *	so upgrading of such fields is impossible.
 */
void
old_8_3_check_for_tsquery_usage(ClusterInfo *cluster)
{
	int			dbnum;
	FILE	   *script = NULL;
	bool		found = false;
	char		output_path[MAXPGPATH];

	prep_status("Checking for tsquery user columns");

	snprintf(output_path, sizeof(output_path), "tables_using_tsquery.txt");

	for (dbnum = 0; dbnum < cluster->dbarr.ndbs; dbnum++)
	{
		PGresult   *res;
		bool		db_used = false;
		int			ntups;
		int			rowno;
		int			i_nspname,
					i_relname,
					i_attname;
		DbInfo	   *active_db = &cluster->dbarr.dbs[dbnum];
		PGconn	   *conn = connectToServer(cluster, active_db->db_name);

		/* Find any user-defined tsquery columns */
		res = executeQueryOrDie(conn,
								"SELECT n.nspname, c.relname, a.attname "
								"FROM	pg_catalog.pg_class c, "
								"		pg_catalog.pg_namespace n, "
								"		pg_catalog.pg_attribute a "
		/* materialized views didn't exist in 8.3, so no need to check 'm' */
								"WHERE	c.relkind = 'r' AND "
								"		c.oid = a.attrelid AND "
								"		NOT a.attisdropped AND "
								"		a.atttypid = 'pg_catalog.tsquery'::pg_catalog.regtype AND "
								"		c.relnamespace = n.oid AND "
		/* exclude possible orphaned temp tables */
								"  		n.nspname !~ '^pg_temp_' AND "
						 "		n.nspname !~ '^pg_toast_temp_' AND "
								"		n.nspname NOT IN ('pg_catalog', 'information_schema')");

		ntups = PQntuples(res);
		i_nspname = PQfnumber(res, "nspname");
		i_relname = PQfnumber(res, "relname");
		i_attname = PQfnumber(res, "attname");
		for (rowno = 0; rowno < ntups; rowno++)
		{
			found = true;
			if (script == NULL && (script = fopen_priv(output_path, "w")) == NULL)
				pg_log(PG_FATAL, "could not open file \"%s\": %s\n", output_path, getErrorText(errno));
			if (!db_used)
			{
				fprintf(script, "Database: %s\n", active_db->db_name);
				db_used = true;
			}
			fprintf(script, "  %s.%s.%s\n",
					PQgetvalue(res, rowno, i_nspname),
					PQgetvalue(res, rowno, i_relname),
					PQgetvalue(res, rowno, i_attname));
		}

		PQclear(res);

		PQfinish(conn);
	}

	if (script)
		fclose(script);

	if (found)
	{
		pg_log(PG_REPORT, "fatal\n");
		pg_log(PG_FATAL,
			   "Your installation contains the \"tsquery\" data type.    This data type\n"
			   "added a new internal field between your old and new clusters so this\n"
		"cluster cannot currently be upgraded.  You can remove the problem\n"
			   "columns and restart the upgrade.  A list of the problem columns is in the\n"
			   "file:\n"
			   "    %s\n\n", output_path);
	}
	else
		check_ok();
}


/*
 *	old_8_3_check_ltree_usage()
 *	8.3 -> 8.4
 *	The internal ltree structure was changed in 8.4 so upgrading is impossible.
 */
void
old_8_3_check_ltree_usage(ClusterInfo *cluster)
{
	int			dbnum;
	FILE	   *script = NULL;
	bool		found = false;
	char		output_path[MAXPGPATH];

	prep_status("Checking for contrib/ltree");

	snprintf(output_path, sizeof(output_path), "contrib_ltree.txt");

	for (dbnum = 0; dbnum < cluster->dbarr.ndbs; dbnum++)
	{
		PGresult   *res;
		bool		db_used = false;
		int			ntups;
		int			rowno;
		int			i_nspname,
					i_proname;
		DbInfo	   *active_db = &cluster->dbarr.dbs[dbnum];
		PGconn	   *conn = connectToServer(cluster, active_db->db_name);

		/* Find any functions coming from contrib/ltree */
		res = executeQueryOrDie(conn,
								"SELECT n.nspname, p.proname "
								"FROM	pg_catalog.pg_proc p, "
								"		pg_catalog.pg_namespace n "
								"WHERE	p.pronamespace = n.oid AND "
								"		p.probin = '$libdir/ltree'");

		ntups = PQntuples(res);
		i_nspname = PQfnumber(res, "nspname");
		i_proname = PQfnumber(res, "proname");
		for (rowno = 0; rowno < ntups; rowno++)
		{
			found = true;
			if (script == NULL && (script = fopen_priv(output_path, "w")) == NULL)
				pg_log(PG_FATAL, "Could not open file \"%s\": %s\n",
					   output_path, getErrorText(errno));
			if (!db_used)
			{
				fprintf(script, "Database: %s\n", active_db->db_name);
				db_used = true;
			}
			fprintf(script, "  %s.%s\n",
					PQgetvalue(res, rowno, i_nspname),
					PQgetvalue(res, rowno, i_proname));
		}

		PQclear(res);

		PQfinish(conn);
	}

	if (script)
		fclose(script);

	if (found)
	{
		pg_log(PG_REPORT, "fatal\n");
		pg_log(PG_FATAL,
			   "Your installation contains the \"ltree\" data type.  This data type\n"
			   "changed its internal storage format between your old and new clusters so this\n"
			   "cluster cannot currently be upgraded.  You can manually upgrade databases\n"
			   "that use \"contrib/ltree\" facilities and remove \"contrib/ltree\" from the old\n"
			   "cluster and restart the upgrade.  A list of the problem functions is in the\n"
			   "file:\n"
			   "    %s\n\n", output_path);
	}
	else
		check_ok();
}


/*
 * old_8_3_rebuild_tsvector_tables()
 *	8.3 -> 8.4
 * 8.3 sorts lexemes by its length and if lengths are the same then it uses
 * alphabetic order;  8.4 sorts lexemes in lexicographical order, e.g.
 *
 * => SELECT 'c bb aaa'::tsvector;
 *	   tsvector
 * ----------------
 *	'aaa' 'bb' 'c'		   -- 8.4
 *	'c' 'bb' 'aaa'		   -- 8.3
 */
void
old_8_3_rebuild_tsvector_tables(ClusterInfo *cluster, bool check_mode)
{
	int			dbnum;
	FILE	   *script = NULL;
	bool		found = false;
	char		output_path[MAXPGPATH];

	prep_status("Checking for tsvector user columns");

	snprintf(output_path, sizeof(output_path), "rebuild_tsvector_tables.sql");

	for (dbnum = 0; dbnum < cluster->dbarr.ndbs; dbnum++)
	{
		PGresult   *res;
		bool		db_used = false;
		char		nspname[NAMEDATALEN] = "",
					relname[NAMEDATALEN] = "";
		int			ntups;
		int			rowno;
		int			i_nspname,
					i_relname,
					i_attname;
		DbInfo	   *active_db = &cluster->dbarr.dbs[dbnum];
		PGconn	   *conn = connectToServer(cluster, active_db->db_name);

		/* Find any user-defined tsvector columns */
		res = executeQueryOrDie(conn,
								"SELECT n.nspname, c.relname, a.attname "
								"FROM	pg_catalog.pg_class c, "
								"		pg_catalog.pg_namespace n, "
								"		pg_catalog.pg_attribute a "
		/* materialized views didn't exist in 8.3, so no need to check 'm' */
								"WHERE	c.relkind = 'r' AND "
								"		c.oid = a.attrelid AND "
								"		NOT a.attisdropped AND "
								"		a.atttypid = 'pg_catalog.tsvector'::pg_catalog.regtype AND "
								"		c.relnamespace = n.oid AND "
		/* exclude possible orphaned temp tables */
								"  		n.nspname !~ '^pg_temp_' AND "
						 "		n.nspname !~ '^pg_toast_temp_' AND "
								"		n.nspname NOT IN ('pg_catalog', 'information_schema')");

/*
 *	This macro is used below to avoid reindexing indexes already rebuilt
 *	because of tsvector columns.
 */
#define SKIP_TSVECTOR_TABLES \
								"i.indrelid NOT IN ( "					\
								"SELECT DISTINCT c.oid "				\
								"FROM	pg_catalog.pg_class c, "		\
								"		pg_catalog.pg_namespace n, "	\
								"		pg_catalog.pg_attribute a "		\
		/* materialized views didn't exist in 8.3, so no need to check 'm' */ \
								"WHERE	c.relkind = 'r' AND "			\
								"		c.oid = a.attrelid AND "		\
								"		NOT a.attisdropped AND "		\
								"		a.atttypid = 'pg_catalog.tsvector'::pg_catalog.regtype AND " \
								"		c.relnamespace = n.oid AND "	\
								"       n.nspname !~ '^pg_' AND "		\
								"		n.nspname != 'information_schema') "

		ntups = PQntuples(res);
		i_nspname = PQfnumber(res, "nspname");
		i_relname = PQfnumber(res, "relname");
		i_attname = PQfnumber(res, "attname");
		for (rowno = 0; rowno < ntups; rowno++)
		{
			found = true;
			if (!check_mode)
			{
				if (script == NULL && (script = fopen_priv(output_path, "w")) == NULL)
					pg_log(PG_FATAL, "could not open file \"%s\": %s\n", output_path, getErrorText(errno));
				if (!db_used)
				{
					fprintf(script, "\\connect %s\n\n",
							quote_identifier(active_db->db_name));
					db_used = true;
				}

				/* Rebuild all tsvector collumns with one ALTER TABLE command */
				if (strcmp(PQgetvalue(res, rowno, i_nspname), nspname) != 0 ||
					strcmp(PQgetvalue(res, rowno, i_relname), relname) != 0)
				{
					if (strlen(nspname) != 0 || strlen(relname) != 0)
						fprintf(script, ";\n\n");
					fprintf(script, "ALTER TABLE %s.%s\n",
						 quote_identifier(PQgetvalue(res, rowno, i_nspname)),
						quote_identifier(PQgetvalue(res, rowno, i_relname)));
				}
				else
					fprintf(script, ",\n");
				strlcpy(nspname, PQgetvalue(res, rowno, i_nspname), sizeof(nspname));
				strlcpy(relname, PQgetvalue(res, rowno, i_relname), sizeof(relname));

				fprintf(script, "ALTER COLUMN %s "
				/* This could have been a custom conversion function call. */
						"TYPE pg_catalog.tsvector USING %s::pg_catalog.text::pg_catalog.tsvector",
						quote_identifier(PQgetvalue(res, rowno, i_attname)),
						quote_identifier(PQgetvalue(res, rowno, i_attname)));
			}
		}
		if (strlen(nspname) != 0 || strlen(relname) != 0)
			fprintf(script, ";\n\n");

		PQclear(res);

		/* XXX Mark tables as not accessible somehow */

		PQfinish(conn);
	}

	if (script)
		fclose(script);

	if (found)
	{
		report_status(PG_WARNING, "warning");
		if (check_mode)
			pg_log(PG_WARNING, "\n"
				   "Your installation contains tsvector columns.  The tsvector internal\n"
				   "storage format changed between your old and new clusters so the tables\n"
				   "must be rebuilt.  After upgrading, you will be given instructions.\n\n");
		else
			pg_log(PG_WARNING, "\n"
				   "Your installation contains tsvector columns.  The tsvector internal\n"
				   "storage format changed between your old and new clusters so the tables\n"
				   "must be rebuilt.  The file:\n"
				   "    %s\n"
				   "when executed by psql by the database superuser will rebuild all tables\n"
				   "with tsvector columns.\n\n",
				   output_path);
	}
	else
		check_ok();
}


/*
 * old_8_3_invalidate_hash_gin_indexes()
 *	8.3 -> 8.4
 *	Hash, Gin, and GiST index binary format has changes from 8.3->8.4
 */
void
old_8_3_invalidate_hash_gin_indexes(ClusterInfo *cluster, bool check_mode)
{
	int			dbnum;
	FILE	   *script = NULL;
	bool		found = false;
	char		output_path[MAXPGPATH];

	prep_status("Checking for hash and GIN indexes");

	snprintf(output_path, sizeof(output_path), "reindex_hash_and_gin.sql");

	for (dbnum = 0; dbnum < cluster->dbarr.ndbs; dbnum++)
	{
		PGresult   *res;
		bool		db_used = false;
		int			ntups;
		int			rowno;
		int			i_nspname,
					i_relname;
		DbInfo	   *active_db = &cluster->dbarr.dbs[dbnum];
		PGconn	   *conn = connectToServer(cluster, active_db->db_name);

		/* find hash and gin indexes */
		res = executeQueryOrDie(conn,
								"SELECT n.nspname, c.relname "
								"FROM	pg_catalog.pg_class c, "
								"		pg_catalog.pg_index i, "
								"		pg_catalog.pg_am a, "
								"		pg_catalog.pg_namespace n "
								"WHERE	i.indexrelid = c.oid AND "
								"		c.relam = a.oid AND "
								"		c.relnamespace = n.oid AND "
							"		a.amname IN ('hash', 'gin') AND "
								SKIP_TSVECTOR_TABLES);

		ntups = PQntuples(res);
		i_nspname = PQfnumber(res, "nspname");
		i_relname = PQfnumber(res, "relname");
		for (rowno = 0; rowno < ntups; rowno++)
		{
			found = true;
			if (!check_mode)
			{
				if (script == NULL && (script = fopen_priv(output_path, "w")) == NULL)
					pg_log(PG_FATAL, "could not open file \"%s\": %s\n", output_path, getErrorText(errno));
				if (!db_used)
				{
					fprintf(script, "\\connect %s\n",
							quote_identifier(active_db->db_name));
					db_used = true;
				}
				fprintf(script, "REINDEX INDEX %s.%s;\n",
						quote_identifier(PQgetvalue(res, rowno, i_nspname)),
						quote_identifier(PQgetvalue(res, rowno, i_relname)));
			}
		}

		PQclear(res);

		if (!check_mode && found)
			/* mark hash and gin indexes as invalid */
			PQclear(executeQueryOrDie(conn,
									  "UPDATE pg_catalog.pg_index i "
									  "SET	indisvalid = false "
									  "FROM	pg_catalog.pg_class c, "
									  "		pg_catalog.pg_am a, "
									  "		pg_catalog.pg_namespace n "
									  "WHERE	i.indexrelid = c.oid AND "
									  "		c.relam = a.oid AND "
									  "		c.relnamespace = n.oid AND "
									"		a.amname IN ('hash', 'gin')"));

		PQfinish(conn);
	}

	if (script)
		fclose(script);

	if (found)
	{
		report_status(PG_WARNING, "warning");
		if (check_mode)
			pg_log(PG_WARNING, "\n"
				   "Your installation contains hash and/or GIN indexes.  These indexes have\n"
				   "different internal formats between your old and new clusters, so they\n"
				   "must be reindexed with the REINDEX command.  After upgrading, you will\n"
				   "be given REINDEX instructions.\n\n");
		else
			pg_log(PG_WARNING, "\n"
				   "Your installation contains hash and/or GIN indexes.  These indexes have\n"
				   "different internal formats between your old and new clusters, so they\n"
				   "must be reindexed with the REINDEX command.  The file:\n"
				   "    %s\n"
				   "when executed by psql by the database superuser will recreate all invalid\n"
			  "indexes; until then, none of these indexes will be used.\n\n",
				   output_path);
	}
	else
		check_ok();
}


/*
 * old_8_3_invalidate_bpchar_pattern_ops_indexes()
 *	8.3 -> 8.4
 *	8.4 bpchar_pattern_ops no longer sorts based on trailing spaces
 */
void
old_8_3_invalidate_bpchar_pattern_ops_indexes(ClusterInfo *cluster,
											  bool check_mode)
{
	int			dbnum;
	FILE	   *script = NULL;
	bool		found = false;
	char		output_path[MAXPGPATH];

	prep_status("Checking for bpchar_pattern_ops indexes");

	snprintf(output_path, sizeof(output_path), "reindex_bpchar_ops.sql");

	for (dbnum = 0; dbnum < cluster->dbarr.ndbs; dbnum++)
	{
		PGresult   *res;
		bool		db_used = false;
		int			ntups;
		int			rowno;
		int			i_nspname,
					i_relname;
		DbInfo	   *active_db = &cluster->dbarr.dbs[dbnum];
		PGconn	   *conn = connectToServer(cluster, active_db->db_name);

		/* find bpchar_pattern_ops indexes */

		/*
		 * Do only non-hash, non-gin indexees;	we already invalidated them
		 * above; no need to reindex twice
		 */
		res = executeQueryOrDie(conn,
								"SELECT n.nspname, c.relname "
								"FROM	pg_catalog.pg_index i, "
								"		pg_catalog.pg_class c, "
								"		pg_catalog.pg_namespace n "
								"WHERE	indexrelid = c.oid AND "
								"		c.relnamespace = n.oid AND "
								"		( "
								"			SELECT	o.oid "
				   "			FROM	pg_catalog.pg_opclass o, "
				  "					pg_catalog.pg_am a"
		"			WHERE	a.amname NOT IN ('hash', 'gin') AND "
			"					a.oid = o.opcmethod AND "
								"					o.opcname = 'bpchar_pattern_ops') "
								"		= ANY (i.indclass) AND "
								SKIP_TSVECTOR_TABLES);

		ntups = PQntuples(res);
		i_nspname = PQfnumber(res, "nspname");
		i_relname = PQfnumber(res, "relname");
		for (rowno = 0; rowno < ntups; rowno++)
		{
			found = true;
			if (!check_mode)
			{
				if (script == NULL && (script = fopen_priv(output_path, "w")) == NULL)
					pg_log(PG_FATAL, "could not open file \"%s\": %s\n", output_path, getErrorText(errno));
				if (!db_used)
				{
					fprintf(script, "\\connect %s\n",
							quote_identifier(active_db->db_name));
					db_used = true;
				}
				fprintf(script, "REINDEX INDEX %s.%s;\n",
						quote_identifier(PQgetvalue(res, rowno, i_nspname)),
						quote_identifier(PQgetvalue(res, rowno, i_relname)));
			}
		}

		PQclear(res);

		if (!check_mode && found)
			/* mark bpchar_pattern_ops indexes as invalid */
			PQclear(executeQueryOrDie(conn,
									  "UPDATE pg_catalog.pg_index i "
									  "SET	indisvalid = false "
									  "FROM	pg_catalog.pg_class c, "
									  "		pg_catalog.pg_namespace n "
									  "WHERE	indexrelid = c.oid AND "
									  "		c.relnamespace = n.oid AND "
									  "		( "
									  "			SELECT	o.oid "
						 "			FROM	pg_catalog.pg_opclass o, "
						"					pg_catalog.pg_am a"
			  "			WHERE	a.amname NOT IN ('hash', 'gin') AND "
				  "					a.oid = o.opcmethod AND "
									  "					o.opcname = 'bpchar_pattern_ops') "
									  "		= ANY (i.indclass)"));

		PQfinish(conn);
	}

	if (script)
		fclose(script);

	if (found)
	{
		report_status(PG_WARNING, "warning");
		if (check_mode)
			pg_log(PG_WARNING, "\n"
				   "Your installation contains indexes using \"bpchar_pattern_ops\".  These\n"
				   "indexes have different internal formats between your old and new clusters\n"
				   "so they must be reindexed with the REINDEX command.  After upgrading, you\n"
				   "will be given REINDEX instructions.\n\n");
		else
			pg_log(PG_WARNING, "\n"
				   "Your installation contains indexes using \"bpchar_pattern_ops\".  These\n"
				   "indexes have different internal formats between your old and new clusters\n"
			"so they must be reindexed with the REINDEX command.  The file:\n"
				   "    %s\n"
				   "when executed by psql by the database superuser will recreate all invalid\n"
			  "indexes; until then, none of these indexes will be used.\n\n",
				   output_path);
	}
	else
		check_ok();
}


/*
 * old_8_3_create_sequence_script()
 *	8.3 -> 8.4
 *	8.4 added the column "start_value" to all sequences.  For this reason,
 *	we don't transfer sequence files but instead use the CREATE SEQUENCE
 *	command from the schema dump, and use setval() to restore the sequence
 *	value and 'is_called' from the old database.  This is safe to run
 *	by pg_upgrade because sequence files are not transferred from the old
 *	server, even in link mode.
 */
char *
old_8_3_create_sequence_script(ClusterInfo *cluster)
{
	int			dbnum;
	FILE	   *script = NULL;
	bool		found = false;
	char	   *output_path = pg_malloc(MAXPGPATH);

	snprintf(output_path, MAXPGPATH, "adjust_sequences.sql");

	prep_status("Creating script to adjust sequences");

	for (dbnum = 0; dbnum < cluster->dbarr.ndbs; dbnum++)
	{
		PGresult   *res;
		bool		db_used = false;
		int			ntups;
		int			rowno;
		int			i_nspname,
					i_relname;
		DbInfo	   *active_db = &cluster->dbarr.dbs[dbnum];
		PGconn	   *conn = connectToServer(cluster, active_db->db_name);

		/* Find any sequences */
		res = executeQueryOrDie(conn,
								"SELECT n.nspname, c.relname "
								"FROM	pg_catalog.pg_class c, "
								"		pg_catalog.pg_namespace n "
								"WHERE	c.relkind = 'S' AND "
								"		c.relnamespace = n.oid AND "
		/* exclude possible orphaned temp tables */
								"  		n.nspname !~ '^pg_temp_' AND "
						 "		n.nspname !~ '^pg_toast_temp_' AND "
								"		n.nspname NOT IN ('pg_catalog', 'information_schema')");

		ntups = PQntuples(res);
		i_nspname = PQfnumber(res, "nspname");
		i_relname = PQfnumber(res, "relname");
		for (rowno = 0; rowno < ntups; rowno++)
		{
			PGresult   *seq_res;
			int			i_last_value,
						i_is_called;
			const char *nspname = PQgetvalue(res, rowno, i_nspname);
			const char *relname = PQgetvalue(res, rowno, i_relname);

			found = true;

			if (script == NULL && (script = fopen_priv(output_path, "w")) == NULL)
				pg_log(PG_FATAL, "could not open file \"%s\": %s\n", output_path, getErrorText(errno));
			if (!db_used)
			{
				fprintf(script, "\\connect %s\n\n",
						quote_identifier(active_db->db_name));
				db_used = true;
			}

			/* Find the desired sequence */
			seq_res = executeQueryOrDie(conn,
										"SELECT s.last_value, s.is_called "
										"FROM	%s.%s s",
										quote_identifier(nspname),
										quote_identifier(relname));

			assert(PQntuples(seq_res) == 1);
			i_last_value = PQfnumber(seq_res, "last_value");
			i_is_called = PQfnumber(seq_res, "is_called");

			fprintf(script, "SELECT setval('%s.%s', %s, '%s');\n",
					quote_identifier(nspname), quote_identifier(relname),
					PQgetvalue(seq_res, 0, i_last_value), PQgetvalue(seq_res, 0, i_is_called));
			PQclear(seq_res);
		}
		if (db_used)
			fprintf(script, "\n");

		PQclear(res);

		PQfinish(conn);
	}

	if (script)
		fclose(script);

	check_ok();

	if (found)
		return output_path;
	else
	{
		pg_free(output_path);
		return NULL;
	}
}

/*
 *	pg_upgrade.c
 *
 *	main source file
 *
 *	Copyright (c) 2010-2011, PostgreSQL Global Development Group
 *	contrib/pg_upgrade/pg_upgrade.c
 */

/*
 *	To simplify the upgrade process, we force certain system values to be
 *	identical between old and new clusters:
 *
 *	We control all assignments of pg_class.oid (and relfilenode) so toast
 *	oids are the same between old and new clusters.  This is important
 *	because toast oids are stored as toast pointers in user tables.
 *
 *	FYI, while pg_class.oid and pg_class.relfilenode are intially the same
 *	in a cluster, but they can diverge due to CLUSTER, REINDEX, or VACUUM
 *	FULL.  The new cluster will have matching pg_class.oid and
 *	pg_class.relfilenode values and be based on the old oid value.  This can
 *	cause the old and new pg_class.relfilenode values to differ.  In summary,
 *	old and new pg_class.oid and new pg_class.relfilenode will have the
 *	same value, and old pg_class.relfilenode might differ.
 *
 *	We control all assignments of pg_type.oid because these oids are stored
 *	in user composite type values.
 *
 *	We control all assignments of pg_enum.oid because these oids are stored
 *	in user tables as enum values.
 *
 *	We control all assignments of pg_authid.oid because these oids are stored
 *	in pg_largeobject_metadata.
 */



#include "pg_upgrade.h"

#ifdef HAVE_LANGINFO_H
#include <langinfo.h>
#endif

static void disable_old_cluster(void);
static void prepare_new_cluster(void);
static void prepare_new_databases(void);
static void create_new_objects(void);
static void copy_clog_xlog_xid(void);
static void set_frozenxids(void);
static void setup(char *argv0, bool live_check);
static void cleanup(void);
static void	get_restricted_token(const char *progname);

#ifdef WIN32
static int	CreateRestrictedProcess(char *cmd, PROCESS_INFORMATION *processInfo, const char *progname);
#endif

/* This is the database used by pg_dumpall to restore global tables */
#define GLOBAL_DUMP_DB	"postgres"

ClusterInfo old_cluster,
			new_cluster;
OSInfo		os_info;

#ifdef WIN32
static char *restrict_env;
#endif

int
main(int argc, char **argv)
{
	char	   *sequence_script_file_name = NULL;
	char	   *deletion_script_file_name = NULL;
	bool		live_check = false;

	parseCommandLine(argc, argv);

	get_restricted_token(os_info.progname);

	output_check_banner(&live_check);

	setup(argv[0], live_check);

	check_cluster_versions();
	check_cluster_compatibility(live_check);

	check_old_cluster(live_check, &sequence_script_file_name);


	/* -- NEW -- */
	start_postmaster(&new_cluster);

	check_new_cluster();
	report_clusters_compatible();

	pg_log(PG_REPORT, "\nPerforming Upgrade\n");
	pg_log(PG_REPORT, "------------------\n");

	disable_old_cluster();
	prepare_new_cluster();

	stop_postmaster(false);

	/*
	 * Destructive Changes to New Cluster
	 */

	copy_clog_xlog_xid();

	/* New now using xids of the old system */

	/* -- NEW -- */
	start_postmaster(&new_cluster);

	prepare_new_databases();

	create_new_objects();

	stop_postmaster(false);

	transfer_all_new_dbs(&old_cluster.dbarr, &new_cluster.dbarr,
						 old_cluster.pgdata, new_cluster.pgdata);

	/*
	 * Assuming OIDs are only used in system tables, there is no need to
	 * restore the OID counter because we have not transferred any OIDs from
	 * the old system, but we do it anyway just in case.  We do it late here
	 * because there is no need to have the schema load use new oids.
	 */
	prep_status("Setting next oid for new cluster");
	exec_prog(true, SYSTEMQUOTE "\"%s/pg_resetxlog\" -o %u \"%s\" > "
			  DEVNULL SYSTEMQUOTE,
			  new_cluster.bindir, old_cluster.controldata.chkpnt_nxtoid, new_cluster.pgdata);
	check_ok();

	create_script_for_old_cluster_deletion(&deletion_script_file_name);

	issue_warnings(sequence_script_file_name);

	pg_log(PG_REPORT, "\nUpgrade complete\n");
	pg_log(PG_REPORT, "----------------\n");

	output_completion_banner(deletion_script_file_name);

	pg_free(deletion_script_file_name);
	pg_free(sequence_script_file_name);

	cleanup();

	return 0;
}

#ifdef WIN32
typedef BOOL(WINAPI * __CreateRestrictedToken) (HANDLE, DWORD, DWORD, PSID_AND_ATTRIBUTES, DWORD, PLUID_AND_ATTRIBUTES, DWORD, PSID_AND_ATTRIBUTES, PHANDLE);

/* Windows API define missing from some versions of MingW headers */
#ifndef  DISABLE_MAX_PRIVILEGE
#define DISABLE_MAX_PRIVILEGE	0x1
#endif

/*
* Create a restricted token and execute the specified process with it.
*
* Returns 0 on failure, non-zero on success, same as CreateProcess().
*
* On NT4, or any other system not containing the required functions, will
* NOT execute anything.
*/
static int
CreateRestrictedProcess(char *cmd, PROCESS_INFORMATION *processInfo, const char *progname)
{
	BOOL		b;
	STARTUPINFO si;
	HANDLE		origToken;
	HANDLE		restrictedToken;
	SID_IDENTIFIER_AUTHORITY NtAuthority = { SECURITY_NT_AUTHORITY };
	SID_AND_ATTRIBUTES dropSids[2];
	__CreateRestrictedToken _CreateRestrictedToken = NULL;
	HANDLE		Advapi32Handle;

	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);

	Advapi32Handle = LoadLibrary("ADVAPI32.DLL");
	if (Advapi32Handle != NULL)
	{
		_CreateRestrictedToken = (__CreateRestrictedToken)GetProcAddress(Advapi32Handle, "CreateRestrictedToken");
	}

	if (_CreateRestrictedToken == NULL)
	{
		fprintf(stderr, _("%s: WARNING: cannot create restricted tokens on this platform\n"), progname);
		if (Advapi32Handle != NULL)
			FreeLibrary(Advapi32Handle);
		return 0;
	}

	/* Open the current token to use as a base for the restricted one */
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &origToken))
	{
		fprintf(stderr, _("%s: could not open process token: error code %lu\n"), progname, GetLastError());
		return 0;
	}

	/* Allocate list of SIDs to remove */
	ZeroMemory(&dropSids, sizeof(dropSids));
	if (!AllocateAndInitializeSid(&NtAuthority, 2,
		SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0,
		0, &dropSids[0].Sid) ||
		!AllocateAndInitializeSid(&NtAuthority, 2,
		SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_POWER_USERS, 0, 0, 0, 0, 0,
		0, &dropSids[1].Sid))
	{
		fprintf(stderr, _("%s: could not to allocate SIDs: error code %lu\n"), progname, GetLastError());
		return 0;
	}

	b = _CreateRestrictedToken(origToken,
						DISABLE_MAX_PRIVILEGE,
						sizeof(dropSids) / sizeof(dropSids[0]),
						dropSids,
						0, NULL,
						0, NULL,
						&restrictedToken);

	FreeSid(dropSids[1].Sid);
	FreeSid(dropSids[0].Sid);
	CloseHandle(origToken);
	FreeLibrary(Advapi32Handle);

	if (!b)
	{
		fprintf(stderr, _("%s: could not create restricted token: error code %lu\n"), progname, GetLastError());
		return 0;
	}

#ifndef __CYGWIN__
	AddUserToTokenDacl(restrictedToken);
#endif

	if (!CreateProcessAsUser(restrictedToken,
							NULL,
							cmd,
							NULL,
							NULL,
							TRUE,
							CREATE_SUSPENDED,
							NULL,
							NULL,
							&si,
							processInfo))

	{
		fprintf(stderr, _("%s: could not start process for command \"%s\": error code %lu\n"), progname, cmd, GetLastError());
		return 0;
	}

	return ResumeThread(processInfo->hThread);
}
#endif

void
get_restricted_token(const char *progname)
{
#ifdef WIN32

	/*
	* Before we execute another program, make sure that we are running with a
	* restricted token. If not, re-execute ourselves with one.
	*/

	if ((restrict_env = getenv("PG_RESTRICT_EXEC")) == NULL
		|| strcmp(restrict_env, "1") != 0)
	{
		PROCESS_INFORMATION pi;
		char	   *cmdline;

		ZeroMemory(&pi, sizeof(pi));

		cmdline = pg_strdup(GetCommandLine());

		putenv("PG_RESTRICT_EXEC=1");

		if (!CreateRestrictedProcess(cmdline, &pi, progname))
		{
			fprintf(stderr, _("%s: could not re-execute with restricted token: error code %lu\n"), progname, GetLastError());
		}
		else
		{
			/*
			* Successfully re-execed. Now wait for child process to capture
			* exitcode.
			*/
			DWORD		x;

			CloseHandle(pi.hThread);
			WaitForSingleObject(pi.hProcess, INFINITE);

			if (!GetExitCodeProcess(pi.hProcess, &x))
			{
				fprintf(stderr, _("%s: could not get exit code from subprocess: error code %lu\n"), progname, GetLastError());
				exit(1);
			}
			exit(x);
		}
	}
#endif
}

static void
setup(char *argv0, bool live_check)
{
	char		exec_path[MAXPGPATH];	/* full path to my executable */

	/*
	 * make sure the user has a clean environment, otherwise, we may confuse
	 * libpq when we connect to one (or both) of the servers.
	 */
	check_pghost_envvar();

	verify_directories();

	/* no postmasters should be running */
	if (!live_check && is_server_running(old_cluster.pgdata))
		pg_log(PG_FATAL, "There seems to be a postmaster servicing the old cluster.\n"
			   "Please shutdown that postmaster and try again.\n");

	/* same goes for the new postmaster */
	if (is_server_running(new_cluster.pgdata))
		pg_log(PG_FATAL, "There seems to be a postmaster servicing the new cluster.\n"
			   "Please shutdown that postmaster and try again.\n");

	/* get path to pg_upgrade executable */
	if (find_my_exec(argv0, exec_path) < 0)
		pg_log(PG_FATAL, "Could not get pathname to pg_upgrade: %s\n", getErrorText(errno));

	/* Trim off program name and keep just path */
	*last_dir_separator(exec_path) = '\0';
	canonicalize_path(exec_path);
	os_info.exec_path = pg_strdup(exec_path);
}


static void
disable_old_cluster(void)
{
	/* rename pg_control so old server cannot be accidentally started */
	rename_old_pg_control();
}


static void
prepare_new_cluster(void)
{
	/*
	 * It would make more sense to freeze after loading the schema, but that
	 * would cause us to lose the frozenids restored by the load. We use
	 * --analyze so autovacuum doesn't update statistics later
	 */
	prep_status("Analyzing all rows in the new cluster");
	exec_prog(true,
			  SYSTEMQUOTE "\"%s/vacuumdb\" --port %d --username \"%s\" "
			  "--all --analyze >> \"%s\" 2>&1" SYSTEMQUOTE,
	  new_cluster.bindir, new_cluster.port, os_info.user,
#ifndef WIN32
	  log_opts.filename
#else
	  DEVNULL
#endif
	  );
	check_ok();

	/*
	 * We do freeze after analyze so pg_statistic is also frozen. template0 is
	 * not frozen here, but data rows were frozen by initdb, and we set its
	 * datfrozenxid and relfrozenxids later to match the new xid counter
	 * later.
	 */
	prep_status("Freezing all rows on the new cluster");
	exec_prog(true,
			  SYSTEMQUOTE "\"%s/vacuumdb\" --port %d --username \"%s\" "
			  "--all --freeze >> \"%s\" 2>&1" SYSTEMQUOTE,
	  new_cluster.bindir, new_cluster.port, os_info.user,
#ifndef WIN32
	  log_opts.filename
#else
	  DEVNULL
#endif
	  );
	check_ok();

	get_pg_database_relfilenode(&new_cluster);
}


static void
prepare_new_databases(void)
{
	/*
	 * We set autovacuum_freeze_max_age to its maximum value so autovacuum
	 * does not launch here and delete clog files, before the frozen xids are
	 * set.
	 */

	set_frozenxids();

	prep_status("Creating databases in the new cluster");

	/*
	 * Install support functions in the global-restore database to preserve
	 * pg_authid.oid.
	 */
	install_support_functions_in_new_db(GLOBAL_DUMP_DB);

	/*
	 * We have to create the databases first so we can install support
	 * functions in all the other databases.  Ideally we could create the
	 * support functions in template1 but pg_dumpall creates database using
	 * the template0 template.
	 */
	exec_prog(true,
			  SYSTEMQUOTE "\"%s/psql\" --set ON_ERROR_STOP=on "
	/* --no-psqlrc prevents AUTOCOMMIT=off */
			  "--no-psqlrc --port %d --username \"%s\" "
			  "-f \"%s/%s\" --dbname template1 >> \"%s\"" SYSTEMQUOTE,
			  new_cluster.bindir, new_cluster.port, os_info.user, os_info.cwd,
			  GLOBALS_DUMP_FILE,
#ifndef WIN32
			  log_opts.filename
#else
			  DEVNULL
#endif
			  );
	check_ok();

	/* we load this to get a current list of databases */
	get_db_and_rel_infos(&new_cluster);
}


static void
create_new_objects(void)
{
	int			dbnum;

	prep_status("Adding support functions to new cluster");

	for (dbnum = 0; dbnum < new_cluster.dbarr.ndbs; dbnum++)
	{
		DbInfo	   *new_db = &new_cluster.dbarr.dbs[dbnum];

		/* skip db we already installed */
		if (strcmp(new_db->db_name, GLOBAL_DUMP_DB) != 0)
			install_support_functions_in_new_db(new_db->db_name);
	}
	check_ok();

	prep_status("Restoring database schema to new cluster");
	exec_prog(true,
			  SYSTEMQUOTE "\"%s/psql\" --set ON_ERROR_STOP=on "
			  "--no-psqlrc --port %d --username \"%s\" "
			  "-f \"%s/%s\" --dbname template1 >> \"%s\"" SYSTEMQUOTE,
			  new_cluster.bindir, new_cluster.port, os_info.user, os_info.cwd,
			  DB_DUMP_FILE,
#ifndef WIN32
			  log_opts.filename
#else
			  DEVNULL
#endif
			  );
	check_ok();

	/* regenerate now that we have objects in the databases */
	get_db_and_rel_infos(&new_cluster);

	uninstall_support_functions_from_new_cluster();
}


static void
copy_clog_xlog_xid(void)
{
	char		old_clog_path[MAXPGPATH];
	char		new_clog_path[MAXPGPATH];

	/* copy old commit logs to new data dir */
	prep_status("Deleting new commit clogs");

	snprintf(old_clog_path, sizeof(old_clog_path), "%s/pg_clog", old_cluster.pgdata);
	snprintf(new_clog_path, sizeof(new_clog_path), "%s/pg_clog", new_cluster.pgdata);
	if (!rmtree(new_clog_path, true))
		pg_log(PG_FATAL, "unable to delete directory %s\n", new_clog_path);
	check_ok();

	prep_status("Copying old commit clogs to new server");
#ifndef WIN32
	exec_prog(true, SYSTEMQUOTE "%s \"%s\" \"%s\"" SYSTEMQUOTE,
			  "cp -Rf",
#else
	/* flags: everything, no confirm, quiet, overwrite read-only */
	exec_prog(true, SYSTEMQUOTE "%s \"%s\" \"%s\\\"" SYSTEMQUOTE,
			  "xcopy /e /y /q /r",
#endif
			  old_clog_path, new_clog_path);
	check_ok();

	/* set the next transaction id of the new cluster */
	prep_status("Setting next transaction id for new cluster");
	exec_prog(true, SYSTEMQUOTE "\"%s/pg_resetxlog\" -f -x %u \"%s\" > " DEVNULL SYSTEMQUOTE,
			  new_cluster.bindir, old_cluster.controldata.chkpnt_nxtxid, new_cluster.pgdata);
	check_ok();

	/* now reset the wal archives in the new cluster */
	prep_status("Resetting WAL archives");
	exec_prog(true, SYSTEMQUOTE "\"%s/pg_resetxlog\" -l %u,%u,%u \"%s\" >> \"%s\" 2>&1" SYSTEMQUOTE,
			  new_cluster.bindir, old_cluster.controldata.chkpnt_tli,
			old_cluster.controldata.logid, old_cluster.controldata.nxtlogseg,
			  new_cluster.pgdata,
#ifndef WIN32
			  log_opts.filename
#else
			  DEVNULL
#endif
			  );
	check_ok();
}


/*
 *	set_frozenxids()
 *
 *	We have frozen all xids, so set relfrozenxid and datfrozenxid
 *	to be the old cluster's xid counter, which we just set in the new
 *	cluster.  User-table frozenxid values will be set by pg_dumpall
 *	--binary-upgrade, but objects not set by the pg_dump must have
 *	proper frozen counters.
 */
static
void
set_frozenxids(void)
{
	int			dbnum;
	PGconn	   *conn,
			   *conn_template1;
	PGresult   *dbres;
	int			ntups;
	int			i_datname;
	int			i_datallowconn;

	prep_status("Setting frozenxid counters in new cluster");

	conn_template1 = connectToServer(&new_cluster, "template1");

	/* set pg_database.datfrozenxid */
	PQclear(executeQueryOrDie(conn_template1,
							  "UPDATE pg_catalog.pg_database "
							  "SET	datfrozenxid = '%u'",
							  old_cluster.controldata.chkpnt_nxtxid));

	/* get database names */
	dbres = executeQueryOrDie(conn_template1,
							  "SELECT	datname, datallowconn "
							  "FROM	pg_catalog.pg_database");

	i_datname = PQfnumber(dbres, "datname");
	i_datallowconn = PQfnumber(dbres, "datallowconn");

	ntups = PQntuples(dbres);
	for (dbnum = 0; dbnum < ntups; dbnum++)
	{
		char	   *datname = PQgetvalue(dbres, dbnum, i_datname);
		char	   *datallowconn = PQgetvalue(dbres, dbnum, i_datallowconn);

		/*
		 * We must update databases where datallowconn = false, e.g.
		 * template0, because autovacuum increments their datfrozenxids and
		 * relfrozenxids even if autovacuum is turned off, and even though all
		 * the data rows are already frozen  To enable this, we temporarily
		 * change datallowconn.
		 */
		if (strcmp(datallowconn, "f") == 0)
			PQclear(executeQueryOrDie(conn_template1,
									  "UPDATE pg_catalog.pg_database "
									  "SET	datallowconn = true "
									  "WHERE datname = '%s'", datname));

		conn = connectToServer(&new_cluster, datname);

		/* set pg_class.relfrozenxid */
		PQclear(executeQueryOrDie(conn,
								  "UPDATE	pg_catalog.pg_class "
								  "SET	relfrozenxid = '%u' "
		/* only heap and TOAST are vacuumed */
								  "WHERE	relkind IN ('r', 't')",
								  old_cluster.controldata.chkpnt_nxtxid));
		PQfinish(conn);

		/* Reset datallowconn flag */
		if (strcmp(datallowconn, "f") == 0)
			PQclear(executeQueryOrDie(conn_template1,
									  "UPDATE pg_catalog.pg_database "
									  "SET	datallowconn = false "
									  "WHERE datname = '%s'", datname));
	}

	PQclear(dbres);

	PQfinish(conn_template1);

	check_ok();
}


static void
cleanup(void)
{
	char		filename[MAXPGPATH];

	if (log_opts.fd)
		fclose(log_opts.fd);

	if (log_opts.debug_fd)
		fclose(log_opts.debug_fd);

	snprintf(filename, sizeof(filename), "%s/%s", os_info.cwd, ALL_DUMP_FILE);
	unlink(filename);
	snprintf(filename, sizeof(filename), "%s/%s", os_info.cwd, GLOBALS_DUMP_FILE);
	unlink(filename);
	snprintf(filename, sizeof(filename), "%s/%s", os_info.cwd, DB_DUMP_FILE);
	unlink(filename);
}

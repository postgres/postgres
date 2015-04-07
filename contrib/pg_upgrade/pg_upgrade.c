/*
 *	pg_upgrade.c
 *
 *	main source file
 *
 *	Copyright (c) 2010, PostgreSQL Global Development Group
 *	$PostgreSQL: pgsql/contrib/pg_upgrade/pg_upgrade.c,v 1.10.2.1 2010/07/13 20:15:51 momjian Exp $
 */

#include "pg_upgrade.h"

#ifdef HAVE_LANGINFO_H
#include <langinfo.h>
#endif

static void disable_old_cluster(migratorContext *ctx);
static void prepare_new_cluster(migratorContext *ctx);
static void prepare_new_databases(migratorContext *ctx);
static void create_new_objects(migratorContext *ctx);
static void copy_clog_xlog_xid(migratorContext *ctx);
static void set_frozenxids(migratorContext *ctx);
static void setup(migratorContext *ctx, char *argv0, bool live_check);
static void cleanup(migratorContext *ctx);
static void	get_restricted_token(const char *progname);

#ifdef WIN32
static char * pg_strdupn(const char *str);
static int	CreateRestrictedProcess(char *cmd, PROCESS_INFORMATION *processInfo, const char *progname);
#endif

#ifdef WIN32
static char *restrict_env;
#endif

int
main(int argc, char **argv)
{
	migratorContext ctx;
	char	   *sequence_script_file_name = NULL;
	char	   *deletion_script_file_name = NULL;
	bool		live_check = false;

	memset(&ctx, 0, sizeof(ctx));

	parseCommandLine(&ctx, argc, argv);

	get_restricted_token(ctx.progname);

	output_check_banner(&ctx, &live_check);

	setup(&ctx, argv[0], live_check);

	check_cluster_versions(&ctx);
	check_cluster_compatibility(&ctx, live_check);

	check_old_cluster(&ctx, live_check, &sequence_script_file_name);


	/* -- NEW -- */
	start_postmaster(&ctx, CLUSTER_NEW, false);

	check_new_cluster(&ctx);
	report_clusters_compatible(&ctx);

	pg_log(&ctx, PG_REPORT, "\nPerforming Migration\n");
	pg_log(&ctx, PG_REPORT, "--------------------\n");

	disable_old_cluster(&ctx);
	prepare_new_cluster(&ctx);

	stop_postmaster(&ctx, false, false);

	/*
	 * Destructive Changes to New Cluster
	 */

	copy_clog_xlog_xid(&ctx);

	/* New now using xids of the old system */

	prepare_new_databases(&ctx);

	create_new_objects(&ctx);

	transfer_all_new_dbs(&ctx, &ctx.old.dbarr, &ctx.new.dbarr,
						 ctx.old.pgdata, ctx.new.pgdata);

	/*
	 * Assuming OIDs are only used in system tables, there is no need to
	 * restore the OID counter because we have not transferred any OIDs from
	 * the old system, but we do it anyway just in case.  We do it late here
	 * because there is no need to have the schema load use new oids.
	 */
	prep_status(&ctx, "Setting next oid for new cluster");
	exec_prog(&ctx, true, SYSTEMQUOTE "\"%s/pg_resetxlog\" -o %u \"%s\" > "
			  DEVNULL SYSTEMQUOTE,
		  ctx.new.bindir, ctx.old.controldata.chkpnt_nxtoid, ctx.new.pgdata);
	check_ok(&ctx);

	create_script_for_old_cluster_deletion(&ctx, &deletion_script_file_name);

	issue_warnings(&ctx, sequence_script_file_name);

	pg_log(&ctx, PG_REPORT, "\nUpgrade complete\n");
	pg_log(&ctx, PG_REPORT, "----------------\n");

	output_completion_banner(&ctx, deletion_script_file_name);

	pg_free(deletion_script_file_name);
	pg_free(sequence_script_file_name);

	cleanup(&ctx);

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

static void
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

		cmdline = pg_strdupn(GetCommandLine());

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
setup(migratorContext *ctx, char *argv0, bool live_check)
{
	char		exec_path[MAXPGPATH];	/* full path to my executable */

	/*
	 * make sure the user has a clean environment, otherwise, we may confuse
	 * libpq when we connect to one (or both) of the servers.
	 */
	check_for_libpq_envvars(ctx);

	verify_directories(ctx);

	/* no postmasters should be running */
	if (!live_check && is_server_running(ctx, ctx->old.pgdata))
	{
		pg_log(ctx, PG_FATAL, "There seems to be a postmaster servicing the old cluster.\n"
			   "Please shutdown that postmaster and try again.\n");
	}

	/* same goes for the new postmaster */
	if (is_server_running(ctx, ctx->new.pgdata))
	{
		pg_log(ctx, PG_FATAL, "There seems to be a postmaster servicing the new cluster.\n"
			   "Please shutdown that postmaster and try again.\n");
	}

	/* get path to pg_upgrade executable */
	if (find_my_exec(argv0, exec_path) < 0)
		pg_log(ctx, PG_FATAL, "Could not get pathname to pg_upgrade: %s\n", getErrorText(errno));

	/* Trim off program name and keep just path */
	*last_dir_separator(exec_path) = '\0';
	canonicalize_path(exec_path);
	ctx->exec_path = pg_strdup(ctx, exec_path);
}


static void
disable_old_cluster(migratorContext *ctx)
{
	/* rename pg_control so old server cannot be accidentally started */
	rename_old_pg_control(ctx);
}


static void
prepare_new_cluster(migratorContext *ctx)
{
	/*
	 * It would make more sense to freeze after loading the schema, but that
	 * would cause us to lose the frozenids restored by the load. We use
	 * --analyze so autovacuum doesn't update statistics later
	 */
	prep_status(ctx, "Analyzing all rows in the new cluster");
	exec_prog(ctx, true,
			  SYSTEMQUOTE "\"%s/vacuumdb\" --port %d --username \"%s\" "
			  "--all --analyze >> \"%s\" 2>&1" SYSTEMQUOTE,
			  ctx->new.bindir, ctx->new.port, ctx->user,
#ifndef WIN32
			  ctx->logfile
#else
			  DEVNULL
#endif
			  );
	check_ok(ctx);

	/*
	 * We do freeze after analyze so pg_statistic is also frozen. template0 is
	 * not frozen here, but data rows were frozen by initdb, and we set its
	 * datfrozenxid and relfrozenxids later to match the new xid counter
	 * later.
	 */
	prep_status(ctx, "Freezing all rows on the new cluster");
	exec_prog(ctx, true,
			  SYSTEMQUOTE "\"%s/vacuumdb\" --port %d --username \"%s\" "
			  "--all --freeze >> \"%s\" 2>&1" SYSTEMQUOTE,
			  ctx->new.bindir, ctx->new.port, ctx->user,
#ifndef WIN32
			  ctx->logfile
#else
			  DEVNULL
#endif
			  );
	check_ok(ctx);

	get_pg_database_relfilenode(ctx, CLUSTER_NEW);
}


static void
prepare_new_databases(migratorContext *ctx)
{
	/* -- NEW -- */
	start_postmaster(ctx, CLUSTER_NEW, false);

	/*
	 * We set autovacuum_freeze_max_age to its maximum value so autovacuum
	 * does not launch here and delete clog files, before the frozen xids are
	 * set.
	 */

	set_frozenxids(ctx);

	/*
	 * We have to create the databases first so we can create the toast table
	 * placeholder relfiles.
	 */
	prep_status(ctx, "Creating databases in the new cluster");
	exec_prog(ctx, true,
			  SYSTEMQUOTE "\"%s/psql\" --set ON_ERROR_STOP=on "
			  /* --no-psqlrc prevents AUTOCOMMIT=off */
			  "--no-psqlrc --port %d --username \"%s\" "
			  "-f \"%s/%s\" --dbname template1 >> \"%s\"" SYSTEMQUOTE,
			  ctx->new.bindir, ctx->new.port, ctx->user, ctx->cwd,
			  GLOBALS_DUMP_FILE,
#ifndef WIN32
			  ctx->logfile
#else
			  DEVNULL
#endif
			  );
	check_ok(ctx);

	get_db_and_rel_infos(ctx, &ctx->new.dbarr, CLUSTER_NEW);

	stop_postmaster(ctx, false, false);
}


static void
create_new_objects(migratorContext *ctx)
{
	/* -- NEW -- */
	start_postmaster(ctx, CLUSTER_NEW, false);

	install_support_functions(ctx);

	prep_status(ctx, "Restoring database schema to new cluster");
	exec_prog(ctx, true,
			  SYSTEMQUOTE "\"%s/psql\" --set ON_ERROR_STOP=on "
			  "--no-psqlrc --port %d --username \"%s\" "
			  "-f \"%s/%s\" --dbname template1 >> \"%s\"" SYSTEMQUOTE,
			  ctx->new.bindir, ctx->new.port, ctx->user, ctx->cwd,
			  DB_DUMP_FILE,
#ifndef WIN32
			  ctx->logfile
#else
			  DEVNULL
#endif
			  );
	check_ok(ctx);

	/* regenerate now that we have db schemas */
	dbarr_free(&ctx->new.dbarr);
	get_db_and_rel_infos(ctx, &ctx->new.dbarr, CLUSTER_NEW);

	uninstall_support_functions(ctx);

	stop_postmaster(ctx, false, false);
}


static void
copy_clog_xlog_xid(migratorContext *ctx)
{
	char		old_clog_path[MAXPGPATH];
	char		new_clog_path[MAXPGPATH];

	/* copy old commit logs to new data dir */
	prep_status(ctx, "Deleting new commit clogs");

	snprintf(old_clog_path, sizeof(old_clog_path), "%s/pg_clog", ctx->old.pgdata);
	snprintf(new_clog_path, sizeof(new_clog_path), "%s/pg_clog", ctx->new.pgdata);
	if (rmtree(new_clog_path, true) != true)
		pg_log(ctx, PG_FATAL, "Unable to delete directory %s\n", new_clog_path);
	check_ok(ctx);

	prep_status(ctx, "Copying old commit clogs to new server");
	/* libpgport's copydir() doesn't work in FRONTEND code */
#ifndef WIN32
	exec_prog(ctx, true, SYSTEMQUOTE "%s \"%s\" \"%s\"" SYSTEMQUOTE,
			  "cp -Rf",
#else
	/* flags: everything, no confirm, quiet, overwrite read-only */
	exec_prog(ctx, true, SYSTEMQUOTE "%s \"%s\" \"%s\\\"" SYSTEMQUOTE,
			  "xcopy /e /y /q /r",
#endif
			  old_clog_path, new_clog_path);
	check_ok(ctx);

	/* set the next transaction id of the new cluster */
	prep_status(ctx, "Setting next transaction id for new cluster");
	exec_prog(ctx, true, SYSTEMQUOTE "\"%s/pg_resetxlog\" -f -x %u \"%s\" > " DEVNULL SYSTEMQUOTE,
	   ctx->new.bindir, ctx->old.controldata.chkpnt_nxtxid, ctx->new.pgdata);
	check_ok(ctx);

	/* now reset the wal archives in the new cluster */
	prep_status(ctx, "Resetting WAL archives");
	exec_prog(ctx, true, SYSTEMQUOTE "\"%s/pg_resetxlog\" -l %u,%u,%u \"%s\" >> \"%s\" 2>&1" SYSTEMQUOTE,
			  ctx->new.bindir, ctx->old.controldata.chkpnt_tli,
			  ctx->old.controldata.logid, ctx->old.controldata.nxtlogseg,
			  ctx->new.pgdata,
#ifndef WIN32
			  ctx->logfile
#else
			  DEVNULL
#endif
			  );
	check_ok(ctx);
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
set_frozenxids(migratorContext *ctx)
{
	int			dbnum;
	PGconn	   *conn,
			   *conn_template1;
	PGresult   *dbres;
	int			ntups;
	int			i_datname;
	int			i_datallowconn;

	prep_status(ctx, "Setting frozenxid counters in new cluster");

	conn_template1 = connectToServer(ctx, "template1", CLUSTER_NEW);

	/* set pg_database.datfrozenxid */
	PQclear(executeQueryOrDie(ctx, conn_template1,
							  "UPDATE pg_catalog.pg_database "
							  "SET	datfrozenxid = '%u'",
							  ctx->old.controldata.chkpnt_nxtxid));

	/* get database names */
	dbres = executeQueryOrDie(ctx, conn_template1,
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
			PQclear(executeQueryOrDie(ctx, conn_template1,
									  "UPDATE pg_catalog.pg_database "
									  "SET	datallowconn = true "
									  "WHERE datname = '%s'", datname));

		conn = connectToServer(ctx, datname, CLUSTER_NEW);

		/* set pg_class.relfrozenxid */
		PQclear(executeQueryOrDie(ctx, conn,
								  "UPDATE	pg_catalog.pg_class "
								  "SET	relfrozenxid = '%u' "
		/* only heap and TOAST are vacuumed */
								  "WHERE	relkind IN ('r', 't')",
								  ctx->old.controldata.chkpnt_nxtxid));
		PQfinish(conn);

		/* Reset datallowconn flag */
		if (strcmp(datallowconn, "f") == 0)
			PQclear(executeQueryOrDie(ctx, conn_template1,
									  "UPDATE pg_catalog.pg_database "
									  "SET	datallowconn = false "
									  "WHERE datname = '%s'", datname));
	}

	PQclear(dbres);

	PQfinish(conn_template1);

	check_ok(ctx);
}


static void
cleanup(migratorContext *ctx)
{
	int			tblnum;
	char		filename[MAXPGPATH];

	for (tblnum = 0; tblnum < ctx->num_tablespaces; tblnum++)
		pg_free(ctx->tablespaces[tblnum]);
	pg_free(ctx->tablespaces);

	dbarr_free(&ctx->old.dbarr);
	dbarr_free(&ctx->new.dbarr);
	pg_free(ctx->logfile);
	pg_free(ctx->user);
	pg_free(ctx->old.major_version_str);
	pg_free(ctx->new.major_version_str);
	pg_free(ctx->old.controldata.lc_collate);
	pg_free(ctx->new.controldata.lc_collate);
	pg_free(ctx->old.controldata.lc_ctype);
	pg_free(ctx->new.controldata.lc_ctype);
	pg_free(ctx->old.controldata.encoding);
	pg_free(ctx->new.controldata.encoding);
	pg_free(ctx->old.tablespace_suffix);
	pg_free(ctx->new.tablespace_suffix);

	if (ctx->log_fd != NULL)
	{
		fclose(ctx->log_fd);
		ctx->log_fd = NULL;
	}

	if (ctx->debug_fd)
		fclose(ctx->debug_fd);

	snprintf(filename, sizeof(filename), "%s/%s", ctx->cwd, ALL_DUMP_FILE);
	unlink(filename);
	snprintf(filename, sizeof(filename), "%s/%s", ctx->cwd, GLOBALS_DUMP_FILE);
	unlink(filename);
	snprintf(filename, sizeof(filename), "%s/%s", ctx->cwd, DB_DUMP_FILE);
	unlink(filename);
}

#ifdef WIN32
static char *
pg_strdupn(const char *str)
{
       char    *result = strdup(str);

       if (!result)
       {
               fprintf(stderr, _("out of memory\n"));
               exit(1);
       }
       return result;
}
#endif

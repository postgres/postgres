/*
 *	controldata.c
 *
 *	controldata functions
 *
 *	Copyright (c) 2010-2024, PostgreSQL Global Development Group
 *	src/bin/pg_upgrade/controldata.c
 */

#include "postgres_fe.h"

#include <ctype.h>

#include "common/string.h"
#include "pg_upgrade.h"


/*
 * get_control_data()
 *
 * gets pg_control information in "ctrl". Assumes that bindir and
 * datadir are valid absolute paths to postgresql bin and pgdata
 * directories respectively *and* pg_resetwal is version compatible
 * with datadir. The main purpose of this function is to get pg_control
 * data in a version independent manner.
 *
 * The approach taken here is to invoke pg_resetwal with -n option
 * and then pipe its output. With little string parsing we get the
 * pg_control data.  pg_resetwal cannot be run while the server is running
 * so we use pg_controldata;  pg_controldata doesn't provide all the fields
 * we need to actually perform the upgrade, but it provides enough for
 * check mode.  We do not implement pg_resetwal -n because it is hard to
 * return valid xid data for a running server.
 */
void
get_control_data(ClusterInfo *cluster)
{
	char		cmd[MAXPGPATH];
	char		bufin[MAX_STRING];
	FILE	   *output;
	char	   *p;
	bool		got_tli = false;
	bool		got_log_id = false;
	bool		got_log_seg = false;
	bool		got_xid = false;
	bool		got_oid = false;
	bool		got_multi = false;
	bool		got_oldestmulti = false;
	bool		got_oldestxid = false;
	bool		got_mxoff = false;
	bool		got_nextxlogfile = false;
	bool		got_float8_pass_by_value = false;
	bool		got_align = false;
	bool		got_blocksz = false;
	bool		got_largesz = false;
	bool		got_walsz = false;
	bool		got_walseg = false;
	bool		got_ident = false;
	bool		got_index = false;
	bool		got_toast = false;
	bool		got_large_object = false;
	bool		got_date_is_int = false;
	bool		got_data_checksum_version = false;
	bool		got_cluster_state = false;
	char	   *lc_collate = NULL;
	char	   *lc_ctype = NULL;
	char	   *lc_monetary = NULL;
	char	   *lc_numeric = NULL;
	char	   *lc_time = NULL;
	char	   *lang = NULL;
	char	   *language = NULL;
	char	   *lc_all = NULL;
	char	   *lc_messages = NULL;
	uint32		tli = 0;
	uint32		logid = 0;
	uint32		segno = 0;
	char	   *resetwal_bin;
	int			rc;
	bool		live_check = (cluster == &old_cluster && user_opts.live_check);

	/*
	 * Because we test the pg_resetwal output as strings, it has to be in
	 * English.  Copied from pg_regress.c.
	 */
	if (getenv("LC_COLLATE"))
		lc_collate = pg_strdup(getenv("LC_COLLATE"));
	if (getenv("LC_CTYPE"))
		lc_ctype = pg_strdup(getenv("LC_CTYPE"));
	if (getenv("LC_MONETARY"))
		lc_monetary = pg_strdup(getenv("LC_MONETARY"));
	if (getenv("LC_NUMERIC"))
		lc_numeric = pg_strdup(getenv("LC_NUMERIC"));
	if (getenv("LC_TIME"))
		lc_time = pg_strdup(getenv("LC_TIME"));
	if (getenv("LANG"))
		lang = pg_strdup(getenv("LANG"));
	if (getenv("LANGUAGE"))
		language = pg_strdup(getenv("LANGUAGE"));
	if (getenv("LC_ALL"))
		lc_all = pg_strdup(getenv("LC_ALL"));
	if (getenv("LC_MESSAGES"))
		lc_messages = pg_strdup(getenv("LC_MESSAGES"));

	unsetenv("LC_COLLATE");
	unsetenv("LC_CTYPE");
	unsetenv("LC_MONETARY");
	unsetenv("LC_NUMERIC");
	unsetenv("LC_TIME");
#ifndef WIN32
	unsetenv("LANG");
#else
	/* On Windows the default locale may not be English, so force it */
	setenv("LANG", "en", 1);
#endif
	unsetenv("LANGUAGE");
	unsetenv("LC_ALL");
	setenv("LC_MESSAGES", "C", 1);

	/*
	 * Check for clean shutdown
	 */
	if (!live_check || cluster == &new_cluster)
	{
		/* only pg_controldata outputs the cluster state */
		snprintf(cmd, sizeof(cmd), "\"%s/pg_controldata\" \"%s\"",
				 cluster->bindir, cluster->pgdata);
		fflush(NULL);

		if ((output = popen(cmd, "r")) == NULL)
			pg_fatal("could not get control data using %s: %m", cmd);

		/* we have the result of cmd in "output". so parse it line by line now */
		while (fgets(bufin, sizeof(bufin), output))
		{
			if ((p = strstr(bufin, "Database cluster state:")) != NULL)
			{
				p = strchr(p, ':');

				if (p == NULL || strlen(p) <= 1)
					pg_fatal("%d: database cluster state problem", __LINE__);

				p++;			/* remove ':' char */

				/*
				 * We checked earlier for a postmaster lock file, and if we
				 * found one, we tried to start/stop the server to replay the
				 * WAL.  However, pg_ctl -m immediate doesn't leave a lock
				 * file, but does require WAL replay, so we check here that
				 * the server was shut down cleanly, from the controldata
				 * perspective.
				 */
				/* Remove trailing newline and leading spaces */
				(void) pg_strip_crlf(p);
				while (*p == ' ')
					p++;
				if (strcmp(p, "shut down in recovery") == 0)
				{
					if (cluster == &old_cluster)
						pg_fatal("The source cluster was shut down while in recovery mode.  To upgrade, use \"rsync\" as documented or shut it down as a primary.");
					else
						pg_fatal("The target cluster was shut down while in recovery mode.  To upgrade, use \"rsync\" as documented or shut it down as a primary.");
				}
				else if (strcmp(p, "shut down") != 0)
				{
					if (cluster == &old_cluster)
						pg_fatal("The source cluster was not shut down cleanly, state reported as: \"%s\"", p);
					else
						pg_fatal("The target cluster was not shut down cleanly, state reported as: \"%s\"", p);
				}
				got_cluster_state = true;
			}
		}

		rc = pclose(output);
		if (rc != 0)
			pg_fatal("could not get control data using %s: %s",
					 cmd, wait_result_to_str(rc));

		if (!got_cluster_state)
		{
			if (cluster == &old_cluster)
				pg_fatal("The source cluster lacks cluster state information:");
			else
				pg_fatal("The target cluster lacks cluster state information:");
		}
	}

	/* pg_resetxlog has been renamed to pg_resetwal in version 10 */
	if (GET_MAJOR_VERSION(cluster->bin_version) <= 906)
		resetwal_bin = "pg_resetxlog\" -n";
	else
		resetwal_bin = "pg_resetwal\" -n";
	snprintf(cmd, sizeof(cmd), "\"%s/%s \"%s\"",
			 cluster->bindir,
			 live_check ? "pg_controldata\"" : resetwal_bin,
			 cluster->pgdata);
	fflush(NULL);

	if ((output = popen(cmd, "r")) == NULL)
		pg_fatal("could not get control data using %s: %m", cmd);

	/* Only in <= 9.2 */
	if (GET_MAJOR_VERSION(cluster->major_version) <= 902)
	{
		cluster->controldata.data_checksum_version = 0;
		got_data_checksum_version = true;
	}

	/* we have the result of cmd in "output". so parse it line by line now */
	while (fgets(bufin, sizeof(bufin), output))
	{
		/* In verbose mode, log each line */
		pg_strip_crlf(bufin);
		pg_log(PG_VERBOSE, "%s", bufin);

		if ((p = strstr(bufin, "pg_control version number:")) != NULL)
		{
			p = strchr(p, ':');

			if (p == NULL || strlen(p) <= 1)
				pg_fatal("%d: pg_resetwal problem", __LINE__);

			p++;				/* remove ':' char */
			cluster->controldata.ctrl_ver = str2uint(p);
		}
		else if ((p = strstr(bufin, "Catalog version number:")) != NULL)
		{
			p = strchr(p, ':');

			if (p == NULL || strlen(p) <= 1)
				pg_fatal("%d: controldata retrieval problem", __LINE__);

			p++;				/* remove ':' char */
			cluster->controldata.cat_ver = str2uint(p);
		}
		else if ((p = strstr(bufin, "Latest checkpoint's TimeLineID:")) != NULL)
		{
			p = strchr(p, ':');

			if (p == NULL || strlen(p) <= 1)
				pg_fatal("%d: controldata retrieval problem", __LINE__);

			p++;				/* remove ':' char */
			tli = str2uint(p);
			got_tli = true;
		}
		else if ((p = strstr(bufin, "First log file ID after reset:")) != NULL)
		{
			p = strchr(p, ':');

			if (p == NULL || strlen(p) <= 1)
				pg_fatal("%d: controldata retrieval problem", __LINE__);

			p++;				/* remove ':' char */
			logid = str2uint(p);
			got_log_id = true;
		}
		else if ((p = strstr(bufin, "First log file segment after reset:")) != NULL)
		{
			p = strchr(p, ':');

			if (p == NULL || strlen(p) <= 1)
				pg_fatal("%d: controldata retrieval problem", __LINE__);

			p++;				/* remove ':' char */
			segno = str2uint(p);
			got_log_seg = true;
		}
		else if ((p = strstr(bufin, "Latest checkpoint's NextXID:")) != NULL)
		{
			p = strchr(p, ':');

			if (p == NULL || strlen(p) <= 1)
				pg_fatal("%d: controldata retrieval problem", __LINE__);

			p++;				/* remove ':' char */
			cluster->controldata.chkpnt_nxtepoch = str2uint(p);

			/*
			 * Delimiter changed from '/' to ':' in 9.6.  We don't test for
			 * the catalog version of the change because the catalog version
			 * is pulled from pg_controldata too, and it isn't worth adding an
			 * order dependency for this --- we just check the string.
			 */
			if (strchr(p, '/') != NULL)
				p = strchr(p, '/');
			else if (GET_MAJOR_VERSION(cluster->major_version) >= 906)
				p = strchr(p, ':');
			else
				p = NULL;

			if (p == NULL || strlen(p) <= 1)
				pg_fatal("%d: controldata retrieval problem", __LINE__);

			p++;				/* remove '/' or ':' char */
			cluster->controldata.chkpnt_nxtxid = str2uint(p);
			got_xid = true;
		}
		else if ((p = strstr(bufin, "Latest checkpoint's NextOID:")) != NULL)
		{
			p = strchr(p, ':');

			if (p == NULL || strlen(p) <= 1)
				pg_fatal("%d: controldata retrieval problem", __LINE__);

			p++;				/* remove ':' char */
			cluster->controldata.chkpnt_nxtoid = str2uint(p);
			got_oid = true;
		}
		else if ((p = strstr(bufin, "Latest checkpoint's NextMultiXactId:")) != NULL)
		{
			p = strchr(p, ':');

			if (p == NULL || strlen(p) <= 1)
				pg_fatal("%d: controldata retrieval problem", __LINE__);

			p++;				/* remove ':' char */
			cluster->controldata.chkpnt_nxtmulti = str2uint(p);
			got_multi = true;
		}
		else if ((p = strstr(bufin, "Latest checkpoint's oldestXID:")) != NULL)
		{
			p = strchr(p, ':');

			if (p == NULL || strlen(p) <= 1)
				pg_fatal("%d: controldata retrieval problem", __LINE__);

			p++;				/* remove ':' char */
			cluster->controldata.chkpnt_oldstxid = str2uint(p);
			got_oldestxid = true;
		}
		else if ((p = strstr(bufin, "Latest checkpoint's oldestMultiXid:")) != NULL)
		{
			p = strchr(p, ':');

			if (p == NULL || strlen(p) <= 1)
				pg_fatal("%d: controldata retrieval problem", __LINE__);

			p++;				/* remove ':' char */
			cluster->controldata.chkpnt_oldstMulti = str2uint(p);
			got_oldestmulti = true;
		}
		else if ((p = strstr(bufin, "Latest checkpoint's NextMultiOffset:")) != NULL)
		{
			p = strchr(p, ':');

			if (p == NULL || strlen(p) <= 1)
				pg_fatal("%d: controldata retrieval problem", __LINE__);

			p++;				/* remove ':' char */
			cluster->controldata.chkpnt_nxtmxoff = str2uint(p);
			got_mxoff = true;
		}
		else if ((p = strstr(bufin, "First log segment after reset:")) != NULL)
		{
			/* Skip the colon and any whitespace after it */
			p = strchr(p, ':');
			if (p == NULL || strlen(p) <= 1)
				pg_fatal("%d: controldata retrieval problem", __LINE__);
			p = strpbrk(p, "01234567890ABCDEF");
			if (p == NULL || strlen(p) <= 1)
				pg_fatal("%d: controldata retrieval problem", __LINE__);

			/* Make sure it looks like a valid WAL file name */
			if (strspn(p, "0123456789ABCDEF") != 24)
				pg_fatal("%d: controldata retrieval problem", __LINE__);

			strlcpy(cluster->controldata.nextxlogfile, p, 25);
			got_nextxlogfile = true;
		}
		else if ((p = strstr(bufin, "Float8 argument passing:")) != NULL)
		{
			p = strchr(p, ':');

			if (p == NULL || strlen(p) <= 1)
				pg_fatal("%d: controldata retrieval problem", __LINE__);

			p++;				/* remove ':' char */
			/* used later for contrib check */
			cluster->controldata.float8_pass_by_value = strstr(p, "by value") != NULL;
			got_float8_pass_by_value = true;
		}
		else if ((p = strstr(bufin, "Maximum data alignment:")) != NULL)
		{
			p = strchr(p, ':');

			if (p == NULL || strlen(p) <= 1)
				pg_fatal("%d: controldata retrieval problem", __LINE__);

			p++;				/* remove ':' char */
			cluster->controldata.align = str2uint(p);
			got_align = true;
		}
		else if ((p = strstr(bufin, "Database block size:")) != NULL)
		{
			p = strchr(p, ':');

			if (p == NULL || strlen(p) <= 1)
				pg_fatal("%d: controldata retrieval problem", __LINE__);

			p++;				/* remove ':' char */
			cluster->controldata.blocksz = str2uint(p);
			got_blocksz = true;
		}
		else if ((p = strstr(bufin, "Blocks per segment of large relation:")) != NULL)
		{
			p = strchr(p, ':');

			if (p == NULL || strlen(p) <= 1)
				pg_fatal("%d: controldata retrieval problem", __LINE__);

			p++;				/* remove ':' char */
			cluster->controldata.largesz = str2uint(p);
			got_largesz = true;
		}
		else if ((p = strstr(bufin, "WAL block size:")) != NULL)
		{
			p = strchr(p, ':');

			if (p == NULL || strlen(p) <= 1)
				pg_fatal("%d: controldata retrieval problem", __LINE__);

			p++;				/* remove ':' char */
			cluster->controldata.walsz = str2uint(p);
			got_walsz = true;
		}
		else if ((p = strstr(bufin, "Bytes per WAL segment:")) != NULL)
		{
			p = strchr(p, ':');

			if (p == NULL || strlen(p) <= 1)
				pg_fatal("%d: controldata retrieval problem", __LINE__);

			p++;				/* remove ':' char */
			cluster->controldata.walseg = str2uint(p);
			got_walseg = true;
		}
		else if ((p = strstr(bufin, "Maximum length of identifiers:")) != NULL)
		{
			p = strchr(p, ':');

			if (p == NULL || strlen(p) <= 1)
				pg_fatal("%d: controldata retrieval problem", __LINE__);

			p++;				/* remove ':' char */
			cluster->controldata.ident = str2uint(p);
			got_ident = true;
		}
		else if ((p = strstr(bufin, "Maximum columns in an index:")) != NULL)
		{
			p = strchr(p, ':');

			if (p == NULL || strlen(p) <= 1)
				pg_fatal("%d: controldata retrieval problem", __LINE__);

			p++;				/* remove ':' char */
			cluster->controldata.index = str2uint(p);
			got_index = true;
		}
		else if ((p = strstr(bufin, "Maximum size of a TOAST chunk:")) != NULL)
		{
			p = strchr(p, ':');

			if (p == NULL || strlen(p) <= 1)
				pg_fatal("%d: controldata retrieval problem", __LINE__);

			p++;				/* remove ':' char */
			cluster->controldata.toast = str2uint(p);
			got_toast = true;
		}
		else if ((p = strstr(bufin, "Size of a large-object chunk:")) != NULL)
		{
			p = strchr(p, ':');

			if (p == NULL || strlen(p) <= 1)
				pg_fatal("%d: controldata retrieval problem", __LINE__);

			p++;				/* remove ':' char */
			cluster->controldata.large_object = str2uint(p);
			got_large_object = true;
		}
		else if ((p = strstr(bufin, "Date/time type storage:")) != NULL)
		{
			p = strchr(p, ':');

			if (p == NULL || strlen(p) <= 1)
				pg_fatal("%d: controldata retrieval problem", __LINE__);

			p++;				/* remove ':' char */
			cluster->controldata.date_is_int = strstr(p, "64-bit integers") != NULL;
			got_date_is_int = true;
		}
		else if ((p = strstr(bufin, "checksum")) != NULL)
		{
			p = strchr(p, ':');

			if (p == NULL || strlen(p) <= 1)
				pg_fatal("%d: controldata retrieval problem", __LINE__);

			p++;				/* remove ':' char */
			cluster->controldata.data_checksum_version = str2uint(p);
			got_data_checksum_version = true;
		}
	}

	rc = pclose(output);
	if (rc != 0)
		pg_fatal("could not get control data using %s: %s",
				 cmd, wait_result_to_str(rc));

	/*
	 * Restore environment variables.  Note all but LANG and LC_MESSAGES were
	 * unset above.
	 */
	if (lc_collate)
		setenv("LC_COLLATE", lc_collate, 1);
	if (lc_ctype)
		setenv("LC_CTYPE", lc_ctype, 1);
	if (lc_monetary)
		setenv("LC_MONETARY", lc_monetary, 1);
	if (lc_numeric)
		setenv("LC_NUMERIC", lc_numeric, 1);
	if (lc_time)
		setenv("LC_TIME", lc_time, 1);
	if (lang)
		setenv("LANG", lang, 1);
	else
		unsetenv("LANG");
	if (language)
		setenv("LANGUAGE", language, 1);
	if (lc_all)
		setenv("LC_ALL", lc_all, 1);
	if (lc_messages)
		setenv("LC_MESSAGES", lc_messages, 1);
	else
		unsetenv("LC_MESSAGES");

	pg_free(lc_collate);
	pg_free(lc_ctype);
	pg_free(lc_monetary);
	pg_free(lc_numeric);
	pg_free(lc_time);
	pg_free(lang);
	pg_free(language);
	pg_free(lc_all);
	pg_free(lc_messages);

	/*
	 * Before 9.3, pg_resetwal reported the xlogid and segno of the first log
	 * file after reset as separate lines. Starting with 9.3, it reports the
	 * WAL file name. If the old cluster is older than 9.3, we construct the
	 * WAL file name from the xlogid and segno.
	 */
	if (GET_MAJOR_VERSION(cluster->major_version) <= 902)
	{
		if (got_tli && got_log_id && got_log_seg)
		{
			snprintf(cluster->controldata.nextxlogfile, 25, "%08X%08X%08X",
					 tli, logid, segno);
			got_nextxlogfile = true;
		}
	}

	/* verify that we got all the mandatory pg_control data */
	if (!got_xid || !got_oid ||
		!got_multi || !got_oldestxid ||
		(!got_oldestmulti &&
		 cluster->controldata.cat_ver >= MULTIXACT_FORMATCHANGE_CAT_VER) ||
		!got_mxoff || (!live_check && !got_nextxlogfile) ||
		!got_float8_pass_by_value || !got_align || !got_blocksz ||
		!got_largesz || !got_walsz || !got_walseg || !got_ident ||
		!got_index || !got_toast ||
		(!got_large_object &&
		 cluster->controldata.ctrl_ver >= LARGE_OBJECT_SIZE_PG_CONTROL_VER) ||
		!got_date_is_int || !got_data_checksum_version)
	{
		if (cluster == &old_cluster)
			pg_log(PG_REPORT,
				   "The source cluster lacks some required control information:");
		else
			pg_log(PG_REPORT,
				   "The target cluster lacks some required control information:");

		if (!got_xid)
			pg_log(PG_REPORT, "  checkpoint next XID");

		if (!got_oid)
			pg_log(PG_REPORT, "  latest checkpoint next OID");

		if (!got_multi)
			pg_log(PG_REPORT, "  latest checkpoint next MultiXactId");

		if (!got_oldestmulti &&
			cluster->controldata.cat_ver >= MULTIXACT_FORMATCHANGE_CAT_VER)
			pg_log(PG_REPORT, "  latest checkpoint oldest MultiXactId");

		if (!got_oldestxid)
			pg_log(PG_REPORT, "  latest checkpoint oldestXID");

		if (!got_mxoff)
			pg_log(PG_REPORT, "  latest checkpoint next MultiXactOffset");

		if (!live_check && !got_nextxlogfile)
			pg_log(PG_REPORT, "  first WAL segment after reset");

		if (!got_float8_pass_by_value)
			pg_log(PG_REPORT, "  float8 argument passing method");

		if (!got_align)
			pg_log(PG_REPORT, "  maximum alignment");

		if (!got_blocksz)
			pg_log(PG_REPORT, "  block size");

		if (!got_largesz)
			pg_log(PG_REPORT, "  large relation segment size");

		if (!got_walsz)
			pg_log(PG_REPORT, "  WAL block size");

		if (!got_walseg)
			pg_log(PG_REPORT, "  WAL segment size");

		if (!got_ident)
			pg_log(PG_REPORT, "  maximum identifier length");

		if (!got_index)
			pg_log(PG_REPORT, "  maximum number of indexed columns");

		if (!got_toast)
			pg_log(PG_REPORT, "  maximum TOAST chunk size");

		if (!got_large_object &&
			cluster->controldata.ctrl_ver >= LARGE_OBJECT_SIZE_PG_CONTROL_VER)
			pg_log(PG_REPORT, "  large-object chunk size");

		if (!got_date_is_int)
			pg_log(PG_REPORT, "  dates/times are integers?");

		/* value added in Postgres 9.3 */
		if (!got_data_checksum_version)
			pg_log(PG_REPORT, "  data checksum version");

		pg_fatal("Cannot continue without required control information, terminating");
	}
}


/*
 * check_control_data()
 *
 * check to make sure the control data settings are compatible
 */
void
check_control_data(ControlData *oldctrl,
				   ControlData *newctrl)
{
	if (oldctrl->align == 0 || oldctrl->align != newctrl->align)
		pg_fatal("old and new pg_controldata alignments are invalid or do not match.\n"
				 "Likely one cluster is a 32-bit install, the other 64-bit");

	if (oldctrl->blocksz == 0 || oldctrl->blocksz != newctrl->blocksz)
		pg_fatal("old and new pg_controldata block sizes are invalid or do not match");

	if (oldctrl->largesz == 0 || oldctrl->largesz != newctrl->largesz)
		pg_fatal("old and new pg_controldata maximum relation segment sizes are invalid or do not match");

	if (oldctrl->walsz == 0 || oldctrl->walsz != newctrl->walsz)
		pg_fatal("old and new pg_controldata WAL block sizes are invalid or do not match");

	if (oldctrl->walseg == 0 || oldctrl->walseg != newctrl->walseg)
		pg_fatal("old and new pg_controldata WAL segment sizes are invalid or do not match");

	if (oldctrl->ident == 0 || oldctrl->ident != newctrl->ident)
		pg_fatal("old and new pg_controldata maximum identifier lengths are invalid or do not match");

	if (oldctrl->index == 0 || oldctrl->index != newctrl->index)
		pg_fatal("old and new pg_controldata maximum indexed columns are invalid or do not match");

	if (oldctrl->toast == 0 || oldctrl->toast != newctrl->toast)
		pg_fatal("old and new pg_controldata maximum TOAST chunk sizes are invalid or do not match");

	/* large_object added in 9.5, so it might not exist in the old cluster */
	if (oldctrl->large_object != 0 &&
		oldctrl->large_object != newctrl->large_object)
		pg_fatal("old and new pg_controldata large-object chunk sizes are invalid or do not match");

	if (oldctrl->date_is_int != newctrl->date_is_int)
		pg_fatal("old and new pg_controldata date/time storage types do not match");

	/*
	 * float8_pass_by_value does not need to match, but is used in
	 * check_for_isn_and_int8_passing_mismatch().
	 */

	/*
	 * We might eventually allow upgrades from checksum to no-checksum
	 * clusters.
	 */
	if (oldctrl->data_checksum_version == 0 &&
		newctrl->data_checksum_version != 0)
		pg_fatal("old cluster does not use data checksums but the new one does");
	else if (oldctrl->data_checksum_version != 0 &&
			 newctrl->data_checksum_version == 0)
		pg_fatal("old cluster uses data checksums but the new one does not");
	else if (oldctrl->data_checksum_version != newctrl->data_checksum_version)
		pg_fatal("old and new cluster pg_controldata checksum versions do not match");
}


void
disable_old_cluster(void)
{
	char		old_path[MAXPGPATH],
				new_path[MAXPGPATH];

	/* rename pg_control so old server cannot be accidentally started */
	prep_status("Adding \".old\" suffix to old global/pg_control");

	snprintf(old_path, sizeof(old_path), "%s/global/pg_control", old_cluster.pgdata);
	snprintf(new_path, sizeof(new_path), "%s/global/pg_control.old", old_cluster.pgdata);
	if (pg_mv_file(old_path, new_path) != 0)
		pg_fatal("could not rename file \"%s\" to \"%s\": %m",
				 old_path, new_path);
	check_ok();

	pg_log(PG_REPORT, "\n"
		   "If you want to start the old cluster, you will need to remove\n"
		   "the \".old\" suffix from %s/global/pg_control.old.\n"
		   "Because \"link\" mode was used, the old cluster cannot be safely\n"
		   "started once the new cluster has been started.",
		   old_cluster.pgdata);
}

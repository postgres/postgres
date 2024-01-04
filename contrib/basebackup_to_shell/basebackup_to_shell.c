/*-------------------------------------------------------------------------
 *
 * basebackup_to_shell.c
 *	  target base backup files to a shell command
 *
 * Copyright (c) 2016-2024, PostgreSQL Global Development Group
 *
 *	  contrib/basebackup_to_shell/basebackup_to_shell.c
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "backup/basebackup_target.h"
#include "common/percentrepl.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "utils/acl.h"
#include "utils/guc.h"

PG_MODULE_MAGIC;

typedef struct bbsink_shell
{
	/* Common information for all types of sink. */
	bbsink		base;

	/* User-supplied target detail string. */
	char	   *target_detail;

	/* Shell command pattern being used for this backup. */
	char	   *shell_command;

	/* The command that is currently running. */
	char	   *current_command;

	/* Pipe to the running command. */
	FILE	   *pipe;
} bbsink_shell;

static void *shell_check_detail(char *target, char *target_detail);
static bbsink *shell_get_sink(bbsink *next_sink, void *detail_arg);

static void bbsink_shell_begin_archive(bbsink *sink,
									   const char *archive_name);
static void bbsink_shell_archive_contents(bbsink *sink, size_t len);
static void bbsink_shell_end_archive(bbsink *sink);
static void bbsink_shell_begin_manifest(bbsink *sink);
static void bbsink_shell_manifest_contents(bbsink *sink, size_t len);
static void bbsink_shell_end_manifest(bbsink *sink);

static const bbsink_ops bbsink_shell_ops = {
	.begin_backup = bbsink_forward_begin_backup,
	.begin_archive = bbsink_shell_begin_archive,
	.archive_contents = bbsink_shell_archive_contents,
	.end_archive = bbsink_shell_end_archive,
	.begin_manifest = bbsink_shell_begin_manifest,
	.manifest_contents = bbsink_shell_manifest_contents,
	.end_manifest = bbsink_shell_end_manifest,
	.end_backup = bbsink_forward_end_backup,
	.cleanup = bbsink_forward_cleanup
};

static char *shell_command = "";
static char *shell_required_role = "";

void
_PG_init(void)
{
	DefineCustomStringVariable("basebackup_to_shell.command",
							   "Shell command to be executed for each backup file.",
							   NULL,
							   &shell_command,
							   "",
							   PGC_SIGHUP,
							   0,
							   NULL, NULL, NULL);

	DefineCustomStringVariable("basebackup_to_shell.required_role",
							   "Backup user must be a member of this role to use shell backup target.",
							   NULL,
							   &shell_required_role,
							   "",
							   PGC_SIGHUP,
							   0,
							   NULL, NULL, NULL);

	MarkGUCPrefixReserved("basebackup_to_shell");

	BaseBackupAddTarget("shell", shell_check_detail, shell_get_sink);
}

/*
 * We choose to defer sanity checking until shell_get_sink(), and so
 * just pass the target detail through without doing anything. However, we do
 * permissions checks here, before any real work has been done.
 */
static void *
shell_check_detail(char *target, char *target_detail)
{
	if (shell_required_role[0] != '\0')
	{
		Oid			roleid;

		StartTransactionCommand();
		roleid = get_role_oid(shell_required_role, true);
		if (!has_privs_of_role(GetUserId(), roleid))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to use basebackup_to_shell")));
		CommitTransactionCommand();
	}

	return target_detail;
}

/*
 * Set up a bbsink to implement this base backup target.
 *
 * This is also a convenient place to sanity check that a target detail was
 * given if and only if %d is present.
 */
static bbsink *
shell_get_sink(bbsink *next_sink, void *detail_arg)
{
	bbsink_shell *sink;
	bool		has_detail_escape = false;
	char	   *c;

	/*
	 * Set up the bbsink.
	 *
	 * We remember the current value of basebackup_to_shell.shell_command to
	 * be certain that it can't change under us during the backup.
	 */
	sink = palloc0(sizeof(bbsink_shell));
	*((const bbsink_ops **) &sink->base.bbs_ops) = &bbsink_shell_ops;
	sink->base.bbs_next = next_sink;
	sink->target_detail = detail_arg;
	sink->shell_command = pstrdup(shell_command);

	/* Reject an empty shell command. */
	if (sink->shell_command[0] == '\0')
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("shell command for backup is not configured"));

	/* Determine whether the shell command we're using contains %d. */
	for (c = sink->shell_command; *c != '\0'; ++c)
	{
		if (c[0] == '%' && c[1] != '\0')
		{
			if (c[1] == 'd')
				has_detail_escape = true;
			++c;
		}
	}

	/* There should be a target detail if %d was used, and not otherwise. */
	if (has_detail_escape && sink->target_detail == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("a target detail is required because the configured command includes %%d"),
				 errhint("Try \"pg_basebackup --target shell:DETAIL ...\"")));
	else if (!has_detail_escape && sink->target_detail != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("a target detail is not permitted because the configured command does not include %%d")));

	/*
	 * Since we're passing the string provided by the user to popen(), it will
	 * be interpreted by the shell, which is a potential security
	 * vulnerability, since the user invoking this module is not necessarily a
	 * superuser. To stay out of trouble, we must disallow any shell
	 * metacharacters here; to be conservative and keep things simple, we
	 * allow only alphanumerics.
	 */
	if (sink->target_detail != NULL)
	{
		char	   *d;
		bool		scary = false;

		for (d = sink->target_detail; *d != '\0'; ++d)
		{
			if (*d >= 'a' && *d <= 'z')
				continue;
			if (*d >= 'A' && *d <= 'Z')
				continue;
			if (*d >= '0' && *d <= '9')
				continue;
			scary = true;
			break;
		}

		if (scary)
			ereport(ERROR,
					errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					errmsg("target detail must contain only alphanumeric characters"));
	}

	return &sink->base;
}

/*
 * Construct the exact shell command that we're actually going to run,
 * making substitutions as appropriate for escape sequences.
 */
static char *
shell_construct_command(const char *base_command, const char *filename,
						const char *target_detail)
{
	return replace_percent_placeholders(base_command, "basebackup_to_shell.command",
										"df", target_detail, filename);
}

/*
 * Finish executing the shell command once all data has been written.
 */
static void
shell_finish_command(bbsink_shell *sink)
{
	int			pclose_rc;

	/* There should be a command running. */
	Assert(sink->current_command != NULL);
	Assert(sink->pipe != NULL);

	/* Close down the pipe we opened. */
	pclose_rc = ClosePipeStream(sink->pipe);
	if (pclose_rc == -1)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close pipe to external command: %m")));
	else if (pclose_rc != 0)
	{
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("shell command \"%s\" failed",
						sink->current_command),
				 errdetail_internal("%s", wait_result_to_str(pclose_rc))));
	}

	/* Clean up. */
	sink->pipe = NULL;
	pfree(sink->current_command);
	sink->current_command = NULL;
}

/*
 * Start up the shell command, substituting %f in for the current filename.
 */
static void
shell_run_command(bbsink_shell *sink, const char *filename)
{
	/* There should not be anything already running. */
	Assert(sink->current_command == NULL);
	Assert(sink->pipe == NULL);

	/* Construct a suitable command. */
	sink->current_command = shell_construct_command(sink->shell_command,
													filename,
													sink->target_detail);

	/* Run it. */
	sink->pipe = OpenPipeStream(sink->current_command, PG_BINARY_W);
	if (sink->pipe == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not execute command \"%s\": %m",
						sink->current_command)));
}

/*
 * Send accumulated data to the running shell command.
 */
static void
shell_send_data(bbsink_shell *sink, size_t len)
{
	/* There should be a command running. */
	Assert(sink->current_command != NULL);
	Assert(sink->pipe != NULL);

	/* Try to write the data. */
	if (fwrite(sink->base.bbs_buffer, len, 1, sink->pipe) != 1 ||
		ferror(sink->pipe))
	{
		if (errno == EPIPE)
		{
			/*
			 * The error we're about to throw would shut down the command
			 * anyway, but we may get a more meaningful error message by doing
			 * this. If not, we'll fall through to the generic error below.
			 */
			shell_finish_command(sink);
			errno = EPIPE;
		}
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to shell backup program: %m")));
	}
}

/*
 * At start of archive, start up the shell command and forward to next sink.
 */
static void
bbsink_shell_begin_archive(bbsink *sink, const char *archive_name)
{
	bbsink_shell *mysink = (bbsink_shell *) sink;

	shell_run_command(mysink, archive_name);
	bbsink_forward_begin_archive(sink, archive_name);
}

/*
 * Send archive contents to command's stdin and forward to next sink.
 */
static void
bbsink_shell_archive_contents(bbsink *sink, size_t len)
{
	bbsink_shell *mysink = (bbsink_shell *) sink;

	shell_send_data(mysink, len);
	bbsink_forward_archive_contents(sink, len);
}

/*
 * At end of archive, shut down the shell command and forward to next sink.
 */
static void
bbsink_shell_end_archive(bbsink *sink)
{
	bbsink_shell *mysink = (bbsink_shell *) sink;

	shell_finish_command(mysink);
	bbsink_forward_end_archive(sink);
}

/*
 * At start of manifest, start up the shell command and forward to next sink.
 */
static void
bbsink_shell_begin_manifest(bbsink *sink)
{
	bbsink_shell *mysink = (bbsink_shell *) sink;

	shell_run_command(mysink, "backup_manifest");
	bbsink_forward_begin_manifest(sink);
}

/*
 * Send manifest contents to command's stdin and forward to next sink.
 */
static void
bbsink_shell_manifest_contents(bbsink *sink, size_t len)
{
	bbsink_shell *mysink = (bbsink_shell *) sink;

	shell_send_data(mysink, len);
	bbsink_forward_manifest_contents(sink, len);
}

/*
 * At end of manifest, shut down the shell command and forward to next sink.
 */
static void
bbsink_shell_end_manifest(bbsink *sink)
{
	bbsink_shell *mysink = (bbsink_shell *) sink;

	shell_finish_command(mysink);
	bbsink_forward_end_manifest(sink);
}

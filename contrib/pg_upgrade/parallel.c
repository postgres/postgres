/*
 *	parallel.c
 *
 *	multi-process support
 *
 *	Copyright (c) 2010-2014, PostgreSQL Global Development Group
 *	contrib/pg_upgrade/parallel.c
 */

#include "postgres_fe.h"

#include "pg_upgrade.h"

#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>

#ifdef WIN32
#include <io.h>
#endif

static int	parallel_jobs;

#ifdef WIN32
/*
 *	Array holding all active threads.  There can't be any gaps/zeros so
 *	it can be passed to WaitForMultipleObjects().  We use two arrays
 *	so the thread_handles array can be passed to WaitForMultipleObjects().
 */
HANDLE	   *thread_handles;

typedef struct
{
	char	   *log_file;
	char	   *opt_log_file;
	char	   *cmd;
} exec_thread_arg;

typedef struct
{
	DbInfoArr  *old_db_arr;
	DbInfoArr  *new_db_arr;
	char	   *old_pgdata;
	char	   *new_pgdata;
	char	   *old_tablespace;
} transfer_thread_arg;

exec_thread_arg **exec_thread_args;
transfer_thread_arg **transfer_thread_args;

/* track current thread_args struct so reap_child() can be used for all cases */
void	  **cur_thread_args;

DWORD		win32_exec_prog(exec_thread_arg *args);
DWORD		win32_transfer_all_new_dbs(transfer_thread_arg *args);
#endif

/*
 *	parallel_exec_prog
 *
 *	This has the same API as exec_prog, except it does parallel execution,
 *	and therefore must throw errors and doesn't return an error status.
 */
void
parallel_exec_prog(const char *log_file, const char *opt_log_file,
				   const char *fmt,...)
{
	va_list		args;
	char		cmd[MAX_STRING];

#ifndef WIN32
	pid_t		child;
#else
	HANDLE		child;
	exec_thread_arg *new_arg;
#endif

	va_start(args, fmt);
	vsnprintf(cmd, sizeof(cmd), fmt, args);
	va_end(args);

	if (user_opts.jobs <= 1)
		/* throw_error must be true to allow jobs */
		exec_prog(log_file, opt_log_file, true, "%s", cmd);
	else
	{
		/* parallel */
#ifdef WIN32
		if (thread_handles == NULL)
			thread_handles = pg_malloc(user_opts.jobs * sizeof(HANDLE));

		if (exec_thread_args == NULL)
		{
			int			i;

			exec_thread_args = pg_malloc(user_opts.jobs * sizeof(exec_thread_arg *));

			/*
			 * For safety and performance, we keep the args allocated during
			 * the entire life of the process, and we don't free the args in a
			 * thread different from the one that allocated it.
			 */
			for (i = 0; i < user_opts.jobs; i++)
				exec_thread_args[i] = pg_malloc0(sizeof(exec_thread_arg));
		}

		cur_thread_args = (void **) exec_thread_args;
#endif
		/* harvest any dead children */
		while (reap_child(false) == true)
			;

		/* must we wait for a dead child? */
		if (parallel_jobs >= user_opts.jobs)
			reap_child(true);

		/* set this before we start the job */
		parallel_jobs++;

		/* Ensure stdio state is quiesced before forking */
		fflush(NULL);

#ifndef WIN32
		child = fork();
		if (child == 0)
			/* use _exit to skip atexit() functions */
			_exit(!exec_prog(log_file, opt_log_file, true, "%s", cmd));
		else if (child < 0)
			/* fork failed */
			pg_fatal("could not create worker process: %s\n", strerror(errno));
#else
		/* empty array element are always at the end */
		new_arg = exec_thread_args[parallel_jobs - 1];

		/* Can only pass one pointer into the function, so use a struct */
		if (new_arg->log_file)
			pg_free(new_arg->log_file);
		new_arg->log_file = pg_strdup(log_file);
		if (new_arg->opt_log_file)
			pg_free(new_arg->opt_log_file);
		new_arg->opt_log_file = opt_log_file ? pg_strdup(opt_log_file) : NULL;
		if (new_arg->cmd)
			pg_free(new_arg->cmd);
		new_arg->cmd = pg_strdup(cmd);

		child = (HANDLE) _beginthreadex(NULL, 0, (void *) win32_exec_prog,
										new_arg, 0, NULL);
		if (child == 0)
			pg_fatal("could not create worker thread: %s\n", strerror(errno));

		thread_handles[parallel_jobs - 1] = child;
#endif
	}

	return;
}


#ifdef WIN32
DWORD
win32_exec_prog(exec_thread_arg *args)
{
	int			ret;

	ret = !exec_prog(args->log_file, args->opt_log_file, true, "%s", args->cmd);

	/* terminates thread */
	return ret;
}
#endif


/*
 *	parallel_transfer_all_new_dbs
 *
 *	This has the same API as transfer_all_new_dbs, except it does parallel execution
 *	by transfering multiple tablespaces in parallel
 */
void
parallel_transfer_all_new_dbs(DbInfoArr *old_db_arr, DbInfoArr *new_db_arr,
							  char *old_pgdata, char *new_pgdata,
							  char *old_tablespace)
{
#ifndef WIN32
	pid_t		child;
#else
	HANDLE		child;
	transfer_thread_arg *new_arg;
#endif

	if (user_opts.jobs <= 1)
		/* throw_error must be true to allow jobs */
		transfer_all_new_dbs(old_db_arr, new_db_arr, old_pgdata, new_pgdata, NULL);
	else
	{
		/* parallel */
#ifdef WIN32
		if (thread_handles == NULL)
			thread_handles = pg_malloc(user_opts.jobs * sizeof(HANDLE));

		if (transfer_thread_args == NULL)
		{
			int			i;

			transfer_thread_args = pg_malloc(user_opts.jobs * sizeof(transfer_thread_arg *));

			/*
			 * For safety and performance, we keep the args allocated during
			 * the entire life of the process, and we don't free the args in a
			 * thread different from the one that allocated it.
			 */
			for (i = 0; i < user_opts.jobs; i++)
				transfer_thread_args[i] = pg_malloc0(sizeof(transfer_thread_arg));
		}

		cur_thread_args = (void **) transfer_thread_args;
#endif
		/* harvest any dead children */
		while (reap_child(false) == true)
			;

		/* must we wait for a dead child? */
		if (parallel_jobs >= user_opts.jobs)
			reap_child(true);

		/* set this before we start the job */
		parallel_jobs++;

		/* Ensure stdio state is quiesced before forking */
		fflush(NULL);

#ifndef WIN32
		child = fork();
		if (child == 0)
		{
			transfer_all_new_dbs(old_db_arr, new_db_arr, old_pgdata, new_pgdata,
								 old_tablespace);
			/* if we take another exit path, it will be non-zero */
			/* use _exit to skip atexit() functions */
			_exit(0);
		}
		else if (child < 0)
			/* fork failed */
			pg_fatal("could not create worker process: %s\n", strerror(errno));
#else
		/* empty array element are always at the end */
		new_arg = transfer_thread_args[parallel_jobs - 1];

		/* Can only pass one pointer into the function, so use a struct */
		new_arg->old_db_arr = old_db_arr;
		new_arg->new_db_arr = new_db_arr;
		if (new_arg->old_pgdata)
			pg_free(new_arg->old_pgdata);
		new_arg->old_pgdata = pg_strdup(old_pgdata);
		if (new_arg->new_pgdata)
			pg_free(new_arg->new_pgdata);
		new_arg->new_pgdata = pg_strdup(new_pgdata);
		if (new_arg->old_tablespace)
			pg_free(new_arg->old_tablespace);
		new_arg->old_tablespace = old_tablespace ? pg_strdup(old_tablespace) : NULL;

		child = (HANDLE) _beginthreadex(NULL, 0, (void *) win32_transfer_all_new_dbs,
										new_arg, 0, NULL);
		if (child == 0)
			pg_fatal("could not create worker thread: %s\n", strerror(errno));

		thread_handles[parallel_jobs - 1] = child;
#endif
	}

	return;
}


#ifdef WIN32
DWORD
win32_transfer_all_new_dbs(transfer_thread_arg *args)
{
	transfer_all_new_dbs(args->old_db_arr, args->new_db_arr, args->old_pgdata,
						 args->new_pgdata, args->old_tablespace);

	/* terminates thread */
	return 0;
}
#endif


/*
 *	collect status from a completed worker child
 */
bool
reap_child(bool wait_for_child)
{
#ifndef WIN32
	int			work_status;
	pid_t		child;
#else
	int			thread_num;
	DWORD		res;
#endif

	if (user_opts.jobs <= 1 || parallel_jobs == 0)
		return false;

#ifndef WIN32
	child = waitpid(-1, &work_status, wait_for_child ? 0 : WNOHANG);
	if (child == (pid_t) -1)
		pg_fatal("waitpid() failed: %s\n", strerror(errno));
	if (child == 0)
		return false;			/* no children, or no dead children */
	if (work_status != 0)
		pg_fatal("child process exited abnormally: status %d\n", work_status);
#else
	/* wait for one to finish */
	thread_num = WaitForMultipleObjects(parallel_jobs, thread_handles,
										false, wait_for_child ? INFINITE : 0);

	if (thread_num == WAIT_TIMEOUT || thread_num == WAIT_FAILED)
		return false;

	/* compute thread index in active_threads */
	thread_num -= WAIT_OBJECT_0;

	/* get the result */
	GetExitCodeThread(thread_handles[thread_num], &res);
	if (res != 0)
		pg_fatal("child worker exited abnormally: %s\n", strerror(errno));

	/* dispose of handle to stop leaks */
	CloseHandle(thread_handles[thread_num]);

	/* Move last slot into dead child's position */
	if (thread_num != parallel_jobs - 1)
	{
		void	   *tmp_args;

		thread_handles[thread_num] = thread_handles[parallel_jobs - 1];

		/*
		 * Move last active thead arg struct into the now-dead slot, and the
		 * now-dead slot to the end for reuse by the next thread. Though the
		 * thread struct is in use by another thread, we can safely swap the
		 * struct pointers within the array.
		 */
		tmp_args = cur_thread_args[thread_num];
		cur_thread_args[thread_num] = cur_thread_args[parallel_jobs - 1];
		cur_thread_args[parallel_jobs - 1] = tmp_args;
	}
#endif

	/* do this after job has been removed */
	parallel_jobs--;

	return true;
}

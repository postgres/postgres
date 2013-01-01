/*
 *	parallel.c
 *
 *	multi-process support
 *
 *	Copyright (c) 2010-2013, PostgreSQL Global Development Group
 *	contrib/pg_upgrade/parallel.c
 */

#include "postgres.h"

#include "pg_upgrade.h"

#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>

#ifdef WIN32
#include <io.h>
#endif

static int parallel_jobs;

#ifdef WIN32
/*
 *	Array holding all active threads.  There can't be any gaps/zeros so
 *	it can be passed to WaitForMultipleObjects().  We use two arrays
 *	so the thread_handles array can be passed to WaitForMultipleObjects().
 */
HANDLE *thread_handles;

typedef struct {
	char log_file[MAXPGPATH];
	char opt_log_file[MAXPGPATH];
	char cmd[MAX_STRING];
} thread_arg;

thread_arg **thread_args;

DWORD win32_exec_prog(thread_arg *args);

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
	thread_arg	*new_arg;
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
			pg_log(PG_FATAL, "could not create worker process: %s\n", strerror(errno));
#else
		if (thread_handles == NULL)
		{
			int i;

			thread_handles = pg_malloc(user_opts.jobs * sizeof(HANDLE));
			thread_args = pg_malloc(user_opts.jobs * sizeof(thread_arg *));

			/*
			 *	For safety and performance, we keep the args allocated during
			 *	the entire life of the process, and we don't free the args
			 *	in a thread different from the one that allocated it.
			 */
			for (i = 0; i < user_opts.jobs; i++)
				thread_args[i] = pg_malloc(sizeof(thread_arg));
		}

		/* use first empty array element */
		new_arg = thread_args[parallel_jobs-1];

		/* Can only pass one pointer into the function, so use a struct */
		strcpy(new_arg->log_file, log_file);
		strcpy(new_arg->opt_log_file, opt_log_file);
		strcpy(new_arg->cmd, cmd);

		child = (HANDLE) _beginthreadex(NULL, 0, (void *) win32_exec_prog,
						new_arg, 0, NULL);
		if (child == 0)
			pg_log(PG_FATAL, "could not create worker thread: %s\n", strerror(errno));

		thread_handles[parallel_jobs-1] = child;
#endif
	}

	return;
}


#ifdef WIN32
DWORD
win32_exec_prog(thread_arg *args)
{
	int ret;

	ret = !exec_prog(args->log_file, args->opt_log_file, true, "%s", args->cmd);

	/* terminates thread */
	return ret;
}
#endif


/*
 *	collect status from a completed worker child
 */
bool
reap_child(bool wait_for_child)
{
#ifndef WIN32
	int work_status;
	int ret;
#else
	int				thread_num;
	DWORD			res;
#endif

	if (user_opts.jobs <= 1 || parallel_jobs == 0)
		return false;

#ifndef WIN32
	ret = waitpid(-1, &work_status, wait_for_child ? 0 : WNOHANG);

	/* no children or, for WNOHANG, no dead children */
	if (ret <= 0 || !WIFEXITED(work_status))
		return false;

	if (WEXITSTATUS(work_status) != 0)
		pg_log(PG_FATAL, "child worker exited abnormally: %s\n", strerror(errno));

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
		pg_log(PG_FATAL, "child worker exited abnormally: %s\n", strerror(errno));

	/* dispose of handle to stop leaks */
	CloseHandle(thread_handles[thread_num]);

	/*	Move last slot into dead child's position */
	if (thread_num != parallel_jobs - 1)
	{
		thread_arg *tmp_args;
	
		thread_handles[thread_num] = thread_handles[parallel_jobs - 1];

		/*
		 *	We must swap the arg struct pointers because the thread we
		 *	just moved is active, and we must make sure it is not
		 *	reused by the next created thread.  Instead, the new thread
		 *	will use the arg struct of the thread that just died.
		 */
		tmp_args = thread_args[thread_num];
		thread_args[thread_num] = thread_args[parallel_jobs - 1];
		thread_args[parallel_jobs - 1] = tmp_args;
	}
#endif

	/* do this after job has been removed */
	parallel_jobs--;

	return true;
}

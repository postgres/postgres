/*
 * task.c
 *		framework for parallelizing pg_upgrade's once-in-each-database tasks
 *
 * This framework provides an efficient way of running the various
 * once-in-each-database tasks required by pg_upgrade.  Specifically, it
 * parallelizes these tasks by managing a set of slots that follow a simple
 * state machine and by using libpq's asynchronous APIs to establish the
 * connections and run the queries.  Callers simply need to create a callback
 * function and build/execute an UpgradeTask.  A simple example follows:
 *
 *		static void
 *		my_process_cb(DbInfo *dbinfo, PGresult *res, void *arg)
 *		{
 *			for (int i = 0; i < PQntuples(res); i++)
 *			{
 *				... process results ...
 *			}
 *		}
 *
 *		void
 *		my_task(ClusterInfo *cluster)
 *		{
 *			UpgradeTask *task = upgrade_task_create();
 *
 *			upgrade_task_add_step(task,
 *								  "... query text ...",
 *								  my_process_cb,
 *								  true,		// let the task free the PGresult
 *								  NULL);	// "arg" pointer for callback
 *			upgrade_task_run(task, cluster);
 *			upgrade_task_free(task);
 *		}
 *
 * Note that multiple steps can be added to a given task.  When there are
 * multiple steps, the task will run all of the steps consecutively in the same
 * database connection before freeing the connection and moving on.  In other
 * words, it only ever initiates one connection to each database in the
 * cluster for a given run.
 *
 * Copyright (c) 2024, PostgreSQL Global Development Group
 * src/bin/pg_upgrade/task.c
 */

#include "postgres_fe.h"

#include "common/connect.h"
#include "fe_utils/string_utils.h"
#include "pg_upgrade.h"

/*
 * dbs_complete stores the number of databases that we have completed
 * processing.  When this value equals the number of databases in the cluster,
 * the task is finished.
 */
static int	dbs_complete;

/*
 * dbs_processing stores the index of the next database in the cluster's array
 * of databases that will be picked up for processing.  It will always be
 * greater than or equal to dbs_complete.
 */
static int	dbs_processing;

/*
 * This struct stores the information for a single step of a task.  Note that
 * the query string is stored in the "queries" PQExpBuffer for the UpgradeTask.
 * All steps in a task are run in a single connection before moving on to the
 * next database (which requires a new connection).
 */
typedef struct UpgradeTaskStep
{
	UpgradeTaskProcessCB process_cb;	/* processes the results of the query */
	bool		free_result;	/* should we free the result? */
	void	   *arg;			/* pointer passed to process_cb */
} UpgradeTaskStep;

/*
 * This struct is a thin wrapper around an array of steps, i.e.,
 * UpgradeTaskStep, plus a PQExpBuffer for all the query strings.
 */
struct UpgradeTask
{
	UpgradeTaskStep *steps;
	int			num_steps;
	PQExpBuffer queries;
};

/*
 * The different states for a parallel slot.
 */
typedef enum UpgradeTaskSlotState
{
	FREE,						/* slot available for use in a new database */
	CONNECTING,					/* waiting for connection to be established */
	RUNNING_QUERIES,			/* running/processing queries in the task */
} UpgradeTaskSlotState;

/*
 * We maintain an array of user_opts.jobs slots to execute the task.
 */
typedef struct UpgradeTaskSlot
{
	UpgradeTaskSlotState state; /* state of the slot */
	int			db_idx;			/* index of the database assigned to slot */
	int			step_idx;		/* index of the current step of task */
	PGconn	   *conn;			/* current connection managed by slot */
	bool		ready;			/* slot is ready for processing */
	bool		select_mode;	/* select() mode: true->read, false->write */
	int			sock;			/* file descriptor for connection's socket */
} UpgradeTaskSlot;

/*
 * Initializes an UpgradeTask.
 */
UpgradeTask *
upgrade_task_create(void)
{
	UpgradeTask *task = pg_malloc0(sizeof(UpgradeTask));

	task->queries = createPQExpBuffer();

	/* All tasks must first set a secure search_path. */
	upgrade_task_add_step(task, ALWAYS_SECURE_SEARCH_PATH_SQL, NULL, true, NULL);

	return task;
}

/*
 * Frees all storage associated with an UpgradeTask.
 */
void
upgrade_task_free(UpgradeTask *task)
{
	destroyPQExpBuffer(task->queries);
	pg_free(task->steps);
	pg_free(task);
}

/*
 * Adds a step to an UpgradeTask.  The steps will be executed in each database
 * in the order in which they are added.
 *
 *	task: task object that must have been initialized via upgrade_task_create()
 *	query: the query text
 *	process_cb: function that processes the results of the query
 *	free_result: should we free the PGresult, or leave it to the caller?
 *	arg: pointer to task-specific data that is passed to each callback
 */
void
upgrade_task_add_step(UpgradeTask *task, const char *query,
					  UpgradeTaskProcessCB process_cb, bool free_result,
					  void *arg)
{
	UpgradeTaskStep *new_step;

	task->steps = pg_realloc(task->steps,
							 ++task->num_steps * sizeof(UpgradeTaskStep));

	new_step = &task->steps[task->num_steps - 1];
	new_step->process_cb = process_cb;
	new_step->free_result = free_result;
	new_step->arg = arg;

	appendPQExpBuffer(task->queries, "%s;", query);
}

/*
 * Build a connection string for the slot's current database and asynchronously
 * start a new connection, but do not wait for the connection to be
 * established.
 */
static void
start_conn(const ClusterInfo *cluster, UpgradeTaskSlot *slot)
{
	PQExpBufferData conn_opts;
	DbInfo	   *dbinfo = &cluster->dbarr.dbs[slot->db_idx];

	/* Build connection string with proper quoting */
	initPQExpBuffer(&conn_opts);
	appendPQExpBufferStr(&conn_opts, "dbname=");
	appendConnStrVal(&conn_opts, dbinfo->db_name);
	appendPQExpBufferStr(&conn_opts, " user=");
	appendConnStrVal(&conn_opts, os_info.user);
	appendPQExpBuffer(&conn_opts, " port=%d", cluster->port);
	if (cluster->sockdir)
	{
		appendPQExpBufferStr(&conn_opts, " host=");
		appendConnStrVal(&conn_opts, cluster->sockdir);
	}

	slot->conn = PQconnectStart(conn_opts.data);

	if (!slot->conn)
		pg_fatal("failed to create connection with connection string: \"%s\"",
				 conn_opts.data);

	termPQExpBuffer(&conn_opts);
}

/*
 * Run the process_cb callback function to process the result of a query, and
 * free the result if the caller indicated we should do so.
 */
static void
process_query_result(const ClusterInfo *cluster, UpgradeTaskSlot *slot,
					 const UpgradeTask *task)
{
	UpgradeTaskStep *steps = &task->steps[slot->step_idx];
	UpgradeTaskProcessCB process_cb = steps->process_cb;
	DbInfo	   *dbinfo = &cluster->dbarr.dbs[slot->db_idx];
	PGresult   *res = PQgetResult(slot->conn);

	if (PQstatus(slot->conn) == CONNECTION_BAD ||
		(PQresultStatus(res) != PGRES_TUPLES_OK &&
		 PQresultStatus(res) != PGRES_COMMAND_OK))
		pg_fatal("connection failure: %s", PQerrorMessage(slot->conn));

	/*
	 * We assume that a NULL process_cb callback function means there's
	 * nothing to process.  This is primarily intended for the initial step in
	 * every task that sets a safe search_path.
	 */
	if (process_cb)
		(*process_cb) (dbinfo, res, steps->arg);

	if (steps->free_result)
		PQclear(res);
}

/*
 * Advances the state machine for a given slot as necessary.
 */
static void
process_slot(const ClusterInfo *cluster, UpgradeTaskSlot *slot, const UpgradeTask *task)
{
	PostgresPollingStatusType status;

	if (!slot->ready)
		return;

	switch (slot->state)
	{
		case FREE:

			/*
			 * If all of the databases in the cluster have been processed or
			 * are currently being processed by other slots, we are done.
			 */
			if (dbs_processing >= cluster->dbarr.ndbs)
				return;

			/*
			 * Claim the next database in the cluster's array and initiate a
			 * new connection.
			 */
			slot->db_idx = dbs_processing++;
			slot->state = CONNECTING;
			start_conn(cluster, slot);

			return;

		case CONNECTING:

			/* Check for connection failure. */
			status = PQconnectPoll(slot->conn);
			if (status == PGRES_POLLING_FAILED)
				pg_fatal("connection failure: %s", PQerrorMessage(slot->conn));

			/* Check whether the connection is still establishing. */
			if (status != PGRES_POLLING_OK)
			{
				slot->select_mode = (status == PGRES_POLLING_READING);
				return;
			}

			/*
			 * Move on to running/processing the queries in the task.
			 */
			slot->state = RUNNING_QUERIES;
			slot->select_mode = true;	/* wait until ready for reading */
			if (!PQsendQuery(slot->conn, task->queries->data))
				pg_fatal("connection failure: %s", PQerrorMessage(slot->conn));

			return;

		case RUNNING_QUERIES:

			/*
			 * Consume any available data and clear the read-ready indicator
			 * for the connection.
			 */
			if (!PQconsumeInput(slot->conn))
				pg_fatal("connection failure: %s", PQerrorMessage(slot->conn));

			/*
			 * Process any results that are ready so that we can free up this
			 * slot for another database as soon as possible.
			 */
			for (; slot->step_idx < task->num_steps; slot->step_idx++)
			{
				/* If no more results are available yet, move on. */
				if (PQisBusy(slot->conn))
					return;

				process_query_result(cluster, slot, task);
			}

			/*
			 * If we just finished processing the result of the last step in
			 * the task, free the slot.  We recursively call this function on
			 * the newly-freed slot so that we can start initiating the next
			 * connection immediately instead of waiting for the next loop
			 * through the slots.
			 */
			dbs_complete++;
			PQfinish(slot->conn);
			memset(slot, 0, sizeof(UpgradeTaskSlot));
			slot->ready = true;

			process_slot(cluster, slot, task);

			return;
	}
}

/*
 * Returns -1 on error, else the number of ready descriptors.
 */
static int
select_loop(int maxFd, fd_set *input, fd_set *output)
{
	fd_set		save_input = *input;
	fd_set		save_output = *output;

	if (maxFd == 0)
		return 0;

	for (;;)
	{
		int			i;

		*input = save_input;
		*output = save_output;

		i = select(maxFd + 1, input, output, NULL, NULL);

#ifndef WIN32
		if (i < 0 && errno == EINTR)
			continue;
#else
		if (i == SOCKET_ERROR && WSAGetLastError() == WSAEINTR)
			continue;
#endif
		return i;
	}
}

/*
 * Wait on the slots to either finish connecting or to receive query results if
 * possible.  This avoids a tight loop in upgrade_task_run().
 */
static void
wait_on_slots(UpgradeTaskSlot *slots, int numslots)
{
	fd_set		input;
	fd_set		output;
	int			maxFd = 0;

	FD_ZERO(&input);
	FD_ZERO(&output);

	for (int i = 0; i < numslots; i++)
	{
		/*
		 * We assume the previous call to process_slot() handled everything
		 * that was marked ready in the previous call to wait_on_slots(), if
		 * any.
		 */
		slots[i].ready = false;

		/*
		 * This function should only ever see free slots as we are finishing
		 * processing the last few databases, at which point we don't have any
		 * databases left for them to process.  We'll never use these slots
		 * again, so we can safely ignore them.
		 */
		if (slots[i].state == FREE)
			continue;

		/*
		 * Add the socket to the set.
		 */
		slots[i].sock = PQsocket(slots[i].conn);
		if (slots[i].sock < 0)
			pg_fatal("invalid socket");
		FD_SET(slots[i].sock, slots[i].select_mode ? &input : &output);
		maxFd = Max(maxFd, slots[i].sock);
	}

	/*
	 * If we found socket(s) to wait on, wait.
	 */
	if (select_loop(maxFd, &input, &output) == -1)
		pg_fatal("select() failed: %m");

	/*
	 * Mark which sockets appear to be ready.
	 */
	for (int i = 0; i < numslots; i++)
		slots[i].ready |= (FD_ISSET(slots[i].sock, &input) ||
						   FD_ISSET(slots[i].sock, &output));
}

/*
 * Runs all the steps of the task in every database in the cluster using
 * user_opts.jobs parallel slots.
 */
void
upgrade_task_run(const UpgradeTask *task, const ClusterInfo *cluster)
{
	int			jobs = Max(1, user_opts.jobs);
	UpgradeTaskSlot *slots = pg_malloc0(sizeof(UpgradeTaskSlot) * jobs);

	dbs_complete = 0;
	dbs_processing = 0;

	/*
	 * Process every slot the first time round.
	 */
	for (int i = 0; i < jobs; i++)
		slots[i].ready = true;

	while (dbs_complete < cluster->dbarr.ndbs)
	{
		for (int i = 0; i < jobs; i++)
			process_slot(cluster, &slots[i], task);

		wait_on_slots(slots, jobs);
	}

	pg_free(slots);
}

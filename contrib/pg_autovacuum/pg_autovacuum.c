/* pg_autovacuum.c
 * All the code for the pg_autovacuum program
 * (c) 2003 Matthew T. O'Connor
 * Revisions by Christopher B. Browne, Liberty RMS
 */

#include "pg_autovacuum.h"
#define TIMEBUFF 256
FILE	   *LOGOUTPUT;
char		timebuffer[TIMEBUFF];
char		logbuffer[4096];

void
log_entry(const char *logentry)
{
	time_t		curtime;
	struct tm  *loctime;

	curtime = time(NULL);
	loctime = localtime(&curtime);
	strftime(timebuffer, TIMEBUFF, "%Y-%m-%d %r", loctime);		/* cbb - %F is not
																 * always available */
	fprintf(LOGOUTPUT, "[%s] %s\n", timebuffer, logentry);
}

/* Function used to detatch the pg_autovacuum daemon from the tty and go into the background *
*	  This code is mostly ripped directly from pm_dameonize in postmaster.c				  *
*	  with unneeded code removed.														  */
void
daemonize()
{
	pid_t		pid;

	pid = fork();
	if (pid == (pid_t) -1)
	{
		log_entry("Error: cannot disassociate from controlling TTY");
		fflush(LOGOUTPUT);
		_exit(1);
	}
	else if (pid)
	{							/* parent */
		/* Parent should just exit, without doing any atexit cleanup */
		_exit(0);
	}

/* GH: If there's no setsid(), we hopefully don't need silent mode.
 * Until there's a better solution.  */
#ifdef HAVE_SETSID
	if (setsid() < 0)
	{
		log_entry("Error: cannot disassociate from controlling TTY");
		fflush(LOGOUTPUT);
		_exit(1);
	}
#endif

}

/* Create and return tbl_info struct with initialized to values from row or res */
tbl_info *
init_table_info(PGresult *res, int row, db_info * dbi)
{
	tbl_info   *new_tbl = (tbl_info *) malloc(sizeof(tbl_info));

	if (!new_tbl)
	{
		log_entry("init_table_info: Cannot get memory");
		fflush(LOGOUTPUT);
		return NULL;
	}

	if (res == NULL)
		return NULL;

	new_tbl->dbi = dbi;			/* set pointer to db */

	new_tbl->schema_name = (char *)
		malloc(strlen(PQgetvalue(res, row, PQfnumber(res, "schemaname"))) + 1);
	if (!new_tbl->schema_name)
	{
		log_entry("init_table_info: malloc failed on new_tbl->schema_name");
		fflush(LOGOUTPUT);
		return NULL;
	}
	strcpy(new_tbl->schema_name,
		   PQgetvalue(res, row, PQfnumber(res, "schemaname")));

	new_tbl->table_name = (char *)
		malloc(strlen(PQgetvalue(res, row, PQfnumber(res, "relname"))) +
			   strlen(new_tbl->schema_name) + 6);
	if (!new_tbl->table_name)
	{
		log_entry("init_table_info: malloc failed on new_tbl->table_name");
		fflush(LOGOUTPUT);
		return NULL;
	}

	/*
	 * Put both the schema and table name in quotes so that we can work
	 * with mixed case table names
	 */
	strcpy(new_tbl->table_name, "\"");
	strcat(new_tbl->table_name, new_tbl->schema_name);
	strcat(new_tbl->table_name, "\".\"");
	strcat(new_tbl->table_name, PQgetvalue(res, row, PQfnumber(res, "relname")));
	strcat(new_tbl->table_name, "\"");

	new_tbl->CountAtLastAnalyze =
		(atol(PQgetvalue(res, row, PQfnumber(res, "n_tup_ins"))) +
		 atol(PQgetvalue(res, row, PQfnumber(res, "n_tup_upd"))) +
		 atol(PQgetvalue(res, row, PQfnumber(res, "n_tup_del"))));
	new_tbl->curr_analyze_count = new_tbl->CountAtLastAnalyze;

	new_tbl->CountAtLastVacuum =
		(atol(PQgetvalue(res, row, PQfnumber(res, "n_tup_del"))) +
		 atol(PQgetvalue(res, row, PQfnumber(res, "n_tup_upd"))));
	new_tbl->curr_vacuum_count = new_tbl->CountAtLastVacuum;

	new_tbl->relid = atooid(PQgetvalue(res, row, PQfnumber(res, "oid")));
	new_tbl->reltuples = atof(PQgetvalue(res, row, PQfnumber(res, "reltuples")));
	new_tbl->relpages = atooid(PQgetvalue(res, row, PQfnumber(res, "relpages")));

	if (strcmp("t", PQgetvalue(res, row, PQfnumber(res, "relisshared"))))
		new_tbl->relisshared = 0;
	else
		new_tbl->relisshared = 1;

	new_tbl->analyze_threshold =
		args->analyze_base_threshold + args->analyze_scaling_factor * new_tbl->reltuples;
	new_tbl->vacuum_threshold =
		args->vacuum_base_threshold + args->vacuum_scaling_factor * new_tbl->reltuples;

	if (args->debug >= 2)
		print_table_info(new_tbl);

	return new_tbl;
}

/* Set thresholds = base_value + scaling_factor * reltuples
   Should be called after a vacuum since vacuum updates values in pg_class */
void
update_table_thresholds(db_info * dbi, tbl_info * tbl, int vacuum_type)
{
	PGresult   *res;
	int			disconnect = 0;
	char		query[128];

	if (dbi->conn == NULL)
	{
		dbi->conn = db_connect(dbi);
		disconnect = 1;
	}

	if (dbi->conn != NULL)
	{
		snprintf(query, sizeof(query), PAGES_QUERY, tbl->relid);
		res = send_query(query, dbi);
		if (res != NULL && PQntuples(res) > 0)
		{
			tbl->reltuples =
				atof(PQgetvalue(res, 0, PQfnumber(res, "reltuples")));
			tbl->relpages = atooid(PQgetvalue(res, 0, PQfnumber(res, "relpages")));

			/*
			 * update vacuum thresholds only of we just did a vacuum
			 * analyze
			 */
			if (vacuum_type == VACUUM_ANALYZE)
			{
				tbl->vacuum_threshold =
					(args->vacuum_base_threshold + args->vacuum_scaling_factor * tbl->reltuples);
				tbl->CountAtLastVacuum = tbl->curr_vacuum_count;
			}

			/* update analyze thresholds */
			tbl->analyze_threshold =
				(args->analyze_base_threshold + args->analyze_scaling_factor * tbl->reltuples);
			tbl->CountAtLastAnalyze = tbl->curr_analyze_count;

			/*
			 * If the stats collector is reporting fewer updates then we
			 * have on record then the stats were probably reset, so we
			 * need to reset also
			 */
			if ((tbl->curr_analyze_count < tbl->CountAtLastAnalyze) ||
				(tbl->curr_vacuum_count < tbl->CountAtLastVacuum))
			{
				tbl->CountAtLastAnalyze = tbl->curr_analyze_count;
				tbl->CountAtLastVacuum = tbl->curr_vacuum_count;
			}
		}

		PQclear(res);
	}
	if (disconnect)
		db_disconnect(dbi);
}

void
update_table_list(db_info * dbi)
{
	int			disconnect = 0;
	PGresult   *res = NULL;
	tbl_info   *tbl = NULL;
	Dlelem	   *tbl_elem = DLGetHead(dbi->table_list);
	int			i = 0,
				t = 0,
				found_match = 0;

	if (dbi->conn == NULL)
	{
		dbi->conn = db_connect(dbi);
		disconnect = 1;
	}

	if (dbi->conn != NULL)
	{
		/*
		 * Get a result set that has all the information we will need to
		 * both remove tables from the list that no longer exist and add
		 * tables to the list that are new
		 */
		res = send_query((char *) TABLE_STATS_QUERY, dbi);
		if (res != NULL)
		{
			t = PQntuples(res);
			
			/*
			* First: use the tbl_list as the outer loop and the result set as
			* the inner loop, this will determine what tables should be
			* removed
			*/
			while (tbl_elem != NULL)
			{
				tbl = ((tbl_info *) DLE_VAL(tbl_elem));
				found_match = 0;
				
				for (i = 0; i < t; i++)
				{					/* loop through result set looking for a
									* match */
					if (tbl->relid == atooid(PQgetvalue(res, i, PQfnumber(res, "oid"))))
					{
						found_match = 1;
						break;
					}
				}
				if (found_match == 0)
				{					/* then we didn't find this tbl_elem in
									* the result set */
					Dlelem	   *elem_to_remove = tbl_elem;
					
					tbl_elem = DLGetSucc(tbl_elem);
					remove_table_from_list(elem_to_remove);
				}
				else
					tbl_elem = DLGetSucc(tbl_elem);
			}						/* Done removing dropped tables from the
									* table_list */
			
			/*
			* Then loop use result set as outer loop and tbl_list as the
			* inner loop to determine what tables are new
			*/
			for (i = 0; i < t; i++)
			{
				tbl_elem = DLGetHead(dbi->table_list);
				found_match = 0;
				while (tbl_elem != NULL)
				{
					tbl = ((tbl_info *) DLE_VAL(tbl_elem));
					if (tbl->relid == atooid(PQgetvalue(res, i, PQfnumber(res, "oid"))))
					{
						found_match = 1;
						break;
					}
					tbl_elem = DLGetSucc(tbl_elem);
				}
				if (found_match == 0)		/* then we didn't find this result
											* now in the tbl_list */
				{
					DLAddTail(dbi->table_list, DLNewElem(init_table_info(res, i, dbi)));
					if (args->debug >= 1)
					{
						sprintf(logbuffer, "added table: %s.%s", dbi->dbname,
								((tbl_info *) DLE_VAL(DLGetTail(dbi->table_list)))->table_name);
						log_entry(logbuffer);
					}
				}
			}						/* end of for loop that adds tables */
		}
		fflush(LOGOUTPUT);
		PQclear(res);
		res = NULL;
		if (args->debug >= 3)
			print_table_list(dbi->table_list);
		if (disconnect)
			db_disconnect(dbi);
	}
}

/* Free memory, and remove the node from the list */
void
remove_table_from_list(Dlelem *tbl_to_remove)
{
	tbl_info   *tbl = ((tbl_info *) DLE_VAL(tbl_to_remove));

	if (args->debug >= 1)
	{
		sprintf(logbuffer, "Removing table: %s from list.", tbl->table_name);
		log_entry(logbuffer);
		fflush(LOGOUTPUT);
	}
	DLRemove(tbl_to_remove);

	if (tbl->schema_name)
	{
		free(tbl->schema_name);
		tbl->schema_name = NULL;
	}
	if (tbl->table_name)
	{
		free(tbl->table_name);
		tbl->table_name = NULL;
	}
	if (tbl)
	{
		free(tbl);
		tbl = NULL;
	}
	DLFreeElem(tbl_to_remove);
}

/* Free the entire table list */
void
free_tbl_list(Dllist *tbl_list)
{
	Dlelem	   *tbl_elem = DLGetHead(tbl_list);
	Dlelem	   *tbl_elem_to_remove = NULL;

	while (tbl_elem != NULL)
	{
		tbl_elem_to_remove = tbl_elem;
		tbl_elem = DLGetSucc(tbl_elem);
		remove_table_from_list(tbl_elem_to_remove);
	}
	DLFreeList(tbl_list);
}

void
print_table_list(Dllist *table_list)
{
	Dlelem	   *table_elem = DLGetHead(table_list);

	while (table_elem != NULL)
	{
		print_table_info(((tbl_info *) DLE_VAL(table_elem)));
		table_elem = DLGetSucc(table_elem);
	}
}

void
print_table_info(tbl_info * tbl)
{
	sprintf(logbuffer, "  table name: %s.%s", tbl->dbi->dbname, tbl->table_name);
	log_entry(logbuffer);
	sprintf(logbuffer, "     relid: %u;   relisshared: %i", tbl->relid, tbl->relisshared);
	log_entry(logbuffer);
	sprintf(logbuffer, "     reltuples: %f;  relpages: %u", tbl->reltuples, tbl->relpages);
	log_entry(logbuffer);
	sprintf(logbuffer, "     curr_analyze_count: %li; curr_vacuum_count: %li",
			tbl->curr_analyze_count, tbl->curr_vacuum_count);
	log_entry(logbuffer);
	sprintf(logbuffer, "     last_analyze_count: %li; last_vacuum_count: %li",
			tbl->CountAtLastAnalyze, tbl->CountAtLastVacuum);
	log_entry(logbuffer);
	sprintf(logbuffer, "     analyze_threshold: %li; vacuum_threshold: %li",
			tbl->analyze_threshold, tbl->vacuum_threshold);
	log_entry(logbuffer);
	fflush(LOGOUTPUT);
}

/* End of table Management Functions */

/* Beginning of DB Management Functions */

/* init_db_list() creates the db_list and initalizes template1 */
Dllist *
init_db_list()
{
	Dllist	   *db_list = DLNewList();
	db_info    *dbs = NULL;
	PGresult   *res = NULL;

	DLAddHead(db_list, DLNewElem(init_dbinfo((char *) "template1", 0, 0)));
	if (DLGetHead(db_list) == NULL)
	{							/* Make sure init_dbinfo was successful */
		log_entry("init_db_list(): Error creating db_list for db: template1.");
		fflush(LOGOUTPUT);
		return NULL;
	}

	/*
	 * We do this just so we can set the proper oid for the template1
	 * database
	 */
	dbs = ((db_info *) DLE_VAL(DLGetHead(db_list)));
	dbs->conn = db_connect(dbs);

	if (dbs->conn != NULL)
	{
		res = send_query(FROZENOID_QUERY, dbs);
		if (res != NULL)
		{
			dbs->oid = atooid(PQgetvalue(res, 0, PQfnumber(res, "oid")));
			dbs->age = atol(PQgetvalue(res, 0, PQfnumber(res, "age")));
			if (res)
				PQclear(res);
	
			if (args->debug >= 2)
				print_db_list(db_list, 0);
		}
		else
			return NULL;
	}
	return db_list;
}

/* Simple function to create an instance of the dbinfo struct
	Initalizes all the pointers and connects to the database  */
db_info *
init_dbinfo(char *dbname, Oid oid, long age)
{
	db_info    *newdbinfo = (db_info *) malloc(sizeof(db_info));

	newdbinfo->analyze_threshold = args->vacuum_base_threshold;
	newdbinfo->vacuum_threshold = args->analyze_base_threshold;
	newdbinfo->dbname = (char *) malloc(strlen(dbname) + 1);
	strcpy(newdbinfo->dbname, dbname);
	newdbinfo->username = NULL;
	if (args->user != NULL)
	{
		newdbinfo->username = (char *) malloc(strlen(args->user) + 1);
		strcpy(newdbinfo->username, args->user);
	}
	newdbinfo->password = NULL;
	if (args->password != NULL)
	{
		newdbinfo->password = (char *) malloc(strlen(args->password) + 1);
		strcpy(newdbinfo->password, args->password);
	}
	newdbinfo->oid = oid;
	newdbinfo->age = age;
	newdbinfo->table_list = DLNewList();
	newdbinfo->conn = NULL;

	if (args->debug >= 2)
		print_table_list(newdbinfo->table_list);

	return newdbinfo;
}

/* Function adds and removes databases from the db_list as appropriate */
void
update_db_list(Dllist *db_list)
{
	int			disconnect = 0;
	PGresult   *res = NULL;
	Dlelem	   *db_elem = DLGetHead(db_list);
	db_info    *dbi = NULL;
	db_info    *dbi_template1 = DLE_VAL(db_elem);
	int			i = 0,
				t = 0,
				found_match = 0;

	if (args->debug >= 2)
	{
		log_entry("updating the database list");
		fflush(LOGOUTPUT);
	}

	if (dbi_template1->conn == NULL)
	{
		dbi_template1->conn = db_connect(dbi_template1);
		disconnect = 1;
	}

	if (dbi_template1->conn != NULL)
	{
		/*
		 * Get a result set that has all the information we will need to
		 * both remove databasews from the list that no longer exist and
		 * add databases to the list that are new
		 */
		res = send_query(FROZENOID_QUERY2, dbi_template1);
		if (res != NULL)
		{
			t = PQntuples(res);
	
			/*
			* First: use the db_list as the outer loop and the result set as
			* the inner loop, this will determine what databases should be
			* removed
			*/
			while (db_elem != NULL)
			{
				dbi = ((db_info *) DLE_VAL(db_elem));
				found_match = 0;
	
				for (i = 0; i < t; i++)
				{					/* loop through result set looking for a
									* match */
					if (dbi->oid == atooid(PQgetvalue(res, i, PQfnumber(res, "oid"))))
					{
						found_match = 1;
	
						/*
						* update the dbi->age so that we ensure
						* xid_wraparound won't happen
						*/
						dbi->age = atol(PQgetvalue(res, i, PQfnumber(res, "age")));
						break;
					}
				}
				if (found_match == 0)
				{					/* then we didn't find this db_elem in the
									* result set */
					Dlelem	   *elem_to_remove = db_elem;
	
					db_elem = DLGetSucc(db_elem);
					remove_db_from_list(elem_to_remove);
				}
				else
					db_elem = DLGetSucc(db_elem);
			}						/* Done removing dropped databases from
									* the table_list */
	
			/*
			* Then loop use result set as outer loop and db_list as the inner
			* loop to determine what databases are new
			*/
			for (i = 0; i < t; i++)
			{
				db_elem = DLGetHead(db_list);
				found_match = 0;
				while (db_elem != NULL)
				{
					dbi = ((db_info *) DLE_VAL(db_elem));
					if (dbi->oid == atooid(PQgetvalue(res, i, PQfnumber(res, "oid"))))
					{
						found_match = 1;
						break;
					}
					db_elem = DLGetSucc(db_elem);
				}
				if (found_match == 0)		/* then we didn't find this result
											* now in the tbl_list */
				{
					DLAddTail(db_list, DLNewElem(init_dbinfo
							(PQgetvalue(res, i, PQfnumber(res, "datname")),
							atooid(PQgetvalue(res, i, PQfnumber(res, "oid"))),
						atol(PQgetvalue(res, i, PQfnumber(res, "age"))))));
					if (args->debug >= 1)
					{
						sprintf(logbuffer, "added database: %s", ((db_info *) DLE_VAL(DLGetTail(db_list)))->dbname);
						log_entry(logbuffer);
					}
				}
			}						/* end of for loop that adds tables */
		}
		fflush(LOGOUTPUT);
		PQclear(res);
		res = NULL;
		if (args->debug >= 3)
			print_db_list(db_list, 0);
		if (disconnect)
			db_disconnect(dbi_template1);
	}
}

/* xid_wraparound_check

From the docs:

With the standard freezing policy, the age column will start at one billion for a
freshly-vacuumed database. When the age approaches two billion, the database must
be vacuumed again to avoid risk of wraparound failures. Recommended practice is
to vacuum each database at least once every half-a-billion (500 million) transactions,
so as to provide plenty of safety margin.

So we do a full database vacuum if age > 1.5billion
return 0 if nothing happened,
return 1 if the database needed a database wide vacuum
*/
int
xid_wraparound_check(db_info * dbi)
{
	/*
	 * FIXME: should probably do something better here so that we don't
	 * vacuum all the databases on the server at the same time.  We have
	 * 500million xacts to work with so we should be able to spread the
	 * load of full database vacuums a bit
	 */
	if (dbi->age > 1500000000 )
	{
		PGresult   *res = NULL;

		res = send_query("VACUUM", dbi);
		/* FIXME: Perhaps should add a check for PQ_COMMAND_OK */
		if (res != NULL)
		{
			PQclear(res);
		}
		return 1;
	}
	return 0;
}

/* Close DB connection, free memory, and remove the node from the list */
void
remove_db_from_list(Dlelem *db_to_remove)
{
	db_info    *dbi = ((db_info *) DLE_VAL(db_to_remove));

	if (args->debug >= 1)
	{
		sprintf(logbuffer, "Removing db: %s from list.", dbi->dbname);
		log_entry(logbuffer);
		fflush(LOGOUTPUT);
	}
	DLRemove(db_to_remove);
	if (dbi->conn)
		db_disconnect(dbi);
	if (dbi->dbname)
	{
		free(dbi->dbname);
		dbi->dbname = NULL;
	}
	if (dbi->username)
	{
		free(dbi->username);
		dbi->username = NULL;
	}
	if (dbi->password)
	{
		free(dbi->password);
		dbi->password = NULL;
	}
	if (dbi->table_list)
	{
		free_tbl_list(dbi->table_list);
		dbi->table_list = NULL;
	}
	if (dbi)
	{
		free(dbi);
		dbi = NULL;
	}
	DLFreeElem(db_to_remove);
}

/* Function is called before program exit to free all memory
		mostly it's just to keep valgrind happy */
void
free_db_list(Dllist *db_list)
{
	Dlelem	   *db_elem = DLGetHead(db_list);
	Dlelem	   *db_elem_to_remove = NULL;

	while (db_elem != NULL)
	{
		db_elem_to_remove = db_elem;
		db_elem = DLGetSucc(db_elem);
		remove_db_from_list(db_elem_to_remove);
		db_elem_to_remove = NULL;
	}
	DLFreeList(db_list);
}

void
print_db_list(Dllist *db_list, int print_table_lists)
{
	Dlelem	   *db_elem = DLGetHead(db_list);

	while (db_elem != NULL)
	{
		print_db_info(((db_info *) DLE_VAL(db_elem)), print_table_lists);
		db_elem = DLGetSucc(db_elem);
	}
}

void
print_db_info(db_info * dbi, int print_tbl_list)
{
	sprintf(logbuffer, "dbname: %s", (dbi->dbname) ? dbi->dbname : "(null)");
	log_entry(logbuffer);
	
	sprintf(logbuffer, "  oid: %u", dbi->oid);
	log_entry(logbuffer);
	
	sprintf(logbuffer, "  username: %s", (dbi->username) ? dbi->username : "(null)");
	log_entry(logbuffer);
	
	sprintf(logbuffer, "  password: %s", (dbi->password) ? dbi->password : "(null)");
	log_entry(logbuffer);
	
	if (dbi->conn != NULL)
		log_entry("  conn is valid, (connected)");
	else
		log_entry("  conn is null, (not connected)");

	sprintf(logbuffer, "  default_analyze_threshold: %li", dbi->analyze_threshold);
	log_entry(logbuffer);
	
	sprintf(logbuffer, "  default_vacuum_threshold: %li", dbi->vacuum_threshold);
	log_entry(logbuffer);
	
	fflush(LOGOUTPUT);
	if (print_tbl_list > 0)
		print_table_list(dbi->table_list);
}

/* End of DB List Management Function */

/* Beginning of misc Functions */

/* Perhaps add some test to this function to make sure that the stats we need are available */
PGconn *
db_connect(db_info * dbi)
{
	PGconn	   *db_conn =
	PQsetdbLogin(args->host, args->port, NULL, NULL, dbi->dbname,
				 dbi->username, dbi->password);

	if (PQstatus(db_conn) != CONNECTION_OK)
	{
		sprintf(logbuffer, "Failed connection to database %s with error: %s.",
				dbi->dbname, PQerrorMessage(db_conn));
		log_entry(logbuffer);
		fflush(LOGOUTPUT);
		PQfinish(db_conn);
		db_conn = NULL;
	}
	return db_conn;
}	/* end of db_connect() */

void
db_disconnect(db_info * dbi)
{
	if (dbi->conn != NULL)
	{
		PQfinish(dbi->conn);
		dbi->conn = NULL;
	}
}

int
check_stats_enabled(db_info * dbi)
{
	PGresult   *res;
	int			ret = 0;

	res = send_query("SHOW stats_row_level", dbi);
	if (res != NULL)
	{
		ret = strcmp("on", PQgetvalue(res, 0, PQfnumber(res, "stats_row_level")));
		PQclear(res);
	}
	return ret;
}

PGresult *
send_query(const char *query, db_info * dbi)
{
	PGresult   *res;

	if (dbi->conn == NULL)
		return NULL;

	if (args->debug >= 4)
		log_entry(query);

	res = PQexec(dbi->conn, query);

	if (!res)
	{
		sprintf(logbuffer,
		   "Fatal error occured while sending query (%s) to database %s",
				query, dbi->dbname);
		log_entry(logbuffer);
		sprintf(logbuffer, "The error is [%s]", PQresultErrorMessage(res));
		log_entry(logbuffer);
		fflush(LOGOUTPUT);
		return NULL;
	}
	if (PQresultStatus(res) != PGRES_TUPLES_OK &&
		PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		sprintf(logbuffer,
		  "Can not refresh statistics information from the database %s.",
				dbi->dbname);
		log_entry(logbuffer);
		sprintf(logbuffer, "The error is [%s]", PQresultErrorMessage(res));
		log_entry(logbuffer);
		fflush(LOGOUTPUT);
		PQclear(res);
		return NULL;
	}
	return res;
}	/* End of send_query() */


void
free_cmd_args()
{
	if (args != NULL)
	{
		if (args->user != NULL)
			free(args->user);
		if (args->password != NULL)
			free(args->password);
		free(args);
	}
}

cmd_args *
get_cmd_args(int argc, char *argv[])
{
	int			c;

	args = (cmd_args *) malloc(sizeof(cmd_args));
	args->sleep_base_value = SLEEPBASEVALUE;
	args->sleep_scaling_factor = SLEEPSCALINGFACTOR;
	args->vacuum_base_threshold = VACBASETHRESHOLD;
	args->vacuum_scaling_factor = VACSCALINGFACTOR;
	args->analyze_base_threshold = -1;
	args->analyze_scaling_factor = -1;
	args->debug = AUTOVACUUM_DEBUG;
	args->daemonize = 0;
	args->user = 0;
	args->password = 0;
	args->host = 0;
	args->logfile = 0;
	args->port = 0;

	/*
	 * Fixme: Should add some sanity checking such as positive integer
	 * values etc
	 */
	while ((c = getopt(argc, argv, "s:S:v:V:a:A:d:U:P:H:L:p:hD")) != -1)
	{
		switch (c)
		{
			case 's':
				args->sleep_base_value = atoi(optarg);
				break;
			case 'S':
				args->sleep_scaling_factor = atof(optarg);
				break;
			case 'v':
				args->vacuum_base_threshold = atoi(optarg);
				break;
			case 'V':
				args->vacuum_scaling_factor = atof(optarg);
				break;
			case 'a':
				args->analyze_base_threshold = atoi(optarg);
				break;
			case 'A':
				args->analyze_scaling_factor = atof(optarg);
				break;
			case 'D':
				args->daemonize++;
				break;
			case 'd':
				args->debug = atoi(optarg);
				break;
			case 'U':
				args->user = optarg;
				break;
			case 'P':
				args->password = optarg;
				break;
			case 'H':
				args->host = optarg;
				break;
			case 'L':
				args->logfile = optarg;
				break;
			case 'p':
				args->port = optarg;
				break;
			case 'h':
				usage();
				exit(0);
			default:

				/*
				 * It's here that we know that things are invalid... It is
				 * not forcibly an error to call usage
				 */
				fprintf(stderr, "Error: Invalid Command Line Options.\n");
				usage();
				exit(1);
				break;
		}

		/*
		 * if values for insert thresholds are not specified, then they
		 * default to 1/2 of the delete values
		 */
		if (args->analyze_base_threshold == -1)
			args->analyze_base_threshold = args->vacuum_base_threshold / 2;
		if (args->analyze_scaling_factor == -1)
			args->analyze_scaling_factor = args->vacuum_scaling_factor / 2;
	}
	return args;
}

void
usage()
{
	int			i = 0;
	float		f = 0;

	fprintf(stderr, "usage: pg_autovacuum \n");
	fprintf(stderr, "   [-D] Daemonize (Detach from tty and run in the background)\n");
	i = AUTOVACUUM_DEBUG;
	fprintf(stderr, "   [-d] debug (debug level=0,1,2,3; default=%i)\n", i);

	i = SLEEPBASEVALUE;
	fprintf(stderr, "   [-s] sleep base value (default=%i)\n", i);
	f = SLEEPSCALINGFACTOR;
	fprintf(stderr, "   [-S] sleep scaling factor (default=%f)\n", f);

	i = VACBASETHRESHOLD;
	fprintf(stderr, "   [-v] vacuum base threshold (default=%i)\n", i);
	f = VACSCALINGFACTOR;
	fprintf(stderr, "   [-V] vacuum scaling factor (default=%f)\n", f);
	i = i / 2;
	fprintf(stderr, "   [-a] analyze base threshold (default=%i)\n", i);
	f = f / 2;
	fprintf(stderr, "   [-A] analyze scaling factor (default=%f)\n", f);

	fprintf(stderr, "   [-L] logfile (default=none)\n");

	fprintf(stderr, "   [-U] username (libpq default)\n");
	fprintf(stderr, "   [-P] password (libpq default)\n");
	fprintf(stderr, "   [-H] host (libpq default)\n");
	fprintf(stderr, "   [-p] port (libpq default)\n");

	fprintf(stderr, "   [-h] help (Show this output)\n");
}

void
print_cmd_args()
{
	sprintf(logbuffer, "Printing command_args");
	log_entry(logbuffer);
	sprintf(logbuffer, "  args->host=%s", (args->host) ? args->host : "(null)");
	log_entry(logbuffer);
	sprintf(logbuffer, "  args->port=%s", (args->port) ? args->port : "(null)");
	log_entry(logbuffer);
	sprintf(logbuffer, "  args->username=%s", (args->user) ? args->user : "(null)");
	log_entry(logbuffer);
	sprintf(logbuffer, "  args->password=%s", (args->password) ? args->password : "(null)");
	log_entry(logbuffer);
	sprintf(logbuffer, "  args->logfile=%s", (args->logfile) ? args->logfile : "(null)");
	log_entry(logbuffer);
	sprintf(logbuffer, "  args->daemonize=%i", args->daemonize);
	log_entry(logbuffer);

	sprintf(logbuffer, "  args->sleep_base_value=%i", args->sleep_base_value);
	log_entry(logbuffer);
	sprintf(logbuffer, "  args->sleep_scaling_factor=%f", args->sleep_scaling_factor);
	log_entry(logbuffer);
	sprintf(logbuffer, "  args->vacuum_base_threshold=%i", args->vacuum_base_threshold);
	log_entry(logbuffer);
	sprintf(logbuffer, "  args->vacuum_scaling_factor=%f", args->vacuum_scaling_factor);
	log_entry(logbuffer);
	sprintf(logbuffer, "  args->analyze_base_threshold=%i", args->analyze_base_threshold);
	log_entry(logbuffer);
	sprintf(logbuffer, "  args->analyze_scaling_factor=%f", args->analyze_scaling_factor);
	log_entry(logbuffer);
	sprintf(logbuffer, "  args->debug=%i", args->debug);
	log_entry(logbuffer);

	fflush(LOGOUTPUT);
}

/* Beginning of AutoVacuum Main Program */
int
main(int argc, char *argv[])
{
	char		buf[256];
	int			j = 0,
				loops = 0;

	/* int numInserts, numDeletes, */
	int			sleep_secs;
	Dllist	   *db_list;
	Dlelem	   *db_elem,
			   *tbl_elem;
	db_info    *dbs;
	tbl_info   *tbl;
	PGresult   *res = NULL;
	double		diff;
	struct timeval now,
				then;

	args = get_cmd_args(argc, argv);	/* Get Command Line Args and put
										 * them in the args struct */

	/* Dameonize if requested */
	if (args->daemonize == 1)
		daemonize();

	if (args->logfile)
	{
		LOGOUTPUT = fopen(args->logfile, "a");
		if (!LOGOUTPUT)
		{
			fprintf(stderr, "Could not open log file - [%s]\n", args->logfile);
			exit(-1);
		}
	}
	else
		LOGOUTPUT = stderr;
	if (args->debug >= 2)
		print_cmd_args();

	/* Init the db list with template1 */
	db_list = init_db_list();
	if (db_list == NULL)
		return 1;
	
	if (check_stats_enabled(((db_info *) DLE_VAL(DLGetHead(db_list)))) != 0)
	{
		log_entry("Error: GUC variable stats_row_level must be enabled.");
		log_entry("       Please fix the problems and try again.");
		fflush(LOGOUTPUT);

		exit(1);
	}

	gettimeofday(&then, 0);		/* for use later to caluculate sleep time */

	while (1)
	{							/* Main Loop */
		db_elem = DLGetHead(db_list);	/* Reset cur_db_node to the
										 * beginning of the db_list */

		dbs = ((db_info *) DLE_VAL(db_elem));	/* get pointer to cur_db's
												 * db_info struct */
		if (dbs->conn == NULL)
		{
			dbs->conn = db_connect(dbs);
			if (dbs->conn == NULL)
			{					/* Serious problem: We can't connect to
								 * template1 */
				log_entry("Error: Cannot connect to template1, exiting.");
				fflush(LOGOUTPUT);
				fclose(LOGOUTPUT);
				exit(1);
			}
		}

		if (loops % UPDATE_INTERVAL == 0)		/* Update the list if it's
												 * time */
			update_db_list(db_list);	/* Add and remove databases from
										 * the list */

		while (db_elem != NULL)
		{						/* Loop through databases in list */
			dbs = ((db_info *) DLE_VAL(db_elem));		/* get pointer to
														 * cur_db's db_info
														 * struct */
			if (dbs->conn == NULL)
				dbs->conn = db_connect(dbs);

			if (dbs->conn != NULL)
			{
				if (loops % UPDATE_INTERVAL == 0)		/* Update the list if
														 * it's time */
					update_table_list(dbs);		/* Add and remove tables
												 * from the list */

				if (xid_wraparound_check(dbs) == 0)
				{
					res = send_query(TABLE_STATS_QUERY, dbs);	/* Get an updated
																 * snapshot of this dbs
																 * table stats */
					if (res != NULL)
					{
						for (j = 0; j < PQntuples(res); j++)
						{			/* loop through result set */
							tbl_elem = DLGetHead(dbs->table_list);	/* Reset tbl_elem to top
																	* of dbs->table_list */
							while (tbl_elem != NULL)
							{		/* Loop through tables in list */
								tbl = ((tbl_info *) DLE_VAL(tbl_elem));		/* set tbl_info =
																			* current_table */
								if (tbl->relid == atooid(PQgetvalue(res, j, PQfnumber(res, "oid"))))
								{
									tbl->curr_analyze_count =
										(atol(PQgetvalue(res, j, PQfnumber(res, "n_tup_ins"))) +
										atol(PQgetvalue(res, j, PQfnumber(res, "n_tup_upd"))) +
										atol(PQgetvalue(res, j, PQfnumber(res, "n_tup_del"))));
									tbl->curr_vacuum_count =
										(atol(PQgetvalue(res, j, PQfnumber(res, "n_tup_del"))) +
										atol(PQgetvalue(res, j, PQfnumber(res, "n_tup_upd"))));
	
									/*
									* Check numDeletes to see if we need to
									* vacuum, if so: Run vacuum analyze
									* (adding analyze is small so we might as
									* well) Update table thresholds and
									* related information if numDeletes is
									* not big enough for vacuum then check
									* numInserts for analyze
									*/
									if (tbl->curr_vacuum_count - tbl->CountAtLastVacuum >= tbl->vacuum_threshold)
									{
										/*
										* if relisshared = t and database !=
										* template1 then only do an analyze
										*/
										if (tbl->relisshared > 0 && strcmp("template1", dbs->dbname))
											snprintf(buf, sizeof(buf), "ANALYZE %s", tbl->table_name);
										else
											snprintf(buf, sizeof(buf), "VACUUM ANALYZE %s", tbl->table_name);
										if (args->debug >= 1)
										{
											sprintf(logbuffer, "Performing: %s", buf);
											log_entry(logbuffer);
											fflush(LOGOUTPUT);
										}
										send_query(buf, dbs);
										update_table_thresholds(dbs, tbl, VACUUM_ANALYZE);
										if (args->debug >= 2)
											print_table_info(tbl);
									}
									else if (tbl->curr_analyze_count - tbl->CountAtLastAnalyze >= tbl->analyze_threshold)
									{
										snprintf(buf, sizeof(buf), "ANALYZE %s", tbl->table_name);
										if (args->debug >= 1)
										{
											sprintf(logbuffer, "Performing: %s", buf);
											log_entry(logbuffer);
											fflush(LOGOUTPUT);
										}
										send_query(buf, dbs);
										update_table_thresholds(dbs, tbl, ANALYZE_ONLY);
										if (args->debug >= 2)
											print_table_info(tbl);
									}
									
									break;	/* once we have found a match, no
											* need to keep checking. */
								}
								
								/*
								* Advance the table pointers for the next
								* loop
								*/
								tbl_elem = DLGetSucc(tbl_elem);
								
							}		/* end for table while loop */
						}			/* end for j loop (tuples in PGresult) */
					}			/* end if (res != NULL) */
				}				/* close of if(xid_wraparound_check()) */
				/* Done working on this db, Clean up, then advance cur_db */
				PQclear(res);
				res = NULL;
				db_disconnect(dbs);
			}
			db_elem = DLGetSucc(db_elem);		/* move on to next DB
												 * regardless */
		}						/* end of db_list while loop */

		/* Figure out how long to sleep etc ... */
		gettimeofday(&now, 0);
		diff = (int) (now.tv_sec - then.tv_sec) * 1000000.0 + (int) (now.tv_usec - then.tv_usec);

		sleep_secs = args->sleep_base_value + args->sleep_scaling_factor * diff / 1000000.0;
		loops++;
		if (args->debug >= 2)
		{
			sprintf(logbuffer,
			 "%i All DBs checked in: %.0f usec, will sleep for %i secs.",
					loops, diff, sleep_secs);
			log_entry(logbuffer);
			fflush(LOGOUTPUT);
		}

		sleep(sleep_secs);		/* Larger Pause between outer loops */

		gettimeofday(&then, 0); /* Reset time counter */

	}							/* end of while loop */

	/*
	 * program is exiting, this should never run, but is here to make
	 * compiler / valgrind happy
	 */
	free_db_list(db_list);
	free_cmd_args();
	return EXIT_SUCCESS;
}

/* pg_autovacuum.c
 * All the code for the pg_autovacuum program
 * (c) 2003 Matthew T. O'Connor
 */

#include "pg_autovacuum.h"

/* Create and return tbl_info struct with initalized to values from row or res */
tbl_info *init_table_info(PGresult *res, int row)
{
  tbl_info *new_tbl=(tbl_info *)malloc(sizeof(tbl_info));

  if(!new_tbl)
  {
    fprintf(stderr,"init_table_info: Cannot get memory\n");
    return NULL;
  }

  if(NULL == res)
    return NULL;

  new_tbl->schema_name=(char *)malloc(strlen(PQgetvalue(res,row,PQfnumber(res,"schemaname")))+1);
  if(!new_tbl->schema_name)
  {
    fprintf(stderr,"init_table_info: malloc failed on new_tbl->schema_name\n");
    return NULL;
  }
  strcpy(new_tbl->schema_name,PQgetvalue(res,row,PQfnumber(res,"schemaname")));

  new_tbl->table_name=(char *)malloc(strlen(PQgetvalue(res,row,PQfnumber(res,"relname"))) + strlen(new_tbl->schema_name)+2);
  if(!new_tbl->table_name)
  {
    fprintf(stderr,"init_table_info: malloc failed on new_tbl->table_name\n");
    return NULL;
  }
  strcpy(new_tbl->table_name,new_tbl->schema_name);
  strcat(new_tbl->table_name,".");
  strcat(new_tbl->table_name,PQgetvalue(res,row,PQfnumber(res,"relname")));

  new_tbl->InsertsAtLastAnalyze=(atol(PQgetvalue(res,row,PQfnumber(res,"n_tup_ins"))) + atol(PQgetvalue(res,row,PQfnumber(res,"n_tup_upd"))));
  new_tbl->DeletesAtLastVacuum =(atol(PQgetvalue(res,row,PQfnumber(res,"n_tup_del"))) + atol(PQgetvalue(res,row,PQfnumber(res,"n_tup_upd"))));

  new_tbl->relfilenode=atoi(PQgetvalue(res,row,PQfnumber(res,"relfilenode")));
  new_tbl->reltuples=atoi(PQgetvalue(res,row,PQfnumber(res,"reltuples")));
  new_tbl->relpages=atoi(PQgetvalue(res,row,PQfnumber(res,"relpages")));

  new_tbl->insertThreshold=args->tuple_base_threshold + args->tuple_scaling_factor*new_tbl->reltuples;
  new_tbl->deleteThreshold=args->tuple_base_threshold + args->tuple_scaling_factor*new_tbl->reltuples;

  if(args->debug >= 2) {print_table_info(new_tbl);}
  
  return new_tbl;
}

/* Set thresholds = base_value + scaling_factor * reltuples
   Should be called after a vacuum since vacuum updates valuesin pg_class */
void update_table_thresholds(db_info *dbi,tbl_info *tbl)
{
  PGresult *res=NULL;
  int disconnect=0;
	char query[128];

  if(NULL==dbi->conn)
  { dbi->conn=db_connect(dbi); disconnect=1;}

  if(NULL != dbi->conn)
  {
    snprintf(query,sizeof(query),"select relfilenode,reltuples,relpages from pg_class where relfilenode=%i",tbl->relfilenode);
    res=send_query(query,dbi);
    if(NULL!=res)
    {
    	tbl->reltuples = atoi(PQgetvalue(res,0,PQfnumber(res,"reltuples")));
    	tbl->relpages = atoi(PQgetvalue(res,0,PQfnumber(res,"relpages")));
      tbl->deleteThreshold = (args->tuple_base_threshold + args->tuple_scaling_factor*tbl->reltuples);
      tbl->insertThreshold = (0.5 * tbl->deleteThreshold);
      PQclear(res);
    }
	}
  if(disconnect)	db_disconnect(dbi);
}

void update_table_list(db_info *dbi)
{
	int disconnect=0;
	PGresult *res=NULL;
	tbl_info *tbl=NULL;
	Dlelem *tbl_elem=DLGetHead(dbi->table_list);
	int i=0,t=0,found_match=0;
	
  if(NULL==dbi->conn)
  {	dbi->conn=db_connect(dbi);   disconnect=1;}

  if(NULL != dbi->conn)
  {
    /* Get a result set that has all the information
    	 we will need to both remove tables from the list
       that no longer exist and add tables to the list
       that are new */
		res=send_query(query_table_stats(dbi),dbi);
		t=PQntuples(res);
   
		/* First: use the tbl_list as the outer loop and
				the result set as the inner loop, this will
				determine what tables should be removed */
    while(NULL != tbl_elem)
		{
			tbl=((tbl_info *)DLE_VAL(tbl_elem));
			found_match=0;
			
			for(i=0;i<t;i++) /* loop through result set looking for a match */
			{
				if(tbl->relfilenode==atoi(PQgetvalue(res,i,PQfnumber(res,"relfilenode"))))
				{
					found_match=1;
					break;
				}
			}
			if(0==found_match) /*then we didn't find this tbl_elem in the result set */
			{
				Dlelem *elem_to_remove=tbl_elem;
				tbl_elem=DLGetSucc(tbl_elem);
				remove_table_from_list(elem_to_remove);
			}
			else
				tbl_elem=DLGetSucc(tbl_elem);
		} /* Done removing dropped tables from the table_list */

		/* Then loop use result set as outer loop and
				tbl_list as the inner loop to determine
				what tables are new */
		for(i=0;i<t;i++)
		{
			tbl_elem=DLGetHead(dbi->table_list);
			found_match=0;
			while(NULL != tbl_elem)
			{
				tbl=((tbl_info *)DLE_VAL(tbl_elem));
				if(tbl->relfilenode==atoi(PQgetvalue(res,i,PQfnumber(res,"relfilenode"))))
				{
					found_match=1;
					break;
				}
				tbl_elem=DLGetSucc(tbl_elem);
			}
			if(0==found_match) /*then we didn't find this result now in the tbl_list */
			{
				DLAddTail(dbi->table_list,DLNewElem(init_table_info(res,i)));
				if(args->debug >= 1) {printf("added table: %s.%s\n",dbi->dbname,((tbl_info *)DLE_VAL(DLGetTail(dbi->table_list)))->table_name);}
			}
		} /* end of for loop that adds tables */
		PQclear(res); res=NULL;
		if(args->debug >= 3)	{print_table_list(dbi->table_list);}
		if(disconnect) db_disconnect(dbi);
	}
}

/* Free memory, and remove the node from the list */
void remove_table_from_list(Dlelem *tbl_to_remove)
{
  tbl_info *tbl=((tbl_info *)DLE_VAL(tbl_to_remove));

  if(args->debug >= 1) {printf("Removing table: %s from list.\n",tbl->table_name);}
  DLRemove(tbl_to_remove);

	if(tbl->schema_name)
	{	free(tbl->schema_name); tbl->schema_name=NULL;}
	if(tbl->table_name)
	{	free(tbl->table_name); tbl->table_name=NULL;}
	if(tbl)
	{	free(tbl); tbl=NULL;}
  DLFreeElem(tbl_to_remove);
}

/* Free the entire table list */
void free_tbl_list(Dllist *tbl_list)
{
	Dlelem *tbl_elem=DLGetHead(tbl_list);
	Dlelem *tbl_elem_to_remove=NULL;
	while(NULL != tbl_elem)
	{
		tbl_elem_to_remove=tbl_elem;
		tbl_elem=DLGetSucc(tbl_elem);
		remove_table_from_list(tbl_elem_to_remove);
	}
	DLFreeList(tbl_list);
}

void print_table_list(Dllist *table_list)
{
  Dlelem *table_elem=DLGetHead(table_list);
  while (NULL != table_elem)
  {
    print_table_info(((tbl_info *)DLE_VAL(table_elem)));
    table_elem=DLGetSucc(table_elem);
  }
}

void print_table_info(tbl_info *tbl)
{
  printf("  table name:     %s\n",tbl->table_name);
  printf("     iThresh: %i; Delete Thresh %i\n",tbl->insertThreshold,tbl->deleteThreshold);
  printf("     relfilenode: %i; reltuples: %i;  relpages: %i\n",tbl->relfilenode,tbl->reltuples,tbl->relpages);
  printf("     InsertsAtLastAnalyze: %li; DeletesAtLastVacuum: %li\n",tbl->InsertsAtLastAnalyze,tbl->DeletesAtLastVacuum);
}

/* End of table Management Functions */

/* Beginning of DB Management Functions */

/* init_db_list() creates the db_list and initalizes template1 */
Dllist *init_db_list()
{
  Dllist   *db_list=DLNewList();
  db_info  *dbs=NULL;
  PGresult *res=NULL;

  DLAddHead(db_list,DLNewElem(init_dbinfo((char *)"template1",0,0)));
  if(NULL == DLGetHead(db_list)) /* Make sure init_dbinfo was successful */
  { printf("init_db_list(): Error creating db_list for db: template1.\n"); return NULL; }

  /* We do this just so we can set the proper oid for the template1 database */
  dbs = ((db_info *)DLE_VAL(DLGetHead(db_list)));
  dbs->conn=db_connect(dbs);

  if(NULL != dbs->conn)
  {
    res=send_query("select oid,age(datfrozenxid) from pg_database where datname = 'template1'",dbs);
    dbs->oid=atoi(PQgetvalue(res,0,PQfnumber(res,"oid")));
    dbs->age=atoi(PQgetvalue(res,0,PQfnumber(res,"age")));
    if(res)
      PQclear(res);

    if(args->debug >= 2) {print_db_list(db_list,0);}
	}
  return db_list;
}

/* Simple function to create an instance of the dbinfo struct
    Initalizes all the pointers and connects to the database  */
db_info *init_dbinfo(char *dbname, int oid, int age)
{
  db_info *newdbinfo=(db_info *)malloc(sizeof(db_info));
  newdbinfo->insertThreshold=args->tuple_base_threshold;
  newdbinfo->deleteThreshold=args->tuple_base_threshold;
  newdbinfo->dbname=(char *)malloc(strlen(dbname)+1);
  strcpy(newdbinfo->dbname,dbname);
  newdbinfo->username=NULL;
  if(NULL != args->user)
  {
  	newdbinfo->username=(char *)malloc(strlen(args->user)+1);
  	strcpy(newdbinfo->username,args->user);
  }
  newdbinfo->password=NULL;
  if(NULL != args->password)
  {
  	newdbinfo->password=(char *)malloc(strlen(args->password)+1);
  	strcpy(newdbinfo->password,args->password);
  }
  newdbinfo->oid=oid;
  newdbinfo->age=age;
  newdbinfo->table_list=DLNewList();
	newdbinfo->conn=NULL;
	
  if(args->debug >= 2) {print_table_list(newdbinfo->table_list);}

  return newdbinfo;
}

/* Function adds and removes databases from the db_list as appropriate */
void update_db_list(Dllist *db_list)
{
	int disconnect=0;
	PGresult *res=NULL;
	Dlelem *db_elem=DLGetHead(db_list);
	db_info *dbi=NULL;
	db_info *dbi_template1=DLE_VAL(db_elem);
	int i=0,t=0,found_match=0;

  if(args->debug >= 2) {printf("updating the database list\n");}

  if(NULL==dbi_template1->conn)
  {	dbi_template1->conn=db_connect(dbi_template1);   disconnect=1;}

  if(NULL != dbi_template1->conn)
  {
    /* 	Get a resu22lt set that has all the information
		we will need to both remove databasews from the list
		that no longer exist and add databases to the list
		that are new */
		res=send_query("select oid,datname,age(datfrozenxid) from pg_database where datname!='template0'",dbi_template1);
		t=PQntuples(res);

		/* First: use the db_list as the outer loop and
				the result set as the inner loop, this will
				determine what databases should be removed */
		while(NULL != db_elem)
		{
			dbi=((db_info *)DLE_VAL(db_elem));
			found_match=0;

			for(i=0;i<t;i++) /* loop through result set looking for a match */
			{
				if(dbi->oid==atoi(PQgetvalue(res,i,PQfnumber(res,"oid"))))
				{
					found_match=1;
					/* update the dbi->age so that we ensure xid_wraparound won't happen */
					dbi->age=atoi(PQgetvalue(res,i,PQfnumber(res,"age")));
					break;
				}
			}
			if(0==found_match) /*then we didn't find this db_elem in the result set */
			{
				Dlelem *elem_to_remove=db_elem;
				db_elem=DLGetSucc(db_elem);
				remove_db_from_list(elem_to_remove);
			}
			else
				db_elem=DLGetSucc(db_elem);
		} /* Done removing dropped databases from the table_list */

		/* Then loop use result set as outer loop and
				db_list as the inner loop to determine
				what databases are new */
		for(i=0;i<t;i++)
		{
			db_elem=DLGetHead(db_list);
			found_match=0;
			while(NULL != db_elem)
			{
				dbi=((db_info *)DLE_VAL(db_elem));
				if(dbi->oid==atoi(PQgetvalue(res,i,PQfnumber(res,"oid"))))
				{
					found_match=1;
					break;
				}
				db_elem=DLGetSucc(db_elem);
			}
			if(0==found_match) /*then we didn't find this result now in the tbl_list */
			{
				DLAddTail(db_list,DLNewElem(init_dbinfo(PQgetvalue(res,i,PQfnumber(res,"datname")),
									atoi(PQgetvalue(res,i,PQfnumber(res,"oid"))),atoi(PQgetvalue(res,i,PQfnumber(res,"age"))))));
				if(args->debug >= 1) {printf("added database: %s\n",((db_info *)DLE_VAL(DLGetTail(db_list)))->dbname);}
			}
		} /* end of for loop that adds tables */
		PQclear(res); res=NULL;
		if(args->debug >= 3)	{print_db_list(db_list,0);}
		if(disconnect) db_disconnect(dbi_template1);
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
int xid_wraparound_check(db_info *dbi)
{
  /* FIXME: should probably do something better here so that we don't vacuum all the
  databases on the server at the same time.  We have 500million xacts to work with so
  we should be able to spread the load of full database vacuums a bit */
	if(1500000000 < dbi->age)
	{
		PGresult *res=NULL;
		res=send_query("vacuum",dbi);
		/* FIXME: Perhaps should add a check for PQ_COMMAND_OK */
		PQclear(res);
		return 1;
	}
	return 0;
}

/* Close DB connection, free memory, and remove the node from the list */
void  remove_db_from_list(Dlelem *db_to_remove)
{
  db_info *dbi=((db_info *)DLE_VAL(db_to_remove));

	if(args->debug >= 1) {printf("Removing db: %s from list.\n",dbi->dbname);}
  DLRemove(db_to_remove);
  if(dbi->conn)
		db_disconnect(dbi);
  if(dbi->dbname)
	{	free(dbi->dbname); dbi->dbname=NULL;}
  if(dbi->username)
	{	free(dbi->username); dbi->username=NULL;}
  if(dbi->password)
	{	free(dbi->password); dbi->password=NULL;}
  if(dbi->table_list)
	{	free_tbl_list(dbi->table_list); dbi->table_list=NULL;}
  if(dbi)
	{	free(dbi); dbi=NULL;}
  DLFreeElem(db_to_remove);
}

/* Function is called before program exit to free all memory
		mostly it's just to keep valgrind happy */
void free_db_list(Dllist *db_list)
{
	Dlelem *db_elem=DLGetHead(db_list);
	Dlelem *db_elem_to_remove=NULL;
	while(NULL != db_elem)
	{
    db_elem_to_remove=db_elem;
		db_elem=DLGetSucc(db_elem);
		remove_db_from_list(db_elem_to_remove);
		db_elem_to_remove=NULL;
	}
	DLFreeList(db_list);
}

void print_db_list(Dllist *db_list, int print_table_lists)
{
  Dlelem *db_elem=DLGetHead(db_list);
  while(NULL != db_elem)
  {
    print_db_info(((db_info *)DLE_VAL(db_elem)),print_table_lists);
    db_elem=DLGetSucc(db_elem);
  }
}

void print_db_info(db_info *dbi, int print_tbl_list)
{
  printf("dbname: %s\n Username %s\n Passwd %s\n",dbi->dbname,dbi->username,dbi->password);
  printf(" oid %i\n InsertThresh: %i\n DeleteThresh: %i\n",dbi->oid,dbi->insertThreshold,dbi->deleteThreshold);
  if(NULL!=dbi->conn)
    printf(" conn is valid, we are connected\n");
  else
    printf(" conn is null, we are not connected.\n");

  if(0 < print_tbl_list)
    print_table_list(dbi->table_list);
}

/* End of DB List Management Function */

/* Begninning of misc Functions */


char *query_table_stats(db_info *dbi)
{
  if(!strcmp(dbi->dbname,"template1")) /* Use template1 to monitor the system tables */
    return (char*)TABLE_STATS_ALL;
  else
    return (char*)TABLE_STATS_USER;
}

/* Perhaps add some test to this function to make sure that the stats we need are availalble */
PGconn *db_connect(db_info *dbi)
{
  PGconn *db_conn=PQsetdbLogin(args->host, args->port, NULL, NULL, dbi->dbname, dbi->username, dbi->password);
  
  if(CONNECTION_OK != PQstatus(db_conn))
  {
    fprintf(stderr,"Failed connection to database %s with error: %s.\n",dbi->dbname,PQerrorMessage(db_conn));
    PQfinish(db_conn);
    db_conn=NULL;
  }
  return db_conn;
} /* end of db_connect() */

void db_disconnect(db_info *dbi)
{
  if(NULL != dbi->conn)
  {
    PQfinish(dbi->conn);
    dbi->conn=NULL;
  }
}

int	check_stats_enabled(db_info *dbi)
{
	PGresult *res=NULL;
	int ret=0;
	res=send_query("show stats_row_level",dbi);
	ret = strcmp("on",PQgetvalue(res,0,PQfnumber(res,"stats_row_level")));
	PQclear(res);
	return ret;
}

PGresult *send_query(const char *query,db_info *dbi)
{
  PGresult *res;

  if(NULL==dbi->conn)
    return NULL;

  res=PQexec(dbi->conn,query);

  if(!res)
  {
    fprintf(stderr,"Fatal error occured while sending query (%s) to database %s\n",query,dbi->dbname);
    fprintf(stderr,"The error is \n%s\n",PQresultErrorMessage(res));
    return NULL;
  }
  if(PQresultStatus(res)!=PGRES_TUPLES_OK && PQresultStatus(res)!=PGRES_COMMAND_OK)
  {
    fprintf(stderr,"Can not refresh statistics information from the database %s.\n",dbi->dbname);
    fprintf(stderr,"The error is \n%s\n",PQresultErrorMessage(res));
    PQclear(res);
    return NULL;
  }
  return res;
} /* End of send_query() */


void free_cmd_args()
{
  if(NULL!=args)
  {
    if(NULL!=args->user)
      free(args->user);
    if(NULL!=args->user)
      free(args->password);
  free(args);
  }
}

cmd_args *get_cmd_args(int argc,char *argv[])
{
  int c;

  args=(cmd_args *)malloc(sizeof(cmd_args));
  args->sleep_base_value=SLEEPVALUE;
  args->sleep_scaling_factor=SLEEPSCALINGFACTOR;
  args->tuple_base_threshold=BASETHRESHOLD;
  args->tuple_scaling_factor=SCALINGFACTOR;
  args->debug=AUTOVACUUM_DEBUG;
  args->user=NULL;
  args->password=NULL;
  args->host=NULL;
  args->port=NULL;
  while (-1 != (c = getopt(argc, argv, "s:S:t:T:d:U:P:H:p:h")))
	{
		switch (c)
		{
			case 's':
				args->sleep_base_value=atoi(optarg);
				break;
			case 'S':
				args->sleep_scaling_factor = atof(optarg);
				break;
			case 't':
				args->tuple_base_threshold = atoi(optarg);
				break;
			case 'T':
				args->tuple_scaling_factor = atof(optarg);
				break;
			case 'd':
				args->debug = atoi(optarg);
				break;
			case 'U':
				args->user=optarg;
				break;
			case 'P':
				args->password=optarg;
				break;
			case 'H':
				args->host=optarg;
				break;
			case 'p':
				args->port=optarg;
				break;
			case 'h':
			default:
				fprintf(stderr, "usage: pg_autovacuum [-d debug][-s sleep base value][-S sleep scaling factor]\n[-t tuple base threshold][-T tulple scaling factor]\n[-U username][-P password][-H host][-p port][-h help]\n");
				exit(1);
				break;
		}
  }

  return args;
}

void print_cmd_args()
{
	printf("Printing command_args\n");
	printf("  args->host=%s\n",args->host);
	printf("  args->port=%s\n",args->port);
	printf("	args->user=%s\n",args->user);
	printf("	args->password=%s\n",args->password);
	printf("	args->sleep_base_value=%i\n",args->sleep_base_value);
	printf("	args->sleep_scaling_factor=%f\n",args->sleep_scaling_factor);
	printf("	args->tuple_base_threshold=%i\n",args->tuple_base_threshold);
	printf("	args->tuple_scaling_factor=%f\n",args->tuple_scaling_factor);
	printf("	args->debug=%i\n",args->debug);
}

/* Beginning of AutoVacuum Main Program */
int main(int argc, char *argv[])
{
  char buf[256];
  int j=0, loops=0;
  int numInserts, numDeletes, sleep_secs;
  Dllist    *db_list;
  Dlelem    *db_elem,*tbl_elem;
  db_info   *dbs;
  tbl_info  *tbl;
  PGresult  *res;
  long long       diff=0;
  struct timeval now,then;

  args=get_cmd_args(argc,argv); /* Get Command Line Args and put them in the args struct */

  if(args->debug >= 2)	{print_cmd_args();}

  db_list=init_db_list();   /* Init the db list with template1 */
  if(NULL == db_list)
    return 1;

	if(0!=check_stats_enabled(((db_info*)DLE_VAL(DLGetHead(db_list)))))
	{
		printf("Error: GUC variable stats_row_level must be enabled.\n       Please fix the problems and try again.\n");
		exit(1);
	}

	gettimeofday(&then, 0); /* for use later to caluculate sleep time */

	while(1)   /* Main Loop */
	{
		db_elem=DLGetHead(db_list); /* Reset cur_db_node to the beginning of the db_list */

		dbs=((db_info *)DLE_VAL(db_elem)); /* get pointer to cur_db's db_info struct */
		if(NULL==dbs->conn) 
		{
			dbs->conn=db_connect(dbs);
			if(NULL==dbs->conn) /* Serious problem: We can't connect to template1 */
			{
				printf("Error: Cannot connect to template1, exiting.\n");
				exit(1);
			}	
		}

		if(0==(loops % UPDATE_INTERVAL)) /* Update the list if it's time */
			update_db_list(db_list); /* Add new databases to the list to be checked, and remove databases that no longer exist */

		while(NULL != db_elem) /* Loop through databases in list */
		{
      dbs=((db_info *)DLE_VAL(db_elem)); /* get pointer to cur_db's db_info struct */
      if(NULL==dbs->conn)
        dbs->conn=db_connect(dbs);

      if(NULL!=dbs->conn)
      {
				if(0==(loops % UPDATE_INTERVAL)) /* Update the list if it's time */
					update_table_list(dbs); /* Add new databases to the list to be checked, and remove databases that no longer exist */

				if(0==xid_wraparound_check(dbs));
				{
          res=send_query(query_table_stats(dbs),dbs);  /* Get an updated snapshot of this dbs table stats */
          for(j=0;j < PQntuples(res);j++) /* loop through result set */
          {
            tbl_elem = DLGetHead(dbs->table_list); /* Reset tbl_elem to top of dbs->table_list */
            while(NULL!=tbl_elem)	/* Loop through tables in list */
            {
              tbl=((tbl_info *)DLE_VAL(tbl_elem)); /* set tbl_info = current_table */
              if(tbl->relfilenode == atoi(PQgetvalue(res,j,PQfnumber(res,"relfilenode"))))
              {
                numInserts=(atol(PQgetvalue(res,j,PQfnumber(res,"n_tup_ins"))) + atol(PQgetvalue(res,j,PQfnumber(res,"n_tup_upd"))));
                numDeletes=(atol(PQgetvalue(res,j,PQfnumber(res,"n_tup_del"))) + atol(PQgetvalue(res,j,PQfnumber(res,"n_tup_upd"))));

                /* Check numDeletes to see if we need to vacuum, if so:
                Run vacuum analyze (adding analyze is small so we might as well)
                Update table thresholds and related information
                if numDeletes is not big enough for vacuum then check numInserts for analyze */
                if((numDeletes - tbl->DeletesAtLastVacuum) >= tbl->deleteThreshold)
                {
                  snprintf(buf,sizeof(buf),"vacuum %s",tbl->table_name);
                  if(args->debug >= 1) {printf("Performing: %s\n",buf);}
                  send_query(buf,dbs);
                  tbl->DeletesAtLastVacuum=numDeletes;
    							update_table_thresholds(dbs,tbl);
                  if(args->debug >= 2)	{print_table_info(tbl);}
                }
                else if((numInserts - tbl->InsertsAtLastAnalyze) >= tbl->insertThreshold)
                {
                  snprintf(buf,sizeof(buf),"analyze %s",tbl->table_name);
                  if(args->debug >= 1) {printf("Performing: %s\n",buf);}
                  send_query(buf,dbs);
                  tbl->InsertsAtLastAnalyze=numInserts;
                  tbl->reltuples=atoi(PQgetvalue(res,j,PQfnumber(res,"reltuples")));
                  tbl->insertThreshold = (args->tuple_base_threshold + args->tuple_scaling_factor*tbl->reltuples);
                  if(args->debug >= 2)	{print_table_info(tbl);}
                }

                /* If the stats collector is reporting fewer updates then we have on record
                then the stats were probably reset, so we need to reset also */
                if((numInserts < tbl->InsertsAtLastAnalyze)||(numDeletes < tbl->DeletesAtLastVacuum))
                {
                  tbl->InsertsAtLastAnalyze=numInserts;
                  tbl->DeletesAtLastVacuum=numDeletes;
                }
                break; /* once we have found a match, no need to keep checking. */
              }
              /* Advance the table pointers for the next loop */
              tbl_elem=DLGetSucc(tbl_elem);

            } /* end for table while loop */
          } /* end for j loop (tuples in PGresult) */
				} /* close of if(xid_wraparound_check()) */
        /* Done working on this db, Clean up, then advance cur_db */
        PQclear(res);  res=NULL;
        db_disconnect(dbs);
			}
			db_elem=DLGetSucc(db_elem); /* move on to next DB regardless */
    } /* end of db_list while loop */

    /* Figure out how long to sleep etc ... */
    gettimeofday(&now, 0);
    diff = (now.tv_sec - then.tv_sec) * 1000000 + (now.tv_usec - then.tv_usec);

    sleep_secs = args->sleep_base_value + args->sleep_scaling_factor*diff/1000000;
    loops++;
    if(args->debug >= 2)
    {	printf("%i All DBs checked in: %lld usec, will sleep for %i secs.\n",loops,diff,sleep_secs);}

    sleep(sleep_secs); /* Larger Pause between outer loops */

    gettimeofday(&then, 0);	/* Reset time counter */

  } /* end of while loop */

  /* 	program is exiting, this should never run, but is here to make compiler / valgrind happy */
  free_db_list(db_list);
  free_cmd_args();
  return EXIT_SUCCESS;
}

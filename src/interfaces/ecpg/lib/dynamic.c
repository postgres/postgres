/* dynamic SQL support routines
 *
 * Copyright (c) 2000, Christof Petig <christof.petig@wtal.de>
 *
 * $Header: /cvsroot/pgsql/src/interfaces/ecpg/lib/Attic/dynamic.c,v 1.4 2000/02/18 16:02:49 meskes Exp $
 */

/* I borrowed the include files from ecpglib.c, maybe we don't need all of them */

#include <sql3types.h>

static struct descriptor
{
	char *name;
	PGresult *result;
	struct descriptor *next;
} *all_descriptors=NULL;

PGconn *ECPG_internal_get_connection(char *name);

unsigned int ECPGDynamicType(Oid type)
{
	switch(type)
	{	case 16:	return SQL3_BOOLEAN;	/* bool */
		case 21:	return SQL3_SMALLINT;	/* int2 */
		case 23:	return SQL3_INTEGER;	/* int4 */
		case 25:	return SQL3_CHARACTER;	/* text */
		case 700:	return SQL3_REAL;		/* float4 */
		case 701:	return SQL3_DOUBLE_PRECISION;	/* float8 */
		case 1042:	return SQL3_CHARACTER;	/* bpchar */
		case 1043:	return SQL3_CHARACTER_VARYING;	/* varchar */
		case 1082:	return SQL3_DATE_TIME_TIMESTAMP;	/* date */
		case 1083:	return SQL3_DATE_TIME_TIMESTAMP;	/* time */
		case 1184:	return SQL3_DATE_TIME_TIMESTAMP;	/* datetime */
		case 1296:	return SQL3_DATE_TIME_TIMESTAMP;	/* timestamp */
		case 1700:	return SQL3_NUMERIC;	/* numeric */
		default:
			return -type;
	}
}

unsigned int ECPGDynamicType_DDT(Oid type)
{	switch(type)
	{	
		case 1082:	return SQL3_DDT_DATE;	/* date */
		case 1083:	return SQL3_DDT_TIME;	/* time */
		case 1184:	return SQL3_DDT_TIMESTAMP_WITH_TIME_ZONE;	/* datetime */
		case 1296:	return SQL3_DDT_TIMESTAMP_WITH_TIME_ZONE;	/* timestamp */
		default:
			return SQL3_DDT_ILLEGAL;
	}
}

// like ECPGexecute
static bool execute_descriptor(int lineno,const char *query
							,struct connection *con,PGresult **resultptr)
{
	bool	status = false;
	PGresult   *results;
	PGnotify   *notify;
	
	/* Now the request is built. */

	if (con->committed && !con->autocommit)
	{
		if ((results = PQexec(con->connection, "begin transaction")) == NULL)
		{
			register_error(ECPG_TRANS, "Error in transaction processing line %d.", lineno);
			return false;
		}
		PQclear(results);
		con->committed = false;
	}

	ECPGlog("execute_descriptor line %d: QUERY: %s on connection %s\n", lineno, query, con->name);
	results = PQexec(con->connection, query);

	if (results == NULL)
	{
		ECPGlog("ECPGexecute line %d: error: %s", lineno,
				PQerrorMessage(con->connection));
		register_error(ECPG_PGSQL, "Postgres error: %s line %d.",
			 PQerrorMessage(con->connection), lineno);
	}
	else
	{	*resultptr=results;
		switch (PQresultStatus(results))
		{	int ntuples;
			case PGRES_TUPLES_OK:
				status = true;
				sqlca.sqlerrd[2] = ntuples = PQntuples(results);
				if (ntuples < 1)
				{
					ECPGlog("execute_descriptor line %d: Incorrect number of matches: %d\n",
							lineno, ntuples);
					register_error(ECPG_NOT_FOUND, "No data found line %d.", lineno);
					status = false;
					break;
				}
				break;
#if 1 /* strictly these are not needed (yet) */
			case PGRES_EMPTY_QUERY:
				/* do nothing */
				register_error(ECPG_EMPTY, "Empty query line %d.", lineno);
				break;
			case PGRES_COMMAND_OK:
				status = true;
				sqlca.sqlerrd[1] = atol(PQoidStatus(results));
				sqlca.sqlerrd[2] = atol(PQcmdTuples(results));
				ECPGlog("ECPGexecute line %d Ok: %s\n", lineno, PQcmdStatus(results));
				break;
			case PGRES_COPY_OUT:
				ECPGlog("ECPGexecute line %d: Got PGRES_COPY_OUT ... tossing.\n", lineno);
				PQendcopy(con->connection);
				break;
			case PGRES_COPY_IN:
				ECPGlog("ECPGexecute line %d: Got PGRES_COPY_IN ... tossing.\n", lineno);
				PQendcopy(con->connection);
				break;
#else
			case PGRES_EMPTY_QUERY:
			case PGRES_COMMAND_OK:
			case PGRES_COPY_OUT:
			case PGRES_COPY_IN:
				break;
#endif
			case PGRES_NONFATAL_ERROR:
			case PGRES_FATAL_ERROR:
			case PGRES_BAD_RESPONSE:
				ECPGlog("ECPGexecute line %d: Error: %s",
						lineno, PQerrorMessage(con->connection));
				register_error(ECPG_PGSQL, "Postgres error: %s line %d.",
							   PQerrorMessage(con->connection), lineno);
				status = false;
				break;
			default:
				ECPGlog("ECPGexecute line %d: Got something else, postgres error.\n",
						lineno);
				register_error(ECPG_PGSQL, "Postgres error: %s line %d.",
							   PQerrorMessage(con->connection), lineno);
				status = false;
				break;
		}
	}

	/* check for asynchronous returns */
	notify = PQnotifies(con->connection);
	if (notify)
	{
		ECPGlog("ECPGexecute line %d: ASYNC NOTIFY of '%s' from backend pid '%d' received\n",
				lineno, notify->relname, notify->be_pid);
		free(notify);
	}
	return status;
}

/* like ECPGdo */
static bool do_descriptor2(int lineno,const char *connection_name,
					PGresult **resultptr, const char *query)
{
	struct connection *con = get_connection(connection_name);
	bool		status=true;
	char *locale = setlocale(LC_NUMERIC, NULL);

	/* Make sure we do NOT honor the locale for numeric input/output */
	/* since the database wants teh standard decimal point */
	setlocale(LC_NUMERIC, "C");

	if (!ecpg_init(con, connection_name, lineno))
	{	setlocale(LC_NUMERIC, locale);
		return(false);
	}

	/* are we connected? */
	if (con == NULL || con->connection == NULL)
	{
		ECPGlog("ECPGdo: not connected to %s\n", con->name);
		register_error(ECPG_NOT_CONN, "Not connected in line %d.", lineno);
		setlocale(LC_NUMERIC, locale);
		return false;
	}

	status = execute_descriptor(lineno,query,con,resultptr);

	/* and reset locale value so our application is not affected */
	setlocale(LC_NUMERIC, locale);
	return (status);
}

bool ECPGdo_descriptor(int line,const char *connection,
							const char *descriptor,const char *query)
{
	struct descriptor *i;
	for (i=all_descriptors;i!=NULL;i=i->next)
	{	if (!strcmp(descriptor,i->name)) 
	    {	
			bool status;

			/* free previous result */
			if (i->result) PQclear(i->result);
	    	i->result=NULL;
	    	
			status=do_descriptor2(line,connection,&i->result,query);
			
			if (!i->result) PQmakeEmptyPGresult(NULL, 0);
			return (status);
	    }
	}
	ECPGraise(line, ECPG_UNKNOWN_DESCRIPTOR, NULL);
	return false;
}
							
PGresult *ECPGresultByDescriptor(int line,const char *name)
{
	struct descriptor *i;
	
	for (i = all_descriptors; i != NULL; i = i->next)
	{
		if (!strcmp(name, i->name)) return i->result;
	}
	
	ECPGraise(line, ECPG_UNKNOWN_DESCRIPTOR, NULL);
	
	return NULL;
} 


bool ECPGdeallocate_desc(int line,const char *name)
{
	struct descriptor *i;
	struct descriptor **lastptr=&all_descriptors;
	for (i=all_descriptors;i;lastptr=&i->next,i=i->next)
	{	if (!strcmp(name,i->name))
		{	*lastptr=i->next;
			free(i->name);
			PQclear(i->result);
			free(i);
			return true;
		}
	}
	ECPGraise(line, ECPG_UNKNOWN_DESCRIPTOR, NULL);
	return false;
} 

bool ECPGallocate_desc(int line,const char *name)
{
	struct descriptor *new=(struct descriptor *)malloc(sizeof(struct descriptor));
	
	new->next=all_descriptors;
	new->name=malloc(strlen(name)+1);
	new->result=PQmakeEmptyPGresult(NULL, 0);
	strcpy(new->name,name);
	all_descriptors=new;
	return true;
}

void
ECPGraise(int line, int code, const char *str)
{
	struct auto_mem *am;
	       
	sqlca.sqlcode=code;
	switch (code)
	{ 
		case ECPG_NOT_FOUND: 
			snprintf(sqlca.sqlerrm.sqlerrmc,sizeof(sqlca.sqlerrm.sqlerrmc),
				"No data found line %d.", line);
			break;
			
		case ECPG_OUT_OF_MEMORY: 
			snprintf(sqlca.sqlerrm.sqlerrmc,sizeof(sqlca.sqlerrm.sqlerrmc),
				"Out of memory in line %d.", line);
			break;
			
		case ECPG_UNSUPPORTED: 
			snprintf(sqlca.sqlerrm.sqlerrmc,sizeof(sqlca.sqlerrm.sqlerrmc),
				"Unsupported type %s in line %d.", str, line);
			break;
			
		case ECPG_TOO_MANY_ARGUMENTS: 
			snprintf(sqlca.sqlerrm.sqlerrmc,sizeof(sqlca.sqlerrm.sqlerrmc),
				"Too many arguments in line %d.", line);
			break;
		
		case ECPG_TOO_FEW_ARGUMENTS: 
			snprintf(sqlca.sqlerrm.sqlerrmc,sizeof(sqlca.sqlerrm.sqlerrmc),
				"Too few arguments in line %d.", line);
			break;
			
		case ECPG_MISSING_INDICATOR: 
			snprintf(sqlca.sqlerrm.sqlerrmc,sizeof(sqlca.sqlerrm.sqlerrmc),
				"NULL value without indicator, line %d.", line);
			break;
			
		case ECPG_UNKNOWN_DESCRIPTOR: 
			snprintf(sqlca.sqlerrm.sqlerrmc,sizeof(sqlca.sqlerrm.sqlerrmc),
				"descriptor not found, line %d.", line);
			break;
			
		case ECPG_INVALID_DESCRIPTOR_INDEX: 
			snprintf(sqlca.sqlerrm.sqlerrmc,sizeof(sqlca.sqlerrm.sqlerrmc),
				"descriptor index out of range, line %d.", line);
			break;
			
	    default:
		    	snprintf(sqlca.sqlerrm.sqlerrmc,sizeof(sqlca.sqlerrm.sqlerrmc),
				"SQL error #%d, line %d.",code, line);
			break;
	}
	
        /* free all memory we have allocated for the user */
        for (am = auto_allocs; am;)
        {
        	struct auto_mem *act = am;
	        
	        am = am->next;
	        free(act->pointer);
	        free(act);
	}

        auto_allocs = NULL;
}

/* Copyright comment */
/*
 * The aim is to get a simpler inteface to the database routines.
 * All the tidieous messing around with tuples is supposed to be hidden
 * by this function.
 */
/* Author: Linus Tolke
   (actually most if the code is "borrowed" from the distribution and just
   slightly modified)
 */

/* Taken over as part of PostgreSQL by Michael Meskes <meskes@debian.org>
   on Feb. 5th, 1998 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdarg.h>
#include <string.h>

#include <libpq-fe.h>
#include <libpq/pqcomm.h>
#include <ecpgtype.h>
#include <ecpglib.h>
#include <sqlca.h>

/* variables visible to the programs */
int			no_auto_trans;

static struct sqlca sqlca_init =
{
	{'S', 'Q', 'L', 'C', 'A', ' ', ' ', ' '},
	sizeof(struct sqlca),
	0,
	{0, {0}},
	{'N', 'O', 'T', ' ', 'S', 'E', 'T', ' '},
	{0, 0, 0, 0, 0, 0},
	{0, 0, 0, 0, 0, 0, 0, 0},
	{0, 0, 0, 0, 0, 0, 0, 0}
};

struct sqlca sqlca =
{
	{'S', 'Q', 'L', 'C', 'A', ' ', ' ', ' '},
	sizeof(struct sqlca),
	0,
	{0, {0}},
	{'N', 'O', 'T', ' ', 'S', 'E', 'T', ' '},
	{0, 0, 0, 0, 0, 0},
	{0, 0, 0, 0, 0, 0, 0, 0},
	{0, 0, 0, 0, 0, 0, 0, 0}
};

static struct connection
{
	char	   *name;
	PGconn	   *connection;
	struct connection *next;
}		   *all_connections = NULL, *actual_connection = NULL;

struct variable
{
	enum ECPGttype type;
	void	   *value;
	long		varcharsize;
	long		arrsize;
	long		offset;
	enum ECPGttype ind_type;
	void	   *ind_value;
	long		ind_varcharsize;
	long		ind_arrsize;
	long		ind_offset;
	struct variable *next;
};

struct statement
{
	int			lineno;
	char	   *command;
	struct variable *inlist;
	struct variable *outlist;
};

static int	simple_debug = 0;
static FILE *debugstream = NULL;
static int	committed = true;

static void
register_error(long code, char *fmt,...)
{
	va_list		args;

	sqlca.sqlcode = code;
	va_start(args, fmt);
	vsprintf(sqlca.sqlerrm.sqlerrmc, fmt, args);
	va_end(args);
	sqlca.sqlerrm.sqlerrml = strlen(sqlca.sqlerrm.sqlerrmc);
}

static void
ECPGfinish(struct connection * act)
{
	if (act != NULL)
	{
		ECPGlog("ECPGfinish: finishing %s.\n", act->name);
		PQfinish(act->connection);
		/* remove act from the list */
		if (act == all_connections)
		{
			all_connections = act->next;
			free(act->name);
			free(act);
		}
		else
		{
			struct connection *con;

			for (con = all_connections; con->next && con->next != act; con = con->next);
			if (con->next)
			{
				con->next = act->next;
				free(act->name);
				free(act);
			}
		}

		if (actual_connection == act)
			actual_connection = all_connections;
	}
	else
		ECPGlog("ECPGfinish: called an extra time.\n");
}

static char *
ecpg_alloc(long size, int lineno)
{
	char	   *new = (char *) malloc(size);

	if (!new)
	{
		ECPGfinish(actual_connection);
		ECPGlog("out of memory\n");
		register_error(ECPG_OUT_OF_MEMORY, "out of memory in line %d", lineno);
		return NULL;
	}

	memset(new, '\0', size);
	return (new);
}

/* This function returns a newly malloced string that has the ' and \
   in the argument quoted with \.
 */
static
char *
quote_postgres(char *arg, int lineno)
{
	char	   *res = (char *) ecpg_alloc(2 * strlen(arg) + 1, lineno);
	int			i,
				ri;

	if (!res)
		return (res);

	for (i = 0, ri = 0; arg[i]; i++, ri++)
	{
		switch (arg[i])
		{
			case '\'':
			case '\\':
				res[ri++] = '\\';
			default:
				;
		}

		res[ri] = arg[i];
	}
	res[ri] = '\0';

	return res;
}

/* create a list of variables */
static bool
create_statement(int lineno, struct statement ** stmt, char *query, va_list ap)
{
	struct variable **list = &((*stmt)->inlist);
	enum ECPGttype type;

	if (!(*stmt = (struct statement *) ecpg_alloc(sizeof(struct statement), lineno)))
		return false;

	(*stmt)->command = query;
	(*stmt)->lineno = lineno;

	list = &((*stmt)->inlist);

	type = va_arg(ap, enum ECPGttype);

	while (type != ECPGt_EORT)
	{
		if (type == ECPGt_EOIT)
			list = &((*stmt)->outlist);
		else
		{
			struct variable *var,
					   *ptr;

			if (!(var = (struct variable *) ecpg_alloc(sizeof(struct variable), lineno)))
				return false;

			var->type = type;
			var->value = va_arg(ap, void *);
			var->varcharsize = va_arg(ap, long);
			var->arrsize = va_arg(ap, long);
			var->offset = va_arg(ap, long);
			var->ind_type = va_arg(ap, enum ECPGttype);
			var->ind_value = va_arg(ap, void *);
			var->ind_varcharsize = va_arg(ap, long);
			var->ind_arrsize = va_arg(ap, long);
			var->ind_offset = va_arg(ap, long);
			var->next = NULL;

			for (ptr = *list; ptr && ptr->next; ptr = ptr->next);

			if (ptr == NULL)
				*list = var;
			else
				ptr->next = var;
		}

		type = va_arg(ap, enum ECPGttype);
	}

	return (true);
}

static bool
ECPGexecute(struct statement * stmt)
{
	bool		status = false;
	char	   *copiedquery;
	PGresult   *results;
	PGnotify   *notify;
	struct variable *var;

	memcpy((char *) &sqlca, (char *) &sqlca_init, sizeof(sqlca));

	copiedquery = strdup(stmt->command);

	/*
	 * Now, if the type is one of the fill in types then we take the
	 * argument and enter that in the string at the first %s position.
	 * Then if there are any more fill in types we fill in at the next and
	 * so on.
	 */
	var = stmt->inlist;
	while (var)
	{
		char	   *newcopy;
		char	   *mallocedval = NULL;
		char	   *tobeinserted = NULL;
		char	   *p;
		char		buff[20];

		/*
		 * Some special treatment is needed for records since we want
		 * their contents to arrive in a comma-separated list on insert (I
		 * think).
		 */

		buff[0] = '\0';

		/* check for null value and set input buffer accordingly */
		switch (var->ind_type)
		{
			case ECPGt_short:
			case ECPGt_unsigned_short:
				if (*(short *) var->ind_value < 0)
					strcpy(buff, "null");
				break;
			case ECPGt_int:
			case ECPGt_unsigned_int:
				if (*(int *) var->ind_value < 0)
					strcpy(buff, "null");
				break;
			case ECPGt_long:
			case ECPGt_unsigned_long:
				if (*(long *) var->ind_value < 0L)
					strcpy(buff, "null");
				break;
			default:
				break;
		}

		if (*buff == '\0')
		{
			switch (var->type)
			{
				case ECPGt_short:
				case ECPGt_int:
					sprintf(buff, "%d", *(int *) var->value);
					tobeinserted = buff;
					break;

				case ECPGt_unsigned_short:
				case ECPGt_unsigned_int:
					sprintf(buff, "%d", *(unsigned int *) var->value);
					tobeinserted = buff;
					break;

				case ECPGt_long:
					sprintf(buff, "%ld", *(long *) var->value);
					tobeinserted = buff;
					break;

				case ECPGt_unsigned_long:
					sprintf(buff, "%ld", *(unsigned long *) var->value);
					tobeinserted = buff;
					break;

				case ECPGt_float:
					sprintf(buff, "%.14g", *(float *) var->value);
					tobeinserted = buff;
					break;

				case ECPGt_double:
					sprintf(buff, "%.14g", *(double *) var->value);
					tobeinserted = buff;
					break;

				case ECPGt_bool:
					sprintf(buff, "'%c'", (*(char *) var->value ? 't' : 'f'));
					tobeinserted = buff;
					break;

				case ECPGt_char:
				case ECPGt_unsigned_char:
					{
						/* set slen to string length if type is char * */
						int			slen = (var->varcharsize == 0) ? strlen((char *) var->value) : var->varcharsize;
						char	   *tmp;

						if (!(newcopy = ecpg_alloc(slen + 1, stmt->lineno)))
							return false;

						strncpy(newcopy, (char *) var->value, slen);
						newcopy[slen] = '\0';

						if (!(mallocedval = (char *) ecpg_alloc(2 * strlen(newcopy) + 3, stmt->lineno)))
							return false;

						strcpy(mallocedval, "'");
						tmp = quote_postgres(newcopy, stmt->lineno);
						if (!tmp)
							return false;

						strcat(mallocedval, tmp);
						strcat(mallocedval, "'");

						free(newcopy);

						tobeinserted = mallocedval;
					}
					break;

				case ECPGt_varchar:
					{
						struct ECPGgeneric_varchar *variable =
						(struct ECPGgeneric_varchar *) (var->value);
						char	   *tmp;

						if (!(newcopy = (char *) ecpg_alloc(variable->len + 1, stmt->lineno)))
							return false;

						strncpy(newcopy, variable->arr, variable->len);
						newcopy[variable->len] = '\0';

						if (!(mallocedval = (char *) ecpg_alloc(2 * strlen(newcopy) + 3, stmt->lineno)))
							return false;

						strcpy(mallocedval, "'");
						tmp = quote_postgres(newcopy, stmt->lineno);
						if (!tmp)
							return false;

						strcat(mallocedval, tmp);
						strcat(mallocedval, "'");

						free(newcopy);

						tobeinserted = mallocedval;
					}
					break;

				default:
					/* Not implemented yet */
					register_error(ECPG_UNSUPPORTED, "Unsupported type %s on line %d.",
								 ECPGtype_name(var->type), stmt->lineno);
					return false;
					break;
			}
		}
		else
			tobeinserted = buff;

		/*
		 * Now tobeinserted points to an area that is to be inserted at
		 * the first %s
		 */
		if (!(newcopy = (char *) ecpg_alloc(strlen(copiedquery) + strlen(tobeinserted) + 1, stmt->lineno)))
			return false;

		strcpy(newcopy, copiedquery);
		if ((p = strstr(newcopy, ";;")) == NULL)
		{

			/*
			 * We have an argument but we dont have the matched up string
			 * in the string
			 */
			register_error(ECPG_TOO_MANY_ARGUMENTS, "Too many arguments line %d.", stmt->lineno);
			return false;
		}
		else
		{
			strcpy(p, tobeinserted);

			/*
			 * The strange thing in the second argument is the rest of the
			 * string from the old string
			 */
			strcat(newcopy,
				   copiedquery
				   + (p - newcopy)
				   + 2 /* Length of ;; */ );
		}

		/*
		 * Now everything is safely copied to the newcopy. Lets free the
		 * oldcopy and let the copiedquery get the var->value from the
		 * newcopy.
		 */
		if (mallocedval != NULL)
		{
			free(mallocedval);
			mallocedval = NULL;
		}

		free(copiedquery);
		copiedquery = newcopy;

		var = var->next;
	}

	/* Check if there are unmatched things left. */
	if (strstr(copiedquery, ";;") != NULL)
	{
		register_error(ECPG_TOO_FEW_ARGUMENTS, "Too few arguments line %d.", stmt->lineno);
		return false;
	}

	/* Now the request is built. */

	if (committed && !no_auto_trans)
	{
		if ((results = PQexec(actual_connection->connection, "begin transaction")) == NULL)
		{
			register_error(ECPG_TRANS, "Error starting transaction line %d.", stmt->lineno);
			return false;
		}
		PQclear(results);
		committed = 0;
	}

	ECPGlog("ECPGexecute line %d: QUERY: %s\n", stmt->lineno, copiedquery);
	results = PQexec(actual_connection->connection, copiedquery);
	free(copiedquery);

	if (results == NULL)
	{
		ECPGlog("ECPGexecute line %d: error: %s", stmt->lineno,
				PQerrorMessage(actual_connection->connection));
		register_error(ECPG_PGSQL, "Postgres error: %s line %d.",
			PQerrorMessage(actual_connection->connection), stmt->lineno);
	}
	else
	{
		sqlca.sqlerrd[2] = 0;
		var = stmt->outlist;
		switch (PQresultStatus(results))
		{
				int			nfields,
							ntuples,
							act_tuple,
							act_field;

			case PGRES_TUPLES_OK:

				/*
				 * XXX Cheap Hack. For now, we see only the last group of
				 * tuples.	This is clearly not the right way to do things
				 * !!
				 */

				nfields = PQnfields(results);
				sqlca.sqlerrd[2] = ntuples = PQntuples(results);
				status = true;

				if (ntuples < 1)
				{
					ECPGlog("ECPGexecute line %d: Incorrect number of matches: %d\n",
							stmt->lineno, ntuples);
					register_error(ECPG_NOT_FOUND, "Data not found line %d.", stmt->lineno);
					status = false;
					break;
				}

				for (act_field = 0; act_field < nfields && status; act_field++)
				{
					char	   *pval;
					char	   *scan_length;

					if (var == NULL)
					{
						ECPGlog("ECPGexecute line %d: Too few arguments.\n", stmt->lineno);
						register_error(ECPG_TOO_FEW_ARGUMENTS, "Too few arguments line %d.", stmt->lineno);
						return (false);
					}

					/*
					 * if we don't have enough space, we cannot read all
					 * tuples
					 */
					if ((var->arrsize > 0 && ntuples > var->arrsize) || (var->ind_arrsize > 0 && ntuples > var->ind_arrsize))
					{
						ECPGlog("ECPGexecute line %d: Incorrect number of matches: %d don't fit into array of %d\n",
								stmt->lineno, ntuples, var->arrsize);
						register_error(ECPG_TOO_MANY_MATCHES, "Too many matches line %d.", stmt->lineno);
						status = false;
						break;
					}
					for (act_tuple = 0; act_tuple < ntuples; act_tuple++)
					{
						pval = PQgetvalue(results, act_tuple, act_field);

						ECPGlog("ECPGexecute line %d: RESULT: %s\n", stmt->lineno, pval ? pval : "");

						/* Now the pval is a pointer to the var->value. */
						/* We will have to decode the var->value */

						/*
						 * check for null var->value and set indicator
						 * accordingly
						 */
						switch (var->ind_type)
						{
							case ECPGt_short:
							case ECPGt_unsigned_short:
								((short *) var->ind_value)[act_tuple] = -PQgetisnull(results, act_tuple, act_field);
								break;
							case ECPGt_int:
							case ECPGt_unsigned_int:
								((int *) var->ind_value)[act_tuple] = -PQgetisnull(results, act_tuple, act_field);
								break;
							case ECPGt_long:
							case ECPGt_unsigned_long:
								((long *) var->ind_value)[act_tuple] = -PQgetisnull(results, act_tuple, act_field);
								break;
							default:
								break;
						}

						switch (var->type)
						{
								long		res;
								unsigned long ures;
								double		dres;

							case ECPGt_short:
							case ECPGt_int:
							case ECPGt_long:
								if (pval)
								{
									res = strtol(pval, &scan_length, 10);
									if (*scan_length != '\0')	/* Garbage left */
									{
										register_error(ECPG_INT_FORMAT, "Not correctly formatted int type: %s line %d.",
													 pval, stmt->lineno);
										status = false;
										res = 0L;
									}
								}
								else
									res = 0L;

								/* Again?! Yes */
								switch (var->type)
								{
									case ECPGt_short:
										((short *) var->value)[act_tuple] = (short) res;
										break;
									case ECPGt_int:
										((int *) var->value)[act_tuple] = (int) res;
										break;
									case ECPGt_long:
										((long *) var->value)[act_tuple] = res;
										break;
									default:
										/* Cannot happen */
										break;
								}
								break;

							case ECPGt_unsigned_short:
							case ECPGt_unsigned_int:
							case ECPGt_unsigned_long:
								if (pval)
								{
									ures = strtoul(pval, &scan_length, 10);
									if (*scan_length != '\0')	/* Garbage left */
									{
										register_error(ECPG_UINT_FORMAT, "Not correctly formatted unsigned type: %s line %d.",
													 pval, stmt->lineno);
										status = false;
										ures = 0L;
									}
								}
								else
									ures = 0L;

								/* Again?! Yes */
								switch (var->type)
								{
									case ECPGt_unsigned_short:
										((unsigned short *) var->value)[act_tuple] = (unsigned short) ures;
										break;
									case ECPGt_unsigned_int:
										((unsigned int *) var->value)[act_tuple] = (unsigned int) ures;
										break;
									case ECPGt_unsigned_long:
										((unsigned long *) var->value)[act_tuple] = ures;
										break;
									default:
										/* Cannot happen */
										break;
								}
								break;


							case ECPGt_float:
							case ECPGt_double:
								if (pval)
								{
									dres = strtod(pval, &scan_length);
									if (*scan_length != '\0')	/* Garbage left */
									{
										register_error(ECPG_FLOAT_FORMAT, "Not correctly formatted floating point type: %s line %d.",
													 pval, stmt->lineno);
										status = false;
										dres = 0.0;
									}
								}
								else
									dres = 0.0;

								/* Again?! Yes */
								switch (var->type)
								{
									case ECPGt_float:
										((float *) var->value)[act_tuple] = dres;
										break;
									case ECPGt_double:
										((double *) var->value)[act_tuple] = dres;
										break;
									default:
										/* Cannot happen */
										break;
								}
								break;

							case ECPGt_bool:
								if (pval)
								{
									if (pval[0] == 'f' && pval[1] == '\0')
									{
										((char *) var->value)[act_tuple] = false;
										break;
									}
									else if (pval[0] == 't' && pval[1] == '\0')
									{
										((char *) var->value)[act_tuple] = true;
										break;
									}
								}

								register_error(ECPG_CONVERT_BOOL, "Unable to convert %s to bool on line %d.",
											   (pval ? pval : "NULL"),
											   stmt->lineno);
								status = false;
								break;

							case ECPGt_char:
							case ECPGt_unsigned_char:
								{
									if (var->varcharsize == 0)
									{
										/* char* */
										strncpy(((char **) var->value)[act_tuple], pval, strlen(pval));
										(((char **) var->value)[act_tuple])[strlen(pval)] = '\0';
									}
									else
									{
										strncpy((char *) ((long) var->value + var->offset * act_tuple), pval, var->varcharsize);
										if (var->varcharsize < strlen(pval))
										{
											/* truncation */
											switch (var->ind_type)
											{
												case ECPGt_short:
												case ECPGt_unsigned_short:
													((short *) var->ind_value)[act_tuple] = var->varcharsize;
													break;
												case ECPGt_int:
												case ECPGt_unsigned_int:
													((int *) var->ind_value)[act_tuple] = var->varcharsize;
													break;
												case ECPGt_long:
												case ECPGt_unsigned_long:
													((long *) var->ind_value)[act_tuple] = var->varcharsize;
													break;
												default:
													break;
											}
											sqlca.sqlwarn[0] = sqlca.sqlwarn[1] = 'W';
										}
									}
								}
								break;

							case ECPGt_varchar:
								{
									struct ECPGgeneric_varchar *variable =
									(struct ECPGgeneric_varchar *) ((long) var->value + var->offset * act_tuple);

									if (var->varcharsize == 0)
										strncpy(variable->arr, pval, strlen(pval));
									else
										strncpy(variable->arr, pval, var->varcharsize);

									variable->len = strlen(pval);
									if (var->varcharsize > 0 && variable->len > var->varcharsize)
									{
										/* truncation */
										switch (var->ind_type)
										{
											case ECPGt_short:
											case ECPGt_unsigned_short:
												((short *) var->ind_value)[act_tuple] = var->varcharsize;
												break;
											case ECPGt_int:
											case ECPGt_unsigned_int:
												((int *) var->ind_value)[act_tuple] = var->varcharsize;
												break;
											case ECPGt_long:
											case ECPGt_unsigned_long:
												((long *) var->ind_value)[act_tuple] = var->varcharsize;
												break;
											default:
												break;
										}
										sqlca.sqlwarn[0] = sqlca.sqlwarn[1] = 'W';

										variable->len = var->varcharsize;
									}
								}
								break;

							default:
								register_error(ECPG_UNSUPPORTED, "Unsupported type %s on line %d.",
								 ECPGtype_name(var->type), stmt->lineno);
								status = false;
								break;
						}
					}
					var = var->next;
				}

				if (status && var != NULL)
				{
					register_error(ECPG_TOO_MANY_ARGUMENTS, "Too many arguments line %d.", stmt->lineno);
					status = false;
				}

				PQclear(results);
				break;
			case PGRES_EMPTY_QUERY:
				/* do nothing */
				register_error(ECPG_EMPTY, "Empty query line %d.", stmt->lineno);
				break;
			case PGRES_COMMAND_OK:
				status = true;
				sqlca.sqlerrd[2] = atol(PQcmdTuples(results));
				ECPGlog("ECPGexecute line %d Ok: %s\n", stmt->lineno, PQcmdStatus(results));
				break;
			case PGRES_NONFATAL_ERROR:
			case PGRES_FATAL_ERROR:
			case PGRES_BAD_RESPONSE:
				ECPGlog("ECPGexecute line %d: Error: %s",
						stmt->lineno, PQerrorMessage(actual_connection->connection));
				register_error(ECPG_PGSQL, "Error: %s line %d.",
							   PQerrorMessage(actual_connection->connection), stmt->lineno);
				status = false;
				break;
			case PGRES_COPY_OUT:
				ECPGlog("ECPGexecute line %d: Got PGRES_COPY_OUT ... tossing.\n", stmt->lineno);
				PQendcopy(results->conn);
				break;
			case PGRES_COPY_IN:
				ECPGlog("ECPGexecute line %d: Got PGRES_COPY_IN ... tossing.\n", stmt->lineno);
				PQendcopy(results->conn);
				break;
			default:
				ECPGlog("ECPGexecute line %d: Got something else, postgres error.\n",
						stmt->lineno);
				register_error(ECPG_PGSQL, "Postgres error line %d.", stmt->lineno);
				status = false;
				break;
		}
	}

	/* check for asynchronous returns */
	notify = PQnotifies(actual_connection->connection);
	if (notify)
	{
		ECPGlog("ECPGexecute line %d: ASYNC NOTIFY of '%s' from backend pid '%d' received\n",
				stmt->lineno, notify->relname, notify->be_pid);
		free(notify);
	}

	return status;
}

bool
ECPGdo(int lineno, char *query,...)
{
	va_list		args;
	struct statement *stmt;

	va_start(args, query);
	if (create_statement(lineno, &stmt, query, args) == false)
		return (false);
	va_end(args);

	/* are we connected? */
	if (actual_connection == NULL || actual_connection->connection == NULL)
	{
		ECPGlog("ECPGdo: not connected\n");
		register_error(ECPG_NOT_CONN, "Not connected in line %d", lineno);
		return false;
	}

	return (ECPGexecute(stmt));
}


bool
ECPGtrans(int lineno, const char *transaction)
{
	PGresult   *res;

	ECPGlog("ECPGtrans line %d action = %s\n", lineno, transaction);
	if ((res = PQexec(actual_connection->connection, transaction)) == NULL)
	{
		register_error(ECPG_TRANS, "Error in transaction processing line %d.", lineno);
		return FALSE;
	}
	PQclear(res);
	if (strcmp(transaction, "commit") == 0 || strcmp(transaction, "rollback") == 0)
		committed = 1;
	return TRUE;
}

bool
ECPGsetconn(int lineno, const char *connection_name)
{
	struct connection *con = all_connections;

	ECPGlog("ECPGsetconn: setting actual connection to %s\n", connection_name);

	for (; con && strcmp(connection_name, con->name) != 0; con = con->next);
	if (con)
	{
		actual_connection = con;
		return true;
	}
	else
	{
		register_error(ECPG_NO_CONN, "No such connection %s in line %d", connection_name, lineno);
		return false;
	}
}

bool
ECPGconnect(int lineno, const char *dbname, const char *user, const char *passwd, const char *connection_name)
{
	struct connection *this = (struct connection *) ecpg_alloc(sizeof(struct connection), lineno);

	if (!this)
		return false;

	if (dbname == NULL && connection_name == NULL)
		connection_name = "DEFAULT";

	/* add connection to our list */
	if (connection_name != NULL)
		this->name = strdup(connection_name);
	else
		this->name = strdup(dbname);

	if (all_connections == NULL)
		this->next = NULL;
	else
		this->next = all_connections;

	actual_connection = all_connections = this;

	ECPGlog("ECPGconnect: opening database %s %s%s\n", dbname ? dbname : "NULL", user ? "for user " : "", user ? user : "");

	sqlca.sqlcode = 0;

	this->connection = PQsetdbLogin(NULL, NULL, NULL, NULL, dbname, user, passwd);

	if (PQstatus(this->connection) == CONNECTION_BAD)
	{
		ECPGfinish(this);
		ECPGlog("connect: could not open database %s %s%s in line %d\n", dbname ? dbname : "NULL", user ? "for user " : "", user ? user : "", lineno);
		register_error(ECPG_CONNECT, "connect: could not open database %s.", dbname ? dbname : "NULL");
		return false;
	}

	return true;
}

bool
ECPGdisconnect(int lineno, const char *connection_name)
{
	struct connection *con;

	if (strcmp(connection_name, "CURRENT") == 0)
		ECPGfinish(actual_connection);
	else if (strcmp(connection_name, "ALL") == 0)
	{
		for (con = all_connections; con;)
		{
			struct connection *f = con;

			con = con->next;
			ECPGfinish(f);
		}
	}
	else
	{
		for (con = all_connections; con && strcmp(con->name, connection_name) != 0; con = con->next);
		if (con == NULL)
		{
			ECPGlog("disconnect: not connected to connection %s\n", connection_name);
			register_error(ECPG_NO_CONN, "No such connection %s in line %d", connection_name, lineno);
			return false;
		}
		else
			ECPGfinish(con);
	}

	return true;
}

void
ECPGdebug(int n, FILE *dbgs)
{
	simple_debug = n;
	debugstream = dbgs;
	ECPGlog("ECPGdebug: set to %d\n", simple_debug);
}

void
ECPGlog(const char *format,...)
{
	va_list		ap;

	if (simple_debug)
	{
		char	   *f = (char *) malloc(strlen(format) + 100);

		if (!f)
			return;

		sprintf(f, "[%d]: %s", getpid(), format);

		va_start(ap, format);
		vfprintf(debugstream, f, ap);
		va_end(ap);

		free(f);
	}
}

/* print out an error message */
void
sqlprint(void)
{
	sqlca.sqlerrm.sqlerrmc[sqlca.sqlerrm.sqlerrml] = '\0';
	printf("sql error %s\n", sqlca.sqlerrm.sqlerrmc);
}

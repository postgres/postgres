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
#include <ctype.h>

#include <libpq-fe.h>
#include <libpq/pqcomm.h>
#include <ecpgtype.h>
#include <ecpglib.h>
#include <sqlca.h>

/* variables visible to the programs */
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
	bool		committed;
	int			autocommit;
	struct connection *next;
}		   *all_connections = NULL, *actual_connection = NULL;

struct variable
{
	enum ECPGttype type;
	void	   *value;
	void	   *pointer;
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
	struct connection *connection;
	struct variable *inlist;
	struct variable *outlist;
};

struct prepared_statement
{
	char	   *name;
	struct statement *stmt;
	struct prepared_statement *next;
}		   *prep_stmts = NULL;

struct auto_mem
{
	void	   *pointer;
	struct auto_mem *next;
}		   *auto_allocs = NULL;

static int	simple_debug = 0;
static FILE *debugstream = NULL;

static void
register_error(long code, char *fmt,...)
{
	va_list		args;
	struct auto_mem *am;

	sqlca.sqlcode = code;
	va_start(args, fmt);
	vsprintf(sqlca.sqlerrm.sqlerrmc, fmt, args);
	va_end(args);
	sqlca.sqlerrm.sqlerrml = strlen(sqlca.sqlerrm.sqlerrmc);

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

static struct connection *
get_connection(const char *connection_name)
{
	struct connection *con = all_connections;

	if (connection_name == NULL || strcmp(connection_name, "CURRENT") == 0)
		return actual_connection;

	for (; con && strcmp(connection_name, con->name) != 0; con = con->next);
	if (con)
		return con;
	else
		return NULL;
}

static bool
ecpg_init(const struct connection *con, const char * connection_name, const int lineno)
{
	memcpy((char *) &sqlca, (char *) &sqlca_init, sizeof(sqlca));
	if (con == NULL)
	{
		register_error(ECPG_NO_CONN, "No such connection %s in line %d.", connection_name ? connection_name : "NULL", lineno);
		return (false);
	}
	
	return (true);
}

static void
ecpg_finish(struct connection * act)
{
	if (act != NULL)
	{
		ECPGlog("ecpg_finish: finishing %s.\n", act->name);
		PQfinish(act->connection);
		/* remove act from the list */
		if (act == all_connections)
			all_connections = act->next;
		else
		{
			struct connection *con;

			for (con = all_connections; con->next && con->next != act; con = con->next);
			if (con->next)
				con->next = act->next;
		}

		if (actual_connection == act)
			actual_connection = all_connections;

		free(act->name);
		free(act);
	}
	else
		ECPGlog("ecpg_finish: called an extra time.\n");
}

static char *
ecpg_alloc(long size, int lineno)
{
	char	   *new = (char *) calloc(1L, size);

	if (!new)
	{
		ECPGlog("out of memory\n");
		register_error(ECPG_OUT_OF_MEMORY, "Out of memory in line %d", lineno);
		return NULL;
	}

	memset(new, '\0', size);
	return (new);
}

static char *
ecpg_strdup(const char *string, int lineno)
{
	char	   *new = strdup(string);

	if (!new)
	{
		ECPGlog("out of memory\n");
		register_error(ECPG_OUT_OF_MEMORY, "Out of memory in line %d", lineno);
		return NULL;
	}

	return (new);
}

static void
add_mem(void *ptr, int lineno)
{
	struct auto_mem *am = (struct auto_mem *) ecpg_alloc(sizeof(struct auto_mem), lineno);

	am->next = auto_allocs;
	auto_allocs = am;
}

/* This function returns a newly malloced string that has the  \
   in the argument quoted with \ and the ' quote with ' as SQL92 says.
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
				res[ri++] = '\'';
				break;
			case '\\':
				res[ri++] = '\\';
				break;
			default:
				;
		}

		res[ri] = arg[i];
	}
	res[ri] = '\0';

	return res;
}

/*
 * create a list of variables
 * The variables are listed with input variables preceeding outputvariables
 * The end of each group is marked by an end marker.
 * per variable we list:
 * type - as defined in ecpgtype.h
 * value - where to store the data
 * varcharsize - length of string in case we have a stringvariable, else 0
 * arraysize - 0 for pointer (we don't know the size of the array),
 * 1 for simple variable, size for arrays
 * offset - offset between ith and (i+1)th entry in an array,
 * normally that means sizeof(type)
 * ind_type - type of indicator variable
 * ind_value - pointer to indicator variable
 * ind_varcharsize - empty
 * ind_arraysize -	arraysize of indicator array
 * ind_offset - indicator offset
 */
static bool
create_statement(int lineno, struct connection * connection, struct statement ** stmt, char *query, va_list ap)
{
	struct variable **list = &((*stmt)->inlist);
	enum ECPGttype type;

	if (!(*stmt = (struct statement *) ecpg_alloc(sizeof(struct statement), lineno)))
		return false;

	(*stmt)->command = query;
	(*stmt)->connection = connection;
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
			var->pointer = va_arg(ap, void *);

			/* if variable is NULL, the statement hasn't been prepared */
			if (var->pointer == NULL)
			{
				ECPGlog("create_statement: invalid statement name\n");
				register_error(ECPG_INVALID_STMT, "Invalid statement name in line %d.", lineno);
				free(var);
				return false;
			}

			var->varcharsize = va_arg(ap, long);
			var->arrsize = va_arg(ap, long);
			var->offset = va_arg(ap, long);

			if (var->arrsize == 0 || var->varcharsize == 0)
				var->value = *((void **) (var->pointer));
			else
				var->value = var->pointer;

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

static void
free_variable(struct variable * var)
{
	struct variable *var_next;

	if (var == (struct variable *) NULL)
		return;
	var_next = var->next;
	free(var);

	while (var_next)
	{
		var = var_next;
		var_next = var->next;
		free(var);
	}
}

static void
free_statement(struct statement * stmt)
{
	if (stmt == (struct statement *) NULL)
		return;
	free_variable(stmt->inlist);
	free_variable(stmt->outlist);
	free(stmt);
}

static char *
next_insert(char *text)
{
	char	   *ptr = text;
	bool		string = false;

	for (; *ptr != '\0' && (*ptr != '?' || string); ptr++)
		if (*ptr == '\'' && *(ptr-1) != '\\')
			string = string ? false : true;

	return (*ptr == '\0') ? NULL : ptr;
}

static bool
ECPGexecute(struct statement * stmt)
{
	bool		status = false;
	char	   *copiedquery;
	PGresult   *results;
	PGnotify   *notify;
	struct variable *var;

	copiedquery = ecpg_strdup(stmt->command, stmt->lineno);

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
					sprintf(buff, "%d", *(short *) var->value);
					tobeinserted = buff;
					break;

				case ECPGt_int:
					sprintf(buff, "%d", *(int *) var->value);
					tobeinserted = buff;
					break;

				case ECPGt_unsigned_short:
					sprintf(buff, "%d", *(unsigned short *) var->value);
					tobeinserted = buff;
					break;

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
				case ECPGt_char_variable:
					{
						int			slen = strlen((char *) var->value);

						if (!(newcopy = ecpg_alloc(slen + 1, stmt->lineno)))
							return false;

						strncpy(newcopy, (char *) var->value, slen);
						newcopy[slen] = '\0';

						tobeinserted = newcopy;
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
		if ((p = next_insert(newcopy)) == NULL)
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
				   + sizeof("?") - 1 /* don't count the '\0' */ );
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
	if (next_insert(copiedquery) != NULL)
	{
		register_error(ECPG_TOO_FEW_ARGUMENTS, "Too few arguments line %d.", stmt->lineno);
		return false;
	}

	/* Now the request is built. */

	if (stmt->connection->committed && !stmt->connection->autocommit)
	{
		if ((results = PQexec(stmt->connection->connection, "begin transaction")) == NULL)
		{
			register_error(ECPG_TRANS, "Error in transaction processing line %d.", stmt->lineno);
			return false;
		}
		PQclear(results);
		stmt->connection->committed = false;
	}

	ECPGlog("ECPGexecute line %d: QUERY: %s on connection %s\n", stmt->lineno, copiedquery, stmt->connection->name);
	results = PQexec(stmt->connection->connection, copiedquery);
	free(copiedquery);

	if (results == NULL)
	{
		ECPGlog("ECPGexecute line %d: error: %s", stmt->lineno,
				PQerrorMessage(stmt->connection->connection));
		register_error(ECPG_PGSQL, "Postgres error: %s line %d.",
			 PQerrorMessage(stmt->connection->connection), stmt->lineno);
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
				nfields = PQnfields(results);
				sqlca.sqlerrd[2] = ntuples = PQntuples(results);
				status = true;

				if (ntuples < 1)
				{
					ECPGlog("ECPGexecute line %d: Incorrect number of matches: %d\n",
							stmt->lineno, ntuples);
					register_error(ECPG_NOT_FOUND, "No data found line %d.", stmt->lineno);
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

					/*
					 * allocate memory for NULL pointers
					 */
					if ((var->arrsize == 0 || var->varcharsize == 0) && var->value == NULL)
					{
						int			len = 0;

						switch (var->type)
						{
							case ECPGt_char:
							case ECPGt_unsigned_char:
								var->varcharsize = 0;
								/* check strlen for each tuple */
								for (act_tuple = 0; act_tuple < ntuples; act_tuple++)
								{
									int			len = strlen(PQgetvalue(results, act_tuple, act_field)) + 1;

									if (len > var->varcharsize)
										var->varcharsize = len;
								}
								var->offset *= var->varcharsize;
								len = var->offset * ntuples;
								break;
							case ECPGt_varchar:
								len = ntuples * (var->varcharsize + sizeof(int));
								break;
							default:
								len = var->offset * ntuples;
								break;
						}
						var->value = (void *) ecpg_alloc(len, stmt->lineno);
						*((void **) var->pointer) = var->value;
						add_mem(var->value, stmt->lineno);
					}

					for (act_tuple = 0; act_tuple < ntuples && status; act_tuple++)
					{
						pval = PQgetvalue(results, act_tuple, act_field);

						ECPGlog("ECPGexecute line %d: RESULT: %s\n", stmt->lineno, pval ? pval : "");

						/* Now the pval is a pointer to the value. */
						/* We will have to decode the value */

						/*
						 * check for null value and set indicator
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
							case ECPGt_NO_INDICATOR:
								if (PQgetisnull(results, act_tuple, act_field))
								{
									register_error(ECPG_MISSING_INDICATOR, "NULL value without indicator variable on line %d.", stmt->lineno);
									status = false;
								}
								break;
							default:
								register_error(ECPG_UNSUPPORTED, "Unsupported indicator type %s on line %d.", ECPGtype_name(var->ind_type), stmt->lineno);
								status = false;
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
									else if (pval[0] == '\0' && PQgetisnull(results, act_tuple, act_field))
									{
										// NULL is valid
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
									strncpy((char *) ((long) var->value + var->offset * act_tuple), pval, var->varcharsize);
									if (var->varcharsize && var->varcharsize < strlen(pval))
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
								register_error(ECPG_UNSUPPORTED, "Unsupported type %s on line %d.", ECPGtype_name(var->type), stmt->lineno);
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

				break;
			case PGRES_EMPTY_QUERY:
				/* do nothing */
				register_error(ECPG_EMPTY, "Empty query line %d.", stmt->lineno);
				break;
			case PGRES_COMMAND_OK:
				status = true;
				sqlca.sqlerrd[1] = atol(PQoidStatus(results));
				sqlca.sqlerrd[2] = atol(PQcmdTuples(results));
				ECPGlog("ECPGexecute line %d Ok: %s\n", stmt->lineno, PQcmdStatus(results));
				break;
			case PGRES_NONFATAL_ERROR:
			case PGRES_FATAL_ERROR:
			case PGRES_BAD_RESPONSE:
				ECPGlog("ECPGexecute line %d: Error: %s",
						stmt->lineno, PQerrorMessage(stmt->connection->connection));
				register_error(ECPG_PGSQL, "Postgres error: %s line %d.",
							   PQerrorMessage(stmt->connection->connection), stmt->lineno);
				status = false;
				break;
			case PGRES_COPY_OUT:
				ECPGlog("ECPGexecute line %d: Got PGRES_COPY_OUT ... tossing.\n", stmt->lineno);
				PQendcopy(stmt->connection->connection);
				break;
			case PGRES_COPY_IN:
				ECPGlog("ECPGexecute line %d: Got PGRES_COPY_IN ... tossing.\n", stmt->lineno);
				PQendcopy(stmt->connection->connection);
				break;
			default:
				ECPGlog("ECPGexecute line %d: Got something else, postgres error.\n",
						stmt->lineno);
				register_error(ECPG_PGSQL, "Postgres error: %s line %d.",
							   PQerrorMessage(stmt->connection->connection), stmt->lineno);
				status = false;
				break;
		}
		PQclear(results);
	}

	/* check for asynchronous returns */
	notify = PQnotifies(stmt->connection->connection);
	if (notify)
	{
		ECPGlog("ECPGexecute line %d: ASYNC NOTIFY of '%s' from backend pid '%d' received\n",
				stmt->lineno, notify->relname, notify->be_pid);
		free(notify);
	}

	return status;
}

bool
ECPGdo(int lineno, const char *connection_name, char *query,...)
{
	va_list		args;
	struct statement *stmt;
	struct connection *con = get_connection(connection_name);
	bool		status;

	if (!ecpg_init(con, connection_name, lineno))
		return(false);

	va_start(args, query);
	if (create_statement(lineno, con, &stmt, query, args) == false)
		return (false);
	va_end(args);

	/* are we connected? */
	if (con == NULL || con->connection == NULL)
	{
		ECPGlog("ECPGdo: not connected to %s\n", con->name);
		register_error(ECPG_NOT_CONN, "Not connected in line %d.", lineno);
		return false;
	}

	status = ECPGexecute(stmt);
	free_statement(stmt);
	return (status);
}

bool
ECPGstatus(int lineno, const char *connection_name)
{
	struct connection *con = get_connection(connection_name);

	if (!ecpg_init(con, connection_name, lineno))
		return(false);

	/* are we connected? */
	if (con->connection == NULL)
	{
		ECPGlog("ECPGdo: not connected to %s\n", con->name);
		register_error(ECPG_NOT_CONN, "Not connected in line %d", lineno);
		return false;
	}

	return (true);
}

bool
ECPGtrans(int lineno, const char *connection_name, const char *transaction)
{
	PGresult   *res;
	struct connection *con = get_connection(connection_name);

	if (!ecpg_init(con, connection_name, lineno))
		return(false);

	ECPGlog("ECPGtrans line %d action = %s connection = %s\n", lineno, transaction, con->name);

	/* if we have no connection we just simulate the command */
	if (con && con->connection)
	{
		if ((res = PQexec(con->connection, transaction)) == NULL)
		{
			register_error(ECPG_TRANS, "Error in transaction processing line %d.", lineno);
			return FALSE;
		}
		PQclear(res);
	}
	if (strcmp(transaction, "commit") == 0 || strcmp(transaction, "rollback") == 0)
	{
		struct prepared_statement *this;

		con->committed = true;

		/* deallocate all prepared statements */
		for (this = prep_stmts; this != NULL; this = this->next)
		{
			bool		b = ECPGdeallocate(lineno, this->name);

			if (!b)
				return false;
		}
	}

	return true;
}

bool
ECPGsetcommit(int lineno, const char *mode, const char *connection_name)
{
	struct connection *con = get_connection(connection_name);
	PGresult   *results;

	if (!ecpg_init(con, connection_name, lineno))
		return(false);

	if (con->autocommit == true && strncmp(mode, "OFF", strlen("OFF")) == 0)
	{
		if (con->committed)
		{
			if ((results = PQexec(con->connection, "begin transaction")) == NULL)
			{
				register_error(ECPG_TRANS, "Error in transaction processing line %d.", lineno);
				return false;
			}
			PQclear(results);
			con->committed = false;
		}
		con->autocommit = false;
	}
	else if (con->autocommit == false && strncmp(mode, "ON", strlen("ON")) == 0)
	{
		if (!con->committed)
		{
			if ((results = PQexec(con->connection, "commit")) == NULL)
			{
				register_error(ECPG_TRANS, "Error in transaction processing line %d.", lineno);
				return false;
			}
			PQclear(results);
			con->committed = true;
		}
		con->autocommit = true;
	}

	return true;
}

bool
ECPGsetconn(int lineno, const char *connection_name)
{
	struct connection *con = get_connection(connection_name);

	if (!ecpg_init(con, connection_name, lineno))
		return(false);

	actual_connection = con;
	return true;
}

bool
ECPGconnect(int lineno, const char *dbname, const char *user, const char *passwd, const char *connection_name, int autocommit)
{
	struct connection *this;


	memcpy((char *) &sqlca, (char *) &sqlca_init, sizeof(sqlca));
	
	if ((this = (struct connection *) ecpg_alloc(sizeof(struct connection), lineno)) == NULL)
		return false;

	if (dbname == NULL && connection_name == NULL)
		connection_name = "DEFAULT";

	/* add connection to our list */
	if (connection_name != NULL)
		this->name = ecpg_strdup(connection_name, lineno);
	else
		this->name = ecpg_strdup(dbname, lineno);

	if (all_connections == NULL)
		this->next = NULL;
	else
		this->next = all_connections;

	actual_connection = all_connections = this;

	ECPGlog("ECPGconnect: opening database %s %s%s\n", dbname ? dbname : "<DEFAULT>", user ? "for user " : "", user ? user : "");

	sqlca.sqlcode = 0;

	this->connection = PQsetdbLogin(NULL, NULL, NULL, NULL, dbname, user, passwd);

	if (PQstatus(this->connection) == CONNECTION_BAD)
	{
		ecpg_finish(this);
		ECPGlog("connect: could not open database %s %s%s in line %d\n", dbname ? dbname : "<DEFAULT>", user ? "for user " : "", user ? user : "", lineno);
		register_error(ECPG_CONNECT, "connect: could not open database %s.", dbname ? dbname : "<DEFAULT>");
		return false;
	}

	this->committed = true;
	this->autocommit = autocommit;

	return true;
}

bool
ECPGdisconnect(int lineno, const char *connection_name)
{
	struct connection *con;

	if (strcmp(connection_name, "ALL") == 0)
	{
		for (con = all_connections; con;)
		{
			struct connection *f = con;

			con = con->next;
			ecpg_finish(f);
		}
	}
	else
	{
		con = get_connection(connection_name);

		if (!ecpg_init(con, connection_name, lineno))
		        return(false);
		else
			ecpg_finish(con);
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

		sprintf(f, "[%d]: %s", (int) getpid(), format);

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
	fprintf(stderr, "sql error %s\n", sqlca.sqlerrm.sqlerrmc);
}

static bool
isvarchar(unsigned char c)
{
	if (isalnum(c))
		return true;

	if (c == '_' || c == '>' || c == '-' || c == '.')
		return true;

	if (c >= 128)
		return true;

	return (false);
}

static void
replace_variables(char *text)
{
	char	   *ptr = text;
	bool		string = false;

	for (; *ptr != '\0'; ptr++)
	{
		if (*ptr == '\'')
			string = string ? false : true;

		if (!string && *ptr == ':')
		{
			*ptr = '?';
			for (++ptr; *ptr && isvarchar(*ptr); ptr++)
				*ptr = ' ';
		}
	}
}

/* handle the EXEC SQL PREPARE statement */
bool
ECPGprepare(int lineno, char *name, char *variable)
{
	struct statement *stmt;
	struct prepared_statement *this;

	/* check if we already have prepared this statement */
	for (this = prep_stmts; this != NULL && strcmp(this->name, name) != 0; this = this->next);
	if (this)
	{
		bool		b = ECPGdeallocate(lineno, name);

		if (!b)
			return false;
	}

	this = (struct prepared_statement *) ecpg_alloc(sizeof(struct prepared_statement), lineno);
	if (!this)
		return false;

	stmt = (struct statement *) ecpg_alloc(sizeof(struct statement), lineno);
	if (!stmt)
	{
		free(this);
		return false;
	}

	/* create statement */
	stmt->lineno = lineno;
	stmt->connection = NULL;
	stmt->command = ecpg_strdup(variable, lineno);
	stmt->inlist = stmt->outlist = NULL;

	/* if we have C variables in our statment replace them with '?' */
	replace_variables(stmt->command);

	/* add prepared statement to our list */
	this->name = ecpg_strdup(name, lineno);
	this->stmt = stmt;

	if (prep_stmts == NULL)
		this->next = NULL;
	else
		this->next = prep_stmts;

	prep_stmts = this;
	return true;
}

/* handle the EXEC SQL DEALLOCATE PREPARE statement */
bool
ECPGdeallocate(int lineno, char *name)
{
	struct prepared_statement *this,
			   *prev;

	/* check if we really have prepared this statement */
	for (this = prep_stmts, prev = NULL; this != NULL && strcmp(this->name, name) != 0; prev = this, this = this->next);
	if (this)
	{
		/* okay, free all the resources */
		free(this->name);
		free(this->stmt->command);
		free(this->stmt);
		if (prev != NULL)
			prev->next = this->next;
		else
			prep_stmts = this->next;

		return true;
	}
	ECPGlog("deallocate_prepare: invalid statement name %s\n", name);
	register_error(ECPG_INVALID_STMT, "Invalid statement name %s in line %d", name, lineno);
	return false;
}

/* return the prepared statement */
char *
ECPGprepared_statement(char *name)
{
	struct prepared_statement *this;

	for (this = prep_stmts; this != NULL && strcmp(this->name, name) != 0; this = this->next);
	return (this) ? this->stmt->command : NULL;
}

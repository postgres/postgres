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

static PGconn *simple_connection = NULL;
static int	simple_debug = 0;
static FILE *debugstream = NULL;
static int	committed = true;

static void
register_error(int code, char *fmt,...)
{
	va_list		args;

	sqlca.sqlcode = code;
	va_start(args, fmt);
	vsprintf(sqlca.sqlerrm.sqlerrmc, fmt, args);
	va_end(args);
	sqlca.sqlerrm.sqlerrml = strlen(sqlca.sqlerrm.sqlerrmc);
}

/* This function returns a newly malloced string that has the ' and \
   in the argument quoted with \.
 */
static
char *
quote_postgres(char *arg)
{
	char	   *res = (char *) malloc(2 * strlen(arg) + 1);
	int			i,
				ri;

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


bool
ECPGdo(int lineno, char *query,...)
{
	va_list		ap;
	bool		status = false;
	char	   *copiedquery;
	PGresult   *results;
	PGnotify   *notify;
	enum ECPGttype type;

	va_start(ap, query);

	sqlca.sqlcode = 0;
	copiedquery = strdup(query);

	type = va_arg(ap, enum ECPGttype);

	/*
	 * Now, if the type is one of the fill in types then we take the
	 * argument and enter that in the string at the first %s position.
	 * Then if there are any more fill in types we fill in at the next and
	 * so on.
	 */
	while (type != ECPGt_EOIT)
	{
		void	   *value = NULL;
		long		varcharsize;
		long		size;
		long		arrsize;

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

		value = va_arg(ap, void *);
		varcharsize = va_arg(ap, long);
		size = va_arg(ap, long);
		arrsize = va_arg(ap, long);

		switch (type)
		{
			case ECPGt_short:
			case ECPGt_int:
				sprintf(buff, "%d", *(int *) value);
				tobeinserted = buff;
				break;

			case ECPGt_unsigned_short:
			case ECPGt_unsigned_int:
				sprintf(buff, "%d", *(unsigned int *) value);
				tobeinserted = buff;
				break;

			case ECPGt_long:
				sprintf(buff, "%ld", *(long *) value);
				tobeinserted = buff;
				break;

			case ECPGt_unsigned_long:
				sprintf(buff, "%ld", *(unsigned long *) value);
				tobeinserted = buff;
				break;

			case ECPGt_float:
				sprintf(buff, "%.14g", *(float *) value);
				tobeinserted = buff;
				break;

			case ECPGt_double:
				sprintf(buff, "%.14g", *(double *) value);
				tobeinserted = buff;
				break;

			case ECPGt_bool:
				sprintf(buff, "'%c'", (*(char *) value ? 't' : 'f'));
				tobeinserted = buff;
				break;

			case ECPGt_char:
			case ECPGt_unsigned_char:
				{
					/* set slen to string length if type is char * */
					int			slen = (varcharsize == 0) ? strlen((char *) value) : varcharsize;

					newcopy = (char *) malloc(slen + 1);
					strncpy(newcopy, (char *) value, slen);
					newcopy[slen] = '\0';

					mallocedval = (char *) malloc(2 * strlen(newcopy) + 3);
					strcpy(mallocedval, "'");
					strcat(mallocedval, quote_postgres(newcopy));
					strcat(mallocedval, "'");

					free(newcopy);

					tobeinserted = mallocedval;
				}
				break;

			case ECPGt_varchar:
			case ECPGt_varchar2:
				{
					struct ECPGgeneric_varchar *var =
					(struct ECPGgeneric_varchar *) value;

					newcopy = (char *) malloc(var->len + 1);
					strncpy(newcopy, var->arr, var->len);
					newcopy[var->len] = '\0';

					mallocedval = (char *) malloc(2 * strlen(newcopy) + 3);
					strcpy(mallocedval, "'");
					strcat(mallocedval, quote_postgres(newcopy));
					strcat(mallocedval, "'");

					free(newcopy);

					tobeinserted = mallocedval;
				}
				break;

			default:
				/* Not implemented yet */
				register_error(-1, "Unsupported type %s on line %d.",
							   ECPGtype_name(type), lineno);
				return false;
				break;
		}

		/*
		 * Now tobeinserted points to an area that is to be inserted at
		 * the first %s
		 */
		newcopy = (char *) malloc(strlen(copiedquery)
								  + strlen(tobeinserted)
								  + 1);
		strcpy(newcopy, copiedquery);
		if ((p = strstr(newcopy, ";;")) == NULL)
		{

			/*
			 * We have an argument but we dont have the matched up string
			 * in the string
			 */
			register_error(-1, "Too many arguments line %d.", lineno);
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
		 * oldcopy and let the copiedquery get the value from the newcopy.
		 */
		if (mallocedval != NULL)
		{
			free(mallocedval);
			mallocedval = NULL;
		}

		free(copiedquery);
		copiedquery = newcopy;

		type = va_arg(ap, enum ECPGttype);
	}

	/* Check if there are unmatched things left. */
	if (strstr(copiedquery, ";;") != NULL)
	{
		register_error(-1, "Too few arguments line %d.", lineno);
		return false;
	}

	/* Now the request is built. */

	if (committed)
	{
		if ((results = PQexec(simple_connection, "begin")) == NULL)
		{
			register_error(-1, "Error starting transaction line %d.", lineno);
			return false;
		}
		PQclear(results);
		committed = 0;
	}

	ECPGlog("ECPGdo line %d: QUERY: %s\n", lineno, copiedquery);
	results = PQexec(simple_connection, copiedquery);
	free(copiedquery);

	if (results == NULL)
	{
		ECPGlog("ECPGdo line %d: error: %s", lineno,
				PQerrorMessage(simple_connection));
		register_error(-1, "Postgres error: %s line %d.",
					   PQerrorMessage(simple_connection), lineno);
	}
	else
		switch (PQresultStatus(results))
		{
				int			m,
							n,
							x;

			case PGRES_TUPLES_OK:

				/*
				 * XXX Cheap Hack. For now, we see only the last group of
				 * tuples.	This is clearly not the right way to do things
				 * !!
				 */

				m = PQnfields(results);
				n = PQntuples(results);

				if (n < 1)
				{
					ECPGlog("ECPGdo line %d: Incorrect number of matches: %d\n",
							lineno, n);
					register_error(1, "Data not found line %d.", lineno);
					break;
				}

				if (n > 1)
				{
					ECPGlog("ECPGdo line %d: Incorrect number of matches: %d\n",
							lineno, n);
					register_error(-1, "To many matches line %d.", lineno);
					break;
				}

				status = true;

				for (x = 0; x < m && status; x++)
				{
					void	   *value = NULL;
					long		varcharsize;
					long		size;
					long		arrsize;

					char	   *pval = PQgetvalue(results, 0, x);

					/*
					 * long int  * res_int; char	** res_charstar; char	 *
					 * res_char; int	 res_len;
					 */
					char	   *scan_length;

					ECPGlog("ECPGdo line %d: RESULT: %s\n", lineno, pval ? pval : "");

					/* No the pval is a pointer to the value. */
					/* We will have to decode the value */
					type = va_arg(ap, enum ECPGttype);
					value = va_arg(ap, void *);
					varcharsize = va_arg(ap, long);
					size = va_arg(ap, long);
					arrsize = va_arg(ap, long);

					switch (type)
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
								if (*scan_length != '\0')		/* Garbage left */
								{
									register_error(-1, "Not correctly formatted int type: %s line %d.",
												   pval, lineno);
									status = false;
									res = 0L;
								}
							}
							else
								res = 0L;

							/* Again?! Yes */
							switch (type)
							{
								case ECPGt_short:
									*(short *) value = (short) res;
									break;
								case ECPGt_int:
									*(int *) value = (int) res;
									break;
								case ECPGt_long:
									*(long *) value = res;
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
								if (*scan_length != '\0')		/* Garbage left */
								{
									register_error(-1, "Not correctly formatted unsigned type: %s line %d.",
												   pval, lineno);
									status = false;
									ures = 0L;
								}
							}
							else
								ures = 0L;

							/* Again?! Yes */
							switch (type)
							{
								case ECPGt_unsigned_short:
									*(unsigned short *) value = (unsigned short) ures;
									break;
								case ECPGt_unsigned_int:
									*(unsigned int *) value = (unsigned int) ures;
									break;
								case ECPGt_unsigned_long:
									*(unsigned long *) value = ures;
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
								if (*scan_length != '\0')		/* Garbage left */
								{
									register_error(-1, "Not correctly formatted floating point type: %s line %d.",
												   pval, lineno);
									status = false;
									dres = 0.0;
								}
							}
							else
								dres = 0.0;

							/* Again?! Yes */
							switch (type)
							{
								case ECPGt_float:
									*(float *) value = dres;
									break;
								case ECPGt_double:
									*(double *) value = dres;
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
									*(char *) value = false;
									break;
								}
								else if (pval[0] == 't' && pval[1] == '\0')
								{
									*(char *) value = true;
									break;
								}
							}

							register_error(-1, "Unable to convert %s to bool on line %d.",
										   (pval ? pval : "NULL"),
										   lineno);
							return false;
							break;

						case ECPGt_char:
						case ECPGt_unsigned_char:
							{
								if (varcharsize == 0)
								{
									/* char* */
									strncpy((char *) value, pval, strlen(pval));
									((char *) value)[strlen(pval)] = '\0';
								}
								else
									strncpy((char *) value, pval, varcharsize);
							}
							break;

						case ECPGt_varchar:
							{
								struct ECPGgeneric_varchar *var =
								(struct ECPGgeneric_varchar *) value;

								strncpy(var->arr, pval, varcharsize);
								var->len = strlen(pval);
								if (var->len > varcharsize)
									var->len = varcharsize;
							}
							break;

						case ECPGt_EORT:
							ECPGlog("ECPGdo line %d: Too few arguments.\n", lineno);
							register_error(-1, "Too few arguments line %d.", lineno);
							status = false;
							break;

						default:
							register_error(-1, "Unsupported type %s on line %d.",
										   ECPGtype_name(type), lineno);
							return false;
							break;
					}
				}

				type = va_arg(ap, enum ECPGttype);

				if (status && type != ECPGt_EORT)
				{
					register_error(-1, "Too many arguments line %d.", lineno);
					return false;
				}

				PQclear(results);
				break;
			case PGRES_EMPTY_QUERY:
				/* do nothing */
				register_error(-1, "Empty query line %d.", lineno);
				break;
			case PGRES_COMMAND_OK:
				status = true;
				ECPGlog("ECPGdo line %d Ok: %s\n", lineno, PQcmdStatus(results));
				break;
			case PGRES_NONFATAL_ERROR:
			case PGRES_FATAL_ERROR:
			case PGRES_BAD_RESPONSE:
				ECPGlog("ECPGdo line %d: Error: %s",
						lineno, PQerrorMessage(simple_connection));
				register_error(-1, "Error: %s line %d.",
							   PQerrorMessage(simple_connection), lineno);
				break;
			case PGRES_COPY_OUT:
				ECPGlog("ECPGdo line %d: Got PGRES_COPY_OUT ... tossing.\n", lineno);
				PQendcopy(results->conn);
				break;
			case PGRES_COPY_IN:
				ECPGlog("ECPGdo line %d: Got PGRES_COPY_IN ... tossing.\n", lineno);
				PQendcopy(results->conn);
				break;
			default:
				ECPGlog("ECPGdo line %d: Got something else, postgres error.\n",
						lineno);
				register_error(-1, "Postgres error line %d.", lineno);
				break;
		}

	/* check for asynchronous returns */
	notify = PQnotifies(simple_connection);
	if (notify)
	{
		ECPGlog("ECPGdo line %d: ASYNC NOTIFY of '%s' from backend pid '%d' received\n",
				lineno, notify->relname, notify->be_pid);
		free(notify);
	}

	va_end(ap);
	return status;
}


bool
ECPGtrans(int lineno, const char * transaction)
{
	PGresult   *res;

	ECPGlog("ECPGtrans line %d action = %s\n", lineno, transaction);
	if ((res = PQexec(simple_connection, transaction)) == NULL)
	{
		register_error(-1, "Error in transaction processing line %d.", lineno);
		return (FALSE);
	}
	PQclear(res);
	committed = 1;
	return (TRUE);
}

/* include these for compatibility */
bool
ECPGcommit(int lineno)
{
	return(ECPGtrans(lineno, "end"));
}

bool
ECPGrollback(int lineno)
{
	return(ECPGtrans(lineno, "abort"));
}

bool
ECPGsetdb(PGconn *newcon)
{
	ECPGfinish();
	simple_connection = newcon;
	return true;
}

bool
ECPGconnect(const char *dbname)
{
	char	   *name = strdup(dbname);

	ECPGlog("ECPGconnect: opening database %s\n", name);

	sqlca.sqlcode = 0;

	ECPGsetdb(PQsetdb(NULL, NULL, NULL, NULL, name));

	free(name);
	name = NULL;

	if (PQstatus(simple_connection) == CONNECTION_BAD)
	{
		ECPGfinish();
		ECPGlog("ECPGconnect: could not open database %s\n", dbname);
		register_error(-1, "ECPGconnect: could not open database %s.", dbname);
		return false;
	}
	return true;
}


bool
ECPGstatus(void)
{
	return PQstatus(simple_connection) != CONNECTION_BAD;
}


bool
ECPGfinish(void)
{
	if (simple_connection != NULL)
	{
		ECPGlog("ECPGfinish: finishing.\n");
		PQfinish(simple_connection);
	}
	else
		ECPGlog("ECPGfinish: called an extra time.\n");
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

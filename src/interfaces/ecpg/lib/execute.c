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

/* Taken over as part of PostgreSQL by Michael Meskes <meskes@postgresql.org>
   on Feb. 5th, 1998 */

#include <stdio.h>
#include <locale.h>

#include "pg_type.h"

#include "ecpgtype.h"
#include "ecpglib.h"
#include "ecpgerrno.h"
#include "extern.h"
#include "sqlca.h"
#include "sql3types.h"

/* variables visible to the programs */
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

/* keep a list of memory we allocated for the user */
static struct auto_mem
{
	void	   *pointer;
	struct auto_mem *next;
}		   *auto_allocs = NULL;

static void
add_mem(void *ptr, int lineno)
{
	struct auto_mem *am = (struct auto_mem *) ecpg_alloc(sizeof(struct auto_mem), lineno);

	am->next = auto_allocs;
	auto_allocs = am;
}

void
free_auto_mem(void)
{
	struct auto_mem *am;

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

/* This function returns a newly malloced string that has the  \
   in the argument quoted with \ and the ' quote with ' as SQL92 says.
 */
static
char *
quote_postgres(char *arg, int lineno)
{
	char	   *res = (char *) ecpg_alloc(2 * strlen(arg) + 3, lineno);
	int			i,
				ri = 0;

	if (!res)
		return (res);

	res[ri++] = '\'';
	for (i = 0; arg[i]; i++, ri++)
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
	res[ri++] = '\'';
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
				ECPGraise(lineno, ECPG_INVALID_STMT, NULL);
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
		if (*ptr == '\'' && *(ptr - 1) != '\\')
			string = string ? false : true;

	return (*ptr == '\0') ? NULL : ptr;
}

/*
 * push a value on the cache
 */

static void
ECPGtypeinfocache_push(struct ECPGtype_information_cache **cache, int oid, bool isarray, int lineno)
{
	struct ECPGtype_information_cache	*new_entry 
						= ecpg_alloc(sizeof(struct ECPGtype_information_cache), lineno);
	new_entry->oid = oid;
	new_entry->isarray = isarray;
	new_entry->next = *cache;
	*cache = new_entry;
}

static bool
ECPGis_type_an_array(int type,const struct statement * stmt,const struct variable *var)
{
	char	   *array_query;
	int		isarray = 0;
	PGresult	*query;
	struct ECPGtype_information_cache	*cache_entry;
	
	if ((stmt->connection->cache_head)==NULL)
	{	
		/* populate cache with well known types to speed things up */
		ECPGtypeinfocache_push(&(stmt->connection->cache_head),  BOOLOID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head),  BYTEAOID, true, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head),  CHAROID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head),  NAMEOID, true, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head),  INT8OID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head),  INT2OID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head),  INT2VECTOROID, true, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head),  INT4OID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head),  REGPROCOID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head),  TEXTOID, true, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head),  OIDOID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head),  TIDOID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head),  XIDOID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head),  CIDOID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head),  OIDVECTOROID, true, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head),  POINTOID, true, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head),  LSEGOID, true, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head),  PATHOID, true, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head),  BOXOID, true, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head),  POLYGONOID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head),  LINEOID, true, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head),  FLOAT4OID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head),  FLOAT8OID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head),  ABSTIMEOID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head),  RELTIMEOID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head),  TINTERVALOID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head),  UNKNOWNOID, true, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head),  CIRCLEOID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head),  CASHOID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head),  INETOID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head),  CIDROID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head),  BPCHAROID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head),  VARCHAROID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head),  DATEOID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head),  TIMEOID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head),  TIMESTAMPOID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head),  INTERVALOID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head),  TIMETZOID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head),  ZPBITOID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head),  VARBITOID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head),  NUMERICOID, false, stmt->lineno);
	}

	for (cache_entry = (stmt->connection->cache_head);cache_entry != NULL;cache_entry=cache_entry->next)
	{
		if (cache_entry->oid==type) 
			return cache_entry->isarray;
	}
		
	array_query = (char *) ecpg_alloc(strlen("select typelem from pg_type where oid=") + 11, stmt->lineno);
	sprintf(array_query, "select typelem from pg_type where oid=%d", type);
	query = PQexec(stmt->connection->connection, array_query);
	if (PQresultStatus(query) == PGRES_TUPLES_OK)
	{
		isarray = atol((char *) PQgetvalue(query, 0, 0));
		if (ECPGDynamicType(type) == SQL3_CHARACTER ||
			ECPGDynamicType(type) == SQL3_CHARACTER_VARYING)
		{

			/*
			 * arrays of character strings are not yet
			 * implemented
			 */
			isarray = false;
		}
		ECPGlog("ECPGexecute line %d: TYPE database: %d C: %d array: %s\n", stmt->lineno, type, var->type, isarray ? "yes" : "no");
		ECPGtypeinfocache_push(&(stmt->connection->cache_head), type, isarray, stmt->lineno);
	}
	PQclear(query);
	return isarray;
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
			case ECPGt_long_long:
			case ECPGt_unsigned_long_long:
	                        if (*(long long int*) var->ind_value < 0LL)
					strcpy(buff, "null");
	                        break;
			default:
				break;
		}

		if (*buff == '\0')
		{
			switch (var->type)
			{
					int			element;

				case ECPGt_short:
					if (!(mallocedval = ecpg_alloc(var->arrsize * 20, stmt->lineno)))
						return false;

					if (var->arrsize > 1)
					{
						strncpy(mallocedval, "'{", sizeof("'{"));

						for (element = 0; element < var->arrsize; element++)
							sprintf(mallocedval + strlen(mallocedval), "%hd,", ((short *) var->value)[element]);

						strncpy(mallocedval + strlen(mallocedval) - 1, "}'", sizeof("}'"));
					}
					else
						sprintf(mallocedval, "%hd", *((short *) var->value));

					tobeinserted = mallocedval;
					break;

				case ECPGt_int:
					if (!(mallocedval = ecpg_alloc(var->arrsize * 20, stmt->lineno)))
						return false;

					if (var->arrsize > 1)
					{
						strncpy(mallocedval, "'{", sizeof("'{"));

						for (element = 0; element < var->arrsize; element++)
							sprintf(mallocedval + strlen(mallocedval), "%d,", ((int *) var->value)[element]);

						strncpy(mallocedval + strlen(mallocedval) - 1, "}'", sizeof("}'"));
					}
					else
						sprintf(mallocedval, "%d", *((int *) var->value));

					tobeinserted = mallocedval;
					break;

				case ECPGt_unsigned_short:
					if (!(mallocedval = ecpg_alloc(var->arrsize * 20, stmt->lineno)))
						return false;

					if (var->arrsize > 1)
					{
						strncpy(mallocedval, "'{", sizeof("'{"));

						for (element = 0; element < var->arrsize; element++)
							sprintf(mallocedval + strlen(mallocedval), "%hu,", ((unsigned short *) var->value)[element]);

						strncpy(mallocedval + strlen(mallocedval) - 1, "}'", sizeof("}'"));
					}
					else
						sprintf(mallocedval, "%hu", *((unsigned short *) var->value));

					tobeinserted = mallocedval;
					break;

				case ECPGt_unsigned_int:
					if (!(mallocedval = ecpg_alloc(var->arrsize * 20, stmt->lineno)))
						return false;

					if (var->arrsize > 1)
					{
						strncpy(mallocedval, "'{", sizeof("'{"));

						for (element = 0; element < var->arrsize; element++)
							sprintf(mallocedval + strlen(mallocedval), "%u,", ((unsigned int *) var->value)[element]);

						strncpy(mallocedval + strlen(mallocedval) - 1, "}'", sizeof("}'"));
					}
					else
						sprintf(mallocedval, "%u", *((unsigned int *) var->value));

					tobeinserted = mallocedval;
					break;

				case ECPGt_long:
					if (!(mallocedval = ecpg_alloc(var->arrsize * 20, stmt->lineno)))
						return false;

					if (var->arrsize > 1)
					{
						strncpy(mallocedval, "'{", sizeof("'{"));

						for (element = 0; element < var->arrsize; element++)
							sprintf(mallocedval + strlen(mallocedval), "%ld,", ((long *) var->value)[element]);

						strncpy(mallocedval + strlen(mallocedval) - 1, "}'", sizeof("}'"));
					}
					else
						sprintf(mallocedval, "%ld", *((long *) var->value));

					tobeinserted = mallocedval;
					break;

				case ECPGt_unsigned_long:
					if (!(mallocedval = ecpg_alloc(var->arrsize * 20, stmt->lineno)))
						return false;

					if (var->arrsize > 1)
					{
						strncpy(mallocedval, "'{", sizeof("'{"));

						for (element = 0; element < var->arrsize; element++)
							sprintf(mallocedval + strlen(mallocedval), "%lu,", ((unsigned long *) var->value)[element]);

						strncpy(mallocedval + strlen(mallocedval) - 1, "}'", sizeof("}'"));
					}
					else
						sprintf(mallocedval, "%lu", *((unsigned long *) var->value));

					tobeinserted = mallocedval;
					break;
					
				case ECPGt_long_long:
					if (!(mallocedval = ecpg_alloc(var->arrsize * 25, stmt->lineno)))
						return false;

					if (var->arrsize > 1)
					{
						strncpy(mallocedval, "'{", sizeof("'{"));

						for (element = 0; element < var->arrsize; element++)
							sprintf(mallocedval + strlen(mallocedval), "%lld,", ((long long *) var->value)[element]);

						strncpy(mallocedval + strlen(mallocedval) - 1, "}'", sizeof("}'"));
					}
					else
						sprintf(mallocedval, "%lld", *((long long *) var->value));

					tobeinserted = mallocedval;
					break;

				case ECPGt_unsigned_long_long:
					if (!(mallocedval = ecpg_alloc(var->arrsize * 25, stmt->lineno)))
						return false;

					if (var->arrsize > 1)
					{
						strncpy(mallocedval, "'{", sizeof("'{"));

						for (element = 0; element < var->arrsize; element++)
							sprintf(mallocedval + strlen(mallocedval), "%llu,", ((unsigned long long *) var->value)[element]);

						strncpy(mallocedval + strlen(mallocedval) - 1, "}'", sizeof("}'"));
					}
					else
						sprintf(mallocedval, "%llu", *((unsigned long long*) var->value));

					tobeinserted = mallocedval;
					break;

				case ECPGt_float:
					if (!(mallocedval = ecpg_alloc(var->arrsize * 20, stmt->lineno)))
						return false;

					if (var->arrsize > 1)
					{
						strncpy(mallocedval, "'{", sizeof("'{"));

						for (element = 0; element < var->arrsize; element++)
							sprintf(mallocedval + strlen(mallocedval), "%.14g,", ((float *) var->value)[element]);

						strncpy(mallocedval + strlen(mallocedval) - 1, "}'", sizeof("}'"));
					}
					else
						sprintf(mallocedval, "%.14g", *((float *) var->value));

					tobeinserted = mallocedval;
					break;

				case ECPGt_double:
					if (!(mallocedval = ecpg_alloc(var->arrsize * 20, stmt->lineno)))
						return false;

					if (var->arrsize > 1)
					{
						strncpy(mallocedval, "'{", sizeof("'{"));

						for (element = 0; element < var->arrsize; element++)
							sprintf(mallocedval + strlen(mallocedval), "%.14g,", ((double *) var->value)[element]);

						strncpy(mallocedval + strlen(mallocedval) - 1, "}'", sizeof("}'"));
					}
					else
						sprintf(mallocedval, "%.14g", *((double *) var->value));

					tobeinserted = mallocedval;
					break;

				case ECPGt_bool:
					if (!(mallocedval = ecpg_alloc(var->arrsize * 20, stmt->lineno)))
						return false;

					if (var->arrsize > 1)
					{
						strncpy(mallocedval, "'{", sizeof("'{"));

						for (element = 0; element < var->arrsize; element++)
							sprintf(mallocedval + strlen(mallocedval), "%c,", (((char *) var->value)[element]) ? 't' : 'f');

						strncpy(mallocedval + strlen(mallocedval) - 1, "}'", sizeof("}'"));
					}
					else
						sprintf(mallocedval, "'%c'", (*((char *) var->value)) ? 't' : 'f');

					tobeinserted = mallocedval;
					break;

				case ECPGt_char:
				case ECPGt_unsigned_char:
					{
						/* set slen to string length if type is char * */
						int			slen = (var->varcharsize == 0) ? strlen((char *) var->value) : var->varcharsize;

						if (!(newcopy = ecpg_alloc(slen + 1, stmt->lineno)))
							return false;

						strncpy(newcopy, (char *) var->value, slen);
						newcopy[slen] = '\0';

						mallocedval = quote_postgres(newcopy, stmt->lineno);
						if (!mallocedval)
							return false;

						free(newcopy);

						tobeinserted = mallocedval;
					}
					break;
				case ECPGt_char_variable:
					{
						int			slen = strlen((char *) var->value);

						if (!(mallocedval = ecpg_alloc(slen + 1, stmt->lineno)))
							return false;

						strncpy(mallocedval, (char *) var->value, slen);
						mallocedval[slen] = '\0';

						tobeinserted = mallocedval;
					}
					break;
				case ECPGt_varchar:
					{
						struct ECPGgeneric_varchar *variable =
						(struct ECPGgeneric_varchar *) (var->value);

						if (!(newcopy = (char *) ecpg_alloc(variable->len + 1, stmt->lineno)))
							return false;

						strncpy(newcopy, variable->arr, variable->len);
						newcopy[variable->len] = '\0';

						mallocedval = quote_postgres(newcopy, stmt->lineno);
						if (!mallocedval)
							return false;

						free(newcopy);

						tobeinserted = mallocedval;
					}
					break;

				default:
					/* Not implemented yet */
					ECPGraise(stmt->lineno, ECPG_UNSUPPORTED, (char *) ECPGtype_name(var->type));
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
			ECPGraise(stmt->lineno, ECPG_TOO_MANY_ARGUMENTS, NULL);
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
		ECPGraise(stmt->lineno, ECPG_TOO_FEW_ARGUMENTS, NULL);
		return false;
	}

	/* Now the request is built. */

	if (stmt->connection->committed && !stmt->connection->autocommit)
	{
		if ((results = PQexec(stmt->connection->connection, "begin transaction")) == NULL)
		{
			ECPGraise(stmt->lineno, ECPG_TRANS, NULL);
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
		ECPGraise(stmt->lineno, ECPG_PGSQL, PQerrorMessage(stmt->connection->connection));
	}
	else
	{
		var = stmt->outlist;
		switch (PQresultStatus(results))
		{
				int			nfields,
							ntuples,
							act_tuple,
							act_field,
							isarray;

			case PGRES_TUPLES_OK:
				nfields = PQnfields(results);
				sqlca.sqlerrd[2] = ntuples = PQntuples(results);
				status = true;

				if (ntuples < 1)
				{
					ECPGlog("ECPGexecute line %d: Incorrect number of matches: %d\n",
							stmt->lineno, ntuples);
					ECPGraise(stmt->lineno, ECPG_NOT_FOUND, NULL);
					status = false;
					break;
				}

				for (act_field = 0; act_field < nfields && status; act_field++)
				{
					if (var == NULL)
					{
						ECPGlog("ECPGexecute line %d: Too few arguments.\n", stmt->lineno);
						ECPGraise(stmt->lineno, ECPG_TOO_FEW_ARGUMENTS, NULL);
						return (false);
					}

					isarray = ECPGis_type_an_array(PQftype(results, act_field), stmt, var);

					if (!isarray)
					{

						/*
						 * if we don't have enough space, we cannot read
						 * all tuples
						 */
						if ((var->arrsize > 0 && ntuples > var->arrsize) || (var->ind_arrsize > 0 && ntuples > var->ind_arrsize))
						{
							ECPGlog("ECPGexecute line %d: Incorrect number of matches: %d don't fit into array of %d\n",
									stmt->lineno, ntuples, var->arrsize);
							ECPGraise(stmt->lineno, ECPG_TOO_MANY_MATCHES, NULL);
							status = false;
							break;
						}
					}
					else
					{

						/*
						 * since we read an array, the variable has to be
						 * an array too
						 */
						if (var->arrsize == 0)
						{
							ECPGlog("ECPGexecute line %d: variable is not an array\n");
							ECPGraise(stmt->lineno, ECPG_NO_ARRAY, NULL);
							status = false;
							break;
						}
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
						if (!get_data(results, act_tuple, act_field, stmt->lineno,
									var->type, var->ind_type, var->value,
									  var->ind_value, var->varcharsize, var->offset, isarray))
							status = false;
					}
					var = var->next;
				}

				if (status && var != NULL)
				{
					ECPGraise(stmt->lineno, ECPG_TOO_MANY_ARGUMENTS, NULL);
					status = false;
				}

				break;
			case PGRES_EMPTY_QUERY:
				/* do nothing */
				ECPGraise(stmt->lineno, ECPG_EMPTY, NULL);
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
				ECPGraise(stmt->lineno, ECPG_PGSQL, PQerrorMessage(stmt->connection->connection));
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
				ECPGraise(stmt->lineno, ECPG_PGSQL, PQerrorMessage(stmt->connection->connection));
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
	bool		status = true;
	char	   *locale = setlocale(LC_NUMERIC, NULL);

	/* Make sure we do NOT honor the locale for numeric input/output */
	/* since the database wants teh standard decimal point */
	setlocale(LC_NUMERIC, "C");

	if (!ecpg_init(con, connection_name, lineno))
	{
		setlocale(LC_NUMERIC, locale);
		return (false);
	}

	va_start(args, query);
	if (create_statement(lineno, con, &stmt, query, args) == false)
	{
		setlocale(LC_NUMERIC, locale);
		return (false);
	}
	va_end(args);

	/* are we connected? */
	if (con == NULL || con->connection == NULL)
	{
		free_statement(stmt);
		ECPGlog("ECPGdo: not connected to %s\n", con->name);
		ECPGraise(lineno, ECPG_NOT_CONN, NULL);
		setlocale(LC_NUMERIC, locale);
		return false;
	}

	status = ECPGexecute(stmt);
	free_statement(stmt);

	/* and reset locale value so our application is not affected */
	setlocale(LC_NUMERIC, locale);
	return (status);
}

/* dynamic SQL support routines
 *
 * Copyright (c) 2000, Christof Petig <christof.petig@wtal.de>
 *
 * $Header: /cvsroot/pgsql/src/interfaces/ecpg/lib/Attic/execute.c,v 1.9 2000/09/20 13:25:51 meskes Exp $
 */

PGconn	   *ECPG_internal_get_connection(char *name);

extern struct descriptor
{
	char	   *name;
	PGresult   *result;
	struct descriptor *next;
}		   *all_descriptors;

/* like ECPGexecute */
static bool
execute_descriptor(int lineno, const char *query
				   ,struct connection * con, PGresult **resultptr)
{
	bool		status = false;
	PGresult   *results;
	PGnotify   *notify;

	/* Now the request is built. */

	if (con->committed && !con->autocommit)
	{
		if ((results = PQexec(con->connection, "begin transaction")) == NULL)
		{
			ECPGraise(lineno, ECPG_TRANS, NULL);
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
		ECPGraise(lineno, ECPG_PGSQL, PQerrorMessage(con->connection));
	}
	else
	{
		*resultptr = results;
		switch (PQresultStatus(results))
		{
				int			ntuples;

			case PGRES_TUPLES_OK:
				status = true;
				sqlca.sqlerrd[2] = ntuples = PQntuples(results);
				if (ntuples < 1)
				{
					ECPGlog("execute_descriptor line %d: Incorrect number of matches: %d\n",
							lineno, ntuples);
					ECPGraise(lineno, ECPG_NOT_FOUND, NULL);
					status = false;
					break;
				}
				break;
#if 1							/* strictly these are not needed (yet) */
			case PGRES_EMPTY_QUERY:
				/* do nothing */
				ECPGraise(lineno, ECPG_EMPTY, NULL);
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
				ECPGraise(lineno, ECPG_PGSQL, PQerrorMessage(con->connection));
				status = false;
				break;
			default:
				ECPGlog("ECPGexecute line %d: Got something else, postgres error.\n",
						lineno);
				ECPGraise(lineno, ECPG_PGSQL, PQerrorMessage(con->connection));
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
static bool
do_descriptor2(int lineno, const char *connection_name,
			   PGresult **resultptr, const char *query)
{
	struct connection *con = get_connection(connection_name);
	bool		status = true;
	char	   *locale = setlocale(LC_NUMERIC, NULL);

	/* Make sure we do NOT honor the locale for numeric input/output */
	/* since the database wants teh standard decimal point */
	setlocale(LC_NUMERIC, "C");

	if (!ecpg_init(con, connection_name, lineno))
	{
		setlocale(LC_NUMERIC, locale);
		return (false);
	}

	/* are we connected? */
	if (con == NULL || con->connection == NULL)
	{
		ECPGlog("do_descriptor2: not connected to %s\n", con->name);
		ECPGraise(lineno, ECPG_NOT_CONN, NULL);
		setlocale(LC_NUMERIC, locale);
		return false;
	}

	status = execute_descriptor(lineno, query, con, resultptr);

	/* and reset locale value so our application is not affected */
	setlocale(LC_NUMERIC, locale);
	return (status);
}

bool
ECPGdo_descriptor(int line, const char *connection,
				  const char *descriptor, const char *query)
{
	struct descriptor *i;

	for (i = all_descriptors; i != NULL; i = i->next)
	{
		if (!strcmp(descriptor, i->name))
		{
			bool		status;

			/* free previous result */
			if (i->result)
				PQclear(i->result);
			i->result = NULL;

			status = do_descriptor2(line, connection, &i->result, query);

			if (!i->result)
				PQmakeEmptyPGresult(NULL, 0);
			return (status);
		}
	}

	ECPGraise(line, ECPG_UNKNOWN_DESCRIPTOR, (char *) descriptor);
	return false;
}

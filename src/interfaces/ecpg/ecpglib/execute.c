/* $Header: /cvsroot/pgsql/src/interfaces/ecpg/ecpglib/execute.c,v 1.21 2003/08/01 08:21:04 meskes Exp $ */

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

#define POSTGRES_ECPG_INTERNAL
#include "postgres_fe.h"

#include <stdio.h>
#include <locale.h>

#include "pg_type.h"

#include "ecpgtype.h"
#include "ecpglib.h"
#include "ecpgerrno.h"
#include "extern.h"
#include "sqlca.h"
#include "sql3types.h"
#include "pgtypes_numeric.h"
#include "pgtypes_date.h"
#include "pgtypes_timestamp.h"
#include "pgtypes_interval.h"

/* This function returns a newly malloced string that has the  \
   in the argument quoted with \ and the ' quoted with ' as SQL92 says.
 */
static char *
quote_postgres(char *arg, int lineno)
{
	char	   *res = (char *) ECPGalloc(2 * strlen(arg) + 3, lineno);
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
 * The variables are listed with input variables preceding outputvariables
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
create_statement(int lineno, int compat, int force_indicator, struct connection * connection, struct statement ** stmt, char *query, va_list ap)
{
	struct variable **list = &((*stmt)->inlist);
	enum ECPGttype type;

	if (!(*stmt = (struct statement *) ECPGalloc(sizeof(struct statement), lineno)))
		return false;

	(*stmt)->command = query;
	(*stmt)->connection = connection;
	(*stmt)->lineno = lineno;
	(*stmt)->compat = compat;
	(*stmt)->force_indicator = force_indicator;

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

			if (!(var = (struct variable *) ECPGalloc(sizeof(struct variable), lineno)))
				return false;

			var->type = type;
			var->pointer = va_arg(ap, char *);

			/* if variable is NULL, the statement hasn't been prepared */
			if (var->pointer == NULL)
			{
				ECPGraise(lineno, ECPG_INVALID_STMT, NULL, compat);
				ECPGfree(var);
				return false;
			}

			var->varcharsize = va_arg(ap, long);
			var->arrsize = va_arg(ap, long);
			var->offset = va_arg(ap, long);

			if (var->arrsize == 0 || var->varcharsize == 0)
				var->value = *((char **) (var->pointer));
			else
				var->value = var->pointer;

			/* negative values are used to indicate an array without given bounds */
			/* reset to zero for us */
			if (var->arrsize < 0)
				var->arrsize = 0;
			if (var->varcharsize < 0)
				var->varcharsize = 0;
		
			var->ind_type = va_arg(ap, enum ECPGttype);
			var->ind_pointer = va_arg(ap, char *);
			var->ind_varcharsize = va_arg(ap, long);
			var->ind_arrsize = va_arg(ap, long);
			var->ind_offset = va_arg(ap, long);
			var->next = NULL;

			if (var->ind_type != ECPGt_NO_INDICATOR
				&& (var->ind_arrsize == 0 || var->ind_varcharsize == 0))
				var->ind_value = *((char **) (var->ind_pointer));
			else
				var->ind_value = var->ind_pointer;
			
			/* negative values are used to indicate an array without given bounds */
			/* reset to zero for us */
			if (var->ind_arrsize < 0)
				var->ind_arrsize = 0;
			if (var->ind_varcharsize < 0)
				var->ind_varcharsize = 0;

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
	ECPGfree(var);

	while (var_next)
	{
		var = var_next;
		var_next = var->next;
		ECPGfree(var);
	}
}

static void
free_statement(struct statement * stmt)
{
	if (stmt == (struct statement *) NULL)
		return;
	free_variable(stmt->inlist);
	free_variable(stmt->outlist);
	ECPGfree(stmt);
}

static char *
next_insert(char *text)
{
	char	   *ptr = text;
	bool		string = false;

	for (; *ptr != '\0' && (*ptr != '?' || string); ptr++)
	{
		if (*ptr == '\\')		/* escape character */
			ptr++;
		else if (*ptr == '\'')
			string = string ? false : true;
	}

	return (*ptr == '\0') ? NULL : ptr;
}

/*
 * push a value on the cache
 */

static void
ECPGtypeinfocache_push(struct ECPGtype_information_cache ** cache, int oid, bool isarray, int lineno)
{
	struct ECPGtype_information_cache *new_entry
	= (struct ECPGtype_information_cache *) ECPGalloc(sizeof(struct ECPGtype_information_cache), lineno);

	new_entry->oid = oid;
	new_entry->isarray = isarray;
	new_entry->next = *cache;
	*cache = new_entry;
}

static bool
ECPGis_type_an_array(int type, const struct statement * stmt, const struct variable * var)
{
	char	   *array_query;
	int			isarray = 0;
	PGresult   *query;
	struct ECPGtype_information_cache *cache_entry;

	if ((stmt->connection->cache_head) == NULL)
	{
		/*
		 * Text like types are not an array for ecpg, but postgres counts
		 * them as an array. This define reminds you to not 'correct'
		 * these values.
		 */
#define not_an_array_in_ecpg false

		/* populate cache with well known types to speed things up */
		ECPGtypeinfocache_push(&(stmt->connection->cache_head), BOOLOID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head), BYTEAOID, not_an_array_in_ecpg, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head), CHAROID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head), NAMEOID, not_an_array_in_ecpg, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head), INT8OID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head), INT2OID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head), INT2VECTOROID, true, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head), INT4OID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head), REGPROCOID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head), TEXTOID, not_an_array_in_ecpg, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head), OIDOID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head), TIDOID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head), XIDOID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head), CIDOID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head), OIDVECTOROID, true, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head), POINTOID, true, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head), LSEGOID, true, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head), PATHOID, not_an_array_in_ecpg, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head), BOXOID, true, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head), POLYGONOID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head), LINEOID, true, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head), FLOAT4OID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head), FLOAT8OID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head), ABSTIMEOID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head), RELTIMEOID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head), TINTERVALOID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head), UNKNOWNOID, not_an_array_in_ecpg, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head), CIRCLEOID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head), CASHOID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head), INETOID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head), CIDROID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head), BPCHAROID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head), VARCHAROID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head), DATEOID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head), TIMEOID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head), TIMESTAMPOID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head), TIMESTAMPTZOID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head), INTERVALOID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head), TIMETZOID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head), ZPBITOID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head), VARBITOID, false, stmt->lineno);
		ECPGtypeinfocache_push(&(stmt->connection->cache_head), NUMERICOID, false, stmt->lineno);
	}

	for (cache_entry = (stmt->connection->cache_head); cache_entry != NULL; cache_entry = cache_entry->next)
	{
		if (cache_entry->oid == type)
			return cache_entry->isarray;
	}

	array_query = (char *) ECPGalloc(strlen("select typelem from pg_type where oid=") + 11, stmt->lineno);
	sprintf(array_query, "select typelem from pg_type where oid=%d", type);
	query = PQexec(stmt->connection->connection, array_query);
	ECPGfree(array_query);
	if (PQresultStatus(query) == PGRES_TUPLES_OK)
	{
		isarray = atol((char *) PQgetvalue(query, 0, 0));
		if (ECPGDynamicType(type) == SQL3_CHARACTER ||
			ECPGDynamicType(type) == SQL3_CHARACTER_VARYING)
		{
			/*
			 * arrays of character strings are not yet implemented
			 */
			isarray = false;
		}
		ECPGlog("ECPGexecute line %d: TYPE database: %d C: %d array: %s\n", stmt->lineno, type, var->type, isarray ? "yes" : "no");
		ECPGtypeinfocache_push(&(stmt->connection->cache_head), type, isarray, stmt->lineno);
	}
	PQclear(query);
	return isarray;
}


bool
ECPGstore_result(const PGresult *results, int act_field,
				 const struct statement * stmt, struct variable * var)
{
	int			isarray,
				act_tuple,
				ntuples = PQntuples(results);
	bool		status = true;

	isarray = ECPGis_type_an_array(PQftype(results, act_field), stmt, var);

	if (!isarray)
	{
		/*
		 * if we don't have enough space, we cannot read all tuples
		 */
		if ((var->arrsize > 0 && ntuples > var->arrsize) || (var->ind_arrsize > 0 && ntuples > var->ind_arrsize))
		{
			ECPGlog("ECPGexecute line %d: Incorrect number of matches: %d don't fit into array of %d\n",
					stmt->lineno, ntuples, var->arrsize);
			ECPGraise(stmt->lineno, ECPG_TOO_MANY_MATCHES, NULL, ECPG_COMPAT_PGSQL);
			return false;
		}
	}
	else
	{
		/*
		 * since we read an array, the variable has to be an array too
		 */
		if (var->arrsize == 0)
		{
			ECPGraise(stmt->lineno, ECPG_NO_ARRAY, NULL, ECPG_COMPAT_PGSQL);
			return false;
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
				if (!var->varcharsize && !var->arrsize)
				{
					/* special mode for handling char**foo=0 */
					for (act_tuple = 0; act_tuple < ntuples; act_tuple++)
						len += strlen(PQgetvalue(results, act_tuple, act_field)) + 1;
					len *= var->offset; /* should be 1, but YMNK */
					len += (ntuples + 1) * sizeof(char *);

					ECPGlog("ECPGstore_result: line %d: allocating %d bytes for %d tuples (char**=0)",
							stmt->lineno, len, ntuples);
				}
				else
				{
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
				}
				break;
			case ECPGt_varchar:
				len = ntuples * (var->varcharsize + sizeof(int));
				break;
			default:
				len = var->offset * ntuples;
				break;
		}
		var->value = (char *) ECPGalloc(len, stmt->lineno);
		*((char **) var->pointer) = var->value;
		ECPGadd_mem(var->value, stmt->lineno);
	}

	/* allocate indicator variable if needed */
	if ((var->ind_arrsize == 0 || var->ind_varcharsize == 0) && var->ind_value == NULL && var->ind_pointer != NULL)
	{
		int			len = var->ind_offset * ntuples;

		var->ind_value = (char *) ECPGalloc(len, stmt->lineno);
		*((char **) var->ind_pointer) = var->ind_value;
		ECPGadd_mem(var->ind_value, stmt->lineno);
	}

	/* fill the variable with the tuple(s) */
	if (!var->varcharsize && !var->arrsize &&
		(var->type == ECPGt_char || var->type == ECPGt_unsigned_char))
	{
		/* special mode for handling char**foo=0 */

		/* filling the array of (char*)s */
		char	  **current_string = (char **) var->value;

		/* storing the data (after the last array element) */
		char	   *current_data_location = (char *) &current_string[ntuples + 1];

		for (act_tuple = 0; act_tuple < ntuples && status; act_tuple++)
		{
			int			len = strlen(PQgetvalue(results, act_tuple, act_field)) + 1;

			if (!ECPGget_data(results, act_tuple, act_field, stmt->lineno,
						 var->type, var->ind_type, current_data_location,
							  var->ind_value, len, 0, 0, isarray, stmt->compat, stmt->force_indicator))
				status = false;
			else
			{
				*current_string = current_data_location;
				current_data_location += len;
				current_string++;
			}
		}

		/* terminate the list */
		*current_string = NULL;
	}
	else
	{
		for (act_tuple = 0; act_tuple < ntuples && status; act_tuple++)
		{
			if (!ECPGget_data(results, act_tuple, act_field, stmt->lineno,
							  var->type, var->ind_type, var->value,
							  var->ind_value, var->varcharsize, var->offset, var->ind_offset, isarray, stmt->compat, stmt->force_indicator))
				status = false;
		}
	}
	return status;
}

static bool
ECPGstore_input(const struct statement * stmt, const struct variable * var,
				const char **tobeinserted_p, bool *malloced_p)
{
	char	   *mallocedval = NULL;
	char	   *newcopy = NULL;

	/*
	 * arrays are not possible unless the attribute is an array too FIXME:
	 * we do not know if the attribute is an array here
	 */

/*	 if (var->arrsize > 1 && ...)
	 {
		ECPGraise(stmt->lineno, ECPG_ARRAY_INSERT, NULL, compat);
		return false;
	 }*/

	/*
	 * Some special treatment is needed for records since we want their
	 * contents to arrive in a comma-separated list on insert (I think).
	 */

	*malloced_p = false;
	*tobeinserted_p = "";

	/* check for null value and set input buffer accordingly */
	switch (var->ind_type)
	{
		case ECPGt_short:
		case ECPGt_unsigned_short:
			if (*(short *) var->ind_value < 0)
				*tobeinserted_p = "null";
			break;
		case ECPGt_int:
		case ECPGt_unsigned_int:
			if (*(int *) var->ind_value < 0)
				*tobeinserted_p = "null";
			break;
		case ECPGt_long:
		case ECPGt_unsigned_long:
			if (*(long *) var->ind_value < 0L)
				*tobeinserted_p = "null";
			break;
#ifdef HAVE_LONG_LONG_INT_64
		case ECPGt_long_long:
		case ECPGt_unsigned_long_long:
			if (*(long long int *) var->ind_value < (long long) 0)
				*tobeinserted_p = "null";
			break;
#endif   /* HAVE_LONG_LONG_INT_64 */
		case ECPGt_NO_INDICATOR:
			if (stmt->force_indicator == false)
			{
				if (ECPGis_informix_null(var->type, var->value))
					*tobeinserted_p = "null";
			}
			break;
		default:
			break;
	}
	if (**tobeinserted_p == '\0')
	{
		switch (var->type)
		{
				int			element;

			case ECPGt_short:
				if (!(mallocedval = ECPGalloc(var->arrsize * 20, stmt->lineno)))
					return false;

				if (var->arrsize > 1)
				{
					strcpy(mallocedval, "array [");

					for (element = 0; element < var->arrsize; element++)
						sprintf(mallocedval + strlen(mallocedval), "%hd,", ((short *) var->value)[element]);

					strcpy(mallocedval + strlen(mallocedval) - 1, "]");
				}
				else
					sprintf(mallocedval, "%hd", *((short *) var->value));

				*tobeinserted_p = mallocedval;
				*malloced_p = true;
				break;

			case ECPGt_int:
				if (!(mallocedval = ECPGalloc(var->arrsize * 20, stmt->lineno)))
					return false;

				if (var->arrsize > 1)
				{
					strcpy(mallocedval, "array [");

					for (element = 0; element < var->arrsize; element++)
						sprintf(mallocedval + strlen(mallocedval), "%d,", ((int *) var->value)[element]);

					strcpy(mallocedval + strlen(mallocedval) - 1, "]");
				}
				else
					sprintf(mallocedval, "%d", *((int *) var->value));

				*tobeinserted_p = mallocedval;
				*malloced_p = true;
				break;

			case ECPGt_unsigned_short:
				if (!(mallocedval = ECPGalloc(var->arrsize * 20, stmt->lineno)))
					return false;

				if (var->arrsize > 1)
				{
					strcpy(mallocedval, "array [");

					for (element = 0; element < var->arrsize; element++)
						sprintf(mallocedval + strlen(mallocedval), "%hu,", ((unsigned short *) var->value)[element]);

					strcpy(mallocedval + strlen(mallocedval) - 1, "]");
				}
				else
					sprintf(mallocedval, "%hu", *((unsigned short *) var->value));

				*tobeinserted_p = mallocedval;
				*malloced_p = true;
				break;

			case ECPGt_unsigned_int:
				if (!(mallocedval = ECPGalloc(var->arrsize * 20, stmt->lineno)))
					return false;

				if (var->arrsize > 1)
				{
					strcpy(mallocedval, "array [");

					for (element = 0; element < var->arrsize; element++)
						sprintf(mallocedval + strlen(mallocedval), "%u,", ((unsigned int *) var->value)[element]);

					strcpy(mallocedval + strlen(mallocedval) - 1, "]");
				}
				else
					sprintf(mallocedval, "%u", *((unsigned int *) var->value));

				*tobeinserted_p = mallocedval;
				*malloced_p = true;
				break;

			case ECPGt_long:
				if (!(mallocedval = ECPGalloc(var->arrsize * 20, stmt->lineno)))
					return false;

				if (var->arrsize > 1)
				{
					strcpy(mallocedval, "array [");

					for (element = 0; element < var->arrsize; element++)
						sprintf(mallocedval + strlen(mallocedval), "%ld,", ((long *) var->value)[element]);

					strcpy(mallocedval + strlen(mallocedval) - 1, "]");
				}
				else
					sprintf(mallocedval, "%ld", *((long *) var->value));

				*tobeinserted_p = mallocedval;
				*malloced_p = true;
				break;

			case ECPGt_unsigned_long:
				if (!(mallocedval = ECPGalloc(var->arrsize * 20, stmt->lineno)))
					return false;

				if (var->arrsize > 1)
				{
					strcpy(mallocedval, "array [");

					for (element = 0; element < var->arrsize; element++)
						sprintf(mallocedval + strlen(mallocedval), "%lu,", ((unsigned long *) var->value)[element]);

					strcpy(mallocedval + strlen(mallocedval) - 1, "]");
				}
				else
					sprintf(mallocedval, "%lu", *((unsigned long *) var->value));

				*tobeinserted_p = mallocedval;
				*malloced_p = true;
				break;
#ifdef HAVE_LONG_LONG_INT_64
			case ECPGt_long_long:
				if (!(mallocedval = ECPGalloc(var->arrsize * 30, stmt->lineno)))
					return false;

				if (var->arrsize > 1)
				{
					strcpy(mallocedval, "array [");

					for (element = 0; element < var->arrsize; element++)
						sprintf(mallocedval + strlen(mallocedval), "%lld,", ((long long *) var->value)[element]);

					strcpy(mallocedval + strlen(mallocedval) - 1, "]");
				}
				else
					sprintf(mallocedval, "%lld", *((long long *) var->value));

				*tobeinserted_p = mallocedval;
				*malloced_p = true;
				break;

			case ECPGt_unsigned_long_long:
				if (!(mallocedval = ECPGalloc(var->arrsize * 30, stmt->lineno)))
					return false;

				if (var->arrsize > 1)
				{
					strcpy(mallocedval, "array [");

					for (element = 0; element < var->arrsize; element++)
						sprintf(mallocedval + strlen(mallocedval), "%llu,", ((unsigned long long *) var->value)[element]);

					strcpy(mallocedval + strlen(mallocedval) - 1, "]");
				}
				else
					sprintf(mallocedval, "%llu", *((unsigned long long *) var->value));

				*tobeinserted_p = mallocedval;
				*malloced_p = true;
				break;
#endif   /* HAVE_LONG_LONG_INT_64 */
			case ECPGt_float:
				if (!(mallocedval = ECPGalloc(var->arrsize * 25, stmt->lineno)))
					return false;

				if (var->arrsize > 1)
				{
					strcpy(mallocedval, "array [");

					for (element = 0; element < var->arrsize; element++)
						sprintf(mallocedval + strlen(mallocedval), "%.14g,", ((float *) var->value)[element]);

					strcpy(mallocedval + strlen(mallocedval) - 1, "]");
				}
				else
					sprintf(mallocedval, "%.14g", *((float *) var->value));

				*tobeinserted_p = mallocedval;
				*malloced_p = true;
				break;

			case ECPGt_double:
				if (!(mallocedval = ECPGalloc(var->arrsize * 25, stmt->lineno)))
					return false;

				if (var->arrsize > 1)
				{
					strcpy(mallocedval, "array [");

					for (element = 0; element < var->arrsize; element++)
						sprintf(mallocedval + strlen(mallocedval), "%.14g,", ((double *) var->value)[element]);

					strcpy(mallocedval + strlen(mallocedval) - 1, "]");
				}
				else
					sprintf(mallocedval, "%.14g", *((double *) var->value));

				*tobeinserted_p = mallocedval;
				*malloced_p = true;
				break;

			case ECPGt_bool:
				if (!(mallocedval = ECPGalloc(var->arrsize +sizeof ("array []"), stmt->lineno)))
					return false;

				if (var->arrsize > 1)
				{
					strcpy(mallocedval, "array [");

					if (var->offset == sizeof(char))
						for (element = 0; element < var->arrsize; element++)
							sprintf(mallocedval + strlen(mallocedval), "%c,", (((char *) var->value)[element]) ? 't' : 'f');

					/*
					 * this is necessary since sizeof(C++'s
					 * bool)==sizeof(int)
					 */
					else if (var->offset == sizeof(int))
						for (element = 0; element < var->arrsize; element++)
							sprintf(mallocedval + strlen(mallocedval), "%c,", (((int *) var->value)[element]) ? 't' : 'f');
					else
						ECPGraise(stmt->lineno, ECPG_CONVERT_BOOL, "different size", ECPG_COMPAT_PGSQL);

					strcpy(mallocedval + strlen(mallocedval) - 1, "]");
				}
				else
				{
					if (var->offset == sizeof(char))
						sprintf(mallocedval, "'%c'", (*((char *) var->value)) ? 't' : 'f');
					else if (var->offset == sizeof(int))
						sprintf(mallocedval, "'%c'", (*((int *) var->value)) ? 't' : 'f');
					else
						ECPGraise(stmt->lineno, ECPG_CONVERT_BOOL, "different size", ECPG_COMPAT_PGSQL);
				}

				*tobeinserted_p = mallocedval;
				*malloced_p = true;
				break;

			case ECPGt_char:
			case ECPGt_unsigned_char:
				{
					/* set slen to string length if type is char * */
					int			slen = (var->varcharsize == 0) ? strlen((char *) var->value) : var->varcharsize;

					if (!(newcopy = ECPGalloc(slen + 1, stmt->lineno)))
						return false;

					strncpy(newcopy, (char *) var->value, slen);
					newcopy[slen] = '\0';

					mallocedval = quote_postgres(newcopy, stmt->lineno);
					if (!mallocedval)
						return false;

					ECPGfree(newcopy);

					*tobeinserted_p = mallocedval;
					*malloced_p = true;
				}
				break;
			case ECPGt_const:
			case ECPGt_char_variable:
				{
					int			slen = strlen((char *) var->value);

					if (!(mallocedval = ECPGalloc(slen + 1, stmt->lineno)))
						return false;

					strncpy(mallocedval, (char *) var->value, slen);
					mallocedval[slen] = '\0';

					*tobeinserted_p = mallocedval;
					*malloced_p = true;
				}
				break;
			case ECPGt_varchar:
				{
					struct ECPGgeneric_varchar *variable =
					(struct ECPGgeneric_varchar *) (var->value);

					if (!(newcopy = (char *) ECPGalloc(variable->len + 1, stmt->lineno)))
						return false;

					strncpy(newcopy, variable->arr, variable->len);
					newcopy[variable->len] = '\0';

					mallocedval = quote_postgres(newcopy, stmt->lineno);
					if (!mallocedval)
						return false;

					ECPGfree(newcopy);

					*tobeinserted_p = mallocedval;
					*malloced_p = true;
				}
				break;

			case ECPGt_decimal:
			case ECPGt_numeric:
				{
					char *str = NULL;
					int slen;
					Numeric *nval = PGTYPESnumeric_new();
					
					if (var->arrsize > 1)
					{
						for (element = 0; element < var->arrsize; element++)
						{
							if (var->type == ECPGt_numeric)
								PGTYPESnumeric_copy((Numeric *)((var + var->offset * element)->value), nval);
							else
								PGTYPESnumeric_from_decimal((Decimal *)((var + var->offset * element)->value), nval);
							
							str = PGTYPESnumeric_to_asc(nval, 0);
							PGTYPESnumeric_free(nval);
							slen = strlen (str);
							
							if (!(mallocedval = ECPGrealloc(mallocedval, strlen(mallocedval) + slen + sizeof("array [] "), stmt->lineno)))
								return false;
							
							if (!element)
								strcpy(mallocedval, "array [");
							
							strncpy(mallocedval + strlen(mallocedval), str , slen + 1);
							strcpy(mallocedval + strlen(mallocedval), ",");
						}
						strcpy(mallocedval + strlen(mallocedval) - 1, "]");
					}
					else
					{
						if (var->type == ECPGt_numeric)
							PGTYPESnumeric_copy((Numeric *)(var->value), nval);
						else
							PGTYPESnumeric_from_decimal((Decimal *)(var->value), nval);
						
						str = PGTYPESnumeric_to_asc(nval, 0);

						PGTYPESnumeric_free(nval);
						slen = strlen (str);
					
						if (!(mallocedval = ECPGalloc(slen + 1, stmt->lineno)))
							return false;

						strncpy(mallocedval, str , slen);
						mallocedval[slen] = '\0';
					}
					
					*tobeinserted_p = mallocedval;
					*malloced_p = true;
					free(str);
				}
				break;

			case ECPGt_interval:
				{
					char *str = NULL;
					int slen;
					
					if (var->arrsize > 1)
					{
						for (element = 0; element < var->arrsize; element++)
						{
							str = quote_postgres(PGTYPESinterval_to_asc((Interval *)((var + var->offset * element)->value)), stmt->lineno);
							slen = strlen (str);
							
							if (!(mallocedval = ECPGrealloc(mallocedval, strlen(mallocedval) + slen + sizeof("array [],interval "), stmt->lineno)))
								return false;
							
							if (!element)
								strcpy(mallocedval, "array [");
						
							strcpy(mallocedval + strlen(mallocedval), "interval ");
							strncpy(mallocedval + strlen(mallocedval), str , slen + 1);
							strcpy(mallocedval + strlen(mallocedval), ",");
						}
						strcpy(mallocedval + strlen(mallocedval) - 1, "]");
					}
					else
					{
						str = quote_postgres(PGTYPESinterval_to_asc((Interval *)(var->value)), stmt->lineno);
						slen = strlen (str);
					
						if (!(mallocedval = ECPGalloc(slen + sizeof("interval ") + 1, stmt->lineno)))
							return false;

						strcpy(mallocedval, "interval ");
						/* also copy trailing '\0' */
						strncpy(mallocedval + strlen(mallocedval), str , slen + 1);
					}
					
					*tobeinserted_p = mallocedval;
					*malloced_p = true;
					free(str);
				}
				break;

			case ECPGt_date:
				{
					char *str = NULL;
					int slen;
					
					if (var->arrsize > 1)
					{
						for (element = 0; element < var->arrsize; element++)
						{
							str = quote_postgres(PGTYPESdate_to_asc(*(Date *)((var + var->offset * element)->value)), stmt->lineno);
							slen = strlen (str);
							
							if (!(mallocedval = ECPGrealloc(mallocedval, strlen(mallocedval) + slen + sizeof("array [],date "), stmt->lineno)))
								return false;
							
							if (!element)
								strcpy(mallocedval, "array [");
							
							strcpy(mallocedval + strlen(mallocedval), "date ");
							strncpy(mallocedval + strlen(mallocedval), str , slen + 1);
							strcpy(mallocedval + strlen(mallocedval), ",");
						}
						strcpy(mallocedval + strlen(mallocedval) - 1, "]");
					}
					else
					{
						str = quote_postgres(PGTYPESdate_to_asc(*(Date *)(var->value)), stmt->lineno);
						slen = strlen (str);
					
						if (!(mallocedval = ECPGalloc(slen + sizeof("date ") + 1, stmt->lineno)))
							return false;

						strcpy(mallocedval, "date ");
						/* also copy trailing '\0' */
						strncpy(mallocedval + strlen(mallocedval), str , slen + 1);
					}
					
					*tobeinserted_p = mallocedval;
					*malloced_p = true;
					free(str);
				}
				break;
				
			case ECPGt_timestamp:
				{
					char *str = NULL;
					int slen;
					
					if (var->arrsize > 1)
					{
						for (element = 0; element < var->arrsize; element++)
						{
							str = quote_postgres(PGTYPEStimestamp_to_asc(*(Timestamp *)((var + var->offset * element)->value)), stmt->lineno);
							slen = strlen (str);
							
							if (!(mallocedval = ECPGrealloc(mallocedval, strlen(mallocedval) + slen + sizeof("array [], timestamp "), stmt->lineno)))
								return false;
							
							if (!element)
								strcpy(mallocedval, "array [");
							
							strcpy(mallocedval + strlen(mallocedval), "timestamp ");
							strncpy(mallocedval + strlen(mallocedval), str , slen + 1);
							strcpy(mallocedval + strlen(mallocedval), ",");
						}
						strcpy(mallocedval + strlen(mallocedval) - 1, "]");
					}
					else
					{
						str = quote_postgres(PGTYPEStimestamp_to_asc(*(Timestamp *)(var->value)), stmt->lineno);
						slen = strlen (str);
					
						if (!(mallocedval = ECPGalloc(slen + sizeof("timestamp") + 1, stmt->lineno)))
							return false;

						strcpy(mallocedval, "timestamp ");
						/* also copy trailing '\0' */
						strncpy(mallocedval + strlen(mallocedval), str , slen + 1);
					}
					
					*tobeinserted_p = mallocedval;
					*malloced_p = true;
					free(str);
				}
				break;
				
			default:
				/* Not implemented yet */
				ECPGraise(stmt->lineno, ECPG_UNSUPPORTED, (char *) ECPGtype_name(var->type), ECPG_COMPAT_PGSQL);
				return false;
				break;
		}
	}
	return true;
}

static bool
ECPGexecute(struct statement * stmt)
{
	bool		status = false;
	char	   *copiedquery;
	char	   *errmsg, *cmdstat;
	PGresult   *results;
	PGnotify   *notify;
	struct variable *var;

	copiedquery = ECPGstrdup(stmt->command, stmt->lineno);

	/*
	 * Now, if the type is one of the fill in types then we take the
	 * argument and enter that in the string at the first %s position.
	 * Then if there are any more fill in types we fill in at the next and
	 * so on.
	 */
	var = stmt->inlist;
	while (var)
	{
		char	   *newcopy = NULL;
		const char *tobeinserted = NULL;
		char	   *p;
		bool		malloced = FALSE;
		int			hostvarl = 0;

		if (!ECPGstore_input(stmt, var, &tobeinserted, &malloced))
			return false;

		/*
		 * Now tobeinserted points to an area that is to be inserted at
		 * the first %s
		 */
		if (!(newcopy = (char *) ECPGalloc(strlen(copiedquery) + strlen(tobeinserted) + 1, stmt->lineno)))
			return false;

		strcpy(newcopy, copiedquery);
		if ((p = next_insert(newcopy + hostvarl)) == NULL)
		{
			/*
			 * We have an argument but we dont have the matched up string
			 * in the string
			 */
			ECPGraise(stmt->lineno, ECPG_TOO_MANY_ARGUMENTS, NULL, stmt->compat);
			return false;
		}
		else
		{
			strcpy(p, tobeinserted);
			hostvarl = strlen(newcopy);

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
		if (malloced)
		{
			ECPGfree((char *) tobeinserted);
			tobeinserted = NULL;
		}

		ECPGfree(copiedquery);
		copiedquery = newcopy;

		var = var->next;
	}

	/* Check if there are unmatched things left. */
	if (next_insert(copiedquery) != NULL)
	{
		ECPGraise(stmt->lineno, ECPG_TOO_FEW_ARGUMENTS, NULL, stmt->compat);
		return false;
	}

	/* Now the request is built. */

	if (stmt->connection->committed && !stmt->connection->autocommit)
	{
		if ((results = PQexec(stmt->connection->connection, "begin transaction")) == NULL)
		{
			ECPGraise(stmt->lineno, ECPG_TRANS, NULL, stmt->compat);
			return false;
		}
		PQclear(results);
		stmt->connection->committed = false;
	}

	ECPGlog("ECPGexecute line %d: QUERY: %s on connection %s\n", stmt->lineno, copiedquery, stmt->connection->name);
	results = PQexec(stmt->connection->connection, copiedquery);
	ECPGfree(copiedquery);

	if (results == NULL)
	{
		errmsg = PQerrorMessage(stmt->connection->connection);
		ECPGlog("ECPGexecute line %d: error: %s", stmt->lineno, errmsg);
		ECPGraise(stmt->lineno, ECPG_PGSQL, errmsg, stmt->compat);
	}
	else

		/*
		 * note: since some of the following code is duplicated in
		 * descriptor.c it should go into a separate function
		 */
	{
		bool		clear_result = TRUE;
		struct sqlca_t *sqlca = ECPGget_sqlca();

		errmsg = PQresultErrorMessage(results);
		
		var = stmt->outlist;
		switch (PQresultStatus(results))
		{
				int			nfields,
							ntuples,
							act_field;

			case PGRES_TUPLES_OK:
				nfields = PQnfields(results);
				sqlca->sqlerrd[2] = ntuples = PQntuples(results);
				status = true;

				if (ntuples < 1)
				{
					if (ntuples)
						ECPGlog("ECPGexecute line %d: Incorrect number of matches: %d\n",
								stmt->lineno, ntuples);
					ECPGraise(stmt->lineno, ECPG_NOT_FOUND, NULL, stmt->compat);
					status = false;
					break;
				}

				if (var != NULL && var->type == ECPGt_descriptor)
				{
					PGresult  **resultpp = ECPGdescriptor_lvalue(stmt->lineno, (const char *) var->pointer);

					if (resultpp == NULL)
						status = false;
					else
					{
						if (*resultpp)
							PQclear(*resultpp);
						*resultpp = results;
						clear_result = FALSE;
						ECPGlog("ECPGexecute putting result (%d tuples) into descriptor '%s'\n", PQntuples(results), (const char *) var->pointer);
					}
					var = var->next;
				}
				else
					for (act_field = 0; act_field < nfields && status; act_field++)
					{
						if (var != NULL)
						{
							status = ECPGstore_result(results, act_field, stmt, var);
							var = var->next;
						}
						else if (!INFORMIX_MODE(stmt->compat))
						{
							ECPGraise(stmt->lineno, ECPG_TOO_FEW_ARGUMENTS, NULL, stmt->compat);
							return (false);
						}
					}

				if (status && var != NULL)
				{
					ECPGraise(stmt->lineno, ECPG_TOO_MANY_ARGUMENTS, NULL, stmt->compat);
					status = false;
				}

				break;
			case PGRES_EMPTY_QUERY:
				/* do nothing */
				ECPGraise(stmt->lineno, ECPG_EMPTY, NULL, stmt->compat);
				break;
			case PGRES_COMMAND_OK:
				status = true;
				cmdstat = PQcmdStatus(results);
				sqlca->sqlerrd[1] = PQoidValue(results);
				sqlca->sqlerrd[2] = atol(PQcmdTuples(results));
				ECPGlog("ECPGexecute line %d Ok: %s\n", stmt->lineno, cmdstat);
				if (stmt->compat != ECPG_COMPAT_INFORMIX_SE &&
						!sqlca->sqlerrd[2] &&
							( !strncmp(cmdstat, "UPDATE", 6)
							  || !strncmp(cmdstat, "INSERT", 6)
							  || !strncmp(cmdstat, "DELETE", 6)))
					ECPGraise(stmt->lineno, ECPG_NOT_FOUND, NULL, stmt->compat);
				break;
			case PGRES_NONFATAL_ERROR:
			case PGRES_FATAL_ERROR:
			case PGRES_BAD_RESPONSE:
				ECPGlog("ECPGexecute line %d: Error: %s", stmt->lineno, errmsg);
				ECPGraise(stmt->lineno, ECPG_PGSQL, errmsg, stmt->compat);
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
				ECPGraise(stmt->lineno, ECPG_PGSQL, errmsg, stmt->compat);
				status = false;
				break;
		}
		if (clear_result)
			PQclear(results);
	}

	/* check for asynchronous returns */
	notify = PQnotifies(stmt->connection->connection);
	if (notify)
	{
		ECPGlog("ECPGexecute line %d: ASYNC NOTIFY of '%s' from backend pid '%d' received\n",
				stmt->lineno, notify->relname, notify->be_pid);
		PQfreemem(notify);
	}

	return status;
}

bool
ECPGdo(int lineno, int compat, int force_indicator, const char *connection_name, char *query,...)
{
	va_list		args;
	struct statement *stmt;
	struct connection *con = ECPGget_connection(connection_name);
	bool		status;
	char	   *oldlocale;

	/* Make sure we do NOT honor the locale for numeric input/output */
	/* since the database wants the standard decimal point */
	oldlocale = strdup(setlocale(LC_NUMERIC, NULL));
	setlocale(LC_NUMERIC, "C");

	if (!ECPGinit(con, connection_name, lineno))
	{
		setlocale(LC_NUMERIC, oldlocale);
		ECPGfree(oldlocale);
		return (false);
	}

	/* construct statement in our own structure */
	va_start(args, query);
	if (create_statement(lineno, compat, force_indicator, con, &stmt, query, args) == false)
	{
		setlocale(LC_NUMERIC, oldlocale);
		ECPGfree(oldlocale);
		return (false);
	}
	va_end(args);

	/* are we connected? */
	if (con == NULL || con->connection == NULL)
	{
		free_statement(stmt);
		ECPGraise(lineno, ECPG_NOT_CONN, (con) ? con->name : "<empty>", stmt->compat);
		setlocale(LC_NUMERIC, oldlocale);
		ECPGfree(oldlocale);
		return false;
	}

	/* initialize auto_mem struct */
	ECPGclear_auto_mem();

	status = ECPGexecute(stmt);
	free_statement(stmt);

	/* and reset locale value so our application is not affected */
	setlocale(LC_NUMERIC, oldlocale);
	ECPGfree(oldlocale);

	return (status);
}

/* old descriptor interface */
bool
ECPGdo_descriptor(int line, const char *connection,
				  const char *descriptor, const char *query)
{
	return ECPGdo(line, ECPG_COMPAT_PGSQL, true, connection, (char *) query, ECPGt_EOIT,
				  ECPGt_descriptor, descriptor, 0L, 0L, 0L,
				  ECPGt_NO_INDICATOR, NULL, 0L, 0L, 0L, ECPGt_EORT);
}


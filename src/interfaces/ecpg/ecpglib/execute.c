/* $PostgreSQL: pgsql/src/interfaces/ecpg/ecpglib/execute.c,v 1.62.2.3 2007/04/27 06:58:17 meskes Exp $ */

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

/*
 *	This function returns a newly malloced string that has ' and \
 *	escaped.
 */
static char *
quote_postgres(char *arg, bool quote, int lineno)
{
	char	*res;
	int	error;
	size_t  length;
	size_t  escaped_len;
	size_t  buffer_len;

	/*
	 * if quote is false we just need to store things in a descriptor they
	 * will be quoted once they are inserted in a statement
	 */
	if (!quote)
		return res = ECPGstrdup(arg, lineno);
	else
	{
		length = strlen(arg);
		buffer_len = 2 * length + 1;
		res = (char *) ECPGalloc(buffer_len + 3, lineno);
		if (!res)
			return (res);

		error = 0;
		escaped_len = PQescapeString(res+1, arg, buffer_len);
		if (error)
		{
			ECPGfree(res);
			return NULL;
		}
		if (length == escaped_len)
		{
			res[0] = res[escaped_len+1] = '\'';
			res[escaped_len+2] = '\0';
		}
		else
		{
			/* 
			 * We don't know if the target database is using
			 * standard_conforming_strings, so we always use E'' strings.
			 */
			memmove(res+2, res+1, escaped_len);
			res[0] = ESCAPE_STRING_SYNTAX;
			res[1] = res[escaped_len+2] = '\'';
			res[escaped_len+3] = '\0';
		}
		ECPGfree(arg);
		return res;
	}
}

static void
free_variable(struct variable * var)
{
	struct variable *var_next;

	if (var == NULL)
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
	if (stmt == NULL)
		return;
	free_variable(stmt->inlist);
	free_variable(stmt->outlist);
	ECPGfree(stmt->command);
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

static bool
ECPGtypeinfocache_push(struct ECPGtype_information_cache ** cache, int oid, bool isarray, int lineno)
{
	struct ECPGtype_information_cache *new_entry
	= (struct ECPGtype_information_cache *) ECPGalloc(sizeof(struct ECPGtype_information_cache), lineno);

	if (new_entry == NULL)
		return (false);

	new_entry->oid = oid;
	new_entry->isarray = isarray;
	new_entry->next = *cache;
	*cache = new_entry;
	return (true);
}

static enum ARRAY_TYPE
ECPGis_type_an_array(int type, const struct statement * stmt, const struct variable * var)
{
	char	   *array_query;
	enum ARRAY_TYPE isarray = ECPG_ARRAY_NOT_SET;
	PGresult   *query;
	struct ECPGtype_information_cache *cache_entry;

	if ((stmt->connection->cache_head) == NULL)
	{
		/*
		 * Text like types are not an array for ecpg, but postgres counts them
		 * as an array. This define reminds you to not 'correct' these values.
		 */
#define not_an_array_in_ecpg ECPG_ARRAY_NONE

		/* populate cache with well known types to speed things up */
		if (!ECPGtypeinfocache_push(&(stmt->connection->cache_head), BOOLOID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ECPGtypeinfocache_push(&(stmt->connection->cache_head), BYTEAOID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ECPGtypeinfocache_push(&(stmt->connection->cache_head), CHAROID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ECPGtypeinfocache_push(&(stmt->connection->cache_head), NAMEOID, not_an_array_in_ecpg, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ECPGtypeinfocache_push(&(stmt->connection->cache_head), INT8OID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ECPGtypeinfocache_push(&(stmt->connection->cache_head), INT2OID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ECPGtypeinfocache_push(&(stmt->connection->cache_head), INT2VECTOROID, ECPG_ARRAY_VECTOR, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ECPGtypeinfocache_push(&(stmt->connection->cache_head), INT4OID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ECPGtypeinfocache_push(&(stmt->connection->cache_head), REGPROCOID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ECPGtypeinfocache_push(&(stmt->connection->cache_head), TEXTOID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ECPGtypeinfocache_push(&(stmt->connection->cache_head), OIDOID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ECPGtypeinfocache_push(&(stmt->connection->cache_head), TIDOID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ECPGtypeinfocache_push(&(stmt->connection->cache_head), XIDOID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ECPGtypeinfocache_push(&(stmt->connection->cache_head), CIDOID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ECPGtypeinfocache_push(&(stmt->connection->cache_head), OIDVECTOROID, ECPG_ARRAY_VECTOR, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ECPGtypeinfocache_push(&(stmt->connection->cache_head), POINTOID, ECPG_ARRAY_VECTOR, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ECPGtypeinfocache_push(&(stmt->connection->cache_head), LSEGOID, ECPG_ARRAY_VECTOR, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ECPGtypeinfocache_push(&(stmt->connection->cache_head), PATHOID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ECPGtypeinfocache_push(&(stmt->connection->cache_head), BOXOID, ECPG_ARRAY_VECTOR, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ECPGtypeinfocache_push(&(stmt->connection->cache_head), POLYGONOID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ECPGtypeinfocache_push(&(stmt->connection->cache_head), LINEOID, ECPG_ARRAY_VECTOR, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ECPGtypeinfocache_push(&(stmt->connection->cache_head), FLOAT4OID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ECPGtypeinfocache_push(&(stmt->connection->cache_head), FLOAT8OID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ECPGtypeinfocache_push(&(stmt->connection->cache_head), ABSTIMEOID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ECPGtypeinfocache_push(&(stmt->connection->cache_head), RELTIMEOID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ECPGtypeinfocache_push(&(stmt->connection->cache_head), TINTERVALOID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ECPGtypeinfocache_push(&(stmt->connection->cache_head), UNKNOWNOID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ECPGtypeinfocache_push(&(stmt->connection->cache_head), CIRCLEOID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ECPGtypeinfocache_push(&(stmt->connection->cache_head), CASHOID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ECPGtypeinfocache_push(&(stmt->connection->cache_head), INETOID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ECPGtypeinfocache_push(&(stmt->connection->cache_head), CIDROID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ECPGtypeinfocache_push(&(stmt->connection->cache_head), BPCHAROID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ECPGtypeinfocache_push(&(stmt->connection->cache_head), VARCHAROID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ECPGtypeinfocache_push(&(stmt->connection->cache_head), DATEOID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ECPGtypeinfocache_push(&(stmt->connection->cache_head), TIMEOID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ECPGtypeinfocache_push(&(stmt->connection->cache_head), TIMESTAMPOID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ECPGtypeinfocache_push(&(stmt->connection->cache_head), TIMESTAMPTZOID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ECPGtypeinfocache_push(&(stmt->connection->cache_head), INTERVALOID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ECPGtypeinfocache_push(&(stmt->connection->cache_head), TIMETZOID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ECPGtypeinfocache_push(&(stmt->connection->cache_head), ZPBITOID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ECPGtypeinfocache_push(&(stmt->connection->cache_head), VARBITOID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ECPGtypeinfocache_push(&(stmt->connection->cache_head), NUMERICOID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
	}

	for (cache_entry = (stmt->connection->cache_head); cache_entry != NULL; cache_entry = cache_entry->next)
	{
		if (cache_entry->oid == type)
			return cache_entry->isarray;
	}

	array_query = (char *) ECPGalloc(strlen("select typlen from pg_type where oid= and typelem<>0") + 11, stmt->lineno);
	if (array_query == NULL)
		return (ECPG_ARRAY_ERROR);

	sprintf(array_query, "select typlen from pg_type where oid=%d and typelem<>0", type);
	query = PQexec(stmt->connection->connection, array_query);
	ECPGfree(array_query);
	if (PQresultStatus(query) == PGRES_TUPLES_OK)
	{
		if (PQntuples(query) == 0)
			isarray = ECPG_ARRAY_NONE;
		else
		{
			isarray = (atol((char *) PQgetvalue(query, 0, 0)) == -1) ? ECPG_ARRAY_ARRAY : ECPG_ARRAY_VECTOR;
			if (ECPGDynamicType(type) == SQL3_CHARACTER ||
				ECPGDynamicType(type) == SQL3_CHARACTER_VARYING)
			{
				/*
				 * arrays of character strings are not yet implemented
				 */
				isarray = ECPG_ARRAY_NONE;
			}
		}
	}
	PQclear(query);
	ECPGtypeinfocache_push(&(stmt->connection->cache_head), type, isarray, stmt->lineno);
	ECPGlog("ECPGis_type_an_array line %d: TYPE database: %d C: %d array: %s\n", stmt->lineno, type, var->type, isarray ? "Yes" : "No");
	return isarray;
}


bool
ECPGstore_result(const PGresult *results, int act_field,
				 const struct statement * stmt, struct variable * var)
{
	enum ARRAY_TYPE isarray;
	int			act_tuple,
				ntuples = PQntuples(results);
	bool		status = true;

	if ((isarray = ECPGis_type_an_array(PQftype(results, act_field), stmt, var)) == ECPG_ARRAY_ERROR)
	{
		ECPGraise(stmt->lineno, ECPG_OUT_OF_MEMORY, ECPG_SQLSTATE_ECPG_OUT_OF_MEMORY, NULL);
		return false;
	}

	if (isarray == ECPG_ARRAY_NONE)
	{
		/*
		 * if we don't have enough space, we cannot read all tuples
		 */
		if ((var->arrsize > 0 && ntuples > var->arrsize) || (var->ind_arrsize > 0 && ntuples > var->ind_arrsize))
		{
			ECPGlog("ECPGstore_result line %d: Incorrect number of matches: %d don't fit into array of %d\n",
					stmt->lineno, ntuples, var->arrsize);
			ECPGraise(stmt->lineno, INFORMIX_MODE(stmt->compat) ? ECPG_INFORMIX_SUBSELECT_NOT_ONE : ECPG_TOO_MANY_MATCHES, ECPG_SQLSTATE_CARDINALITY_VIOLATION, NULL);
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
			ECPGraise(stmt->lineno, ECPG_NO_ARRAY, ECPG_SQLSTATE_DATATYPE_MISMATCH, NULL);
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
		ECPGlog("ECPGstore_result: line %d: allocating memory for %d tuples\n", stmt->lineno, ntuples);
		var->value = (char *) ECPGalloc(len, stmt->lineno);
		if (!var->value)
			return false;
		*((char **) var->pointer) = var->value;
		ECPGadd_mem(var->value, stmt->lineno);
	}

	/* allocate indicator variable if needed */
	if ((var->ind_arrsize == 0 || var->ind_varcharsize == 0) && var->ind_value == NULL && var->ind_pointer != NULL)
	{
		int			len = var->ind_offset * ntuples;

		var->ind_value = (char *) ECPGalloc(len, stmt->lineno);
		if (!var->ind_value)
			return false;
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
							  var->ind_value, len, 0, var->ind_offset, isarray, stmt->compat, stmt->force_indicator))
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

bool
ECPGstore_input(const int lineno, const bool force_indicator, const struct variable * var,
				const char **tobeinserted_p, bool *malloced_p, bool quote)
{
	char	   *mallocedval = NULL;
	char	   *newcopy = NULL;

	/*
	 * arrays are not possible unless the attribute is an array too FIXME: we
	 * do not know if the attribute is an array here
	 */
#if 0
	if (var->arrsize > 1 &&...)
	{
		ECPGraise(lineno, ECPG_ARRAY_INSERT, ECPG_SQLSTATE_DATATYPE_MISMATCH, NULL);
		return false;
	}
#endif

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
			if (force_indicator == false)
			{
				if (ECPGis_noind_null(var->type, var->value))
					*tobeinserted_p = "null";
			}
			break;
		default:
			break;
	}
	if (**tobeinserted_p == '\0')
	{
		int			asize = var->arrsize ? var->arrsize : 1;

		switch (var->type)
		{
				int			element;

			case ECPGt_short:
				if (!(mallocedval = ECPGalloc(asize * 20, lineno)))
					return false;

				if (asize > 1)
				{
					strcpy(mallocedval, "array [");

					for (element = 0; element < asize; element++)
						sprintf(mallocedval + strlen(mallocedval), "%hd,", ((short *) var->value)[element]);

					strcpy(mallocedval + strlen(mallocedval) - 1, "]");
				}
				else
					sprintf(mallocedval, "%hd", *((short *) var->value));

				*tobeinserted_p = mallocedval;
				*malloced_p = true;
				break;

			case ECPGt_int:
				if (!(mallocedval = ECPGalloc(asize * 20, lineno)))
					return false;

				if (asize > 1)
				{
					strcpy(mallocedval, "array [");

					for (element = 0; element < asize; element++)
						sprintf(mallocedval + strlen(mallocedval), "%d,", ((int *) var->value)[element]);

					strcpy(mallocedval + strlen(mallocedval) - 1, "]");
				}
				else
					sprintf(mallocedval, "%d", *((int *) var->value));

				*tobeinserted_p = mallocedval;
				*malloced_p = true;
				break;

			case ECPGt_unsigned_short:
				if (!(mallocedval = ECPGalloc(asize * 20, lineno)))
					return false;

				if (asize > 1)
				{
					strcpy(mallocedval, "array [");

					for (element = 0; element < asize; element++)
						sprintf(mallocedval + strlen(mallocedval), "%hu,", ((unsigned short *) var->value)[element]);

					strcpy(mallocedval + strlen(mallocedval) - 1, "]");
				}
				else
					sprintf(mallocedval, "%hu", *((unsigned short *) var->value));

				*tobeinserted_p = mallocedval;
				*malloced_p = true;
				break;

			case ECPGt_unsigned_int:
				if (!(mallocedval = ECPGalloc(asize * 20, lineno)))
					return false;

				if (asize > 1)
				{
					strcpy(mallocedval, "array [");

					for (element = 0; element < asize; element++)
						sprintf(mallocedval + strlen(mallocedval), "%u,", ((unsigned int *) var->value)[element]);

					strcpy(mallocedval + strlen(mallocedval) - 1, "]");
				}
				else
					sprintf(mallocedval, "%u", *((unsigned int *) var->value));

				*tobeinserted_p = mallocedval;
				*malloced_p = true;
				break;

			case ECPGt_long:
				if (!(mallocedval = ECPGalloc(asize * 20, lineno)))
					return false;

				if (asize > 1)
				{
					strcpy(mallocedval, "array [");

					for (element = 0; element < asize; element++)
						sprintf(mallocedval + strlen(mallocedval), "%ld,", ((long *) var->value)[element]);

					strcpy(mallocedval + strlen(mallocedval) - 1, "]");
				}
				else
					sprintf(mallocedval, "%ld", *((long *) var->value));

				*tobeinserted_p = mallocedval;
				*malloced_p = true;
				break;

			case ECPGt_unsigned_long:
				if (!(mallocedval = ECPGalloc(asize * 20, lineno)))
					return false;

				if (asize > 1)
				{
					strcpy(mallocedval, "array [");

					for (element = 0; element < asize; element++)
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
				if (!(mallocedval = ECPGalloc(asize * 30, lineno)))
					return false;

				if (asize > 1)
				{
					strcpy(mallocedval, "array [");

					for (element = 0; element < asize; element++)
						sprintf(mallocedval + strlen(mallocedval), "%lld,", ((long long *) var->value)[element]);

					strcpy(mallocedval + strlen(mallocedval) - 1, "]");
				}
				else
					sprintf(mallocedval, "%lld", *((long long *) var->value));

				*tobeinserted_p = mallocedval;
				*malloced_p = true;
				break;

			case ECPGt_unsigned_long_long:
				if (!(mallocedval = ECPGalloc(asize * 30, lineno)))
					return false;

				if (asize > 1)
				{
					strcpy(mallocedval, "array [");

					for (element = 0; element < asize; element++)
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
				if (!(mallocedval = ECPGalloc(asize * 25, lineno)))
					return false;

				if (asize > 1)
				{
					strcpy(mallocedval, "array [");

					for (element = 0; element < asize; element++)
						sprintf(mallocedval + strlen(mallocedval), "%.14g,", ((float *) var->value)[element]);

					strcpy(mallocedval + strlen(mallocedval) - 1, "]");
				}
				else
					sprintf(mallocedval, "%.14g", *((float *) var->value));

				*tobeinserted_p = mallocedval;
				*malloced_p = true;
				break;

			case ECPGt_double:
				if (!(mallocedval = ECPGalloc(asize * 25, lineno)))
					return false;

				if (asize > 1)
				{
					strcpy(mallocedval, "array [");

					for (element = 0; element < asize; element++)
						sprintf(mallocedval + strlen(mallocedval), "%.14g,", ((double *) var->value)[element]);

					strcpy(mallocedval + strlen(mallocedval) - 1, "]");
				}
				else
					sprintf(mallocedval, "%.14g", *((double *) var->value));

				*tobeinserted_p = mallocedval;
				*malloced_p = true;
				break;

			case ECPGt_bool:
				if (!(mallocedval = ECPGalloc(var->arrsize + sizeof("array []"), lineno)))
					return false;

				if (var->arrsize > 1)
				{
					strcpy(mallocedval, "array [");

					if (var->offset == sizeof(char))
						for (element = 0; element < var->arrsize; element++)
							sprintf(mallocedval + strlen(mallocedval), "%c,", (((char *) var->value)[element]) ? 't' : 'f');

					/*
					 * this is necessary since sizeof(C++'s bool)==sizeof(int)
					 */
					else if (var->offset == sizeof(int))
						for (element = 0; element < var->arrsize; element++)
							sprintf(mallocedval + strlen(mallocedval), "%c,", (((int *) var->value)[element]) ? 't' : 'f');
					else
						ECPGraise(lineno, ECPG_CONVERT_BOOL, ECPG_SQLSTATE_DATATYPE_MISMATCH, "different size");

					strcpy(mallocedval + strlen(mallocedval) - 1, "]");
				}
				else
				{
					if (var->offset == sizeof(char))
						sprintf(mallocedval, "'%c'", (*((char *) var->value)) ? 't' : 'f');
					else if (var->offset == sizeof(int))
						sprintf(mallocedval, "'%c'", (*((int *) var->value)) ? 't' : 'f');
					else
						ECPGraise(lineno, ECPG_CONVERT_BOOL, ECPG_SQLSTATE_DATATYPE_MISMATCH, "different size");
				}

				*tobeinserted_p = mallocedval;
				*malloced_p = true;
				break;

			case ECPGt_char:
			case ECPGt_unsigned_char:
				{
					/* set slen to string length if type is char * */
					int			slen = (var->varcharsize == 0) ? strlen((char *) var->value) : var->varcharsize;

					if (!(newcopy = ECPGalloc(slen + 1, lineno)))
						return false;

					strncpy(newcopy, (char *) var->value, slen);
					newcopy[slen] = '\0';

					mallocedval = quote_postgres(newcopy, quote, lineno);
					if (!mallocedval)
						return false;

					*tobeinserted_p = mallocedval;
					*malloced_p = true;
				}
				break;
			case ECPGt_const:
			case ECPGt_char_variable:
				{
					int			slen = strlen((char *) var->value);

					if (!(mallocedval = ECPGalloc(slen + 1, lineno)))
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

					if (!(newcopy = (char *) ECPGalloc(variable->len + 1, lineno)))
						return false;

					strncpy(newcopy, variable->arr, variable->len);
					newcopy[variable->len] = '\0';

					mallocedval = quote_postgres(newcopy, quote, lineno);
					if (!mallocedval)
						return false;

					*tobeinserted_p = mallocedval;
					*malloced_p = true;
				}
				break;

			case ECPGt_decimal:
			case ECPGt_numeric:
				{
					char	   *str = NULL;
					int			slen;
					numeric    *nval;

					if (var->arrsize > 1)
					{
						for (element = 0; element < var->arrsize; element++)
						{
							nval = PGTYPESnumeric_new();
							if (!nval)
								return false;

							if (var->type == ECPGt_numeric)
								PGTYPESnumeric_copy((numeric *) ((var + var->offset * element)->value), nval);
							else
								PGTYPESnumeric_from_decimal((decimal *) ((var + var->offset * element)->value), nval);

							str = PGTYPESnumeric_to_asc(nval, nval->dscale);
							slen = strlen(str);
							PGTYPESnumeric_free(nval);

							if (!(mallocedval = ECPGrealloc(mallocedval, strlen(mallocedval) + slen + sizeof("array [] "), lineno)))
							{
								ECPGfree(str);
								return false;
							}

							if (!element)
								strcpy(mallocedval, "array [");

							strncpy(mallocedval + strlen(mallocedval), str, slen + 1);
							strcpy(mallocedval + strlen(mallocedval), ",");
							ECPGfree(str);
						}
						strcpy(mallocedval + strlen(mallocedval) - 1, "]");
					}
					else
					{
						nval = PGTYPESnumeric_new();
						if (!nval)
							return false;

						if (var->type == ECPGt_numeric)
							PGTYPESnumeric_copy((numeric *) (var->value), nval);
						else
							PGTYPESnumeric_from_decimal((decimal *) (var->value), nval);

						str = PGTYPESnumeric_to_asc(nval, nval->dscale);
						slen = strlen(str);
						PGTYPESnumeric_free(nval);

						if (!(mallocedval = ECPGalloc(slen + 1, lineno)))
						{
							free(str);
							return false;
						}

						strncpy(mallocedval, str, slen);
						mallocedval[slen] = '\0';
						ECPGfree(str);
					}

					*tobeinserted_p = mallocedval;
					*malloced_p = true;
				}
				break;

			case ECPGt_interval:
				{
					char	   *str = NULL;
					int			slen;

					if (var->arrsize > 1)
					{
						for (element = 0; element < var->arrsize; element++)
						{
							str = quote_postgres(PGTYPESinterval_to_asc((interval *) ((var + var->offset * element)->value)), quote, lineno);
							if (!str)
								return false;
							slen = strlen(str);

							if (!(mallocedval = ECPGrealloc(mallocedval, strlen(mallocedval) + slen + sizeof("array [],interval "), lineno)))
							{
								ECPGfree(str);
								return false;
							}

							if (!element)
								strcpy(mallocedval, "array [");

							strcpy(mallocedval + strlen(mallocedval), "interval ");
							strncpy(mallocedval + strlen(mallocedval), str, slen + 1);
							strcpy(mallocedval + strlen(mallocedval), ",");
							ECPGfree(str);
						}
						strcpy(mallocedval + strlen(mallocedval) - 1, "]");
					}
					else
					{
						str = quote_postgres(PGTYPESinterval_to_asc((interval *) (var->value)), quote, lineno);
						if (!str)
							return false;
						slen = strlen(str);

						if (!(mallocedval = ECPGalloc(slen + sizeof("interval ") + 1, lineno)))
						{
							ECPGfree(str);
							return false;
						}

						strcpy(mallocedval, "interval ");
						/* also copy trailing '\0' */
						strncpy(mallocedval + strlen(mallocedval), str, slen + 1);
						ECPGfree(str);
					}

					*tobeinserted_p = mallocedval;
					*malloced_p = true;
				}
				break;

			case ECPGt_date:
				{
					char	   *str = NULL;
					int			slen;

					if (var->arrsize > 1)
					{
						for (element = 0; element < var->arrsize; element++)
						{
							str = quote_postgres(PGTYPESdate_to_asc(*(date *) ((var + var->offset * element)->value)), quote, lineno);
							if (!str)
								return false;
							slen = strlen(str);

							if (!(mallocedval = ECPGrealloc(mallocedval, strlen(mallocedval) + slen + sizeof("array [],date "), lineno)))
							{
								ECPGfree(str);
								return false;
							}

							if (!element)
								strcpy(mallocedval, "array [");

							strcpy(mallocedval + strlen(mallocedval), "date ");
							strncpy(mallocedval + strlen(mallocedval), str, slen + 1);
							strcpy(mallocedval + strlen(mallocedval), ",");
							ECPGfree(str);
						}
						strcpy(mallocedval + strlen(mallocedval) - 1, "]");
					}
					else
					{
						str = quote_postgres(PGTYPESdate_to_asc(*(date *) (var->value)), quote, lineno);
						if (!str)
							return false;
						slen = strlen(str);

						if (!(mallocedval = ECPGalloc(slen + sizeof("date ") + 1, lineno)))
						{
							ECPGfree(str);
							return false;
						}

						strcpy(mallocedval, "date ");
						/* also copy trailing '\0' */
						strncpy(mallocedval + strlen(mallocedval), str, slen + 1);
						ECPGfree(str);
					}

					*tobeinserted_p = mallocedval;
					*malloced_p = true;
				}
				break;

			case ECPGt_timestamp:
				{
					char	   *str = NULL;
					int			slen;

					if (var->arrsize > 1)
					{
						for (element = 0; element < var->arrsize; element++)
						{
							str = quote_postgres(PGTYPEStimestamp_to_asc(*(timestamp *) ((var + var->offset * element)->value)), quote, lineno);
							if (!str)
								return false;

							slen = strlen(str);

							if (!(mallocedval = ECPGrealloc(mallocedval, strlen(mallocedval) + slen + sizeof("array [], timestamp "), lineno)))
							{
								ECPGfree(str);
								return false;
							}

							if (!element)
								strcpy(mallocedval, "array [");

							strcpy(mallocedval + strlen(mallocedval), "timestamp ");
							strncpy(mallocedval + strlen(mallocedval), str, slen + 1);
							strcpy(mallocedval + strlen(mallocedval), ",");
							ECPGfree(str);
						}
						strcpy(mallocedval + strlen(mallocedval) - 1, "]");
					}
					else
					{
						str = quote_postgres(PGTYPEStimestamp_to_asc(*(timestamp *) (var->value)), quote, lineno);
						if (!str)
							return false;
						slen = strlen(str);

						if (!(mallocedval = ECPGalloc(slen + sizeof("timestamp") + 1, lineno)))
						{
							ECPGfree(str);
							return false;
						}

						strcpy(mallocedval, "timestamp ");
						/* also copy trailing '\0' */
						strncpy(mallocedval + strlen(mallocedval), str, slen + 1);
						ECPGfree(str);
					}

					*tobeinserted_p = mallocedval;
					*malloced_p = true;
				}
				break;

			case ECPGt_descriptor:
				break;

			default:
				/* Not implemented yet */
				ECPGraise(lineno, ECPG_UNSUPPORTED, ECPG_SQLSTATE_ECPG_INTERNAL_ERROR, (char *) ECPGtype_name(var->type));
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
	char	   *cmdstat;
	PGresult   *results;
	PGnotify   *notify;
	struct variable *var;
	int			desc_counter = 0;

	copiedquery = ECPGstrdup(stmt->command, stmt->lineno);

	/*
	 * Now, if the type is one of the fill in types then we take the argument
	 * and enter that in the string at the first %s position. Then if there
	 * are any more fill in types we fill in at the next and so on.
	 */
	var = stmt->inlist;

	while (var)
	{
		char	   *newcopy = NULL;
		const char *tobeinserted;
		char	   *p;
		bool		malloced = FALSE;
		int			hostvarl = 0;

		tobeinserted = NULL;

		/*
		 * A descriptor is a special case since it contains many variables but
		 * is listed only once.
		 */
		if (var->type == ECPGt_descriptor)
		{
			/*
			 * We create an additional variable list here, so the same logic
			 * applies.
			 */
			struct variable desc_inlist;
			struct descriptor *desc;
			struct descriptor_item *desc_item;

			for (desc = all_descriptors; desc; desc = desc->next)
			{
				if (strcmp(var->pointer, desc->name) == 0)
					break;
			}

			if (desc == NULL)
			{
				ECPGraise(stmt->lineno, ECPG_UNKNOWN_DESCRIPTOR, ECPG_SQLSTATE_INVALID_SQL_DESCRIPTOR_NAME, var->pointer);
				ECPGfree(copiedquery);
				return false;
			}

			desc_counter++;
			if (desc->count < 0 || desc->count >= desc_counter)
			{
				for (desc_item = desc->items; desc_item; desc_item = desc_item->next)
				{
					if (desc_item->num == desc_counter)
					{
						desc_inlist.type = ECPGt_char;
						desc_inlist.value = desc_item->data;
						desc_inlist.pointer = &(desc_item->data);
						desc_inlist.varcharsize = strlen(desc_item->data);
						desc_inlist.arrsize = 1;
						desc_inlist.offset = 0;
						if (!desc_item->indicator)
						{
							desc_inlist.ind_type = ECPGt_NO_INDICATOR;
							desc_inlist.ind_value = desc_inlist.ind_pointer = NULL;
							desc_inlist.ind_varcharsize = desc_inlist.ind_arrsize = desc_inlist.ind_offset = 0;
						}
						else
						{
							desc_inlist.ind_type = ECPGt_int;
							desc_inlist.ind_value = &(desc_item->indicator);
							desc_inlist.ind_pointer = &(desc_inlist.ind_value);
							desc_inlist.ind_varcharsize = desc_inlist.ind_arrsize = 1;
							desc_inlist.ind_offset = 0;
						}
						if (!ECPGstore_input(stmt->lineno, stmt->force_indicator, &desc_inlist, &tobeinserted, &malloced, true))
						{
							ECPGfree(copiedquery);
							return false;
						}

						break;
					}
				}

				if (!desc_item) /* no more entries found in descriptor */
					desc_counter = 0;
			}
			else
				desc_counter = 0;
		}
		else
		{
			if (!ECPGstore_input(stmt->lineno, stmt->force_indicator, var, &tobeinserted, &malloced, true))
				return false;
		}

		if (tobeinserted)
		{
			/*
			 * Now tobeinserted points to an area that is to be inserted at
			 * the first %s
			 */
			if (!(newcopy = (char *) ECPGalloc(strlen(copiedquery)
											   + strlen(tobeinserted)
											   + 1, stmt->lineno)))
			{
				ECPGfree(copiedquery);
				return false;
			}

			strcpy(newcopy, copiedquery);
			if ((p = next_insert(newcopy + hostvarl)) == NULL)
			{
				/*
				 * We have an argument but we dont have the matched up string
				 * in the string
				 */
				ECPGraise(stmt->lineno, ECPG_TOO_MANY_ARGUMENTS,
						ECPG_SQLSTATE_USING_CLAUSE_DOES_NOT_MATCH_PARAMETERS,
						  NULL);
				ECPGfree(copiedquery);
				ECPGfree(newcopy);
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
		}

		if (desc_counter == 0)
			var = var->next;
	}

	/* Check if there are unmatched things left. */
	if (next_insert(copiedquery) != NULL)
	{
		ECPGraise(stmt->lineno, ECPG_TOO_FEW_ARGUMENTS,
				  ECPG_SQLSTATE_USING_CLAUSE_DOES_NOT_MATCH_PARAMETERS, NULL);
		ECPGfree(copiedquery);
		return false;
	}

	/* Now the request is built. */

	if (stmt->connection->committed && !stmt->connection->autocommit)
	{
		if ((results = PQexec(stmt->connection->connection, "begin transaction")) == NULL)
		{
			ECPGraise(stmt->lineno, ECPG_TRANS,
					  ECPG_SQLSTATE_TRANSACTION_RESOLUTION_UNKNOWN, NULL);
			ECPGfree(copiedquery);
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
		ECPGlog("ECPGexecute line %d: error: %s", stmt->lineno, PQerrorMessage(stmt->connection->connection));
		ECPGraise_backend(stmt->lineno, NULL, stmt->connection->connection, stmt->compat);
	}
	else

		/*
		 * note: since some of the following code is duplicated in
		 * descriptor.c it should go into a separate function
		 */
	{
		bool		clear_result = TRUE;
		struct sqlca_t *sqlca = ECPGget_sqlca();

		var = stmt->outlist;
		switch (PQresultStatus(results))
		{
				int			nfields,
							ntuples,
							act_field;

			case PGRES_TUPLES_OK:
				nfields = PQnfields(results);
				sqlca->sqlerrd[2] = ntuples = PQntuples(results);
				ECPGlog("ECPGexecute line %d: Correctly got %d tuples with %d fields\n", stmt->lineno, ntuples, nfields);
				status = true;

				if (ntuples < 1)
				{
					if (ntuples)
						ECPGlog("ECPGexecute line %d: Incorrect number of matches: %d\n",
								stmt->lineno, ntuples);
					ECPGraise(stmt->lineno, ECPG_NOT_FOUND, ECPG_SQLSTATE_NO_DATA, NULL);
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
							ECPGraise(stmt->lineno, ECPG_TOO_FEW_ARGUMENTS, ECPG_SQLSTATE_USING_CLAUSE_DOES_NOT_MATCH_TARGETS, NULL);
							return (false);
						}
					}

				if (status && var != NULL)
				{
					ECPGraise(stmt->lineno, ECPG_TOO_MANY_ARGUMENTS, ECPG_SQLSTATE_USING_CLAUSE_DOES_NOT_MATCH_TARGETS, NULL);
					status = false;
				}

				break;
			case PGRES_EMPTY_QUERY:
				/* do nothing */
				ECPGraise(stmt->lineno, ECPG_EMPTY, ECPG_SQLSTATE_ECPG_INTERNAL_ERROR, NULL);
				break;
			case PGRES_COMMAND_OK:
				status = true;
				cmdstat = PQcmdStatus(results);
				sqlca->sqlerrd[1] = PQoidValue(results);
				sqlca->sqlerrd[2] = atol(PQcmdTuples(results));
				ECPGlog("ECPGexecute line %d Ok: %s\n", stmt->lineno, cmdstat);
				if (stmt->compat != ECPG_COMPAT_INFORMIX_SE &&
					!sqlca->sqlerrd[2] &&
					(!strncmp(cmdstat, "UPDATE", 6)
					 || !strncmp(cmdstat, "INSERT", 6)
					 || !strncmp(cmdstat, "DELETE", 6)))
					ECPGraise(stmt->lineno, ECPG_NOT_FOUND, ECPG_SQLSTATE_NO_DATA, NULL);
				break;
			case PGRES_NONFATAL_ERROR:
			case PGRES_FATAL_ERROR:
			case PGRES_BAD_RESPONSE:
				ECPGlog("ECPGexecute line %d: Error: %s", stmt->lineno, PQresultErrorMessage(results));
				ECPGraise_backend(stmt->lineno, results, stmt->connection->connection, stmt->compat);
				status = false;
				break;
			case PGRES_COPY_OUT:
				{
					char	   *buffer;
					int			res;

					ECPGlog("ECPGexecute line %d: Got PGRES_COPY_OUT\n", stmt->lineno);
					while ((res = PQgetCopyData(stmt->connection->connection,
												&buffer, 0)) > 0)
					{
						printf("%s", buffer);
						PQfreemem(buffer);
					}
					if (res == -1)
					{
						/* COPY done */
						PQclear(results);
						results = PQgetResult(stmt->connection->connection);
						if (PQresultStatus(results) == PGRES_COMMAND_OK)
							ECPGlog("ECPGexecute line %d: Got PGRES_COMMAND_OK after PGRES_COPY_OUT\n", stmt->lineno);
						else
							ECPGlog("ECPGexecute line %d: Got error after PGRES_COPY_OUT: %s", PQresultErrorMessage(results));
					}
					break;
				}
			case PGRES_COPY_IN:
				ECPGlog("ECPGexecute line %d: Got PGRES_COPY_IN ... tossing.\n", stmt->lineno);
				PQendcopy(stmt->connection->connection);
				break;
			default:
				ECPGlog("ECPGexecute line %d: Got something else, postgres error.\n",
						stmt->lineno);
				ECPGraise_backend(stmt->lineno, results, stmt->connection->connection, stmt->compat);
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
ECPGdo(int lineno, int compat, int force_indicator, const char *connection_name, const char *query,...)
{
	va_list		args;
	struct statement *stmt;
	struct connection *con;
	bool		status;
	char	   *oldlocale;
	enum ECPGttype type;
	struct variable **list;

	/* Make sure we do NOT honor the locale for numeric input/output */
	/* since the database wants the standard decimal point */
	oldlocale = ECPGstrdup(setlocale(LC_NUMERIC, NULL), lineno);
	setlocale(LC_NUMERIC, "C");

#ifdef ENABLE_THREAD_SAFETY
	ecpg_pthreads_init();
#endif

	con = ECPGget_connection(connection_name);

	if (!ECPGinit(con, connection_name, lineno))
	{
		setlocale(LC_NUMERIC, oldlocale);
		ECPGfree(oldlocale);
		return (false);
	}

	/* construct statement in our own structure */
	va_start(args, query);

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
	if (!(stmt = (struct statement *) ECPGalloc(sizeof(struct statement), lineno)))
	{
		setlocale(LC_NUMERIC, oldlocale);
		ECPGfree(oldlocale);
		va_end(args);
		return false;
	}

	stmt->command = ECPGstrdup(query, lineno);
	stmt->connection = con;
	stmt->lineno = lineno;
	stmt->compat = compat;
	stmt->force_indicator = force_indicator;

	list = &(stmt->inlist);

	type = va_arg(args, enum ECPGttype);

	while (type != ECPGt_EORT)
	{
		if (type == ECPGt_EOIT)
			list = &(stmt->outlist);
		else
		{
			struct variable *var,
					   *ptr;

			if (!(var = (struct variable *) ECPGalloc(sizeof(struct variable), lineno)))
			{
				setlocale(LC_NUMERIC, oldlocale);
				ECPGfree(oldlocale);
				free_statement(stmt);
				va_end(args);
				return false;
			}

			var->type = type;
			var->pointer = va_arg(args, char *);

			var->varcharsize = va_arg(args, long);
			var->arrsize = va_arg(args, long);
			var->offset = va_arg(args, long);

			if (var->arrsize == 0 || var->varcharsize == 0)
				var->value = *((char **) (var->pointer));
			else
				var->value = var->pointer;

			/*
			 * negative values are used to indicate an array without given bounds
			 */
			/* reset to zero for us */
			if (var->arrsize < 0)
				var->arrsize = 0;
			if (var->varcharsize < 0)
				var->varcharsize = 0;

			var->next = NULL;

			var->ind_type = va_arg(args, enum ECPGttype);
			var->ind_pointer = va_arg(args, char *);
			var->ind_varcharsize = va_arg(args, long);
			var->ind_arrsize = va_arg(args, long);
			var->ind_offset = va_arg(args, long);

			if (var->ind_type != ECPGt_NO_INDICATOR
				&& (var->ind_arrsize == 0 || var->ind_varcharsize == 0))
				var->ind_value = *((char **) (var->ind_pointer));
			else
				var->ind_value = var->ind_pointer;

			/*
			 * negative values are used to indicate an array without given bounds
			 */
			/* reset to zero for us */
			if (var->ind_arrsize < 0)
				var->ind_arrsize = 0;
			if (var->ind_varcharsize < 0)
				var->ind_varcharsize = 0;

			/* if variable is NULL, the statement hasn't been prepared */
			if (var->pointer == NULL)
			{
				ECPGraise(lineno, ECPG_INVALID_STMT, ECPG_SQLSTATE_INVALID_SQL_STATEMENT_NAME, NULL);
				ECPGfree(var);
				setlocale(LC_NUMERIC, oldlocale);
				ECPGfree(oldlocale);
				free_statement(stmt);
				va_end(args);
				return false;
			}

			for (ptr = *list; ptr && ptr->next; ptr = ptr->next);

			if (ptr == NULL)
				*list = var;
			else
				ptr->next = var;
		}

		type = va_arg(args, enum ECPGttype);
	}

	va_end(args);

	/* are we connected? */
	if (con == NULL || con->connection == NULL)
	{
		free_statement(stmt);
		ECPGraise(lineno, ECPG_NOT_CONN, ECPG_SQLSTATE_ECPG_INTERNAL_ERROR, (con) ? con->name : "<empty>");
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

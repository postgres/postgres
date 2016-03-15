/* src/interfaces/ecpg/ecpglib/execute.c */

/*
 * The aim is to get a simpler interface to the database routines.
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
#include <float.h>
#include <math.h>

#include "pg_type.h"

#include "ecpgtype.h"
#include "ecpglib.h"
#include "ecpgerrno.h"
#include "extern.h"
#include "sqlca.h"
#include "sqlda-native.h"
#include "sqlda-compat.h"
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
	char	   *res;
	size_t		length;
	size_t		escaped_len;
	size_t		buffer_len;

	/*
	 * if quote is false we just need to store things in a descriptor they
	 * will be quoted once they are inserted in a statement
	 */
	if (!quote)
		return arg;
	else
	{
		length = strlen(arg);
		buffer_len = 2 * length + 1;
		res = (char *) ecpg_alloc(buffer_len + 3, lineno);
		if (!res)
			return (res);
		escaped_len = PQescapeString(res + 1, arg, buffer_len);
		if (length == escaped_len)
		{
			res[0] = res[escaped_len + 1] = '\'';
			res[escaped_len + 2] = '\0';
		}
		else
		{
			/*
			 * We don't know if the target database is using
			 * standard_conforming_strings, so we always use E'' strings.
			 */
			memmove(res + 2, res + 1, escaped_len);
			res[0] = ESCAPE_STRING_SYNTAX;
			res[1] = res[escaped_len + 2] = '\'';
			res[escaped_len + 3] = '\0';
		}
		ecpg_free(arg);
		return res;
	}
}

static void
free_variable(struct variable * var)
{
	struct variable *var_next;

	while (var)
	{
		var_next = var->next;
		ecpg_free(var);
		var = var_next;
	}
}

static void
free_statement(struct statement * stmt)
{
	if (stmt == NULL)
		return;
	free_variable(stmt->inlist);
	free_variable(stmt->outlist);
	ecpg_free(stmt->command);
	ecpg_free(stmt->name);
	ecpg_free(stmt->oldlocale);
	ecpg_free(stmt);
}

static int
next_insert(char *text, int pos, bool questionmarks)
{
	bool		string = false;
	int			p = pos;

	for (; text[p] != '\0'; p++)
	{
		if (text[p] == '\\')	/* escape character */
			p++;
		else if (text[p] == '\'')
			string = string ? false : true;
		else if (!string)
		{
			if (text[p] == '$' && isdigit((unsigned char) text[p + 1]))
			{
				/* this can be either a dollar quote or a variable */
				int			i;

				for (i = p + 1; isdigit((unsigned char) text[i]); i++)
					 /* empty loop body */ ;
				if (!isalpha((unsigned char) text[i]) &&
					isascii((unsigned char) text[i]) &&text[i] != '_')
					/* not dollar delimited quote */
					return p;
			}
			else if (questionmarks && text[p] == '?')
			{
				/* also allow old style placeholders */
				return p;
			}
		}
	}

	return -1;
}

static bool
ecpg_type_infocache_push(struct ECPGtype_information_cache ** cache, int oid, enum ARRAY_TYPE isarray, int lineno)
{
	struct ECPGtype_information_cache *new_entry
	= (struct ECPGtype_information_cache *) ecpg_alloc(sizeof(struct ECPGtype_information_cache), lineno);

	if (new_entry == NULL)
		return (false);

	new_entry->oid = oid;
	new_entry->isarray = isarray;
	new_entry->next = *cache;
	*cache = new_entry;
	return (true);
}

static enum ARRAY_TYPE
ecpg_is_type_an_array(int type, const struct statement * stmt, const struct variable * var)
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
		if (!ecpg_type_infocache_push(&(stmt->connection->cache_head), BOOLOID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ecpg_type_infocache_push(&(stmt->connection->cache_head), BYTEAOID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ecpg_type_infocache_push(&(stmt->connection->cache_head), CHAROID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ecpg_type_infocache_push(&(stmt->connection->cache_head), NAMEOID, not_an_array_in_ecpg, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ecpg_type_infocache_push(&(stmt->connection->cache_head), INT8OID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ecpg_type_infocache_push(&(stmt->connection->cache_head), INT2OID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ecpg_type_infocache_push(&(stmt->connection->cache_head), INT2VECTOROID, ECPG_ARRAY_VECTOR, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ecpg_type_infocache_push(&(stmt->connection->cache_head), INT4OID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ecpg_type_infocache_push(&(stmt->connection->cache_head), REGPROCOID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ecpg_type_infocache_push(&(stmt->connection->cache_head), TEXTOID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ecpg_type_infocache_push(&(stmt->connection->cache_head), OIDOID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ecpg_type_infocache_push(&(stmt->connection->cache_head), TIDOID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ecpg_type_infocache_push(&(stmt->connection->cache_head), XIDOID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ecpg_type_infocache_push(&(stmt->connection->cache_head), CIDOID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ecpg_type_infocache_push(&(stmt->connection->cache_head), OIDVECTOROID, ECPG_ARRAY_VECTOR, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ecpg_type_infocache_push(&(stmt->connection->cache_head), POINTOID, ECPG_ARRAY_VECTOR, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ecpg_type_infocache_push(&(stmt->connection->cache_head), LSEGOID, ECPG_ARRAY_VECTOR, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ecpg_type_infocache_push(&(stmt->connection->cache_head), PATHOID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ecpg_type_infocache_push(&(stmt->connection->cache_head), BOXOID, ECPG_ARRAY_VECTOR, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ecpg_type_infocache_push(&(stmt->connection->cache_head), POLYGONOID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ecpg_type_infocache_push(&(stmt->connection->cache_head), LINEOID, ECPG_ARRAY_VECTOR, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ecpg_type_infocache_push(&(stmt->connection->cache_head), FLOAT4OID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ecpg_type_infocache_push(&(stmt->connection->cache_head), FLOAT8OID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ecpg_type_infocache_push(&(stmt->connection->cache_head), ABSTIMEOID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ecpg_type_infocache_push(&(stmt->connection->cache_head), RELTIMEOID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ecpg_type_infocache_push(&(stmt->connection->cache_head), TINTERVALOID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ecpg_type_infocache_push(&(stmt->connection->cache_head), UNKNOWNOID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ecpg_type_infocache_push(&(stmt->connection->cache_head), CIRCLEOID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ecpg_type_infocache_push(&(stmt->connection->cache_head), CASHOID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ecpg_type_infocache_push(&(stmt->connection->cache_head), INETOID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ecpg_type_infocache_push(&(stmt->connection->cache_head), CIDROID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ecpg_type_infocache_push(&(stmt->connection->cache_head), BPCHAROID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ecpg_type_infocache_push(&(stmt->connection->cache_head), VARCHAROID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ecpg_type_infocache_push(&(stmt->connection->cache_head), DATEOID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ecpg_type_infocache_push(&(stmt->connection->cache_head), TIMEOID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ecpg_type_infocache_push(&(stmt->connection->cache_head), TIMESTAMPOID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ecpg_type_infocache_push(&(stmt->connection->cache_head), TIMESTAMPTZOID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ecpg_type_infocache_push(&(stmt->connection->cache_head), INTERVALOID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ecpg_type_infocache_push(&(stmt->connection->cache_head), TIMETZOID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ecpg_type_infocache_push(&(stmt->connection->cache_head), ZPBITOID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ecpg_type_infocache_push(&(stmt->connection->cache_head), VARBITOID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
		if (!ecpg_type_infocache_push(&(stmt->connection->cache_head), NUMERICOID, ECPG_ARRAY_NONE, stmt->lineno))
			return (ECPG_ARRAY_ERROR);
	}

	for (cache_entry = (stmt->connection->cache_head); cache_entry != NULL; cache_entry = cache_entry->next)
	{
		if (cache_entry->oid == type)
			return cache_entry->isarray;
	}

	array_query = (char *) ecpg_alloc(strlen("select typlen from pg_type where oid= and typelem<>0") + 11, stmt->lineno);
	if (array_query == NULL)
		return (ECPG_ARRAY_ERROR);

	sprintf(array_query, "select typlen from pg_type where oid=%d and typelem<>0", type);
	query = PQexec(stmt->connection->connection, array_query);
	ecpg_free(array_query);
	if (!ecpg_check_PQresult(query, stmt->lineno, stmt->connection->connection, stmt->compat))
		return (ECPG_ARRAY_ERROR);
	else if (PQresultStatus(query) == PGRES_TUPLES_OK)
	{
		if (PQntuples(query) == 0)
			isarray = ECPG_ARRAY_NONE;
		else
		{
			isarray = (atol((char *) PQgetvalue(query, 0, 0)) == -1) ? ECPG_ARRAY_ARRAY : ECPG_ARRAY_VECTOR;
			if (ecpg_dynamic_type(type) == SQL3_CHARACTER ||
				ecpg_dynamic_type(type) == SQL3_CHARACTER_VARYING)
			{
				/*
				 * arrays of character strings are not yet implemented
				 */
				isarray = ECPG_ARRAY_NONE;
			}
		}
		PQclear(query);
	}
	else
		return (ECPG_ARRAY_ERROR);

	ecpg_type_infocache_push(&(stmt->connection->cache_head), type, isarray, stmt->lineno);
	ecpg_log("ecpg_is_type_an_array on line %d: type (%d); C (%d); array (%s)\n", stmt->lineno, type, var->type, ECPG_IS_ARRAY(isarray) ? "yes" : "no");
	return isarray;
}


bool
ecpg_store_result(const PGresult *results, int act_field,
				  const struct statement * stmt, struct variable * var)
{
	enum ARRAY_TYPE isarray;
	int			act_tuple,
				ntuples = PQntuples(results);
	bool		status = true;

	if ((isarray = ecpg_is_type_an_array(PQftype(results, act_field), stmt, var)) == ECPG_ARRAY_ERROR)
	{
		ecpg_raise(stmt->lineno, ECPG_OUT_OF_MEMORY, ECPG_SQLSTATE_ECPG_OUT_OF_MEMORY, NULL);
		return false;
	}

	if (isarray == ECPG_ARRAY_NONE)
	{
		/*
		 * if we don't have enough space, we cannot read all tuples
		 */
		if ((var->arrsize > 0 && ntuples > var->arrsize) || (var->ind_arrsize > 0 && ntuples > var->ind_arrsize))
		{
			ecpg_log("ecpg_store_result on line %d: incorrect number of matches; %d don't fit into array of %ld\n",
					 stmt->lineno, ntuples, var->arrsize);
			ecpg_raise(stmt->lineno, INFORMIX_MODE(stmt->compat) ? ECPG_INFORMIX_SUBSELECT_NOT_ONE : ECPG_TOO_MANY_MATCHES, ECPG_SQLSTATE_CARDINALITY_VIOLATION, NULL);
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
			ecpg_raise(stmt->lineno, ECPG_NO_ARRAY, ECPG_SQLSTATE_DATATYPE_MISMATCH, NULL);
			return false;
		}
	}

	/*
	 * allocate memory for NULL pointers
	 */
	if ((var->arrsize == 0 || var->varcharsize == 0) && var->value == NULL)
	{
		int			len = 0;

		if (!PQfformat(results, act_field))
		{
			switch (var->type)
			{
				case ECPGt_char:
				case ECPGt_unsigned_char:
				case ECPGt_string:
					if (!var->varcharsize && !var->arrsize)
					{
						/* special mode for handling char**foo=0 */
						for (act_tuple = 0; act_tuple < ntuples; act_tuple++)
							len += strlen(PQgetvalue(results, act_tuple, act_field)) + 1;
						len *= var->offset;		/* should be 1, but YMNK */
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
		}
		else
		{
			for (act_tuple = 0; act_tuple < ntuples; act_tuple++)
				len += PQgetlength(results, act_tuple, act_field);
		}

		ecpg_log("ecpg_store_result on line %d: allocating memory for %d tuples\n", stmt->lineno, ntuples);
		var->value = (char *) ecpg_auto_alloc(len, stmt->lineno);
		if (!var->value)
			return false;
		*((char **) var->pointer) = var->value;
	}

	/* allocate indicator variable if needed */
	if ((var->ind_arrsize == 0 || var->ind_varcharsize == 0) && var->ind_value == NULL && var->ind_pointer != NULL)
	{
		int			len = var->ind_offset * ntuples;

		var->ind_value = (char *) ecpg_auto_alloc(len, stmt->lineno);
		if (!var->ind_value)
			return false;
		*((char **) var->ind_pointer) = var->ind_value;
	}

	/* fill the variable with the tuple(s) */
	if (!var->varcharsize && !var->arrsize &&
		(var->type == ECPGt_char || var->type == ECPGt_unsigned_char || var->type == ECPGt_string))
	{
		/* special mode for handling char**foo=0 */

		/* filling the array of (char*)s */
		char	  **current_string = (char **) var->value;

		/* storing the data (after the last array element) */
		char	   *current_data_location = (char *) &current_string[ntuples + 1];

		for (act_tuple = 0; act_tuple < ntuples && status; act_tuple++)
		{
			int			len = strlen(PQgetvalue(results, act_tuple, act_field)) + 1;

			if (!ecpg_get_data(results, act_tuple, act_field, stmt->lineno,
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
			if (!ecpg_get_data(results, act_tuple, act_field, stmt->lineno,
							   var->type, var->ind_type, var->value,
							   var->ind_value, var->varcharsize, var->offset, var->ind_offset, isarray, stmt->compat, stmt->force_indicator))
				status = false;
		}
	}
	return status;
}

static void
sprintf_double_value(char *ptr, double value, const char *delim)
{
	if (isnan(value))
		sprintf(ptr, "%s%s", "NaN", delim);
	else if (isinf(value))
	{
		if (value < 0)
			sprintf(ptr, "%s%s", "-Infinity", delim);
		else
			sprintf(ptr, "%s%s", "Infinity", delim);
	}
	else
		sprintf(ptr, "%.15g%s", value, delim);
}

static void
sprintf_float_value(char *ptr, float value, const char *delim)
{
	if (isnan(value))
		sprintf(ptr, "%s%s", "NaN", delim);
	else if (isinf(value))
	{
		if (value < 0)
			sprintf(ptr, "%s%s", "-Infinity", delim);
		else
			sprintf(ptr, "%s%s", "Infinity", delim);
	}
	else
		sprintf(ptr, "%.15g%s", value, delim);
}

bool
ecpg_store_input(const int lineno, const bool force_indicator, const struct variable * var,
				 char **tobeinserted_p, bool quote)
{
	char	   *mallocedval = NULL;
	char	   *newcopy = NULL;

	/*
	 * arrays are not possible unless the column is an array, too FIXME: we do
	 * not know if the column is an array here array input to singleton column
	 * will result in a runtime error
	 */

	/*
	 * Some special treatment is needed for records since we want their
	 * contents to arrive in a comma-separated list on insert (I think).
	 */

	*tobeinserted_p = "";

	/* check for null value and set input buffer accordingly */
	switch (var->ind_type)
	{
		case ECPGt_short:
		case ECPGt_unsigned_short:
			if (*(short *) var->ind_value < 0)
				*tobeinserted_p = NULL;
			break;
		case ECPGt_int:
		case ECPGt_unsigned_int:
			if (*(int *) var->ind_value < 0)
				*tobeinserted_p = NULL;
			break;
		case ECPGt_long:
		case ECPGt_unsigned_long:
			if (*(long *) var->ind_value < 0L)
				*tobeinserted_p = NULL;
			break;
#ifdef HAVE_LONG_LONG_INT
		case ECPGt_long_long:
		case ECPGt_unsigned_long_long:
			if (*(long long int *) var->ind_value < (long long) 0)
				*tobeinserted_p = NULL;
			break;
#endif   /* HAVE_LONG_LONG_INT */
		case ECPGt_NO_INDICATOR:
			if (force_indicator == false)
			{
				if (ECPGis_noind_null(var->type, var->value))
					*tobeinserted_p = NULL;
			}
			break;
		default:
			break;
	}
	if (*tobeinserted_p != NULL)
	{
		int			asize = var->arrsize ? var->arrsize : 1;

		switch (var->type)
		{
				int			element;

			case ECPGt_short:
				if (!(mallocedval = ecpg_alloc(asize * 20, lineno)))
					return false;

				if (asize > 1)
				{
					strcpy(mallocedval, "{");

					for (element = 0; element < asize; element++)
						sprintf(mallocedval + strlen(mallocedval), "%hd,", ((short *) var->value)[element]);

					strcpy(mallocedval + strlen(mallocedval) - 1, "}");
				}
				else
					sprintf(mallocedval, "%hd", *((short *) var->value));

				*tobeinserted_p = mallocedval;
				break;

			case ECPGt_int:
				if (!(mallocedval = ecpg_alloc(asize * 20, lineno)))
					return false;

				if (asize > 1)
				{
					strcpy(mallocedval, "{");

					for (element = 0; element < asize; element++)
						sprintf(mallocedval + strlen(mallocedval), "%d,", ((int *) var->value)[element]);

					strcpy(mallocedval + strlen(mallocedval) - 1, "}");
				}
				else
					sprintf(mallocedval, "%d", *((int *) var->value));

				*tobeinserted_p = mallocedval;
				break;

			case ECPGt_unsigned_short:
				if (!(mallocedval = ecpg_alloc(asize * 20, lineno)))
					return false;

				if (asize > 1)
				{
					strcpy(mallocedval, "{");

					for (element = 0; element < asize; element++)
						sprintf(mallocedval + strlen(mallocedval), "%hu,", ((unsigned short *) var->value)[element]);

					strcpy(mallocedval + strlen(mallocedval) - 1, "}");
				}
				else
					sprintf(mallocedval, "%hu", *((unsigned short *) var->value));

				*tobeinserted_p = mallocedval;
				break;

			case ECPGt_unsigned_int:
				if (!(mallocedval = ecpg_alloc(asize * 20, lineno)))
					return false;

				if (asize > 1)
				{
					strcpy(mallocedval, "{");

					for (element = 0; element < asize; element++)
						sprintf(mallocedval + strlen(mallocedval), "%u,", ((unsigned int *) var->value)[element]);

					strcpy(mallocedval + strlen(mallocedval) - 1, "}");
				}
				else
					sprintf(mallocedval, "%u", *((unsigned int *) var->value));

				*tobeinserted_p = mallocedval;
				break;

			case ECPGt_long:
				if (!(mallocedval = ecpg_alloc(asize * 20, lineno)))
					return false;

				if (asize > 1)
				{
					strcpy(mallocedval, "{");

					for (element = 0; element < asize; element++)
						sprintf(mallocedval + strlen(mallocedval), "%ld,", ((long *) var->value)[element]);

					strcpy(mallocedval + strlen(mallocedval) - 1, "}");
				}
				else
					sprintf(mallocedval, "%ld", *((long *) var->value));

				*tobeinserted_p = mallocedval;
				break;

			case ECPGt_unsigned_long:
				if (!(mallocedval = ecpg_alloc(asize * 20, lineno)))
					return false;

				if (asize > 1)
				{
					strcpy(mallocedval, "{");

					for (element = 0; element < asize; element++)
						sprintf(mallocedval + strlen(mallocedval), "%lu,", ((unsigned long *) var->value)[element]);

					strcpy(mallocedval + strlen(mallocedval) - 1, "}");
				}
				else
					sprintf(mallocedval, "%lu", *((unsigned long *) var->value));

				*tobeinserted_p = mallocedval;
				break;
#ifdef HAVE_LONG_LONG_INT
			case ECPGt_long_long:
				if (!(mallocedval = ecpg_alloc(asize * 30, lineno)))
					return false;

				if (asize > 1)
				{
					strcpy(mallocedval, "{");

					for (element = 0; element < asize; element++)
						sprintf(mallocedval + strlen(mallocedval), "%lld,", ((long long int *) var->value)[element]);

					strcpy(mallocedval + strlen(mallocedval) - 1, "}");
				}
				else
					sprintf(mallocedval, "%lld", *((long long int *) var->value));

				*tobeinserted_p = mallocedval;
				break;

			case ECPGt_unsigned_long_long:
				if (!(mallocedval = ecpg_alloc(asize * 30, lineno)))
					return false;

				if (asize > 1)
				{
					strcpy(mallocedval, "{");

					for (element = 0; element < asize; element++)
						sprintf(mallocedval + strlen(mallocedval), "%llu,", ((unsigned long long int *) var->value)[element]);

					strcpy(mallocedval + strlen(mallocedval) - 1, "}");
				}
				else
					sprintf(mallocedval, "%llu", *((unsigned long long int *) var->value));

				*tobeinserted_p = mallocedval;
				break;
#endif   /* HAVE_LONG_LONG_INT */
			case ECPGt_float:
				if (!(mallocedval = ecpg_alloc(asize * 25, lineno)))
					return false;

				if (asize > 1)
				{
					strcpy(mallocedval, "{");

					for (element = 0; element < asize; element++)
						sprintf_float_value(mallocedval + strlen(mallocedval), ((float *) var->value)[element], ",");

					strcpy(mallocedval + strlen(mallocedval) - 1, "}");
				}
				else
					sprintf_float_value(mallocedval, *((float *) var->value), "");

				*tobeinserted_p = mallocedval;
				break;

			case ECPGt_double:
				if (!(mallocedval = ecpg_alloc(asize * 25, lineno)))
					return false;

				if (asize > 1)
				{
					strcpy(mallocedval, "{");

					for (element = 0; element < asize; element++)
						sprintf_double_value(mallocedval + strlen(mallocedval), ((double *) var->value)[element], ",");

					strcpy(mallocedval + strlen(mallocedval) - 1, "}");
				}
				else
					sprintf_double_value(mallocedval, *((double *) var->value), "");

				*tobeinserted_p = mallocedval;
				break;

			case ECPGt_bool:
				if (!(mallocedval = ecpg_alloc(var->arrsize + sizeof("{}"), lineno)))
					return false;

				if (var->arrsize > 1)
				{
					strcpy(mallocedval, "{");

					for (element = 0; element < asize; element++)
						sprintf(mallocedval + strlen(mallocedval), "%c,", (((bool *) var->value)[element]) ? 't' : 'f');

					strcpy(mallocedval + strlen(mallocedval) - 1, "}");
				}
				else
				{
					if (var->offset == sizeof(char))
						sprintf(mallocedval, "%c", (*((char *) var->value)) ? 't' : 'f');
					else if (var->offset == sizeof(int))
						sprintf(mallocedval, "%c", (*((int *) var->value)) ? 't' : 'f');
					else
						ecpg_raise(lineno, ECPG_CONVERT_BOOL, ECPG_SQLSTATE_DATATYPE_MISMATCH, NULL);
				}

				*tobeinserted_p = mallocedval;
				break;

			case ECPGt_char:
			case ECPGt_unsigned_char:
			case ECPGt_string:
				{
					/* set slen to string length if type is char * */
					int			slen = (var->varcharsize == 0) ? strlen((char *) var->value) : (unsigned int) var->varcharsize;

					if (!(newcopy = ecpg_alloc(slen + 1, lineno)))
						return false;

					strncpy(newcopy, (char *) var->value, slen);
					newcopy[slen] = '\0';

					mallocedval = quote_postgres(newcopy, quote, lineno);
					if (!mallocedval)
					{
						ecpg_free(newcopy);
						return false;
					}

					*tobeinserted_p = mallocedval;
				}
				break;
			case ECPGt_const:
			case ECPGt_char_variable:
				{
					int			slen = strlen((char *) var->value);

					if (!(mallocedval = ecpg_alloc(slen + 1, lineno)))
						return false;

					strncpy(mallocedval, (char *) var->value, slen);
					mallocedval[slen] = '\0';

					*tobeinserted_p = mallocedval;
				}
				break;
			case ECPGt_varchar:
				{
					struct ECPGgeneric_varchar *variable =
					(struct ECPGgeneric_varchar *) (var->value);

					if (!(newcopy = (char *) ecpg_alloc(variable->len + 1, lineno)))
						return false;

					strncpy(newcopy, variable->arr, variable->len);
					newcopy[variable->len] = '\0';

					mallocedval = quote_postgres(newcopy, quote, lineno);
					if (!mallocedval)
					{
						ecpg_free(newcopy);
						return false;
					}

					*tobeinserted_p = mallocedval;
				}
				break;

			case ECPGt_decimal:
			case ECPGt_numeric:
				{
					char	   *str = NULL;
					int			slen;
					numeric    *nval;

					if (var->arrsize > 1)
						mallocedval = ecpg_strdup("{", lineno);
					else
						mallocedval = ecpg_strdup("", lineno);

					if (!mallocedval)
						return false;

					for (element = 0; element < asize; element++)
					{
						int			result;

						nval = PGTYPESnumeric_new();
						if (!nval)
						{
							ecpg_free(mallocedval);
							return false;
						}

						if (var->type == ECPGt_numeric)
							result = PGTYPESnumeric_copy(&(((numeric *) (var->value))[element]), nval);
						else
							result = PGTYPESnumeric_from_decimal(&(((decimal *) (var->value))[element]), nval);

						if (result != 0)
						{
							PGTYPESnumeric_free(nval);
							ecpg_free(mallocedval);
							return false;
						}

						str = PGTYPESnumeric_to_asc(nval, nval->dscale);
						slen = strlen(str);
						PGTYPESnumeric_free(nval);

						if (!(newcopy = ecpg_realloc(mallocedval, strlen(mallocedval) + slen + 2, lineno)))
						{
							ecpg_free(mallocedval);
							ecpg_free(str);
							return false;
						}
						mallocedval = newcopy;

						/* also copy trailing '\0' */
						memcpy(mallocedval + strlen(mallocedval), str, slen + 1);
						if (var->arrsize > 1)
							strcpy(mallocedval + strlen(mallocedval), ",");

						ecpg_free(str);
					}

					if (var->arrsize > 1)
						strcpy(mallocedval + strlen(mallocedval) - 1, "}");

					*tobeinserted_p = mallocedval;
				}
				break;

			case ECPGt_interval:
				{
					char	   *str = NULL;
					int			slen;

					if (var->arrsize > 1)
						mallocedval = ecpg_strdup("{", lineno);
					else
						mallocedval = ecpg_strdup("", lineno);

					if (!mallocedval)
						return false;

					for (element = 0; element < asize; element++)
					{
						str = quote_postgres(PGTYPESinterval_to_asc(&(((interval *) (var->value))[element])), quote, lineno);
						if (!str)
						{
							ecpg_free(mallocedval);
							return false;
						}

						slen = strlen(str);

						if (!(newcopy = ecpg_realloc(mallocedval, strlen(mallocedval) + slen + 2, lineno)))
						{
							ecpg_free(mallocedval);
							ecpg_free(str);
							return false;
						}
						mallocedval = newcopy;

						/* also copy trailing '\0' */
						memcpy(mallocedval + strlen(mallocedval), str, slen + 1);
						if (var->arrsize > 1)
							strcpy(mallocedval + strlen(mallocedval), ",");

						ecpg_free(str);
					}

					if (var->arrsize > 1)
						strcpy(mallocedval + strlen(mallocedval) - 1, "}");

					*tobeinserted_p = mallocedval;
				}
				break;

			case ECPGt_date:
				{
					char	   *str = NULL;
					int			slen;

					if (var->arrsize > 1)
						mallocedval = ecpg_strdup("{", lineno);
					else
						mallocedval = ecpg_strdup("", lineno);

					if (!mallocedval)
						return false;

					for (element = 0; element < asize; element++)
					{
						str = quote_postgres(PGTYPESdate_to_asc(((date *) (var->value))[element]), quote, lineno);
						if (!str)
						{
							ecpg_free(mallocedval);
							return false;
						}

						slen = strlen(str);

						if (!(newcopy = ecpg_realloc(mallocedval, strlen(mallocedval) + slen + 2, lineno)))
						{
							ecpg_free(mallocedval);
							ecpg_free(str);
							return false;
						}
						mallocedval = newcopy;

						/* also copy trailing '\0' */
						memcpy(mallocedval + strlen(mallocedval), str, slen + 1);
						if (var->arrsize > 1)
							strcpy(mallocedval + strlen(mallocedval), ",");

						ecpg_free(str);
					}

					if (var->arrsize > 1)
						strcpy(mallocedval + strlen(mallocedval) - 1, "}");

					*tobeinserted_p = mallocedval;
				}
				break;

			case ECPGt_timestamp:
				{
					char	   *str = NULL;
					int			slen;

					if (var->arrsize > 1)
						mallocedval = ecpg_strdup("{", lineno);
					else
						mallocedval = ecpg_strdup("", lineno);

					if (!mallocedval)
						return false;

					for (element = 0; element < asize; element++)
					{
						str = quote_postgres(PGTYPEStimestamp_to_asc(((timestamp *) (var->value))[element]), quote, lineno);
						if (!str)
						{
							ecpg_free(mallocedval);
							return false;
						}

						slen = strlen(str);

						if (!(newcopy = ecpg_realloc(mallocedval, strlen(mallocedval) + slen + 2, lineno)))
						{
							ecpg_free(mallocedval);
							ecpg_free(str);
							return false;
						}
						mallocedval = newcopy;

						/* also copy trailing '\0' */
						memcpy(mallocedval + strlen(mallocedval), str, slen + 1);
						if (var->arrsize > 1)
							strcpy(mallocedval + strlen(mallocedval), ",");

						ecpg_free(str);
					}

					if (var->arrsize > 1)
						strcpy(mallocedval + strlen(mallocedval) - 1, "}");

					*tobeinserted_p = mallocedval;
				}
				break;

			case ECPGt_descriptor:
			case ECPGt_sqlda:
				break;

			default:
				/* Not implemented yet */
				ecpg_raise(lineno, ECPG_UNSUPPORTED, ECPG_SQLSTATE_ECPG_INTERNAL_ERROR, ecpg_type_name(var->type));
				return false;
				break;
		}
	}
	return true;
}

void
ecpg_free_params(struct statement * stmt, bool print)
{
	int			n;

	for (n = 0; n < stmt->nparams; n++)
	{
		if (print)
			ecpg_log("ecpg_free_params on line %d: parameter %d = %s\n", stmt->lineno, n + 1, stmt->paramvalues[n] ? stmt->paramvalues[n] : "null");
		ecpg_free(stmt->paramvalues[n]);
	}
	ecpg_free(stmt->paramvalues);
	stmt->paramvalues = NULL;
	stmt->nparams = 0;
}

static bool
insert_tobeinserted(int position, int ph_len, struct statement * stmt, char *tobeinserted)
{
	char	   *newcopy;

	if (!(newcopy = (char *) ecpg_alloc(strlen(stmt->command)
										+ strlen(tobeinserted)
										+ 1, stmt->lineno)))
	{
		ecpg_free(tobeinserted);
		return false;
	}

	strcpy(newcopy, stmt->command);
	strcpy(newcopy + position - 1, tobeinserted);

	/*
	 * The strange thing in the second argument is the rest of the string from
	 * the old string
	 */
	strcat(newcopy,
		   stmt->command
		   + position
		   + ph_len - 1);

	ecpg_free(stmt->command);
	stmt->command = newcopy;

	ecpg_free((char *) tobeinserted);
	return true;
}

/*
 * ecpg_build_params
 *		Build statement parameters
 *
 * The input values are taken from user variables, and the results are stored
 * in arrays which can be used by PQexecParams().
 */
bool
ecpg_build_params(struct statement * stmt)
{
	struct variable *var;
	int			desc_counter = 0;
	int			position = 0;

	/*
	 * If the type is one of the fill in types then we take the argument and
	 * enter it to our parameter array at the first position. Then if there
	 * are any more fill in types we add more parameters.
	 */
	var = stmt->inlist;
	while (var)
	{
		char	   *tobeinserted;
		int			counter = 1;

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

			desc = ecpg_find_desc(stmt->lineno, var->pointer);
			if (desc == NULL)
				return false;

			desc_counter++;
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
					if (!ecpg_store_input(stmt->lineno, stmt->force_indicator, &desc_inlist, &tobeinserted, false))
						return false;

					break;
				}
			}
			if (desc->count == desc_counter)
				desc_counter = 0;
		}
		else if (var->type == ECPGt_sqlda)
		{
			if (INFORMIX_MODE(stmt->compat))
			{
				struct sqlda_compat *sqlda = *(struct sqlda_compat **) var->pointer;
				struct variable desc_inlist;
				int			i;

				if (sqlda == NULL)
					return false;

				desc_counter++;
				for (i = 0; i < sqlda->sqld; i++)
				{
					if (i + 1 == desc_counter)
					{
						desc_inlist.type = sqlda->sqlvar[i].sqltype;
						desc_inlist.value = sqlda->sqlvar[i].sqldata;
						desc_inlist.pointer = &(sqlda->sqlvar[i].sqldata);
						switch (desc_inlist.type)
						{
							case ECPGt_char:
							case ECPGt_varchar:
								desc_inlist.varcharsize = strlen(sqlda->sqlvar[i].sqldata);
								break;
							default:
								desc_inlist.varcharsize = 0;
								break;
						}
						desc_inlist.arrsize = 1;
						desc_inlist.offset = 0;
						if (sqlda->sqlvar[i].sqlind)
						{
							desc_inlist.ind_type = ECPGt_short;
							/* ECPG expects indicator value < 0 */
							if (*(sqlda->sqlvar[i].sqlind))
								*(sqlda->sqlvar[i].sqlind) = -1;
							desc_inlist.ind_value = sqlda->sqlvar[i].sqlind;
							desc_inlist.ind_pointer = &(sqlda->sqlvar[i].sqlind);
							desc_inlist.ind_varcharsize = desc_inlist.ind_arrsize = 1;
							desc_inlist.ind_offset = 0;
						}
						else
						{
							desc_inlist.ind_type = ECPGt_NO_INDICATOR;
							desc_inlist.ind_value = desc_inlist.ind_pointer = NULL;
							desc_inlist.ind_varcharsize = desc_inlist.ind_arrsize = desc_inlist.ind_offset = 0;
						}
						if (!ecpg_store_input(stmt->lineno, stmt->force_indicator, &desc_inlist, &tobeinserted, false))
							return false;

						break;
					}
				}
				if (sqlda->sqld == desc_counter)
					desc_counter = 0;
			}
			else
			{
				struct sqlda_struct *sqlda = *(struct sqlda_struct **) var->pointer;
				struct variable desc_inlist;
				int			i;

				if (sqlda == NULL)
					return false;

				desc_counter++;
				for (i = 0; i < sqlda->sqln; i++)
				{
					if (i + 1 == desc_counter)
					{
						desc_inlist.type = sqlda->sqlvar[i].sqltype;
						desc_inlist.value = sqlda->sqlvar[i].sqldata;
						desc_inlist.pointer = &(sqlda->sqlvar[i].sqldata);
						switch (desc_inlist.type)
						{
							case ECPGt_char:
							case ECPGt_varchar:
								desc_inlist.varcharsize = strlen(sqlda->sqlvar[i].sqldata);
								break;
							default:
								desc_inlist.varcharsize = 0;
								break;
						}
						desc_inlist.arrsize = 1;
						desc_inlist.offset = 0;
						if (sqlda->sqlvar[i].sqlind)
						{
							desc_inlist.ind_type = ECPGt_short;
							/* ECPG expects indicator value < 0 */
							if (*(sqlda->sqlvar[i].sqlind))
								*(sqlda->sqlvar[i].sqlind) = -1;
							desc_inlist.ind_value = sqlda->sqlvar[i].sqlind;
							desc_inlist.ind_pointer = &(sqlda->sqlvar[i].sqlind);
							desc_inlist.ind_varcharsize = desc_inlist.ind_arrsize = 1;
							desc_inlist.ind_offset = 0;
						}
						else
						{
							desc_inlist.ind_type = ECPGt_NO_INDICATOR;
							desc_inlist.ind_value = desc_inlist.ind_pointer = NULL;
							desc_inlist.ind_varcharsize = desc_inlist.ind_arrsize = desc_inlist.ind_offset = 0;
						}
						if (!ecpg_store_input(stmt->lineno, stmt->force_indicator, &desc_inlist, &tobeinserted, false))
							return false;

						break;
					}
				}
				if (sqlda->sqln == desc_counter)
					desc_counter = 0;
			}

		}
		else
		{
			if (!ecpg_store_input(stmt->lineno, stmt->force_indicator, var, &tobeinserted, false))
				return false;
		}

		/*
		 * now tobeinserted points to an area that contains the next
		 * parameter; now find the position in the string where it belongs
		 */
		if ((position = next_insert(stmt->command, position, stmt->questionmarks) + 1) == 0)
		{
			/*
			 * We have an argument but we dont have the matched up placeholder
			 * in the string
			 */
			ecpg_raise(stmt->lineno, ECPG_TOO_MANY_ARGUMENTS,
					   ECPG_SQLSTATE_USING_CLAUSE_DOES_NOT_MATCH_PARAMETERS,
					   NULL);
			ecpg_free_params(stmt, false);
			return false;
		}

		/*
		 * if var->type=ECPGt_char_variable we have a dynamic cursor we have
		 * to simulate a dynamic cursor because there is no backend
		 * functionality for it
		 */
		if (var->type == ECPGt_char_variable)
		{
			int			ph_len = (stmt->command[position] == '?') ? strlen("?") : strlen("$1");

			if (!insert_tobeinserted(position, ph_len, stmt, tobeinserted))
			{
				ecpg_free_params(stmt, false);
				return false;
			}
			tobeinserted = NULL;
		}

		/*
		 * if the placeholder is '$0' we have to replace it on the client side
		 * this is for places we want to support variables at that are not
		 * supported in the backend
		 */
		else if (stmt->command[position] == '0')
		{
			if (!insert_tobeinserted(position, 2, stmt, tobeinserted))
			{
				ecpg_free_params(stmt, false);
				return false;
			}
			tobeinserted = NULL;
		}
		else
		{
			char	  **paramvalues;

			if (!(paramvalues = (char **) ecpg_realloc(stmt->paramvalues, sizeof(char *) * (stmt->nparams + 1), stmt->lineno)))
			{
				ecpg_free_params(stmt, false);
				return false;
			}

			stmt->nparams++;
			stmt->paramvalues = paramvalues;
			stmt->paramvalues[stmt->nparams - 1] = tobeinserted;

			/* let's see if this was an old style placeholder */
			if (stmt->command[position] == '?')
			{
				/* yes, replace with new style */
				int			buffersize = sizeof(int) * CHAR_BIT * 10 / 3;		/* a rough guess of the
																				 * size we need */

				if (!(tobeinserted = (char *) ecpg_alloc(buffersize, stmt->lineno)))
				{
					ecpg_free_params(stmt, false);
					return false;
				}

				snprintf(tobeinserted, buffersize, "$%d", counter++);

				if (!insert_tobeinserted(position, 2, stmt, tobeinserted))
				{
					ecpg_free_params(stmt, false);
					return false;
				}
				tobeinserted = NULL;
			}
		}

		if (desc_counter == 0)
			var = var->next;
	}

	/* Check if there are unmatched things left. */
	if (next_insert(stmt->command, position, stmt->questionmarks) >= 0)
	{
		ecpg_raise(stmt->lineno, ECPG_TOO_FEW_ARGUMENTS,
				 ECPG_SQLSTATE_USING_CLAUSE_DOES_NOT_MATCH_PARAMETERS, NULL);
		ecpg_free_params(stmt, false);
		return false;
	}

	return true;
}

/*
 * ecpg_autostart_transaction
 *		If we are in non-autocommit mode, automatically start a transaction.
 */
bool
ecpg_autostart_transaction(struct statement * stmt)
{
	if (PQtransactionStatus(stmt->connection->connection) == PQTRANS_IDLE && !stmt->connection->autocommit)
	{
		stmt->results = PQexec(stmt->connection->connection, "begin transaction");
		if (!ecpg_check_PQresult(stmt->results, stmt->lineno, stmt->connection->connection, stmt->compat))
		{
			ecpg_free_params(stmt, false);
			return false;
		}
		PQclear(stmt->results);
		stmt->results = NULL;
	}
	return true;
}

/*
 * ecpg_execute
 *		Execute the SQL statement.
 */
bool
ecpg_execute(struct statement * stmt)
{
	ecpg_log("ecpg_execute on line %d: query: %s; with %d parameter(s) on connection %s\n", stmt->lineno, stmt->command, stmt->nparams, stmt->connection->name);
	if (stmt->statement_type == ECPGst_execute)
	{
		stmt->results = PQexecPrepared(stmt->connection->connection, stmt->name, stmt->nparams, (const char *const *) stmt->paramvalues, NULL, NULL, 0);
		ecpg_log("ecpg_execute on line %d: using PQexecPrepared for \"%s\"\n", stmt->lineno, stmt->command);
	}
	else
	{
		if (stmt->nparams == 0)
		{
			stmt->results = PQexec(stmt->connection->connection, stmt->command);
			ecpg_log("ecpg_execute on line %d: using PQexec\n", stmt->lineno);
		}
		else
		{
			stmt->results = PQexecParams(stmt->connection->connection, stmt->command, stmt->nparams, NULL, (const char *const *) stmt->paramvalues, NULL, NULL, 0);
			ecpg_log("ecpg_execute on line %d: using PQexecParams\n", stmt->lineno);
		}
	}

	ecpg_free_params(stmt, true);

	if (!ecpg_check_PQresult(stmt->results, stmt->lineno, stmt->connection->connection, stmt->compat))
		return false;

	return true;
}

/*-------
 * ecpg_process_output
 *
 *	Process the statement result and store it into application variables.  This
 *	function can be called repeatedly during the same statement in case cursor
 *	readahead is used and the application does FETCH N which overflows the
 *	readahead window.
 *
 * Parameters
 *	stmt	statement structure holding the PGresult and
 *			the list of output variables
 *	clear_result
 *			PQclear() the result upon returning from this function
 *
 * Returns success as boolean. Also an SQL error is raised in case of failure.
 *-------
 */
bool
ecpg_process_output(struct statement * stmt, bool clear_result)
{
	struct variable *var;
	bool		status = false;
	char	   *cmdstat;
	PGnotify   *notify;
	struct sqlca_t *sqlca = ECPGget_sqlca();
	int			nfields,
				ntuples,
				act_field;

	if (sqlca == NULL)
	{
		ecpg_raise(stmt->lineno, ECPG_OUT_OF_MEMORY,
				   ECPG_SQLSTATE_ECPG_OUT_OF_MEMORY, NULL);
		return (false);
	}

	var = stmt->outlist;
	switch (PQresultStatus(stmt->results))
	{
		case PGRES_TUPLES_OK:
			nfields = PQnfields(stmt->results);
			sqlca->sqlerrd[2] = ntuples = PQntuples(stmt->results);

			ecpg_log("ecpg_process_output on line %d: correctly got %d tuples with %d fields\n", stmt->lineno, ntuples, nfields);
			status = true;

			if (ntuples < 1)
			{
				if (ntuples)
					ecpg_log("ecpg_process_output on line %d: incorrect number of matches (%d)\n",
							 stmt->lineno, ntuples);
				ecpg_raise(stmt->lineno, ECPG_NOT_FOUND, ECPG_SQLSTATE_NO_DATA, NULL);
				status = false;
				break;
			}

			if (var != NULL && var->type == ECPGt_descriptor)
			{
				struct descriptor *desc = ecpg_find_desc(stmt->lineno, var->pointer);

				if (desc == NULL)
					status = false;
				else
				{
					if (desc->result)
						PQclear(desc->result);
					desc->result = stmt->results;
					clear_result = false;
					ecpg_log("ecpg_process_output on line %d: putting result (%d tuples) into descriptor %s\n",
							 stmt->lineno, PQntuples(stmt->results), (const char *) var->pointer);
				}
				var = var->next;
			}
			else if (var != NULL && var->type == ECPGt_sqlda)
			{
				if (INFORMIX_MODE(stmt->compat))
				{
					struct sqlda_compat **_sqlda = (struct sqlda_compat **) var->pointer;
					struct sqlda_compat *sqlda = *_sqlda;
					struct sqlda_compat *sqlda_new;
					int			i;

					/*
					 * If we are passed in a previously existing sqlda (chain)
					 * then free it.
					 */
					while (sqlda)
					{
						sqlda_new = sqlda->desc_next;
						free(sqlda);
						sqlda = sqlda_new;
					}
					*_sqlda = sqlda = sqlda_new = NULL;
					for (i = ntuples - 1; i >= 0; i--)
					{
						/*
						 * Build a new sqlda structure. Note that only
						 * fetching 1 record is supported
						 */
						sqlda_new = ecpg_build_compat_sqlda(stmt->lineno, stmt->results, i, stmt->compat);

						if (!sqlda_new)
						{
							/* cleanup all SQLDAs we created up */
							while (sqlda)
							{
								sqlda_new = sqlda->desc_next;
								free(sqlda);
								sqlda = sqlda_new;
							}
							*_sqlda = NULL;

							ecpg_log("ecpg_process_output on line %d: out of memory allocating a new sqlda\n", stmt->lineno);
							status = false;
							break;
						}
						else
						{
							ecpg_log("ecpg_process_output on line %d: new sqlda was built\n", stmt->lineno);

							*_sqlda = sqlda_new;

							ecpg_set_compat_sqlda(stmt->lineno, _sqlda, stmt->results, i, stmt->compat);
							ecpg_log("ecpg_process_output on line %d: putting result (1 tuple %d fields) into sqlda descriptor\n",
									 stmt->lineno, PQnfields(stmt->results));

							sqlda_new->desc_next = sqlda;
							sqlda = sqlda_new;
						}
					}
				}
				else
				{
					struct sqlda_struct **_sqlda = (struct sqlda_struct **) var->pointer;
					struct sqlda_struct *sqlda = *_sqlda;
					struct sqlda_struct *sqlda_new;
					int			i;

					/*
					 * If we are passed in a previously existing sqlda (chain)
					 * then free it.
					 */
					while (sqlda)
					{
						sqlda_new = sqlda->desc_next;
						free(sqlda);
						sqlda = sqlda_new;
					}
					*_sqlda = sqlda = sqlda_new = NULL;
					for (i = ntuples - 1; i >= 0; i--)
					{
						/*
						 * Build a new sqlda structure. Note that only
						 * fetching 1 record is supported
						 */
						sqlda_new = ecpg_build_native_sqlda(stmt->lineno, stmt->results, i, stmt->compat);

						if (!sqlda_new)
						{
							/* cleanup all SQLDAs we created up */
							while (sqlda)
							{
								sqlda_new = sqlda->desc_next;
								free(sqlda);
								sqlda = sqlda_new;
							}
							*_sqlda = NULL;

							ecpg_log("ecpg_process_output on line %d: out of memory allocating a new sqlda\n", stmt->lineno);
							status = false;
							break;
						}
						else
						{
							ecpg_log("ecpg_process_output on line %d: new sqlda was built\n", stmt->lineno);

							*_sqlda = sqlda_new;

							ecpg_set_native_sqlda(stmt->lineno, _sqlda, stmt->results, i, stmt->compat);
							ecpg_log("ecpg_process_output on line %d: putting result (1 tuple %d fields) into sqlda descriptor\n",
									 stmt->lineno, PQnfields(stmt->results));

							sqlda_new->desc_next = sqlda;
							sqlda = sqlda_new;
						}
					}
				}

				var = var->next;
			}
			else
				for (act_field = 0; act_field < nfields && status; act_field++)
				{
					if (var != NULL)
					{
						status = ecpg_store_result(stmt->results, act_field, stmt, var);
						var = var->next;
					}
					else if (!INFORMIX_MODE(stmt->compat))
					{
						ecpg_raise(stmt->lineno, ECPG_TOO_FEW_ARGUMENTS, ECPG_SQLSTATE_USING_CLAUSE_DOES_NOT_MATCH_TARGETS, NULL);
						return (false);
					}
				}

			if (status && var != NULL)
			{
				ecpg_raise(stmt->lineno, ECPG_TOO_MANY_ARGUMENTS, ECPG_SQLSTATE_USING_CLAUSE_DOES_NOT_MATCH_TARGETS, NULL);
				status = false;
			}

			break;
		case PGRES_COMMAND_OK:
			status = true;
			cmdstat = PQcmdStatus(stmt->results);
			sqlca->sqlerrd[1] = PQoidValue(stmt->results);
			sqlca->sqlerrd[2] = atol(PQcmdTuples(stmt->results));
			ecpg_log("ecpg_process_output on line %d: OK: %s\n", stmt->lineno, cmdstat);
			if (stmt->compat != ECPG_COMPAT_INFORMIX_SE &&
				!sqlca->sqlerrd[2] &&
				(strncmp(cmdstat, "UPDATE", 6) == 0
				 || strncmp(cmdstat, "INSERT", 6) == 0
				 || strncmp(cmdstat, "DELETE", 6) == 0))
				ecpg_raise(stmt->lineno, ECPG_NOT_FOUND, ECPG_SQLSTATE_NO_DATA, NULL);
			break;
		case PGRES_COPY_OUT:
			{
				char	   *buffer;
				int			res;

				ecpg_log("ecpg_process_output on line %d: COPY OUT data transfer in progress\n", stmt->lineno);
				while ((res = PQgetCopyData(stmt->connection->connection,
											&buffer, 0)) > 0)
				{
					printf("%s", buffer);
					PQfreemem(buffer);
				}
				if (res == -1)
				{
					/* COPY done */
					PQclear(stmt->results);
					stmt->results = PQgetResult(stmt->connection->connection);
					if (PQresultStatus(stmt->results) == PGRES_COMMAND_OK)
						ecpg_log("ecpg_process_output on line %d: got PGRES_COMMAND_OK after PGRES_COPY_OUT\n", stmt->lineno);
					else
						ecpg_log("ecpg_process_output on line %d: got error after PGRES_COPY_OUT: %s", stmt->lineno, PQresultErrorMessage(stmt->results));
				}
				break;
			}
		default:

			/*
			 * execution should never reach this code because it is already
			 * handled in ECPGcheck_PQresult()
			 */
			ecpg_log("ecpg_process_output on line %d: unknown execution status type\n",
					 stmt->lineno);
			ecpg_raise_backend(stmt->lineno, stmt->results, stmt->connection->connection, stmt->compat);
			status = false;
			break;
	}

	if (clear_result)
	{
		PQclear(stmt->results);
		stmt->results = NULL;
	}

	/* check for asynchronous returns */
	notify = PQnotifies(stmt->connection->connection);
	if (notify)
	{
		ecpg_log("ecpg_process_output on line %d: asynchronous notification of \"%s\" from backend PID %d received\n",
				 stmt->lineno, notify->relname, notify->be_pid);
		PQfreemem(notify);
	}

	return status;
}

/*
 * ecpg_do_prologue
 *
 * Initialize various infrastructure elements for executing the statement:
 *
 *	- create the statement structure
 *	- set the C numeric locale for communicating with the backend
 *	- preprocess the variable list of input/output parameters into
 *	  linked lists
 */
bool
ecpg_do_prologue(int lineno, const int compat, const int force_indicator,
				 const char *connection_name, const bool questionmarks,
				 enum ECPG_statement_type statement_type, const char *query,
				 va_list args, struct statement ** stmt_out)
{
	struct statement *stmt;
	struct connection *con;
	enum ECPGttype type;
	struct variable **list;
	char	   *prepname;

	*stmt_out = NULL;

	if (!query)
	{
		ecpg_raise(lineno, ECPG_EMPTY, ECPG_SQLSTATE_ECPG_INTERNAL_ERROR, NULL);
		return false;
	}

	stmt = (struct statement *) ecpg_alloc(sizeof(struct statement), lineno);

	if (stmt == NULL)
		return false;

	/*
	 * Make sure we do NOT honor the locale for numeric input/output since the
	 * database wants the standard decimal point
	 */
	stmt->oldlocale = ecpg_strdup(setlocale(LC_NUMERIC, NULL), lineno);
	if (stmt->oldlocale == NULL)
	{
		ecpg_do_epilogue(stmt);
		return false;
	}
	setlocale(LC_NUMERIC, "C");

#ifdef ENABLE_THREAD_SAFETY
	ecpg_pthreads_init();
#endif

	con = ecpg_get_connection(connection_name);

	if (!ecpg_init(con, connection_name, lineno))
	{
		ecpg_do_epilogue(stmt);
		return false;
	}

	/*
	 * If statement type is ECPGst_prepnormal we are supposed to prepare the
	 * statement before executing them
	 */
	if (statement_type == ECPGst_prepnormal)
	{
		if (!ecpg_auto_prepare(lineno, connection_name, compat, &prepname, query))
		{
			ecpg_do_epilogue(stmt);
			return false;
		}

		/*
		 * statement is now prepared, so instead of the query we have to
		 * execute the name
		 */
		stmt->command = prepname;
		statement_type = ECPGst_execute;
	}
	else
		stmt->command = ecpg_strdup(query, lineno);

	stmt->name = NULL;

	if (statement_type == ECPGst_execute)
	{
		/* if we have an EXECUTE command, only the name is send */
		char	   *command = ecpg_prepared(stmt->command, con);

		if (command)
		{
			stmt->name = stmt->command;
			stmt->command = ecpg_strdup(command, lineno);
		}
		else
		{
			ecpg_raise(lineno, ECPG_INVALID_STMT, ECPG_SQLSTATE_INVALID_SQL_STATEMENT_NAME, stmt->command);
			ecpg_do_epilogue(stmt);
			return (false);
		}
	}

	stmt->connection = con;
	stmt->lineno = lineno;
	stmt->compat = compat;
	stmt->force_indicator = force_indicator;
	stmt->questionmarks = questionmarks;
	stmt->statement_type = statement_type;

	/*------
	 * create a list of variables
	 *
	 * The variables are listed with input variables preceding outputvariables
	 * The end of each group is marked by an end marker. per variable we list:
	 *
	 * type - as defined in ecpgtype.h
	 * value - where to store the data
	 * varcharsize - length of string in case we have a stringvariable, else 0
	 * arraysize - 0 for pointer (we don't know the size of the array), 1 for
	 * simple variable, size for arrays
	 * offset - offset between ith and (i+1)th entry in an array, normally
	 * that means sizeof(type)
	 * ind_type - type of indicator variable
	 * ind_value - pointer to indicator variable
	 * ind_varcharsize - empty
	 * ind_arraysize - arraysize of indicator array
	 * ind_offset - indicator offset
	 *------
	 */

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

			if (!(var = (struct variable *) ecpg_alloc(sizeof(struct variable), lineno)))
			{
				ecpg_do_epilogue(stmt);
				return false;
			}

			var->type = type;
			var->pointer = va_arg(args, char *);

			var->varcharsize = va_arg(args, long);
			var->arrsize = va_arg(args, long);
			var->offset = va_arg(args, long);

			/*
			 * Unknown array size means pointer to an array. Unknown
			 * varcharsize usually also means pointer. But if the type is
			 * character and the array size is known, it is an array of
			 * pointers to char, so use var->pointer as it is.
			 */
			if (var->arrsize == 0 ||
				(var->varcharsize == 0 && ((var->type != ECPGt_char && var->type != ECPGt_unsigned_char) || (var->arrsize <= 1))))
				var->value = *((char **) (var->pointer));
			else
				var->value = var->pointer;

			/*
			 * negative values are used to indicate an array without given
			 * bounds
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
			 * negative values are used to indicate an array without given
			 * bounds
			 */
			/* reset to zero for us */
			if (var->ind_arrsize < 0)
				var->ind_arrsize = 0;
			if (var->ind_varcharsize < 0)
				var->ind_varcharsize = 0;

			/* if variable is NULL, the statement hasn't been prepared */
			if (var->pointer == NULL)
			{
				ecpg_raise(lineno, ECPG_INVALID_STMT, ECPG_SQLSTATE_INVALID_SQL_STATEMENT_NAME, NULL);
				ecpg_free(var);
				ecpg_do_epilogue(stmt);
				return false;
			}

			for (ptr = *list; ptr && ptr->next; ptr = ptr->next)
				;

			if (ptr == NULL)
				*list = var;
			else
				ptr->next = var;
		}

		type = va_arg(args, enum ECPGttype);
	}

	/* are we connected? */
	if (con == NULL || con->connection == NULL)
	{
		ecpg_raise(lineno, ECPG_NOT_CONN, ECPG_SQLSTATE_ECPG_INTERNAL_ERROR, (con) ? con->name : ecpg_gettext("<empty>"));
		ecpg_do_epilogue(stmt);
		return false;
	}

	/* initialize auto_mem struct */
	ecpg_clear_auto_mem();

	*stmt_out = stmt;

	return true;
}

/*
 * ecpg_do_epilogue
 *	  Restore the application locale and free the statement structure.
 */
void
ecpg_do_epilogue(struct statement * stmt)
{
	if (stmt == NULL)
		return;

	if (stmt->oldlocale)
		setlocale(LC_NUMERIC, stmt->oldlocale);

	free_statement(stmt);
}

/*
 * Execute SQL statements in the backend.
 * The input/output parameters (variable argument list) are passed
 * in a va_list, so other functions can use this interface.
 */
bool
ecpg_do(const int lineno, const int compat, const int force_indicator, const char *connection_name, const bool questionmarks, const int st, const char *query, va_list args)
{
	struct statement *stmt = NULL;

	if (!ecpg_do_prologue(lineno, compat, force_indicator, connection_name,
						  questionmarks, (enum ECPG_statement_type) st,
						  query, args, &stmt))
		goto fail;

	if (!ecpg_build_params(stmt))
		goto fail;

	if (!ecpg_autostart_transaction(stmt))
		goto fail;

	if (!ecpg_execute(stmt))
		goto fail;

	if (!ecpg_process_output(stmt, true))
		goto fail;

	ecpg_do_epilogue(stmt);
	return true;

fail:
	ecpg_do_epilogue(stmt);
	return false;
}

/*
 * Execute SQL statements in the backend.
 * The input/output parameters are passed as variable-length argument list.
 */
bool
ECPGdo(const int lineno, const int compat, const int force_indicator, const char *connection_name, const bool questionmarks, const int st, const char *query,...)
{
	va_list		args;
	bool		ret;

	va_start(args, query);
	ret = ecpg_do(lineno, compat, force_indicator, connection_name,
				  questionmarks, st, query, args);
	va_end(args);

	return ret;
}

/* old descriptor interface */
bool
ECPGdo_descriptor(int line, const char *connection,
				  const char *descriptor, const char *query)
{
	return ECPGdo(line, ECPG_COMPAT_PGSQL, true, connection, '\0', 0, query, ECPGt_EOIT,
				  ECPGt_descriptor, descriptor, 0L, 0L, 0L,
				  ECPGt_NO_INDICATOR, NULL, 0L, 0L, 0L, ECPGt_EORT);
}

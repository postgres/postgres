/* src/interfaces/ecpg/ecpglib/error.c */

#define POSTGRES_ECPG_INTERNAL
#include "postgres_fe.h"

#include "ecpgerrno.h"
#include "ecpglib.h"
#include "ecpglib_extern.h"
#include "ecpgtype.h"
#include "sqlca.h"

void
ecpg_raise(int line, int code, const char *sqlstate, const char *str)
{
	struct sqlca_t *sqlca = ECPGget_sqlca();

	if (sqlca == NULL)
	{
		ecpg_log("out of memory");
		ECPGfree_auto_mem();
		return;
	}

	sqlca->sqlcode = code;
	strncpy(sqlca->sqlstate, sqlstate, sizeof(sqlca->sqlstate));

	switch (code)
	{
		case ECPG_NOT_FOUND:
			snprintf(sqlca->sqlerrm.sqlerrmc, sizeof(sqlca->sqlerrm.sqlerrmc),
			/*------
			   translator: this string will be truncated at 149 characters expanded.  */
					 ecpg_gettext("no data found on line %d"), line);
			break;

		case ECPG_OUT_OF_MEMORY:
			snprintf(sqlca->sqlerrm.sqlerrmc, sizeof(sqlca->sqlerrm.sqlerrmc),
			/*------
			   translator: this string will be truncated at 149 characters expanded.  */
					 ecpg_gettext("out of memory on line %d"), line);
			break;

		case ECPG_UNSUPPORTED:
			snprintf(sqlca->sqlerrm.sqlerrmc, sizeof(sqlca->sqlerrm.sqlerrmc),
			/*------
			   translator: this string will be truncated at 149 characters expanded.  */
					 ecpg_gettext("unsupported type \"%s\" on line %d"), str, line);
			break;

		case ECPG_TOO_MANY_ARGUMENTS:
			snprintf(sqlca->sqlerrm.sqlerrmc, sizeof(sqlca->sqlerrm.sqlerrmc),
			/*------
			   translator: this string will be truncated at 149 characters expanded.  */
					 ecpg_gettext("too many arguments on line %d"), line);
			break;

		case ECPG_TOO_FEW_ARGUMENTS:
			snprintf(sqlca->sqlerrm.sqlerrmc, sizeof(sqlca->sqlerrm.sqlerrmc),
			/*------
			   translator: this string will be truncated at 149 characters expanded.  */
					 ecpg_gettext("too few arguments on line %d"), line);
			break;

		case ECPG_INT_FORMAT:
			snprintf(sqlca->sqlerrm.sqlerrmc, sizeof(sqlca->sqlerrm.sqlerrmc),
			/*------
			   translator: this string will be truncated at 149 characters expanded.  */
					 ecpg_gettext("invalid input syntax for type int: \"%s\", on line %d"), str, line);
			break;

		case ECPG_UINT_FORMAT:
			snprintf(sqlca->sqlerrm.sqlerrmc, sizeof(sqlca->sqlerrm.sqlerrmc),
			/*------
			   translator: this string will be truncated at 149 characters expanded.  */
					 ecpg_gettext("invalid input syntax for type unsigned int: \"%s\", on line %d"), str, line);
			break;

		case ECPG_FLOAT_FORMAT:
			snprintf(sqlca->sqlerrm.sqlerrmc, sizeof(sqlca->sqlerrm.sqlerrmc),
			/*------
			   translator: this string will be truncated at 149 characters expanded.  */
					 ecpg_gettext("invalid input syntax for floating-point type: \"%s\", on line %d"), str, line);
			break;

		case ECPG_CONVERT_BOOL:
			if (str)
				snprintf(sqlca->sqlerrm.sqlerrmc, sizeof(sqlca->sqlerrm.sqlerrmc),
				/*------
				   translator: this string will be truncated at 149 characters expanded.  */
						 ecpg_gettext("invalid syntax for type boolean: \"%s\", on line %d"), str, line);
			else
				snprintf(sqlca->sqlerrm.sqlerrmc, sizeof(sqlca->sqlerrm.sqlerrmc),
				/*------
				   translator: this string will be truncated at 149 characters expanded.  */
						 ecpg_gettext("could not convert boolean value: size mismatch, on line %d"), line);
			break;

		case ECPG_EMPTY:
			snprintf(sqlca->sqlerrm.sqlerrmc, sizeof(sqlca->sqlerrm.sqlerrmc),
			/*------
			   translator: this string will be truncated at 149 characters expanded.  */
					 ecpg_gettext("empty query on line %d"), line);
			break;

		case ECPG_MISSING_INDICATOR:
			snprintf(sqlca->sqlerrm.sqlerrmc, sizeof(sqlca->sqlerrm.sqlerrmc),
			/*------
			   translator: this string will be truncated at 149 characters expanded.  */
					 ecpg_gettext("null value without indicator on line %d"), line);
			break;

		case ECPG_NO_ARRAY:
			snprintf(sqlca->sqlerrm.sqlerrmc, sizeof(sqlca->sqlerrm.sqlerrmc),
			/*------
			   translator: this string will be truncated at 149 characters expanded.  */
					 ecpg_gettext("variable does not have an array type on line %d"), line);
			break;

		case ECPG_DATA_NOT_ARRAY:
			snprintf(sqlca->sqlerrm.sqlerrmc, sizeof(sqlca->sqlerrm.sqlerrmc),
			/*------
			   translator: this string will be truncated at 149 characters expanded.  */
					 ecpg_gettext("data read from server is not an array on line %d"), line);
			break;

		case ECPG_ARRAY_INSERT:
			snprintf(sqlca->sqlerrm.sqlerrmc, sizeof(sqlca->sqlerrm.sqlerrmc),
			/*------
			   translator: this string will be truncated at 149 characters expanded.  */
					 ecpg_gettext("inserting an array of variables is not supported on line %d"), line);
			break;

		case ECPG_NO_CONN:
			snprintf(sqlca->sqlerrm.sqlerrmc, sizeof(sqlca->sqlerrm.sqlerrmc),
			/*------
			   translator: this string will be truncated at 149 characters expanded.  */
					 ecpg_gettext("connection \"%s\" does not exist on line %d"), str, line);
			break;

		case ECPG_NOT_CONN:
			snprintf(sqlca->sqlerrm.sqlerrmc, sizeof(sqlca->sqlerrm.sqlerrmc),
			/*------
			   translator: this string will be truncated at 149 characters expanded.  */
					 ecpg_gettext("not connected to connection \"%s\" on line %d"), str, line);
			break;

		case ECPG_INVALID_STMT:
			snprintf(sqlca->sqlerrm.sqlerrmc, sizeof(sqlca->sqlerrm.sqlerrmc),
			/*------
			   translator: this string will be truncated at 149 characters expanded.  */
					 ecpg_gettext("invalid statement name \"%s\" on line %d"), str, line);
			break;

		case ECPG_UNKNOWN_DESCRIPTOR:
			snprintf(sqlca->sqlerrm.sqlerrmc, sizeof(sqlca->sqlerrm.sqlerrmc),
			/*------
			   translator: this string will be truncated at 149 characters expanded.  */
					 ecpg_gettext("descriptor \"%s\" not found on line %d"), str, line);
			break;

		case ECPG_INVALID_DESCRIPTOR_INDEX:
			snprintf(sqlca->sqlerrm.sqlerrmc, sizeof(sqlca->sqlerrm.sqlerrmc),
			/*------
			   translator: this string will be truncated at 149 characters expanded.  */
					 ecpg_gettext("descriptor index out of range on line %d"), line);
			break;

		case ECPG_UNKNOWN_DESCRIPTOR_ITEM:
			snprintf(sqlca->sqlerrm.sqlerrmc, sizeof(sqlca->sqlerrm.sqlerrmc),
			/*------
			   translator: this string will be truncated at 149 characters expanded.  */
					 ecpg_gettext("unrecognized descriptor item \"%s\" on line %d"), str, line);
			break;

		case ECPG_VAR_NOT_NUMERIC:
			snprintf(sqlca->sqlerrm.sqlerrmc, sizeof(sqlca->sqlerrm.sqlerrmc),
			/*------
			   translator: this string will be truncated at 149 characters expanded.  */
					 ecpg_gettext("variable does not have a numeric type on line %d"), line);
			break;

		case ECPG_VAR_NOT_CHAR:
			snprintf(sqlca->sqlerrm.sqlerrmc, sizeof(sqlca->sqlerrm.sqlerrmc),
			/*------
			   translator: this string will be truncated at 149 characters expanded.  */
					 ecpg_gettext("variable does not have a character type on line %d"), line);
			break;

		case ECPG_TRANS:
			snprintf(sqlca->sqlerrm.sqlerrmc, sizeof(sqlca->sqlerrm.sqlerrmc),
			/*------
			   translator: this string will be truncated at 149 characters expanded.  */
					 ecpg_gettext("error in transaction processing on line %d"), line);
			break;

		case ECPG_CONNECT:
			snprintf(sqlca->sqlerrm.sqlerrmc, sizeof(sqlca->sqlerrm.sqlerrmc),
			/*------
			   translator: this string will be truncated at 149 characters expanded.  */
					 ecpg_gettext("could not connect to database \"%s\" on line %d"), str, line);
			break;

		default:
			snprintf(sqlca->sqlerrm.sqlerrmc, sizeof(sqlca->sqlerrm.sqlerrmc),
			/*------
			   translator: this string will be truncated at 149 characters expanded.  */
					 ecpg_gettext("SQL error %d on line %d"), code, line);
			break;
	}

	sqlca->sqlerrm.sqlerrml = strlen(sqlca->sqlerrm.sqlerrmc);
	ecpg_log("raising sqlcode %d on line %d: %s\n", code, line, sqlca->sqlerrm.sqlerrmc);

	/* free all memory we have allocated for the user */
	ECPGfree_auto_mem();
}

void
ecpg_raise_backend(int line, PGresult *result, PGconn *conn, int compat)
{
	struct sqlca_t *sqlca = ECPGget_sqlca();
	char	   *sqlstate;
	char	   *message;

	if (sqlca == NULL)
	{
		ecpg_log("out of memory");
		ECPGfree_auto_mem();
		return;
	}

	if (result)
	{
		sqlstate = PQresultErrorField(result, PG_DIAG_SQLSTATE);
		if (sqlstate == NULL)
			sqlstate = ECPG_SQLSTATE_ECPG_INTERNAL_ERROR;
		message = PQresultErrorField(result, PG_DIAG_MESSAGE_PRIMARY);
	}
	else
	{
		sqlstate = ECPG_SQLSTATE_ECPG_INTERNAL_ERROR;
		message = PQerrorMessage(conn);
	}

	if (strcmp(sqlstate, ECPG_SQLSTATE_ECPG_INTERNAL_ERROR) == 0)
	{
		/*
		 * we might get here if the connection breaks down, so let's check for
		 * this instead of giving just the generic internal error
		 */
		if (PQstatus(conn) == CONNECTION_BAD)
		{
			sqlstate = "57P02";
			message = ecpg_gettext("the connection to the server was lost");
		}
	}

	/* copy error message */
	snprintf(sqlca->sqlerrm.sqlerrmc, sizeof(sqlca->sqlerrm.sqlerrmc), "%s on line %d", message, line);
	sqlca->sqlerrm.sqlerrml = strlen(sqlca->sqlerrm.sqlerrmc);

	/* copy SQLSTATE */
	strncpy(sqlca->sqlstate, sqlstate, sizeof(sqlca->sqlstate));

	/* assign SQLCODE for backward compatibility */
	if (strncmp(sqlca->sqlstate, "23505", sizeof(sqlca->sqlstate)) == 0)
		sqlca->sqlcode = INFORMIX_MODE(compat) ? ECPG_INFORMIX_DUPLICATE_KEY : ECPG_DUPLICATE_KEY;
	else if (strncmp(sqlca->sqlstate, "21000", sizeof(sqlca->sqlstate)) == 0)
		sqlca->sqlcode = INFORMIX_MODE(compat) ? ECPG_INFORMIX_SUBSELECT_NOT_ONE : ECPG_SUBSELECT_NOT_ONE;
	else
		sqlca->sqlcode = ECPG_PGSQL;

	/* %.*s is safe here as long as sqlstate is all-ASCII */
	ecpg_log("raising sqlstate %.*s (sqlcode %ld): %s\n",
			 (int) sizeof(sqlca->sqlstate), sqlca->sqlstate, sqlca->sqlcode, sqlca->sqlerrm.sqlerrmc);

	/* free all memory we have allocated for the user */
	ECPGfree_auto_mem();
}

/* filter out all error codes */
bool
ecpg_check_PQresult(PGresult *results, int lineno, PGconn *connection, enum COMPAT_MODE compat)
{
	if (results == NULL)
	{
		ecpg_log("ecpg_check_PQresult on line %d: no result - %s", lineno, PQerrorMessage(connection));
		ecpg_raise_backend(lineno, NULL, connection, compat);
		return false;
	}

	switch (PQresultStatus(results))
	{

		case PGRES_TUPLES_OK:
			return true;
			break;
		case PGRES_EMPTY_QUERY:
			/* do nothing */
			ecpg_raise(lineno, ECPG_EMPTY, ECPG_SQLSTATE_ECPG_INTERNAL_ERROR, NULL);
			PQclear(results);
			return false;
			break;
		case PGRES_COMMAND_OK:
			return true;
			break;
		case PGRES_NONFATAL_ERROR:
		case PGRES_FATAL_ERROR:
		case PGRES_BAD_RESPONSE:
			ecpg_log("ecpg_check_PQresult on line %d: bad response - %s", lineno, PQresultErrorMessage(results));
			ecpg_raise_backend(lineno, results, connection, compat);
			PQclear(results);
			return false;
			break;
		case PGRES_COPY_OUT:
			return true;
			break;
		case PGRES_COPY_IN:
			ecpg_log("ecpg_check_PQresult on line %d: COPY IN data transfer in progress\n", lineno);
			PQendcopy(connection);
			PQclear(results);
			return false;
			break;
		default:
			ecpg_log("ecpg_check_PQresult on line %d: unknown execution status type\n",
					 lineno);
			ecpg_raise_backend(lineno, results, connection, compat);
			PQclear(results);
			return false;
			break;
	}
}

/* print out an error message */
void
sqlprint(void)
{
	struct sqlca_t *sqlca = ECPGget_sqlca();

	if (sqlca == NULL)
	{
		ecpg_log("out of memory");
		return;
	}

	sqlca->sqlerrm.sqlerrmc[sqlca->sqlerrm.sqlerrml] = '\0';
	fprintf(stderr, ecpg_gettext("SQL error: %s\n"), sqlca->sqlerrm.sqlerrmc);
}

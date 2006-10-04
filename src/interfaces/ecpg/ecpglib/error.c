/* $PostgreSQL: pgsql/src/interfaces/ecpg/ecpglib/error.c,v 1.15 2006/10/04 00:30:11 momjian Exp $ */

#define POSTGRES_ECPG_INTERNAL
#include "postgres_fe.h"

#include "ecpgerrno.h"
#include "ecpgtype.h"
#include "ecpglib.h"
#include "extern.h"
#include "sqlca.h"


void
ECPGraise(int line, int code, const char *sqlstate, const char *str)
{
	struct sqlca_t *sqlca = ECPGget_sqlca();

	sqlca->sqlcode = code;
	strncpy(sqlca->sqlstate, sqlstate, sizeof(sqlca->sqlstate));

	switch (code)
	{
		case ECPG_NOT_FOUND:
			snprintf(sqlca->sqlerrm.sqlerrmc, sizeof(sqlca->sqlerrm.sqlerrmc),
					 "No data found in line %d.", line);
			break;

		case ECPG_OUT_OF_MEMORY:
			snprintf(sqlca->sqlerrm.sqlerrmc, sizeof(sqlca->sqlerrm.sqlerrmc),
					 "Out of memory in line %d.", line);
			break;

		case ECPG_UNSUPPORTED:
			snprintf(sqlca->sqlerrm.sqlerrmc, sizeof(sqlca->sqlerrm.sqlerrmc),
					 "Unsupported type %s in line %d.", str, line);
			break;

		case ECPG_TOO_MANY_ARGUMENTS:
			snprintf(sqlca->sqlerrm.sqlerrmc, sizeof(sqlca->sqlerrm.sqlerrmc),
					 "Too many arguments in line %d.", line);
			break;

		case ECPG_TOO_FEW_ARGUMENTS:
			snprintf(sqlca->sqlerrm.sqlerrmc, sizeof(sqlca->sqlerrm.sqlerrmc),
					 "Too few arguments in line %d.", line);
			break;

		case ECPG_INT_FORMAT:
			snprintf(sqlca->sqlerrm.sqlerrmc, sizeof(sqlca->sqlerrm.sqlerrmc),
				 "Not correctly formatted int type: %s line %d.", str, line);
			break;

		case ECPG_UINT_FORMAT:
			snprintf(sqlca->sqlerrm.sqlerrmc, sizeof(sqlca->sqlerrm.sqlerrmc),
					 "Not correctly formatted unsigned type: %s in line %d.", str, line);
			break;

		case ECPG_FLOAT_FORMAT:
			snprintf(sqlca->sqlerrm.sqlerrmc, sizeof(sqlca->sqlerrm.sqlerrmc),
					 "Not correctly formatted floating-point type: %s in line %d.", str, line);
			break;

		case ECPG_CONVERT_BOOL:
			snprintf(sqlca->sqlerrm.sqlerrmc, sizeof(sqlca->sqlerrm.sqlerrmc),
					 "Unable to convert %s to bool on line %d.", str, line);
			break;

		case ECPG_EMPTY:
			snprintf(sqlca->sqlerrm.sqlerrmc, sizeof(sqlca->sqlerrm.sqlerrmc),
					 "Empty query in line %d.", line);
			break;

		case ECPG_MISSING_INDICATOR:
			snprintf(sqlca->sqlerrm.sqlerrmc, sizeof(sqlca->sqlerrm.sqlerrmc),
					 "NULL value without indicator in line %d.", line);
			break;

		case ECPG_NO_ARRAY:
			snprintf(sqlca->sqlerrm.sqlerrmc, sizeof(sqlca->sqlerrm.sqlerrmc),
					 "Variable is not an array in line %d.", line);
			break;

		case ECPG_DATA_NOT_ARRAY:
			snprintf(sqlca->sqlerrm.sqlerrmc, sizeof(sqlca->sqlerrm.sqlerrmc),
				 "Data read from backend is not an array in line %d.", line);
			break;

		case ECPG_ARRAY_INSERT:
			snprintf(sqlca->sqlerrm.sqlerrmc, sizeof(sqlca->sqlerrm.sqlerrmc),
				 "Trying to insert an array of variables in line %d.", line);
			break;

		case ECPG_NO_CONN:
			snprintf(sqlca->sqlerrm.sqlerrmc, sizeof(sqlca->sqlerrm.sqlerrmc),
					 "No such connection %s in line %d.", str, line);
			break;

		case ECPG_NOT_CONN:
			snprintf(sqlca->sqlerrm.sqlerrmc, sizeof(sqlca->sqlerrm.sqlerrmc),
					 "Not connected to '%s' in line %d.", str, line);
			break;

		case ECPG_INVALID_STMT:
			snprintf(sqlca->sqlerrm.sqlerrmc, sizeof(sqlca->sqlerrm.sqlerrmc),
					 "Invalid statement name %s in line %d.", str, line);
			break;

		case ECPG_UNKNOWN_DESCRIPTOR:
			snprintf(sqlca->sqlerrm.sqlerrmc, sizeof(sqlca->sqlerrm.sqlerrmc),
					 "Descriptor %s not found in line %d.", str, line);
			break;

		case ECPG_INVALID_DESCRIPTOR_INDEX:
			snprintf(sqlca->sqlerrm.sqlerrmc, sizeof(sqlca->sqlerrm.sqlerrmc),
					 "Descriptor index out of range in line %d.", line);
			break;

		case ECPG_UNKNOWN_DESCRIPTOR_ITEM:
			snprintf(sqlca->sqlerrm.sqlerrmc, sizeof(sqlca->sqlerrm.sqlerrmc),
					 "Unknown descriptor item %s in line %d.", str, line);
			break;

		case ECPG_VAR_NOT_NUMERIC:
			snprintf(sqlca->sqlerrm.sqlerrmc, sizeof(sqlca->sqlerrm.sqlerrmc),
					 "Variable is not a numeric type in line %d.", line);
			break;

		case ECPG_VAR_NOT_CHAR:
			snprintf(sqlca->sqlerrm.sqlerrmc, sizeof(sqlca->sqlerrm.sqlerrmc),
					 "Variable is not a character type in line %d.", line);
			break;

		case ECPG_TRANS:
			snprintf(sqlca->sqlerrm.sqlerrmc, sizeof(sqlca->sqlerrm.sqlerrmc),
					 "Error in transaction processing in line %d.", line);
			break;

		case ECPG_CONNECT:
			snprintf(sqlca->sqlerrm.sqlerrmc, sizeof(sqlca->sqlerrm.sqlerrmc),
				  "Could not connect to database %s in line %d.", str, line);
			break;

		default:
			snprintf(sqlca->sqlerrm.sqlerrmc, sizeof(sqlca->sqlerrm.sqlerrmc),
					 "SQL error #%d in line %d.", code, line);
			break;
	}

	sqlca->sqlerrm.sqlerrml = strlen(sqlca->sqlerrm.sqlerrmc);
	ECPGlog("raising sqlcode %d in line %d, '%s'.\n", code, line, sqlca->sqlerrm.sqlerrmc);

	/* free all memory we have allocated for the user */
	ECPGfree_auto_mem();
}

void
ECPGraise_backend(int line, PGresult *result, PGconn *conn, int compat)
{
	struct sqlca_t *sqlca = ECPGget_sqlca();
	char	   *sqlstate;
	char	   *message;

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

	/* copy error message */
	snprintf(sqlca->sqlerrm.sqlerrmc, sizeof(sqlca->sqlerrm.sqlerrmc),
			 "'%s' in line %d.", message, line);
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

	ECPGlog("raising sqlstate %.*s (sqlcode: %d) in line %d, '%s'.\n",
			sizeof(sqlca->sqlstate), sqlca->sqlstate, sqlca->sqlcode, line, sqlca->sqlerrm.sqlerrmc);

	/* free all memory we have allocated for the user */
	ECPGfree_auto_mem();
}

/* print out an error message */
void
sqlprint(void)
{
	struct sqlca_t *sqlca = ECPGget_sqlca();

	sqlca->sqlerrm.sqlerrmc[sqlca->sqlerrm.sqlerrml] = '\0';
	fprintf(stderr, "sql error %s\n", sqlca->sqlerrm.sqlerrmc);
}

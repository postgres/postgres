#include <stdio.h>

#include <ecpgerrno.h>
#include <ecpgtype.h>
#include <ecpglib.h>
#include "extern.h"
#include <sqlca.h>

void
ECPGraise(int line, int code, char *str)
{
	sqlca.sqlcode = code;

	switch (code)
	{
		case ECPG_NOT_FOUND:
			snprintf(sqlca.sqlerrm.sqlerrmc, sizeof(sqlca.sqlerrm.sqlerrmc),
					 "No data found in line %d.", line);
			break;

		case ECPG_OUT_OF_MEMORY:
			snprintf(sqlca.sqlerrm.sqlerrmc, sizeof(sqlca.sqlerrm.sqlerrmc),
					 "Out of memory in line %d.", line);
			break;

		case ECPG_UNSUPPORTED:
			snprintf(sqlca.sqlerrm.sqlerrmc, sizeof(sqlca.sqlerrm.sqlerrmc),
					 "Unsupported type %s in line %d.", str, line);
			break;

		case ECPG_TOO_MANY_ARGUMENTS:
			snprintf(sqlca.sqlerrm.sqlerrmc, sizeof(sqlca.sqlerrm.sqlerrmc),
					 "Too many arguments in line %d.", line);
			break;

		case ECPG_TOO_FEW_ARGUMENTS:
			snprintf(sqlca.sqlerrm.sqlerrmc, sizeof(sqlca.sqlerrm.sqlerrmc),
					 "Too few arguments in line %d.", line);
			break;

		case ECPG_INT_FORMAT:
			snprintf(sqlca.sqlerrm.sqlerrmc, sizeof(sqlca.sqlerrm.sqlerrmc),
			 "Not correctly formatted int type: %s line %d.", str, line);
			break;

		case ECPG_UINT_FORMAT:
			snprintf(sqlca.sqlerrm.sqlerrmc, sizeof(sqlca.sqlerrm.sqlerrmc),
					 "Not correctly formatted unsigned type: %s in line %d.", str, line);
			break;

		case ECPG_FLOAT_FORMAT:
			snprintf(sqlca.sqlerrm.sqlerrmc, sizeof(sqlca.sqlerrm.sqlerrmc),
					 "Not correctly formatted floating point type: %s in line %d.", str, line);
			break;

		case ECPG_CONVERT_BOOL:
			snprintf(sqlca.sqlerrm.sqlerrmc, sizeof(sqlca.sqlerrm.sqlerrmc),
				  "Unable to convert %s to bool on line %d.", str, line);
			break;

		case ECPG_EMPTY:
			snprintf(sqlca.sqlerrm.sqlerrmc, sizeof(sqlca.sqlerrm.sqlerrmc),
					 "Empty query in line %d.", line);
			break;

		case ECPG_MISSING_INDICATOR:
			snprintf(sqlca.sqlerrm.sqlerrmc, sizeof(sqlca.sqlerrm.sqlerrmc),
					 "NULL value without indicator in line %d.", line);
			break;

		case ECPG_NO_ARRAY:
			snprintf(sqlca.sqlerrm.sqlerrmc, sizeof(sqlca.sqlerrm.sqlerrmc),
					 "variable is not an array in line %d.", line);
			break;

		case ECPG_DATA_NOT_ARRAY:
			snprintf(sqlca.sqlerrm.sqlerrmc, sizeof(sqlca.sqlerrm.sqlerrmc),
			 "data read from backend is not an array in line %d.", line);
			break;

		case ECPG_NO_CONN:
			snprintf(sqlca.sqlerrm.sqlerrmc, sizeof(sqlca.sqlerrm.sqlerrmc),
					 "No such connection %s in line %d.", str, line);
			break;

		case ECPG_NOT_CONN:
			snprintf(sqlca.sqlerrm.sqlerrmc, sizeof(sqlca.sqlerrm.sqlerrmc),
					 "Not connected in line %d.", line);
			break;

		case ECPG_INVALID_STMT:
			snprintf(sqlca.sqlerrm.sqlerrmc, sizeof(sqlca.sqlerrm.sqlerrmc),
					 "Invalid statement name in line %d.", line);
			break;

		case ECPG_UNKNOWN_DESCRIPTOR:
			snprintf(sqlca.sqlerrm.sqlerrmc, sizeof(sqlca.sqlerrm.sqlerrmc),
					 "Sescriptor %s not found in line %d.", str, line);
			break;

		case ECPG_INVALID_DESCRIPTOR_INDEX:
			snprintf(sqlca.sqlerrm.sqlerrmc, sizeof(sqlca.sqlerrm.sqlerrmc),
					 "Sescriptor index out of range in line %d.", line);
			break;

		case ECPG_UNKNOWN_DESCRIPTOR_ITEM:
			snprintf(sqlca.sqlerrm.sqlerrmc, sizeof(sqlca.sqlerrm.sqlerrmc),
					 "Unknown descriptor item %s in line %d.", str, line);
			break;

		case ECPG_VAR_NOT_NUMERIC:
			snprintf(sqlca.sqlerrm.sqlerrmc, sizeof(sqlca.sqlerrm.sqlerrmc),
					 "Variable is not a numeric type in line %d.", line);
			break;

		case ECPG_VAR_NOT_CHAR:
			snprintf(sqlca.sqlerrm.sqlerrmc, sizeof(sqlca.sqlerrm.sqlerrmc),
				   "Variable is not a character type in line %d.", line);
			break;

		case ECPG_PGSQL:
			/* strip trailing newline */
			if (str[strlen(str) - 1] == '\n')
				str[strlen(str) - 1] = '\0';

			snprintf(sqlca.sqlerrm.sqlerrmc, sizeof(sqlca.sqlerrm.sqlerrmc),
					 "'%s' in line %d.", str, line);
			break;

		case ECPG_TRANS:
			snprintf(sqlca.sqlerrm.sqlerrmc, sizeof(sqlca.sqlerrm.sqlerrmc),
					 "Error in transaction processing in line %d.", line);
			break;

		case ECPG_CONNECT:
			snprintf(sqlca.sqlerrm.sqlerrmc, sizeof(sqlca.sqlerrm.sqlerrmc),
			  "Could not connect to database %s in line %d.", str, line);
			break;

		default:
			snprintf(sqlca.sqlerrm.sqlerrmc, sizeof(sqlca.sqlerrm.sqlerrmc),
					 "SQL error #%d in line %d.", code, line);
			break;
	}

	sqlca.sqlerrm.sqlerrml = strlen(sqlca.sqlerrm.sqlerrmc);

	/* free all memory we have allocated for the user */
	free_auto_mem();
}

/* print out an error message */
void
sqlprint(void)
{
	sqlca.sqlerrm.sqlerrmc[sqlca.sqlerrm.sqlerrml] = '\0';
	fprintf(stderr, "sql error %s\n", sqlca.sqlerrm.sqlerrmc);
}

#include <stdlib.h>
#include <string.h>

#include "ecpgtype.h"
#include "ecpglib.h"
#include "ecpgerrno.h"
#include "extern.h"
#include "sqlca.h"

bool
get_data(PGresult *results, int act_tuple, int act_field, int lineno,
		 enum ECPGttype type, enum ECPGttype ind_type,
		 void *var, void *ind, long varcharsize, long offset,
		 bool isarray)
{
	char	   *pval = (char *) PQgetvalue(results, act_tuple, act_field);

	ECPGlog("get_data line %d: RESULT: %s\n", lineno, pval ? pval : "");

	/* pval is a pointer to the value */
	/* let's check is it really is an array if it should be one */
	if (isarray)
	{
		if (*pval != '{')
		{
			ECPGlog("get_data: data entry does not look like an array in line %d\n", lineno);
			ECPGraise(lineno, ECPG_DATA_NOT_ARRAY, NULL);
			return (false);
		}

		switch (type)
		{
			case ECPGt_char:
		        case ECPGt_unsigned_char:
		        case ECPGt_varchar:
		 		break;
		
		        default:
		                pval++;
		                break;
		}
	}

	/* We will have to decode the value */

	/*
	 * check for null value and set indicator accordingly
	 */
	switch (ind_type)
	{
		case ECPGt_short:
		case ECPGt_unsigned_short:
			((short *) ind)[act_tuple] = -PQgetisnull(results, act_tuple, act_field);
			break;
		case ECPGt_int:
		case ECPGt_unsigned_int:
			((int *) ind)[act_tuple] = -PQgetisnull(results, act_tuple, act_field);
			break;
		case ECPGt_long:
		case ECPGt_unsigned_long:
			((long *) ind)[act_tuple] = -PQgetisnull(results, act_tuple, act_field);
			break;
		case ECPGt_NO_INDICATOR:
			if (PQgetisnull(results, act_tuple, act_field))
			{
				ECPGraise(lineno, ECPG_MISSING_INDICATOR, NULL);
				return (false);
			}
			break;
		default:
			ECPGraise(lineno, ECPG_UNSUPPORTED, ECPGtype_name(ind_type));
			return (false);
			break;
	}

	do
	{
		switch (type)
		{
				long		res;
				unsigned long ures;
				double		dres;
				char	   *scan_length;

			case ECPGt_short:
			case ECPGt_int:
			case ECPGt_long:
				if (pval)
				{
					res = strtol(pval, &scan_length, 10);
					if ((isarray && *scan_length != ',' && *scan_length != '}')
						|| (!isarray && *scan_length != '\0'))	/* Garbage left */
					{
						ECPGraise(lineno, ECPG_INT_FORMAT, pval);
						return (false);
						res = 0L;
					}
				}
				else
					res = 0L;

				switch (type)
				{
					case ECPGt_short:
						((short *) var)[act_tuple] = (short) res;
						break;
					case ECPGt_int:
						((int *) var)[act_tuple] = (int) res;
						break;
					case ECPGt_long:
						((long *) var)[act_tuple] = res;
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
					if ((isarray && *scan_length != ',' && *scan_length != '}')
						|| (!isarray && *scan_length != '\0'))	/* Garbage left */
					{
						ECPGraise(lineno, ECPG_UINT_FORMAT, pval);
						return (false);
						ures = 0L;
					}
				}
				else
					ures = 0L;

				switch (type)
				{
					case ECPGt_unsigned_short:
						((unsigned short *) var)[act_tuple] = (unsigned short) ures;
						break;
					case ECPGt_unsigned_int:
						((unsigned int *) var)[act_tuple] = (unsigned int) ures;
						break;
					case ECPGt_unsigned_long:
						((unsigned long *) var)[act_tuple] = ures;
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
					if (isarray && *pval == '"')
						dres = strtod(pval + 1, &scan_length);
					else
						dres = strtod(pval, &scan_length);
					
					if (isarray && *scan_length == '"')
					        scan_length++;
					
					if ((isarray && *scan_length != ',' && *scan_length != '}')
						|| (!isarray && *scan_length != '\0'))	/* Garbage left */
					{
						ECPGraise(lineno, ECPG_FLOAT_FORMAT, pval);
						return (false);
						dres = 0.0;
					}
				}
				else
					dres = 0.0;

				switch (type)
				{
					case ECPGt_float:
						((float *) var)[act_tuple] = dres;
						break;
					case ECPGt_double:
						((double *) var)[act_tuple] = dres;
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
						((char *) var)[act_tuple] = false;
						break;
					}
					else if (pval[0] == 't' && pval[1] == '\0')
					{
						((char *) var)[act_tuple] = true;
						break;
					}
					else if (pval[0] == '\0' && PQgetisnull(results, act_tuple, act_field))
					{
						/* NULL is valid */
						break;
					}
				}

				ECPGraise(lineno, ECPG_CONVERT_BOOL, pval);
				return (false);
				break;

			case ECPGt_char:
			case ECPGt_unsigned_char:
				{
					strncpy((char *) ((long) var + offset * act_tuple), pval, varcharsize);
					if (varcharsize && varcharsize < strlen(pval))
					{
						/* truncation */
						switch (ind_type)
						{
							case ECPGt_short:
							case ECPGt_unsigned_short:
								((short *) ind)[act_tuple] = varcharsize;
								break;
							case ECPGt_int:
							case ECPGt_unsigned_int:
								((int *) ind)[act_tuple] = varcharsize;
								break;
							case ECPGt_long:
							case ECPGt_unsigned_long:
								((long *) ind)[act_tuple] = varcharsize;
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
					(struct ECPGgeneric_varchar *) ((long) var + offset * act_tuple);

					if (varcharsize == 0)
						strncpy(variable->arr, pval, strlen(pval));
					else
						strncpy(variable->arr, pval, varcharsize);

					variable->len = strlen(pval);
					if (varcharsize > 0 && variable->len > varcharsize)
					{
						/* truncation */
						switch (ind_type)
						{
							case ECPGt_short:
							case ECPGt_unsigned_short:
								((short *) ind)[act_tuple] = varcharsize;
								break;
							case ECPGt_int:
							case ECPGt_unsigned_int:
								((int *) ind)[act_tuple] = varcharsize;
								break;
							case ECPGt_long:
							case ECPGt_unsigned_long:
								((long *) ind)[act_tuple] = varcharsize;
								break;
							default:
								break;
						}
						sqlca.sqlwarn[0] = sqlca.sqlwarn[1] = 'W';

						variable->len = varcharsize;
					}
				}
				break;

			default:
				ECPGraise(lineno, ECPG_UNSUPPORTED, ECPGtype_name(type));
				return (false);
				break;
		}
		if (isarray)
		{
			bool		string = false;

			/* set array to next entry */
			++act_tuple;

			/* set pval to the next entry */
			for (; string || (*pval != ',' && *pval != '}'); ++pval)
				if (*pval == '"')
					string = string ? false : true;

			if (*pval == ',')
				++pval;
		}
	} while (isarray && *pval != '}');

	return (true);
}

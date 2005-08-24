/* $Header: /cvsroot/pgsql/src/interfaces/ecpg/ecpglib/data.c,v 1.20.2.2 2005/08/24 10:35:54 meskes Exp $ */

#define POSTGRES_ECPG_INTERNAL
#include "postgres_fe.h"

#include <stdlib.h>
#include <string.h>

#include "ecpgtype.h"
#include "ecpglib.h"
#include "ecpgerrno.h"
#include "extern.h"
#include "sqlca.h"
#include "pgtypes_numeric.h"
#include "pgtypes_date.h"
#include "pgtypes_timestamp.h"
#include "pgtypes_interval.h"

static bool garbage_left(enum ARRAY_TYPE isarray, char *scan_length, enum COMPAT_MODE compat)
{
	/* INFORMIX allows for selecting a numeric into an int, the result is truncated */
	if (isarray == ECPG_ARRAY_NONE && INFORMIX_MODE(compat) && *scan_length == '.') 
		return false;
	
	if (isarray == ECPG_ARRAY_ARRAY && *scan_length != ',' && *scan_length != '}')
		return true;

	if (isarray == ECPG_ARRAY_VECTOR && *scan_length != ' ' && *scan_length != '\0')
		return true;

	if (isarray == ECPG_ARRAY_NONE && *scan_length != ' ' && *scan_length != '\0')
		return true;

	return false;
}

bool
ECPGget_data(const PGresult *results, int act_tuple, int act_field, int lineno,
			 enum ECPGttype type, enum ECPGttype ind_type,
			 char *var, char *ind, long varcharsize, long offset,
			 long ind_offset, enum ARRAY_TYPE isarray, enum COMPAT_MODE compat, bool force_indicator)
{
	struct sqlca_t *sqlca = ECPGget_sqlca();
	char	   *pval = (char *) PQgetvalue(results, act_tuple, act_field);
	int			value_for_indicator = 0;

	ECPGlog("ECPGget_data line %d: RESULT: %s offset: %ld array: %d\n", lineno, pval ? pval : "", offset, isarray);

	/* We will have to decode the value */

	/*
	 * check for null value and set indicator accordingly, i.e. -1 if NULL
	 * and 0 if not
	 */
	if (PQgetisnull(results, act_tuple, act_field))
		value_for_indicator = -1;

	switch (ind_type)
	{
		case ECPGt_short:
		case ECPGt_unsigned_short:
			*((short *) (ind + ind_offset * act_tuple)) = value_for_indicator;
			break;
		case ECPGt_int:
		case ECPGt_unsigned_int:
			*((int *) (ind + ind_offset * act_tuple)) = value_for_indicator;
			break;
		case ECPGt_long:
		case ECPGt_unsigned_long:
			*((long *) (ind + ind_offset * act_tuple)) = value_for_indicator;
			break;
#ifdef HAVE_LONG_LONG_INT_64
		case ECPGt_long_long:
		case ECPGt_unsigned_long_long:
			*((long long int *) (ind + ind_offset * act_tuple)) = value_for_indicator;
			break;
#endif   /* HAVE_LONG_LONG_INT_64 */
		case ECPGt_NO_INDICATOR:
			if (value_for_indicator == -1)
			{
				if (force_indicator == false)
				{
					/*
					 * Informix has an additional way to specify NULLs
					 * note that this uses special values to denote NULL
					 */
					ECPGset_informix_null(type, var + offset * act_tuple);
				}
				else
				{
					ECPGraise(lineno, ECPG_MISSING_INDICATOR, ECPG_SQLSTATE_NULL_VALUE_NO_INDICATOR_PARAMETER, NULL);
					return (false);
				}
			}
			break;
		default:
			ECPGraise(lineno, ECPG_UNSUPPORTED, ECPG_SQLSTATE_ECPG_INTERNAL_ERROR, ECPGtype_name(ind_type));
			return (false);
			break;
	}

	if (value_for_indicator == -1)
		return (true);

	/* pval is a pointer to the value */
	/* let's check if it really is an array if it should be one */
	if (isarray == ECPG_ARRAY_ARRAY)
	{
		if (*pval != '{')
		{
			ECPGraise(lineno, ECPG_DATA_NOT_ARRAY, ECPG_SQLSTATE_DATATYPE_MISMATCH, NULL);
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

	do
	{
		switch (type)
		{
				long		res;
				unsigned long ures;
				double		dres;
				char	   *scan_length;
				numeric    *nres;
				date		ddres;
				timestamp	tres;
				interval   *ires;

			case ECPGt_short:
			case ECPGt_int:
			case ECPGt_long:
				if (pval)
				{
					res = strtol(pval, &scan_length, 10);
					if (garbage_left(isarray, scan_length, compat))
					{
						ECPGraise(lineno, ECPG_INT_FORMAT, ECPG_SQLSTATE_DATATYPE_MISMATCH, pval);
						return (false);
					}
				}
				else
					res = 0L;

				switch (type)
				{
					case ECPGt_short:
						*((short *) (var + offset * act_tuple)) = (short) res;
						break;
					case ECPGt_int:
						*((int *) (var + offset * act_tuple)) = (int) res;
						break;
					case ECPGt_long:
						*((long *) (var + offset * act_tuple)) = (long) res;
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
					if (garbage_left(isarray, scan_length, compat))
					{
						ECPGraise(lineno, ECPG_UINT_FORMAT, ECPG_SQLSTATE_DATATYPE_MISMATCH, pval);
						return (false);
					}
				}
				else
					ures = 0L;

				switch (type)
				{
					case ECPGt_unsigned_short:
						*((unsigned short *) (var + offset * act_tuple)) = (unsigned short) ures;
						break;
					case ECPGt_unsigned_int:
						*((unsigned int *) (var + offset * act_tuple)) = (unsigned int) ures;
						break;
					case ECPGt_unsigned_long:
						*((unsigned long *) (var + offset * act_tuple)) = (unsigned long) ures;
						break;
					default:
						/* Cannot happen */
						break;
				}
				break;

#ifdef HAVE_LONG_LONG_INT_64
#ifdef HAVE_STRTOLL
			case ECPGt_long_long:
				if (pval)
				{
					*((long long int *) (var + offset * act_tuple)) = strtoll(pval, &scan_length, 10);
					if (garbage_left(isarray, scan_length, compat))
					{
						ECPGraise(lineno, ECPG_INT_FORMAT, ECPG_SQLSTATE_DATATYPE_MISMATCH, pval);
						return (false);
					}
				}
				else
					*((long long int *) (var + offset * act_tuple)) = (long long) 0;

				break;
#endif   /* HAVE_STRTOLL */
#ifdef HAVE_STRTOULL
			case ECPGt_unsigned_long_long:
				if (pval)
				{
					*((unsigned long long int *) (var + offset * act_tuple)) = strtoull(pval, &scan_length, 10);
					if ((isarray && *scan_length != ',' && *scan_length != '}')
						|| (!isarray && !(INFORMIX_MODE(compat) && *scan_length == '.') && *scan_length != '\0' && *scan_length != ' '))	/* Garbage left */
					{
						ECPGraise(lineno, ECPG_UINT_FORMAT, ECPG_SQLSTATE_DATATYPE_MISMATCH, pval);
						return (false);
					}
				}
				else
					*((unsigned long long int *) (var + offset * act_tuple)) = (long long) 0;

				break;
#endif   /* HAVE_STRTOULL */
#endif   /* HAVE_LONG_LONG_INT_64 */

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

					if (garbage_left(isarray, scan_length, compat))
					{
						ECPGraise(lineno, ECPG_FLOAT_FORMAT, ECPG_SQLSTATE_DATATYPE_MISMATCH, pval);
						return (false);
					}
				}
				else
					dres = 0.0;

				switch (type)
				{
					case ECPGt_float:
						*((float *) (var + offset * act_tuple)) = dres;
						break;
					case ECPGt_double:
						*((double *) (var + offset * act_tuple)) = dres;
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
						if (offset == sizeof(char))
							*((char *) (var + offset * act_tuple)) = false;
						else if (offset == sizeof(int))
							*((int *) (var + offset * act_tuple)) = false;
						else
							ECPGraise(lineno, ECPG_CONVERT_BOOL, ECPG_SQLSTATE_DATATYPE_MISMATCH, "different size");
						break;
					}
					else if (pval[0] == 't' && pval[1] == '\0')
					{
						if (offset == sizeof(char))
							*((char *) (var + offset * act_tuple)) = true;
						else if (offset == sizeof(int))
							*((int *) (var + offset * act_tuple)) = true;
						else
							ECPGraise(lineno, ECPG_CONVERT_BOOL, ECPG_SQLSTATE_DATATYPE_MISMATCH, "different size");
						break;
					}
					else if (pval[0] == '\0' && PQgetisnull(results, act_tuple, act_field))
					{
						/* NULL is valid */
						break;
					}
				}

				ECPGraise(lineno, ECPG_CONVERT_BOOL, ECPG_SQLSTATE_DATATYPE_MISMATCH, pval);
				return (false);
				break;

			case ECPGt_char:
			case ECPGt_unsigned_char:
				{
					if (varcharsize == 0)
						strncpy((char *) ((long) var + offset * act_tuple), pval, strlen(pval) + 1);
					else
					{
						strncpy((char *) ((long) var + offset * act_tuple), pval, varcharsize);

						if (varcharsize < strlen(pval))
						{
							/* truncation */
							switch (ind_type)
							{
								case ECPGt_short:
								case ECPGt_unsigned_short:
									*((short *) (ind + ind_offset * act_tuple)) = strlen(pval);
									break;
								case ECPGt_int:
								case ECPGt_unsigned_int:
									*((int *) (ind + ind_offset * act_tuple)) = strlen(pval);
									break;
								case ECPGt_long:
								case ECPGt_unsigned_long:
									*((long *) (ind + ind_offset * act_tuple)) = strlen(pval);
									break;
#ifdef HAVE_LONG_LONG_INT_64
								case ECPGt_long_long:
								case ECPGt_unsigned_long_long:
									*((long long int *) (ind + ind_offset * act_tuple)) = strlen(pval);
									break;
#endif   /* HAVE_LONG_LONG_INT_64 */
								default:
									break;
							}
							sqlca->sqlwarn[0] = sqlca->sqlwarn[1] = 'W';
						}
					}
				}
				break;

			case ECPGt_varchar:
				{
					struct ECPGgeneric_varchar *variable =
					(struct ECPGgeneric_varchar *) ((long) var + offset * act_tuple);

					variable->len = strlen(pval);
					if (varcharsize == 0)
						strncpy(variable->arr, pval, variable->len);
					else
					{
						strncpy(variable->arr, pval, varcharsize);

						if (variable->len > varcharsize)
						{
							/* truncation */
							switch (ind_type)
							{
								case ECPGt_short:
								case ECPGt_unsigned_short:
									*((short *) (ind + offset * act_tuple)) = variable->len;
									break;
								case ECPGt_int:
								case ECPGt_unsigned_int:
									*((int *) (ind + offset * act_tuple)) = variable->len;
									break;
								case ECPGt_long:
								case ECPGt_unsigned_long:
									*((long *) (ind + offset * act_tuple)) = variable->len;
									break;
#ifdef HAVE_LONG_LONG_INT_64
								case ECPGt_long_long:
								case ECPGt_unsigned_long_long:
									*((long long int *) (ind + ind_offset * act_tuple)) = variable->len;
									break;
#endif   /* HAVE_LONG_LONG_INT_64 */
								default:
									break;
							}
							sqlca->sqlwarn[0] = sqlca->sqlwarn[1] = 'W';

							variable->len = varcharsize;
						}
					}
				}
				break;

			case ECPGt_decimal:
			case ECPGt_numeric:
				if (pval)
				{
					if (isarray && *pval == '"')
						nres = PGTYPESnumeric_from_asc(pval + 1, &scan_length);
					else
						nres = PGTYPESnumeric_from_asc(pval, &scan_length);

					/* did we get an error? */
					if (errno != 0)
					{
						ECPGlog("ECPGget_data line %d: RESULT: %s errno %d\n", lineno, pval ? pval : "", errno);

						if (INFORMIX_MODE(compat))
						{
							/* Informix wants its own NULL value here instead of an error */
							ECPGset_informix_null(ECPGt_numeric, nres);
						}
						else
						{
							ECPGraise(lineno, ECPG_NUMERIC_FORMAT, ECPG_SQLSTATE_DATATYPE_MISMATCH, pval);
							return (false);
						}
					}
					else
					{
						if (isarray && *scan_length == '"')
							scan_length++;

						if (garbage_left(isarray, scan_length, compat))
						{
							ECPGraise(lineno, ECPG_NUMERIC_FORMAT, ECPG_SQLSTATE_DATATYPE_MISMATCH, pval);
							return (false);
						}
					}
				}
				else
					nres = PGTYPESnumeric_from_asc("0.0", &scan_length);

				if (type == ECPGt_numeric)
					PGTYPESnumeric_copy(nres, (numeric *) (var + offset * act_tuple));
				else
					PGTYPESnumeric_to_decimal(nres, (decimal *) (var + offset * act_tuple));
				break;

			case ECPGt_interval:
				if (pval)
				{
					if (isarray && *pval == '"')
						ires = PGTYPESinterval_from_asc(pval + 1, &scan_length);
					else
						ires = PGTYPESinterval_from_asc(pval, &scan_length);

					/* did we get an error? */
					if (errno != 0)
					{
						if (INFORMIX_MODE(compat))
						{
							/* Informix wants its own NULL value here instead of an error */
							ECPGset_informix_null(ECPGt_interval, ires);
						}
						else
						{
							ECPGraise(lineno, ECPG_INTERVAL_FORMAT, ECPG_SQLSTATE_DATATYPE_MISMATCH, pval);
							return (false);
						}
					}
					else
					{
						if (isarray && *scan_length == '"')
							scan_length++;

						if (garbage_left(isarray, scan_length, compat))
						{
							ECPGraise(lineno, ECPG_INTERVAL_FORMAT, ECPG_SQLSTATE_DATATYPE_MISMATCH, pval);
							return (false);
						}
					}
				}
				else
					ires = PGTYPESinterval_from_asc("0 seconds", NULL);

				PGTYPESinterval_copy(ires, (interval *) (var + offset * act_tuple));
				break;
			case ECPGt_date:
				if (pval)
				{
					if (isarray && *pval == '"')
						ddres = PGTYPESdate_from_asc(pval + 1, &scan_length);
					else
						ddres = PGTYPESdate_from_asc(pval, &scan_length);

					/* did we get an error? */
					if (errno != 0)
					{
						if (INFORMIX_MODE(compat))
						{
							/* Informix wants its own NULL value here instead of an error */
							ECPGset_informix_null(ECPGt_date, &ddres);
						}
						else
						{
							ECPGraise(lineno, ECPG_DATE_FORMAT, ECPG_SQLSTATE_DATATYPE_MISMATCH, pval);
							return (false);
						}
					}
					else
					{
						if (isarray && *scan_length == '"')
							scan_length++;

						if (garbage_left(isarray, scan_length, compat))
						{
							ECPGraise(lineno, ECPG_DATE_FORMAT, ECPG_SQLSTATE_DATATYPE_MISMATCH, pval);
							return (false);
						}
					}

					*((date *) (var + offset * act_tuple)) = ddres;
				}
				break;

			case ECPGt_timestamp:
				if (pval)
				{
					if (isarray && *pval == '"')
						tres = PGTYPEStimestamp_from_asc(pval + 1, &scan_length);
					else
						tres = PGTYPEStimestamp_from_asc(pval, &scan_length);

					/* did we get an error? */
					if (errno != 0)
					{
						if (INFORMIX_MODE(compat))
						{
							/* Informix wants its own NULL value here instead of an error */
							ECPGset_informix_null(ECPGt_timestamp, &tres);
						}
						else
						{
							ECPGraise(lineno, ECPG_TIMESTAMP_FORMAT, ECPG_SQLSTATE_DATATYPE_MISMATCH, pval);
							return (false);
						}
					}
					else
					{
						if (isarray && *scan_length == '"')
							scan_length++;

						if (garbage_left(isarray, scan_length, compat))
						{
							ECPGraise(lineno, ECPG_TIMESTAMP_FORMAT, ECPG_SQLSTATE_DATATYPE_MISMATCH, pval);
							return (false);
						}
					}

					*((timestamp *) (var + offset * act_tuple)) = tres;
				}
				break;

			default:
				ECPGraise(lineno, ECPG_UNSUPPORTED, ECPG_SQLSTATE_ECPG_INTERNAL_ERROR, ECPGtype_name(type));
				return (false);
				break;
		}
		if (isarray == ECPG_ARRAY_ARRAY)
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
		else if (isarray == ECPG_ARRAY_VECTOR)
		{
			bool		string = false;

			/* set array to next entry */
			++act_tuple;

			/* set pval to the next entry */
			for (; string || (*pval != ' ' && *pval != '\0'); ++pval)
				if (*pval == '"')
					string = string ? false : true;

			if (*pval == ' ')
				++pval;
		}
	} while ((isarray == ECPG_ARRAY_ARRAY && *pval != '}') || (isarray == ECPG_ARRAY_VECTOR && *pval != '\0'));

	return (true);
}

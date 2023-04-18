/* src/interfaces/ecpg/ecpglib/data.c */

#define POSTGRES_ECPG_INTERNAL
#include "postgres_fe.h"

#include <math.h>

#include "ecpgerrno.h"
#include "ecpglib.h"
#include "ecpglib_extern.h"
#include "ecpgtype.h"
#include "pgtypes_date.h"
#include "pgtypes_interval.h"
#include "pgtypes_numeric.h"
#include "pgtypes_timestamp.h"
#include "sqlca.h"

/* returns true if character c is a delimiter for the given array type */
static bool
array_delimiter(enum ARRAY_TYPE isarray, char c)
{
	if (isarray == ECPG_ARRAY_ARRAY && c == ',')
		return true;

	if (isarray == ECPG_ARRAY_VECTOR && c == ' ')
		return true;

	return false;
}

/* returns true if character c marks the boundary for the given array type */
static bool
array_boundary(enum ARRAY_TYPE isarray, char c)
{
	if (isarray == ECPG_ARRAY_ARRAY && c == '}')
		return true;

	if (isarray == ECPG_ARRAY_VECTOR && c == '\0')
		return true;

	return false;
}

/* returns true if some garbage is found at the end of the scanned string */
static bool
garbage_left(enum ARRAY_TYPE isarray, char **scan_length, enum COMPAT_MODE compat)
{
	/*
	 * INFORMIX allows for selecting a numeric into an int, the result is
	 * truncated
	 */
	if (isarray == ECPG_ARRAY_NONE)
	{
		if (INFORMIX_MODE(compat) && **scan_length == '.')
		{
			/* skip invalid characters */
			do
			{
				(*scan_length)++;
			} while (isdigit((unsigned char) **scan_length));
		}

		if (**scan_length != ' ' && **scan_length != '\0')
			return true;
	}
	else if (ECPG_IS_ARRAY(isarray) && !array_delimiter(isarray, **scan_length) && !array_boundary(isarray, **scan_length))
		return true;

	return false;
}

/* stolen code from src/backend/utils/adt/float.c */
#if defined(WIN32) && !defined(NAN)
static const uint32 nan[2] = {0xffffffff, 0x7fffffff};

#define NAN (*(const double *) nan)
#endif

static double
get_float8_infinity(void)
{
#ifdef INFINITY
	return (double) INFINITY;
#else
	return (double) (HUGE_VAL * HUGE_VAL);
#endif
}

static double
get_float8_nan(void)
{
	/* (double) NAN doesn't work on some NetBSD/MIPS releases */
#if defined(NAN) && !(defined(__NetBSD__) && defined(__mips__))
	return (double) NAN;
#else
	return (double) (0.0 / 0.0);
#endif
}

static bool
check_special_value(char *ptr, double *retval, char **endptr)
{
	if (pg_strncasecmp(ptr, "NaN", 3) == 0)
	{
		*retval = get_float8_nan();
		*endptr = ptr + 3;
		return true;
	}
	else if (pg_strncasecmp(ptr, "Infinity", 8) == 0)
	{
		*retval = get_float8_infinity();
		*endptr = ptr + 8;
		return true;
	}
	else if (pg_strncasecmp(ptr, "-Infinity", 9) == 0)
	{
		*retval = -get_float8_infinity();
		*endptr = ptr + 9;
		return true;
	}

	return false;
}

/* imported from src/backend/utils/adt/encode.c */

unsigned
ecpg_hex_enc_len(unsigned srclen)
{
	return srclen << 1;
}

unsigned
ecpg_hex_dec_len(unsigned srclen)
{
	return srclen >> 1;
}

static inline char
get_hex(char c)
{
	static const int8 hexlookup[128] = {
		-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
		0, 1, 2, 3, 4, 5, 6, 7, 8, 9, -1, -1, -1, -1, -1, -1,
		-1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
		-1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	};
	int			res = -1;

	if (c > 0 && c < 127)
		res = hexlookup[(unsigned char) c];

	return (char) res;
}

static unsigned
hex_decode(const char *src, unsigned len, char *dst)
{
	const char *s,
			   *srcend;
	char		v1,
				v2,
			   *p;

	srcend = src + len;
	s = src;
	p = dst;
	while (s < srcend)
	{
		if (*s == ' ' || *s == '\n' || *s == '\t' || *s == '\r')
		{
			s++;
			continue;
		}
		v1 = get_hex(*s++) << 4;
		if (s >= srcend)
			return -1;

		v2 = get_hex(*s++);
		*p++ = v1 | v2;
	}

	return p - dst;
}

unsigned
ecpg_hex_encode(const char *src, unsigned len, char *dst)
{
	static const char hextbl[] = "0123456789abcdef";
	const char *end = src + len;

	while (src < end)
	{
		*dst++ = hextbl[(*src >> 4) & 0xF];
		*dst++ = hextbl[*src & 0xF];
		src++;
	}
	return len * 2;
}

bool
ecpg_get_data(const PGresult *results, int act_tuple, int act_field, int lineno,
			  enum ECPGttype type, enum ECPGttype ind_type,
			  char *var, char *ind, long varcharsize, long offset,
			  long ind_offset, enum ARRAY_TYPE isarray, enum COMPAT_MODE compat, bool force_indicator)
{
	struct sqlca_t *sqlca = ECPGget_sqlca();
	char	   *pval = (char *) PQgetvalue(results, act_tuple, act_field);
	int			binary = PQfformat(results, act_field);
	int			size = PQgetlength(results, act_tuple, act_field);
	int			value_for_indicator = 0;
	long		log_offset;

	if (sqlca == NULL)
	{
		ecpg_raise(lineno, ECPG_OUT_OF_MEMORY,
				   ECPG_SQLSTATE_ECPG_OUT_OF_MEMORY, NULL);
		return false;
	}

	/*
	 * If we are running in a regression test, do not log the offset variable,
	 * it depends on the machine's alignment.
	 */
	if (ecpg_internal_regression_mode)
		log_offset = -1;
	else
		log_offset = offset;

	ecpg_log("ecpg_get_data on line %d: RESULT: %s offset: %ld; array: %s\n", lineno, pval ? (binary ? "BINARY" : pval) : "EMPTY", log_offset, ECPG_IS_ARRAY(isarray) ? "yes" : "no");

	/* pval is a pointer to the value */
	if (!pval)
	{
		/*
		 * This should never happen because we already checked that we found
		 * at least one tuple, but let's play it safe.
		 */
		ecpg_raise(lineno, ECPG_NOT_FOUND, ECPG_SQLSTATE_NO_DATA, NULL);
		return false;
	}

	/* We will have to decode the value */

	/*
	 * check for null value and set indicator accordingly, i.e. -1 if NULL and
	 * 0 if not
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
		case ECPGt_long_long:
		case ECPGt_unsigned_long_long:
			*((long long int *) (ind + ind_offset * act_tuple)) = value_for_indicator;
			break;
		case ECPGt_NO_INDICATOR:
			if (value_for_indicator == -1)
			{
				if (force_indicator == false)
				{
					/*
					 * Informix has an additional way to specify NULLs note
					 * that this uses special values to denote NULL
					 */
					ECPGset_noind_null(type, var + offset * act_tuple);
				}
				else
				{
					ecpg_raise(lineno, ECPG_MISSING_INDICATOR,
							   ECPG_SQLSTATE_NULL_VALUE_NO_INDICATOR_PARAMETER,
							   NULL);
					return false;
				}
			}
			break;
		default:
			ecpg_raise(lineno, ECPG_UNSUPPORTED,
					   ECPG_SQLSTATE_ECPG_INTERNAL_ERROR,
					   ecpg_type_name(ind_type));
			return false;
			break;
	}

	if (value_for_indicator == -1)
		return true;

	/* let's check if it really is an array if it should be one */
	if (isarray == ECPG_ARRAY_ARRAY)
	{
		if (*pval != '{')
		{
			ecpg_raise(lineno, ECPG_DATA_NOT_ARRAY,
					   ECPG_SQLSTATE_DATATYPE_MISMATCH, NULL);
			return false;
		}

		switch (type)
		{
			case ECPGt_char:
			case ECPGt_unsigned_char:
			case ECPGt_varchar:
			case ECPGt_string:
				break;

			default:
				pval++;
				break;
		}
	}

	do
	{
		if (binary)
		{
			if (varcharsize == 0 || varcharsize * offset >= size)
				memcpy(var + offset * act_tuple, pval, size);
			else
			{
				memcpy(var + offset * act_tuple, pval, varcharsize * offset);

				if (varcharsize * offset < size)
				{
					/* truncation */
					switch (ind_type)
					{
						case ECPGt_short:
						case ECPGt_unsigned_short:
							*((short *) (ind + ind_offset * act_tuple)) = size;
							break;
						case ECPGt_int:
						case ECPGt_unsigned_int:
							*((int *) (ind + ind_offset * act_tuple)) = size;
							break;
						case ECPGt_long:
						case ECPGt_unsigned_long:
							*((long *) (ind + ind_offset * act_tuple)) = size;
							break;
						case ECPGt_long_long:
						case ECPGt_unsigned_long_long:
							*((long long int *) (ind + ind_offset * act_tuple)) = size;
							break;
						default:
							break;
					}
					sqlca->sqlwarn[0] = sqlca->sqlwarn[1] = 'W';
				}
			}
			pval += size;
		}
		else
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
					char	   *endptr,
								endchar;

				case ECPGt_short:
				case ECPGt_int:
				case ECPGt_long:
					res = strtol(pval, &scan_length, 10);
					if (garbage_left(isarray, &scan_length, compat))
					{
						ecpg_raise(lineno, ECPG_INT_FORMAT,
								   ECPG_SQLSTATE_DATATYPE_MISMATCH, pval);
						return false;
					}
					pval = scan_length;

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
					ures = strtoul(pval, &scan_length, 10);
					if (garbage_left(isarray, &scan_length, compat))
					{
						ecpg_raise(lineno, ECPG_UINT_FORMAT,
								   ECPG_SQLSTATE_DATATYPE_MISMATCH, pval);
						return false;
					}
					pval = scan_length;

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

#ifdef HAVE_STRTOLL
				case ECPGt_long_long:
					*((long long int *) (var + offset * act_tuple)) = strtoll(pval, &scan_length, 10);
					if (garbage_left(isarray, &scan_length, compat))
					{
						ecpg_raise(lineno, ECPG_INT_FORMAT, ECPG_SQLSTATE_DATATYPE_MISMATCH, pval);
						return false;
					}
					pval = scan_length;

					break;
#endif							/* HAVE_STRTOLL */
#ifdef HAVE_STRTOULL
				case ECPGt_unsigned_long_long:
					*((unsigned long long int *) (var + offset * act_tuple)) = strtoull(pval, &scan_length, 10);
					if (garbage_left(isarray, &scan_length, compat))
					{
						ecpg_raise(lineno, ECPG_UINT_FORMAT, ECPG_SQLSTATE_DATATYPE_MISMATCH, pval);
						return false;
					}
					pval = scan_length;

					break;
#endif							/* HAVE_STRTOULL */

				case ECPGt_float:
				case ECPGt_double:
					if (isarray && *pval == '"')
						pval++;

					if (!check_special_value(pval, &dres, &scan_length))
						dres = strtod(pval, &scan_length);

					if (isarray && *scan_length == '"')
						scan_length++;

					/* no special INFORMIX treatment for floats */
					if (garbage_left(isarray, &scan_length, ECPG_COMPAT_PGSQL))
					{
						ecpg_raise(lineno, ECPG_FLOAT_FORMAT,
								   ECPG_SQLSTATE_DATATYPE_MISMATCH, pval);
						return false;
					}
					pval = scan_length;

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
					if (pval[0] == 'f' && pval[1] == '\0')
					{
						*((bool *) (var + offset * act_tuple)) = false;
						pval++;
						break;
					}
					else if (pval[0] == 't' && pval[1] == '\0')
					{
						*((bool *) (var + offset * act_tuple)) = true;
						pval++;
						break;
					}
					else if (pval[0] == '\0' && PQgetisnull(results, act_tuple, act_field))
					{
						/* NULL is valid */
						break;
					}

					ecpg_raise(lineno, ECPG_CONVERT_BOOL,
							   ECPG_SQLSTATE_DATATYPE_MISMATCH, pval);
					return false;
					break;

				case ECPGt_bytea:
					{
						struct ECPGgeneric_bytea *variable =
						(struct ECPGgeneric_bytea *) (var + offset * act_tuple);
						long		dst_size,
									src_size,
									dec_size;

						dst_size = ecpg_hex_enc_len(varcharsize);
						src_size = size - 2;	/* exclude backslash + 'x' */
						dec_size = src_size < dst_size ? src_size : dst_size;
						variable->len = hex_decode(pval + 2, dec_size, variable->arr);

						if (dst_size < src_size)
						{
							long		rcv_size = ecpg_hex_dec_len(size - 2);

							/* truncation */
							switch (ind_type)
							{
								case ECPGt_short:
								case ECPGt_unsigned_short:
									*((short *) (ind + ind_offset * act_tuple)) = rcv_size;
									break;
								case ECPGt_int:
								case ECPGt_unsigned_int:
									*((int *) (ind + ind_offset * act_tuple)) = rcv_size;
									break;
								case ECPGt_long:
								case ECPGt_unsigned_long:
									*((long *) (ind + ind_offset * act_tuple)) = rcv_size;
									break;
								case ECPGt_long_long:
								case ECPGt_unsigned_long_long:
									*((long long int *) (ind + ind_offset * act_tuple)) = rcv_size;
									break;
								default:
									break;
							}
							sqlca->sqlwarn[0] = sqlca->sqlwarn[1] = 'W';
						}

						pval += size;
					}
					break;

				case ECPGt_char:
				case ECPGt_unsigned_char:
				case ECPGt_string:
					{
						char	   *str = (char *) (var + offset * act_tuple);

						/*
						 * If varcharsize is unknown and the offset is that of
						 * char *, then this variable represents the array of
						 * character pointers. So, use extra indirection.
						 */
						if (varcharsize == 0 && offset == sizeof(char *))
							str = *(char **) str;

						if (varcharsize > size)
						{
							/*
							 * compatibility mode, blank pad and null
							 * terminate char array
							 */
							if (ORACLE_MODE(compat) && (type == ECPGt_char || type == ECPGt_unsigned_char))
							{
								memset(str, ' ', varcharsize);
								memcpy(str, pval, size);
								str[varcharsize - 1] = '\0';

								/*
								 * compatibility mode empty string gets -1
								 * indicator but no warning
								 */
								if (size == 0)
								{
									/* truncation */
									switch (ind_type)
									{
										case ECPGt_short:
										case ECPGt_unsigned_short:
											*((short *) (ind + ind_offset * act_tuple)) = -1;
											break;
										case ECPGt_int:
										case ECPGt_unsigned_int:
											*((int *) (ind + ind_offset * act_tuple)) = -1;
											break;
										case ECPGt_long:
										case ECPGt_unsigned_long:
											*((long *) (ind + ind_offset * act_tuple)) = -1;
											break;
										case ECPGt_long_long:
										case ECPGt_unsigned_long_long:
											*((long long int *) (ind + ind_offset * act_tuple)) = -1;
											break;
										default:
											break;
									}
								}
							}
							else
							{
								strncpy(str, pval, size + 1);
							}
							/* do the rtrim() */
							if (type == ECPGt_string)
							{
								char	   *last = str + size;

								while (last > str && (*last == ' ' || *last == '\0'))
								{
									*last = '\0';
									last--;
								}
							}
						}
						else
						{
							int			charsize = varcharsize;

							/*
							 * assume that the caller provided storage exactly
							 * fit when varcharsize is zero.
							 */
							if (varcharsize == 0)
								charsize = size + 1;

							strncpy(str, pval, charsize);

							/* compatibility mode, null terminate char array */
							if (ORACLE_MODE(compat) && (charsize - 1) < size)
							{
								if (type == ECPGt_char || type == ECPGt_unsigned_char)
									str[charsize - 1] = '\0';
							}

							if (charsize < size || (ORACLE_MODE(compat) && (charsize - 1) < size))
							{
								/* truncation */
								switch (ind_type)
								{
									case ECPGt_short:
									case ECPGt_unsigned_short:
										*((short *) (ind + ind_offset * act_tuple)) = size;
										break;
									case ECPGt_int:
									case ECPGt_unsigned_int:
										*((int *) (ind + ind_offset * act_tuple)) = size;
										break;
									case ECPGt_long:
									case ECPGt_unsigned_long:
										*((long *) (ind + ind_offset * act_tuple)) = size;
										break;
									case ECPGt_long_long:
									case ECPGt_unsigned_long_long:
										*((long long int *) (ind + ind_offset * act_tuple)) = size;
										break;
									default:
										break;
								}
								sqlca->sqlwarn[0] = sqlca->sqlwarn[1] = 'W';
							}
						}
						pval += size;
					}
					break;

				case ECPGt_varchar:
					{
						struct ECPGgeneric_varchar *variable =
						(struct ECPGgeneric_varchar *) (var + offset * act_tuple);

						variable->len = size;
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
										*((short *) (ind + ind_offset * act_tuple)) = variable->len;
										break;
									case ECPGt_int:
									case ECPGt_unsigned_int:
										*((int *) (ind + ind_offset * act_tuple)) = variable->len;
										break;
									case ECPGt_long:
									case ECPGt_unsigned_long:
										*((long *) (ind + ind_offset * act_tuple)) = variable->len;
										break;
									case ECPGt_long_long:
									case ECPGt_unsigned_long_long:
										*((long long int *) (ind + ind_offset * act_tuple)) = variable->len;
										break;
									default:
										break;
								}
								sqlca->sqlwarn[0] = sqlca->sqlwarn[1] = 'W';

								variable->len = varcharsize;
							}
						}
						pval += size;
					}
					break;

				case ECPGt_decimal:
				case ECPGt_numeric:
					for (endptr = pval; *endptr && *endptr != ',' && *endptr != '}'; endptr++);
					endchar = *endptr;
					*endptr = '\0';
					nres = PGTYPESnumeric_from_asc(pval, &scan_length);
					*endptr = endchar;

					/* did we get an error? */
					if (nres == NULL)
					{
						ecpg_log("ecpg_get_data on line %d: RESULT %s; errno %d\n",
								 lineno, pval, errno);

						if (INFORMIX_MODE(compat))
						{
							/*
							 * Informix wants its own NULL value here instead
							 * of an error
							 */
							nres = PGTYPESnumeric_new();
							if (nres)
								ECPGset_noind_null(ECPGt_numeric, nres);
							else
							{
								ecpg_raise(lineno, ECPG_OUT_OF_MEMORY,
										   ECPG_SQLSTATE_ECPG_OUT_OF_MEMORY, NULL);
								return false;
							}
						}
						else
						{
							ecpg_raise(lineno, ECPG_NUMERIC_FORMAT,
									   ECPG_SQLSTATE_DATATYPE_MISMATCH, pval);
							return false;
						}
					}
					else
					{
						if (!isarray && garbage_left(isarray, &scan_length, compat))
						{
							free(nres);
							ecpg_raise(lineno, ECPG_NUMERIC_FORMAT,
									   ECPG_SQLSTATE_DATATYPE_MISMATCH, pval);
							return false;
						}
					}
					pval = scan_length;

					if (type == ECPGt_numeric)
						PGTYPESnumeric_copy(nres, (numeric *) (var + offset * act_tuple));
					else
						PGTYPESnumeric_to_decimal(nres, (decimal *) (var + offset * act_tuple));

					PGTYPESnumeric_free(nres);
					break;

				case ECPGt_interval:
					if (*pval == '"')
						pval++;

					for (endptr = pval; *endptr && *endptr != ',' && *endptr != '"' && *endptr != '}'; endptr++);
					endchar = *endptr;
					*endptr = '\0';
					ires = PGTYPESinterval_from_asc(pval, &scan_length);
					*endptr = endchar;

					/* did we get an error? */
					if (ires == NULL)
					{
						ecpg_log("ecpg_get_data on line %d: RESULT %s; errno %d\n",
								 lineno, pval, errno);

						if (INFORMIX_MODE(compat))
						{
							/*
							 * Informix wants its own NULL value here instead
							 * of an error
							 */
							ires = (interval *) ecpg_alloc(sizeof(interval), lineno);
							if (!ires)
								return false;

							ECPGset_noind_null(ECPGt_interval, ires);
						}
						else
						{
							ecpg_raise(lineno, ECPG_INTERVAL_FORMAT,
									   ECPG_SQLSTATE_DATATYPE_MISMATCH, pval);
							return false;
						}
					}
					else
					{
						if (*scan_length == '"')
							scan_length++;

						if (!isarray && garbage_left(isarray, &scan_length, compat))
						{
							free(ires);
							ecpg_raise(lineno, ECPG_INTERVAL_FORMAT,
									   ECPG_SQLSTATE_DATATYPE_MISMATCH, pval);
							return false;
						}
					}
					pval = scan_length;

					PGTYPESinterval_copy(ires, (interval *) (var + offset * act_tuple));
					free(ires);
					break;

				case ECPGt_date:
					if (*pval == '"')
						pval++;

					for (endptr = pval; *endptr && *endptr != ',' && *endptr != '"' && *endptr != '}'; endptr++);
					endchar = *endptr;
					*endptr = '\0';
					ddres = PGTYPESdate_from_asc(pval, &scan_length);
					*endptr = endchar;

					/* did we get an error? */
					if (errno != 0)
					{
						ecpg_log("ecpg_get_data on line %d: RESULT %s; errno %d\n",
								 lineno, pval, errno);

						if (INFORMIX_MODE(compat))
						{
							/*
							 * Informix wants its own NULL value here instead
							 * of an error
							 */
							ECPGset_noind_null(ECPGt_date, &ddres);
						}
						else
						{
							ecpg_raise(lineno, ECPG_DATE_FORMAT,
									   ECPG_SQLSTATE_DATATYPE_MISMATCH, pval);
							return false;
						}
					}
					else
					{
						if (*scan_length == '"')
							scan_length++;

						if (!isarray && garbage_left(isarray, &scan_length, compat))
						{
							ecpg_raise(lineno, ECPG_DATE_FORMAT,
									   ECPG_SQLSTATE_DATATYPE_MISMATCH, pval);
							return false;
						}
					}

					*((date *) (var + offset * act_tuple)) = ddres;
					pval = scan_length;
					break;

				case ECPGt_timestamp:
					if (*pval == '"')
						pval++;

					for (endptr = pval; *endptr && *endptr != ',' && *endptr != '"' && *endptr != '}'; endptr++);
					endchar = *endptr;
					*endptr = '\0';
					tres = PGTYPEStimestamp_from_asc(pval, &scan_length);
					*endptr = endchar;

					/* did we get an error? */
					if (errno != 0)
					{
						ecpg_log("ecpg_get_data on line %d: RESULT %s; errno %d\n",
								 lineno, pval, errno);

						if (INFORMIX_MODE(compat))
						{
							/*
							 * Informix wants its own NULL value here instead
							 * of an error
							 */
							ECPGset_noind_null(ECPGt_timestamp, &tres);
						}
						else
						{
							ecpg_raise(lineno, ECPG_TIMESTAMP_FORMAT,
									   ECPG_SQLSTATE_DATATYPE_MISMATCH, pval);
							return false;
						}
					}
					else
					{
						if (*scan_length == '"')
							scan_length++;

						if (!isarray && garbage_left(isarray, &scan_length, compat))
						{
							ecpg_raise(lineno, ECPG_TIMESTAMP_FORMAT,
									   ECPG_SQLSTATE_DATATYPE_MISMATCH, pval);
							return false;
						}
					}

					*((timestamp *) (var + offset * act_tuple)) = tres;
					pval = scan_length;
					break;

				default:
					ecpg_raise(lineno, ECPG_UNSUPPORTED,
							   ECPG_SQLSTATE_ECPG_INTERNAL_ERROR,
							   ecpg_type_name(type));
					return false;
					break;
			}
			if (ECPG_IS_ARRAY(isarray))
			{
				bool		string = false;

				/* set array to next entry */
				++act_tuple;

				/* set pval to the next entry */

				/*
				 * *pval != '\0' should not be needed, but is used as a safety
				 * guard
				 */
				for (; *pval != '\0' && (string || (!array_delimiter(isarray, *pval) && !array_boundary(isarray, *pval))); ++pval)
					if (*pval == '"')
						string = string ? false : true;

				if (array_delimiter(isarray, *pval))
					++pval;
			}
		}
	} while (*pval != '\0' && !array_boundary(isarray, *pval));

	return true;
}

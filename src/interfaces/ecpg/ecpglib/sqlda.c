/*
 * SQLDA support routines
 *
 * The allocated memory area pointed by an sqlda pointer
 * contains both the metadata and the data, so freeing up
 * is a simple free(sqlda) as expected by the ESQL/C examples.
 */

#define POSTGRES_ECPG_INTERNAL
#include "postgres_fe.h"
#include "pg_type.h"

#include "ecpg-pthread-win32.h"
#include "decimal.h"
#include "ecpgtype.h"
#include "ecpglib.h"
#include "ecpgerrno.h"
#include "extern.h"
#include "sqlca.h"
#include "sqlda-native.h"
#include "sqlda-compat.h"

/*
 * Compute the next variable's offset with
 * the current variable's size and alignment.
 *
 *
 * Returns:
 * - the current variable's offset in *current
 * - the next variable's offset in *next
 */
static void
ecpg_sqlda_align_add_size(long offset, int alignment, int size, long *current, long *next)
{
	if (offset % alignment)
		offset += alignment - (offset % alignment);
	if (current)
		*current = offset;
	offset += size;
	if (next)
		*next = offset;
}

static long
sqlda_compat_empty_size(const PGresult *res)
{
	long		offset;
	int			i;
	int			sqld = PQnfields(res);

	/* Initial size to store main structure and field structures */
	offset = sizeof(struct sqlda_compat) + sqld * sizeof(struct sqlvar_compat);

	/* Add space for field names */
	for (i = 0; i < sqld; i++)
		offset += strlen(PQfname(res, i)) + 1;

	/* Add padding to the first field value */
	ecpg_sqlda_align_add_size(offset, sizeof(int), 0, &offset, NULL);

	return offset;
}

static long
sqlda_common_total_size(const PGresult *res, int row, enum COMPAT_MODE compat, long offset)
{
	int			sqld = PQnfields(res);
	int			i;
	long		next_offset;

	/* Add space for the field values */
	for (i = 0; i < sqld; i++)
	{
		enum ECPGttype type = sqlda_dynamic_type(PQftype(res, i), compat);

		switch (type)
		{
			case ECPGt_short:
			case ECPGt_unsigned_short:
				ecpg_sqlda_align_add_size(offset, sizeof(short), sizeof(short), &offset, &next_offset);
				break;
			case ECPGt_int:
			case ECPGt_unsigned_int:
				ecpg_sqlda_align_add_size(offset, sizeof(int), sizeof(int), &offset, &next_offset);
				break;
			case ECPGt_long:
			case ECPGt_unsigned_long:
				ecpg_sqlda_align_add_size(offset, sizeof(long), sizeof(long), &offset, &next_offset);
				break;
			case ECPGt_long_long:
			case ECPGt_unsigned_long_long:
				ecpg_sqlda_align_add_size(offset, sizeof(long long), sizeof(long long), &offset, &next_offset);
				break;
			case ECPGt_bool:
				ecpg_sqlda_align_add_size(offset, sizeof(bool), sizeof(bool), &offset, &next_offset);
				break;
			case ECPGt_float:
				ecpg_sqlda_align_add_size(offset, sizeof(float), sizeof(float), &offset, &next_offset);
				break;
			case ECPGt_double:
				ecpg_sqlda_align_add_size(offset, sizeof(double), sizeof(double), &offset, &next_offset);
				break;
			case ECPGt_decimal:
				ecpg_sqlda_align_add_size(offset, sizeof(int), sizeof(decimal), &offset, &next_offset);
				break;
			case ECPGt_numeric:

				/*
				 * We align the numeric struct to allow it to store a pointer,
				 * while the digits array is aligned to int (which seems like
				 * overkill, but let's keep compatibility here).
				 *
				 * Unfortunately we need to deconstruct the value twice to
				 * find out the digits array's size and then later fill it.
				 */
				ecpg_sqlda_align_add_size(offset, sizeof(NumericDigit *), sizeof(numeric), &offset, &next_offset);
				if (!PQgetisnull(res, row, i))
				{
					char	   *val = PQgetvalue(res, row, i);
					numeric    *num;

					num = PGTYPESnumeric_from_asc(val, NULL);
					if (!num)
						break;
					if (num->buf)
						ecpg_sqlda_align_add_size(next_offset, sizeof(int), num->digits - num->buf + num->ndigits, &offset, &next_offset);
					PGTYPESnumeric_free(num);
				}
				break;
			case ECPGt_date:
				ecpg_sqlda_align_add_size(offset, sizeof(date), sizeof(date), &offset, &next_offset);
				break;
			case ECPGt_timestamp:
				ecpg_sqlda_align_add_size(offset, sizeof(int64), sizeof(timestamp), &offset, &next_offset);
				break;
			case ECPGt_interval:
				ecpg_sqlda_align_add_size(offset, sizeof(int64), sizeof(interval), &offset, &next_offset);
				break;
			case ECPGt_char:
			case ECPGt_unsigned_char:
			case ECPGt_string:
			default:
				{
					long		datalen = strlen(PQgetvalue(res, row, i)) + 1;

					ecpg_sqlda_align_add_size(offset, sizeof(int), datalen, &offset, &next_offset);
					break;
				}
		}
		offset = next_offset;
	}
	return offset;
}


static long
sqlda_compat_total_size(const PGresult *res, int row, enum COMPAT_MODE compat)
{
	long		offset;

	offset = sqlda_compat_empty_size(res);

	if (row < 0)
		return offset;

	offset = sqlda_common_total_size(res, row, compat, offset);
	return offset;
}

static long
sqlda_native_empty_size(const PGresult *res)
{
	long		offset;
	int			sqld = PQnfields(res);

	/* Initial size to store main structure and field structures */
	offset = sizeof(struct sqlda_struct) + (sqld - 1) * sizeof(struct sqlvar_struct);

	/* Add padding to the first field value */
	ecpg_sqlda_align_add_size(offset, sizeof(int), 0, &offset, NULL);

	return offset;
}

static long
sqlda_native_total_size(const PGresult *res, int row, enum COMPAT_MODE compat)
{
	long		offset;

	offset = sqlda_native_empty_size(res);

	if (row < 0)
		return offset;

	offset = sqlda_common_total_size(res, row, compat, offset);
	return offset;
}

/*
 * Build "struct sqlda_compat" (metadata only) from PGresult
 * leaving enough space for the field values in
 * the given row number
 */
struct sqlda_compat *
ecpg_build_compat_sqlda(int line, PGresult *res, int row, enum COMPAT_MODE compat)
{
	struct sqlda_compat *sqlda;
	struct sqlvar_compat *sqlvar;
	char	   *fname;
	long		size;
	int			sqld;
	int			i;

	size = sqlda_compat_total_size(res, row, compat);
	sqlda = (struct sqlda_compat *) ecpg_alloc(size, line);
	if (!sqlda)
		return NULL;

	memset(sqlda, 0, size);
	sqlvar = (struct sqlvar_compat *) (sqlda + 1);
	sqld = PQnfields(res);
	fname = (char *) (sqlvar + sqld);

	sqlda->sqld = sqld;
	ecpg_log("ecpg_build_compat_sqlda on line %d sqld = %d\n", line, sqld);
	sqlda->desc_occ = size;		/* cheat here, keep the full allocated size */
	sqlda->sqlvar = sqlvar;

	for (i = 0; i < sqlda->sqld; i++)
	{
		sqlda->sqlvar[i].sqltype = sqlda_dynamic_type(PQftype(res, i), compat);
		strcpy(fname, PQfname(res, i));
		sqlda->sqlvar[i].sqlname = fname;
		fname += strlen(sqlda->sqlvar[i].sqlname) + 1;

		/*
		 * this is reserved for future use, so we leave it empty for the time
		 * being
		 */
		/* sqlda->sqlvar[i].sqlformat = (char *) (long) PQfformat(res, i); */
		sqlda->sqlvar[i].sqlxid = PQftype(res, i);
		sqlda->sqlvar[i].sqltypelen = PQfsize(res, i);
	}

	return sqlda;
}

/*
 * Sets values from PGresult.
 */
static int16 value_is_null = -1;
static int16 value_is_not_null = 0;

void
ecpg_set_compat_sqlda(int lineno, struct sqlda_compat ** _sqlda, const PGresult *res, int row, enum COMPAT_MODE compat)
{
	struct sqlda_compat *sqlda = (*_sqlda);
	int			i;
	long		offset,
				next_offset;

	if (row < 0)
		return;

	/* Offset for the first field value */
	offset = sqlda_compat_empty_size(res);

	/*
	 * Set sqlvar[i]->sqldata pointers and convert values to correct format
	 */
	for (i = 0; i < sqlda->sqld; i++)
	{
		int			isnull;
		int			datalen;
		bool		set_data = true;

		switch (sqlda->sqlvar[i].sqltype)
		{
			case ECPGt_short:
			case ECPGt_unsigned_short:
				ecpg_sqlda_align_add_size(offset, sizeof(short), sizeof(short), &offset, &next_offset);
				sqlda->sqlvar[i].sqldata = (char *) sqlda + offset;
				sqlda->sqlvar[i].sqllen = sizeof(short);
				break;
			case ECPGt_int:
			case ECPGt_unsigned_int:
				ecpg_sqlda_align_add_size(offset, sizeof(int), sizeof(int), &offset, &next_offset);
				sqlda->sqlvar[i].sqldata = (char *) sqlda + offset;
				sqlda->sqlvar[i].sqllen = sizeof(int);
				break;
			case ECPGt_long:
			case ECPGt_unsigned_long:
				ecpg_sqlda_align_add_size(offset, sizeof(long), sizeof(long), &offset, &next_offset);
				sqlda->sqlvar[i].sqldata = (char *) sqlda + offset;
				sqlda->sqlvar[i].sqllen = sizeof(long);
				break;
			case ECPGt_long_long:
			case ECPGt_unsigned_long_long:
				ecpg_sqlda_align_add_size(offset, sizeof(long long), sizeof(long long), &offset, &next_offset);
				sqlda->sqlvar[i].sqldata = (char *) sqlda + offset;
				sqlda->sqlvar[i].sqllen = sizeof(long long);
				break;
			case ECPGt_bool:
				ecpg_sqlda_align_add_size(offset, sizeof(bool), sizeof(bool), &offset, &next_offset);
				sqlda->sqlvar[i].sqldata = (char *) sqlda + offset;
				sqlda->sqlvar[i].sqllen = sizeof(bool);
				break;
			case ECPGt_float:
				ecpg_sqlda_align_add_size(offset, sizeof(float), sizeof(float), &offset, &next_offset);
				sqlda->sqlvar[i].sqldata = (char *) sqlda + offset;
				sqlda->sqlvar[i].sqllen = sizeof(float);
				break;
			case ECPGt_double:
				ecpg_sqlda_align_add_size(offset, sizeof(double), sizeof(double), &offset, &next_offset);
				sqlda->sqlvar[i].sqldata = (char *) sqlda + offset;
				sqlda->sqlvar[i].sqllen = sizeof(double);
				break;
			case ECPGt_decimal:
				ecpg_sqlda_align_add_size(offset, sizeof(int), sizeof(decimal), &offset, &next_offset);
				sqlda->sqlvar[i].sqldata = (char *) sqlda + offset;
				sqlda->sqlvar[i].sqllen = sizeof(decimal);
				break;
			case ECPGt_numeric:
				{
					numeric    *num;
					char	   *val;

					set_data = false;

					ecpg_sqlda_align_add_size(offset, sizeof(NumericDigit *), sizeof(numeric), &offset, &next_offset);
					sqlda->sqlvar[i].sqldata = (char *) sqlda + offset;
					sqlda->sqlvar[i].sqllen = sizeof(numeric);

					if (PQgetisnull(res, row, i))
					{
						ECPGset_noind_null(ECPGt_numeric, sqlda->sqlvar[i].sqldata);
						break;
					}

					val = PQgetvalue(res, row, i);
					num = PGTYPESnumeric_from_asc(val, NULL);
					if (!num)
					{
						ECPGset_noind_null(ECPGt_numeric, sqlda->sqlvar[i].sqldata);
						break;
					}

					memcpy(sqlda->sqlvar[i].sqldata, num, sizeof(numeric));

					if (num->buf)
					{
						ecpg_sqlda_align_add_size(next_offset, sizeof(int), num->digits - num->buf + num->ndigits, &offset, &next_offset);
						memcpy((char *) sqlda + offset, num->buf, num->digits - num->buf + num->ndigits);

						((numeric *) sqlda->sqlvar[i].sqldata)->buf = (NumericDigit *) sqlda + offset;
						((numeric *) sqlda->sqlvar[i].sqldata)->digits = (NumericDigit *) sqlda + offset + (num->digits - num->buf);
					}

					PGTYPESnumeric_free(num);

					break;
				}
			case ECPGt_date:
				ecpg_sqlda_align_add_size(offset, sizeof(date), sizeof(date), &offset, &next_offset);
				sqlda->sqlvar[i].sqldata = (char *) sqlda + offset;
				sqlda->sqlvar[i].sqllen = sizeof(date);
				break;
			case ECPGt_timestamp:
				ecpg_sqlda_align_add_size(offset, sizeof(int64), sizeof(timestamp), &offset, &next_offset);
				sqlda->sqlvar[i].sqldata = (char *) sqlda + offset;
				sqlda->sqlvar[i].sqllen = sizeof(timestamp);
				break;
			case ECPGt_interval:
				ecpg_sqlda_align_add_size(offset, sizeof(int64), sizeof(interval), &offset, &next_offset);
				sqlda->sqlvar[i].sqldata = (char *) sqlda + offset;
				sqlda->sqlvar[i].sqllen = sizeof(interval);
				break;
			case ECPGt_char:
			case ECPGt_unsigned_char:
			case ECPGt_string:
			default:
				datalen = strlen(PQgetvalue(res, row, i)) + 1;
				ecpg_sqlda_align_add_size(offset, sizeof(int), datalen, &offset, &next_offset);
				sqlda->sqlvar[i].sqldata = (char *) sqlda + offset;
				sqlda->sqlvar[i].sqllen = datalen;
				if (datalen > 32768)
					sqlda->sqlvar[i].sqlilongdata = sqlda->sqlvar[i].sqldata;
				break;
		}

		isnull = PQgetisnull(res, row, i);
		ecpg_log("ecpg_set_compat_sqlda on line %d row %d col %d %s\n", lineno, row, i, isnull ? "IS NULL" : "IS NOT NULL");
		sqlda->sqlvar[i].sqlind = isnull ? &value_is_null : &value_is_not_null;
		sqlda->sqlvar[i].sqlitype = ECPGt_short;
		sqlda->sqlvar[i].sqlilen = sizeof(short);
		if (!isnull)
		{
			if (set_data)
				ecpg_get_data(res, row, i, lineno,
							  sqlda->sqlvar[i].sqltype, ECPGt_NO_INDICATOR,
							  sqlda->sqlvar[i].sqldata, NULL, 0, 0, 0,
							  ECPG_ARRAY_NONE, compat, false);
		}
		else
			ECPGset_noind_null(sqlda->sqlvar[i].sqltype, sqlda->sqlvar[i].sqldata);

		offset = next_offset;
	}
}

struct sqlda_struct *
ecpg_build_native_sqlda(int line, PGresult *res, int row, enum COMPAT_MODE compat)
{
	struct sqlda_struct *sqlda;
	long		size;
	int			i;

	size = sqlda_native_total_size(res, row, compat);
	sqlda = (struct sqlda_struct *) ecpg_alloc(size, line);
	if (!sqlda)
		return NULL;

	memset(sqlda, 0, size);

	sprintf(sqlda->sqldaid, "SQLDA  ");
	sqlda->sqld = sqlda->sqln = PQnfields(res);
	ecpg_log("ecpg_build_native_sqlda on line %d sqld = %d\n", line, sqlda->sqld);
	sqlda->sqldabc = sizeof(struct sqlda_struct) + (sqlda->sqld - 1) * sizeof(struct sqlvar_struct);

	for (i = 0; i < sqlda->sqld; i++)
	{
		char	   *fname;

		sqlda->sqlvar[i].sqltype = sqlda_dynamic_type(PQftype(res, i), compat);
		fname = PQfname(res, i);
		sqlda->sqlvar[i].sqlname.length = strlen(fname);
		strcpy(sqlda->sqlvar[i].sqlname.data, fname);
	}

	return sqlda;
}

void
ecpg_set_native_sqlda(int lineno, struct sqlda_struct ** _sqlda, const PGresult *res, int row, enum COMPAT_MODE compat)
{
	struct sqlda_struct *sqlda = (*_sqlda);
	int			i;
	long		offset,
				next_offset;

	if (row < 0)
		return;

	/* Offset for the first field value */
	offset = sqlda_native_empty_size(res);

	/*
	 * Set sqlvar[i]->sqldata pointers and convert values to correct format
	 */
	for (i = 0; i < sqlda->sqld; i++)
	{
		int			isnull;
		int			datalen;
		bool		set_data = true;

		switch (sqlda->sqlvar[i].sqltype)
		{
			case ECPGt_short:
			case ECPGt_unsigned_short:
				ecpg_sqlda_align_add_size(offset, sizeof(short), sizeof(short), &offset, &next_offset);
				sqlda->sqlvar[i].sqldata = (char *) sqlda + offset;
				sqlda->sqlvar[i].sqllen = sizeof(short);
				break;
			case ECPGt_int:
			case ECPGt_unsigned_int:
				ecpg_sqlda_align_add_size(offset, sizeof(int), sizeof(int), &offset, &next_offset);
				sqlda->sqlvar[i].sqldata = (char *) sqlda + offset;
				sqlda->sqlvar[i].sqllen = sizeof(int);
				break;
			case ECPGt_long:
			case ECPGt_unsigned_long:
				ecpg_sqlda_align_add_size(offset, sizeof(long), sizeof(long), &offset, &next_offset);
				sqlda->sqlvar[i].sqldata = (char *) sqlda + offset;
				sqlda->sqlvar[i].sqllen = sizeof(long);
				break;
			case ECPGt_long_long:
			case ECPGt_unsigned_long_long:
				ecpg_sqlda_align_add_size(offset, sizeof(long long), sizeof(long long), &offset, &next_offset);
				sqlda->sqlvar[i].sqldata = (char *) sqlda + offset;
				sqlda->sqlvar[i].sqllen = sizeof(long long);
				break;
			case ECPGt_bool:
				ecpg_sqlda_align_add_size(offset, sizeof(bool), sizeof(bool), &offset, &next_offset);
				sqlda->sqlvar[i].sqldata = (char *) sqlda + offset;
				sqlda->sqlvar[i].sqllen = sizeof(bool);
				break;
			case ECPGt_float:
				ecpg_sqlda_align_add_size(offset, sizeof(float), sizeof(float), &offset, &next_offset);
				sqlda->sqlvar[i].sqldata = (char *) sqlda + offset;
				sqlda->sqlvar[i].sqllen = sizeof(float);
				break;
			case ECPGt_double:
				ecpg_sqlda_align_add_size(offset, sizeof(double), sizeof(double), &offset, &next_offset);
				sqlda->sqlvar[i].sqldata = (char *) sqlda + offset;
				sqlda->sqlvar[i].sqllen = sizeof(double);
				break;
			case ECPGt_decimal:
				ecpg_sqlda_align_add_size(offset, sizeof(int), sizeof(decimal), &offset, &next_offset);
				sqlda->sqlvar[i].sqldata = (char *) sqlda + offset;
				sqlda->sqlvar[i].sqllen = sizeof(decimal);
				break;
			case ECPGt_numeric:
				{
					numeric    *num;
					char	   *val;

					set_data = false;

					ecpg_sqlda_align_add_size(offset, sizeof(NumericDigit *), sizeof(numeric), &offset, &next_offset);
					sqlda->sqlvar[i].sqldata = (char *) sqlda + offset;
					sqlda->sqlvar[i].sqllen = sizeof(numeric);

					if (PQgetisnull(res, row, i))
					{
						ECPGset_noind_null(ECPGt_numeric, sqlda->sqlvar[i].sqldata);
						break;
					}

					val = PQgetvalue(res, row, i);
					num = PGTYPESnumeric_from_asc(val, NULL);
					if (!num)
					{
						ECPGset_noind_null(ECPGt_numeric, sqlda->sqlvar[i].sqldata);
						break;
					}

					memcpy(sqlda->sqlvar[i].sqldata, num, sizeof(numeric));

					if (num->buf)
					{
						ecpg_sqlda_align_add_size(next_offset, sizeof(int), num->digits - num->buf + num->ndigits, &offset, &next_offset);
						memcpy((char *) sqlda + offset, num->buf, num->digits - num->buf + num->ndigits);

						((numeric *) sqlda->sqlvar[i].sqldata)->buf = (NumericDigit *) sqlda + offset;
						((numeric *) sqlda->sqlvar[i].sqldata)->digits = (NumericDigit *) sqlda + offset + (num->digits - num->buf);
					}

					PGTYPESnumeric_free(num);

					break;
				}
			case ECPGt_date:
				ecpg_sqlda_align_add_size(offset, sizeof(date), sizeof(date), &offset, &next_offset);
				sqlda->sqlvar[i].sqldata = (char *) sqlda + offset;
				sqlda->sqlvar[i].sqllen = sizeof(date);
				break;
			case ECPGt_timestamp:
				ecpg_sqlda_align_add_size(offset, sizeof(int64), sizeof(timestamp), &offset, &next_offset);
				sqlda->sqlvar[i].sqldata = (char *) sqlda + offset;
				sqlda->sqlvar[i].sqllen = sizeof(timestamp);
				break;
			case ECPGt_interval:
				ecpg_sqlda_align_add_size(offset, sizeof(int64), sizeof(interval), &offset, &next_offset);
				sqlda->sqlvar[i].sqldata = (char *) sqlda + offset;
				sqlda->sqlvar[i].sqllen = sizeof(interval);
				break;
			case ECPGt_char:
			case ECPGt_unsigned_char:
			case ECPGt_string:
			default:
				datalen = strlen(PQgetvalue(res, row, i)) + 1;
				ecpg_sqlda_align_add_size(offset, sizeof(int), datalen, &offset, &next_offset);
				sqlda->sqlvar[i].sqldata = (char *) sqlda + offset;
				sqlda->sqlvar[i].sqllen = datalen;
				break;
		}

		isnull = PQgetisnull(res, row, i);
		ecpg_log("ecpg_set_native_sqlda on line %d row %d col %d %s\n", lineno, row, i, isnull ? "IS NULL" : "IS NOT NULL");
		sqlda->sqlvar[i].sqlind = isnull ? &value_is_null : &value_is_not_null;
		if (!isnull)
		{
			if (set_data)
				ecpg_get_data(res, row, i, lineno,
							  sqlda->sqlvar[i].sqltype, ECPGt_NO_INDICATOR,
							  sqlda->sqlvar[i].sqldata, NULL, 0, 0, 0,
							  ECPG_ARRAY_NONE, compat, false);
		}

		offset = next_offset;
	}
}

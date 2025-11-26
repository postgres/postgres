/*-------------------------------------------------------------------------
 *
 * pg_datemath.c
 *		Enhanced date difference functions for PostgreSQL.
 *
 * This extension provides datediff(datepart, start_date, end_date) which
 * calculates the difference between two dates using a hybrid calculation
 * model: full calendar units plus contextual fractions based on actual
 * period lengths.
 *
 * Copyright (c) 2024, PostgreSQL Global Development Group
 *
 * contrib/pg_datemath/pg_datemath.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <math.h>

#include "datatype/timestamp.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datetime.h"
#include "utils/numeric.h"
#include "utils/timestamp.h"

PG_MODULE_MAGIC_EXT(
					.name = "pg_datemath",
					.version = PG_VERSION
);

/*
 * Datepart enumeration for routing calculation logic
 */
typedef enum
{
	DATEPART_DAY,
	DATEPART_WEEK,
	DATEPART_MONTH,
	DATEPART_QUARTER,
	DATEPART_YEAR,
	DATEPART_INVALID
} DatepartType;

/*
 * parse_datepart - convert datepart string to enum
 *
 * Performs case-insensitive comparison and handles aliases.
 * Returns DATEPART_INVALID for unrecognized input.
 */
static DatepartType
parse_datepart(const char *datepart_str)
{
	char		lower[32];
	int			i;

	/* Convert to lowercase for comparison */
	for (i = 0; datepart_str[i] && i < 31; i++)
		lower[i] = tolower((unsigned char) datepart_str[i]);
	lower[i] = '\0';

	/* Match canonical names and aliases */
	if (strcmp(lower, "year") == 0 ||
		strcmp(lower, "yy") == 0 ||
		strcmp(lower, "yyyy") == 0 ||
		strcmp(lower, "y") == 0 ||
		strcmp(lower, "years") == 0)
		return DATEPART_YEAR;

	if (strcmp(lower, "quarter") == 0 ||
		strcmp(lower, "qq") == 0 ||
		strcmp(lower, "q") == 0 ||
		strcmp(lower, "quarters") == 0)
		return DATEPART_QUARTER;

	if (strcmp(lower, "month") == 0 ||
		strcmp(lower, "mm") == 0 ||
		strcmp(lower, "m") == 0 ||
		strcmp(lower, "months") == 0)
		return DATEPART_MONTH;

	if (strcmp(lower, "week") == 0 ||
		strcmp(lower, "wk") == 0 ||
		strcmp(lower, "ww") == 0 ||
		strcmp(lower, "w") == 0 ||
		strcmp(lower, "weeks") == 0)
		return DATEPART_WEEK;

	if (strcmp(lower, "day") == 0 ||
		strcmp(lower, "dd") == 0 ||
		strcmp(lower, "d") == 0 ||
		strcmp(lower, "days") == 0)
		return DATEPART_DAY;

	return DATEPART_INVALID;
}

/*
 * days_in_month_helper - get days in a specific month
 *
 * Uses PostgreSQL's day_tab array.
 * month is 1-based (1=January, 12=December)
 */
static int
days_in_month_helper(int year, int month)
{
	return day_tab[isleap(year) ? 1 : 0][month - 1];
}

/*
 * is_end_of_month - check if day is the last day of its month
 */
static bool
is_end_of_month(int year, int month, int day)
{
	return day == days_in_month_helper(year, month);
}

/*
 * days_in_quarter - get total days in a specific quarter
 *
 * Quarter is 1-4.
 * Q1: Jan+Feb+Mar, Q2: Apr+May+Jun, Q3: Jul+Aug+Sep, Q4: Oct+Nov+Dec
 */
static int
days_in_quarter(int year, int quarter)
{
	int			first_month = (quarter - 1) * 3 + 1;
	int			days = 0;
	int			i;

	for (i = 0; i < 3; i++)
		days += days_in_month_helper(year, first_month + i);

	return days;
}

/*
 * day_of_quarter - get day position within a quarter (1-92)
 */
static int
day_of_quarter(int year, int month, int day)
{
	int			quarter = (month - 1) / 3 + 1;
	int			first_month = (quarter - 1) * 3 + 1;
	int			days = 0;
	int			m;

	/* Sum days in complete months before this month within the quarter */
	for (m = first_month; m < month; m++)
		days += days_in_month_helper(year, m);

	return days + day;
}

/*
 * bankers_round - round to 3 decimal places using HALF_EVEN (banker's rounding)
 *
 * Decimal results are rounded to exactly 3 decimal places using HALF_EVEN
 * (banker's) rounding for consistent, unbiased results.
 */
static double
bankers_round(double value)
{
	double		scaled = value * 1000.0;
	double		integer_part;
	double		frac = modf(scaled, &integer_part);
	int64		int_val = (int64) integer_part;

	/*
	 * Banker's rounding: round half to even
	 * If fraction is exactly 0.5, round to nearest even number
	 */
	if (fabs(frac) == 0.5)
	{
		/* Round to even */
		if (int_val % 2 == 0)
			scaled = integer_part;		/* Already even, truncate */
		else
			scaled = integer_part + (value >= 0 ? 1.0 : -1.0);	/* Round away */
	}
	else
	{
		/* Standard rounding */
		scaled = round(scaled);
	}

	return scaled / 1000.0;
}

/*
 * make_numeric_result - convert double to NUMERIC with 3 decimal places
 *
 * Uses string conversion approach for precise decimal representation.
 */
static Datum
make_numeric_result(double value)
{
	char		result_str[32];
	Datum		result;

	snprintf(result_str, sizeof(result_str), "%.3f", value);
	result = DirectFunctionCall3(numeric_in,
								 CStringGetDatum(result_str),
								 ObjectIdGetDatum(InvalidOid),
								 Int32GetDatum(-1));
	return result;
}

/*
 * compute_diff_day - calculate day difference
 *
 * Simple subtraction, returns whole number as NUMERIC.
 */
static Datum
compute_diff_day(int start_y, int start_m, int start_d,
				 int end_y, int end_m, int end_d)
{
	int			start_jd = date2j(start_y, start_m, start_d);
	int			end_jd = date2j(end_y, end_m, end_d);
	int64		diff = (int64) end_jd - (int64) start_jd;

	return NumericGetDatum(int64_to_numeric(diff));
}

/*
 * compute_diff_week - calculate week difference
 *
 * Total days / 7, rounded to 3 decimal places.
 */
static Datum
compute_diff_week(int start_y, int start_m, int start_d,
				  int end_y, int end_m, int end_d)
{
	int			start_jd = date2j(start_y, start_m, start_d);
	int			end_jd = date2j(end_y, end_m, end_d);
	int64		days = (int64) end_jd - (int64) start_jd;
	double		weeks = (double) days / 7.0;

	return make_numeric_result(bankers_round(weeks));
}

/*
 * compute_diff_month - calculate month difference using hybrid model
 *
 * Calculation model:
 * - Aligned dates (same day-of-month or both end-of-month) return whole numbers
 * - Non-aligned: full months + (remaining days / days in partial period)
 */
static Datum
compute_diff_month(int start_y, int start_m, int start_d,
				   int end_y, int end_m, int end_d)
{
	bool		negated = false;
	int			full_months;
	int			remaining_days;
	int			partial_period_days;
	double		result;
	bool		start_eom;
	bool		end_eom;
	bool		aligned;
	int			anniversary_y, anniversary_m, anniversary_d;
	int			anniversary_jd, end_jd;

	/* Handle negative spans by swapping and negating result */
	if (start_y > end_y ||
		(start_y == end_y && start_m > end_m) ||
		(start_y == end_y && start_m == end_m && start_d > end_d))
	{
		int			tmp_y = start_y, tmp_m = start_m, tmp_d = start_d;

		start_y = end_y;
		start_m = end_m;
		start_d = end_d;
		end_y = tmp_y;
		end_m = tmp_m;
		end_d = tmp_d;
		negated = true;
	}

	/* Check for calendar alignment */
	start_eom = is_end_of_month(start_y, start_m, start_d);
	end_eom = is_end_of_month(end_y, end_m, end_d);
	aligned = (start_d == end_d) || (start_eom && end_eom);

	/* Calculate full months */
	full_months = (end_y - start_y) * 12 + (end_m - start_m);

	if (aligned)
	{
		/* Aligned dates return whole numbers */
		result = (double) full_months;
	}
	else
	{
		/*
		 * Find the last "anniversary" before or on end_date.
		 * Anniversary is the same day-of-month as start_d, or end-of-month
		 * if start was end-of-month.
		 */
		if (end_d < start_d)
			full_months--;

		if (full_months < 0)
			full_months = 0;

		/* Calculate anniversary date */
		anniversary_y = start_y + (start_m + full_months - 1) / 12;
		anniversary_m = ((start_m - 1 + full_months) % 12) + 1;

		/*
		 * Handle case where start_d doesn't exist in anniversary month
		 * (e.g., Jan 31 -> Feb has no 31st)
		 */
		if (start_d > days_in_month_helper(anniversary_y, anniversary_m))
			anniversary_d = days_in_month_helper(anniversary_y, anniversary_m);
		else
			anniversary_d = start_d;

		/* Calculate remaining days after anniversary */
		anniversary_jd = date2j(anniversary_y, anniversary_m, anniversary_d);
		end_jd = date2j(end_y, end_m, end_d);
		remaining_days = end_jd - anniversary_jd;

		/*
		 * Calculate partial period length (days from anniversary to next
		 * anniversary)
		 */
		{
			int			next_anniversary_y = anniversary_y + (anniversary_m) / 12;
			int			next_anniversary_m = (anniversary_m % 12) + 1;
			int			next_anniversary_d;
			int			next_anniversary_jd;

			if (start_d > days_in_month_helper(next_anniversary_y, next_anniversary_m))
				next_anniversary_d = days_in_month_helper(next_anniversary_y, next_anniversary_m);
			else
				next_anniversary_d = start_d;

			next_anniversary_jd = date2j(next_anniversary_y, next_anniversary_m, next_anniversary_d);
			partial_period_days = next_anniversary_jd - anniversary_jd;
		}

		if (partial_period_days <= 0)
			partial_period_days = 1;	/* Safety guard */

		result = (double) full_months + (double) remaining_days / (double) partial_period_days;
	}

	if (negated)
		result = -result;

	return make_numeric_result(bankers_round(result));
}

/*
 * compute_diff_quarter - calculate quarter difference using hybrid model
 *
 * Similar to month but with quarter-based periods.
 */
static Datum
compute_diff_quarter(int start_y, int start_m, int start_d,
					 int end_y, int end_m, int end_d)
{
	bool		negated = false;
	int			start_quarter, end_quarter;
	int			start_day_of_qtr, end_day_of_qtr;
	int			full_quarters;
	int			remaining_days;
	int			partial_period_days;
	double		result;

	/* Handle negative spans */
	if (start_y > end_y ||
		(start_y == end_y && start_m > end_m) ||
		(start_y == end_y && start_m == end_m && start_d > end_d))
	{
		int			tmp_y = start_y, tmp_m = start_m, tmp_d = start_d;

		start_y = end_y;
		start_m = end_m;
		start_d = end_d;
		end_y = tmp_y;
		end_m = tmp_m;
		end_d = tmp_d;
		negated = true;
	}

	start_quarter = (start_m - 1) / 3 + 1;
	end_quarter = (end_m - 1) / 3 + 1;
	start_day_of_qtr = day_of_quarter(start_y, start_m, start_d);
	end_day_of_qtr = day_of_quarter(end_y, end_m, end_d);

	/* Calculate full quarters */
	full_quarters = (end_y - start_y) * 4 + (end_quarter - start_quarter);

	/* Check alignment: same day-of-quarter position */
	if (start_day_of_qtr == end_day_of_qtr)
	{
		result = (double) full_quarters;
	}
	else
	{
		/*
		 * Non-aligned: find anniversary (same position in quarter), calculate
		 * remaining days
		 */
		int			anniversary_y, anniversary_quarter, anniversary_m, anniversary_d;
		int			anniversary_jd, end_jd;
		/* Adjust full_quarters if end is before anniversary position */
		if (end_day_of_qtr < start_day_of_qtr)
			full_quarters--;

		if (full_quarters < 0)
			full_quarters = 0;

		/* Calculate anniversary date */
		anniversary_quarter = start_quarter + full_quarters;
		anniversary_y = start_y + (anniversary_quarter - 1) / 4;
		anniversary_quarter = ((anniversary_quarter - 1) % 4) + 1;

		/* Convert day-of-quarter back to month and day */
		{
			int			first_month = (anniversary_quarter - 1) * 3 + 1;
			int			days_remaining = start_day_of_qtr;
			int			m;
			bool		found = false;

			anniversary_m = first_month;
			anniversary_d = 1;	/* Default initialization */
			for (m = first_month; m <= first_month + 2 && days_remaining > 0; m++)
			{
				int			days_in_m = days_in_month_helper(anniversary_y, m);

				if (days_remaining <= days_in_m)
				{
					anniversary_m = m;
					anniversary_d = days_remaining;
					found = true;
					break;
				}
				days_remaining -= days_in_m;
			}

			/* Handle overflow (day position exceeds quarter length) */
			if (!found)
			{
				anniversary_m = first_month + 2;
				anniversary_d = days_in_month_helper(anniversary_y, anniversary_m);
			}
		}

		/* Ensure anniversary_d is valid */
		if (anniversary_d > days_in_month_helper(anniversary_y, anniversary_m))
			anniversary_d = days_in_month_helper(anniversary_y, anniversary_m);

		anniversary_jd = date2j(anniversary_y, anniversary_m, anniversary_d);
		end_jd = date2j(end_y, end_m, end_d);
		remaining_days = end_jd - anniversary_jd;

		/* Partial period is the quarter containing the anniversary */
		partial_period_days = days_in_quarter(anniversary_y, anniversary_quarter);

		if (partial_period_days <= 0)
			partial_period_days = 1;

		result = (double) full_quarters + (double) remaining_days / (double) partial_period_days;
	}

	if (negated)
		result = -result;

	return make_numeric_result(bankers_round(result));
}

/*
 * compute_diff_year - calculate year difference using hybrid model
 *
 * Similar to month but with year-based periods.
 */
static Datum
compute_diff_year(int start_y, int start_m, int start_d,
				  int end_y, int end_m, int end_d)
{
	bool		negated = false;
	int			full_years;
	int			remaining_days;
	int			partial_period_days;
	double		result;
	bool		aligned;
	int			anniversary_y, anniversary_m, anniversary_d;
	int			anniversary_jd, end_jd;

	/* Handle negative spans */
	if (start_y > end_y ||
		(start_y == end_y && start_m > end_m) ||
		(start_y == end_y && start_m == end_m && start_d > end_d))
	{
		int			tmp_y = start_y, tmp_m = start_m, tmp_d = start_d;

		start_y = end_y;
		start_m = end_m;
		start_d = end_d;
		end_y = tmp_y;
		end_m = tmp_m;
		end_d = tmp_d;
		negated = true;
	}

	/* Check alignment: same month and day, or Feb 29 -> Feb 28 in non-leap */
	aligned = (start_m == end_m && start_d == end_d);

	/* Special case: Feb 29 in leap year aligns with Feb 28 in non-leap */
	if (!aligned && start_m == 2 && start_d == 29 && end_m == 2 && end_d == 28)
	{
		if (!isleap(end_y))
			aligned = true;
	}
	if (!aligned && start_m == 2 && start_d == 28 && end_m == 2 && end_d == 29)
	{
		if (!isleap(start_y))
			aligned = true;
	}

	/* Calculate full years */
	full_years = end_y - start_y;
	if (end_m < start_m || (end_m == start_m && end_d < start_d))
		full_years--;

	if (full_years < 0)
		full_years = 0;

	if (aligned && full_years > 0)
	{
		result = (double) full_years;
	}
	else if (aligned && full_years == 0 && end_y > start_y)
	{
		/* Exact one year */
		result = 1.0;
	}
	else if (start_y == end_y && start_m == end_m && start_d == end_d)
	{
		/* Same date */
		result = 0.0;
	}
	else
	{
		/* Non-aligned: calculate fractional part */
		anniversary_y = start_y + full_years;
		anniversary_m = start_m;

		/* Handle Feb 29 when anniversary year is not a leap year */
		if (start_m == 2 && start_d == 29 && !isleap(anniversary_y))
			anniversary_d = 28;
		else if (start_d > days_in_month_helper(anniversary_y, anniversary_m))
			anniversary_d = days_in_month_helper(anniversary_y, anniversary_m);
		else
			anniversary_d = start_d;

		anniversary_jd = date2j(anniversary_y, anniversary_m, anniversary_d);
		end_jd = date2j(end_y, end_m, end_d);
		remaining_days = end_jd - anniversary_jd;

		/*
		 * Partial period: days from anniversary to next anniversary The
		 * period uses the year that contains the partial span
		 */
		{
			int			next_anniversary_y = anniversary_y + 1;
			int			next_anniversary_m = anniversary_m;
			int			next_anniversary_d;
			int			next_anniversary_jd;

			if (start_m == 2 && start_d == 29 && !isleap(next_anniversary_y))
				next_anniversary_d = 28;
			else if (start_d > days_in_month_helper(next_anniversary_y, next_anniversary_m))
				next_anniversary_d = days_in_month_helper(next_anniversary_y, next_anniversary_m);
			else
				next_anniversary_d = start_d;

			next_anniversary_jd = date2j(next_anniversary_y, next_anniversary_m, next_anniversary_d);
			partial_period_days = next_anniversary_jd - anniversary_jd;
		}

		if (partial_period_days <= 0)
			partial_period_days = 1;

		result = (double) full_years + (double) remaining_days / (double) partial_period_days;
	}

	if (negated)
		result = -result;

	return make_numeric_result(bankers_round(result));
}

/*
 * datediff_internal - core calculation dispatcher
 *
 * Takes year, month, day for both dates and computes the difference
 * based on the specified datepart.
 */
static Datum
datediff_internal(const char *datepart_str,
				  int start_y, int start_m, int start_d,
				  int end_y, int end_m, int end_d)
{
	DatepartType datepart = parse_datepart(datepart_str);

	/* Validate datepart */
	if (datepart == DATEPART_INVALID)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("Invalid datepart: '%s'", datepart_str),
				 errhint("Valid options: year, quarter, month, week, day")));
	}

	/* Dispatch to appropriate calculator */
	switch (datepart)
	{
		case DATEPART_DAY:
			return compute_diff_day(start_y, start_m, start_d,
									end_y, end_m, end_d);
		case DATEPART_WEEK:
			return compute_diff_week(start_y, start_m, start_d,
									 end_y, end_m, end_d);
		case DATEPART_MONTH:
			return compute_diff_month(start_y, start_m, start_d,
									  end_y, end_m, end_d);
		case DATEPART_QUARTER:
			return compute_diff_quarter(start_y, start_m, start_d,
										end_y, end_m, end_d);
		case DATEPART_YEAR:
			return compute_diff_year(start_y, start_m, start_d,
									 end_y, end_m, end_d);
		default:
			/* Should not reach here */
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("Unexpected datepart type")));
			return (Datum) 0;	/* Keep compiler happy */
	}
}

/*-------------------------------------------------------------------------
 * Public Entry Points
 *-------------------------------------------------------------------------
 */

PG_FUNCTION_INFO_V1(datediff_date);

/*
 * datediff_date - DATE version of datediff
 */
Datum
datediff_date(PG_FUNCTION_ARGS)
{
	text	   *datepart_text = PG_GETARG_TEXT_PP(0);
	DateADT		start_date = PG_GETARG_DATEADT(1);
	DateADT		end_date = PG_GETARG_DATEADT(2);
	char	   *datepart_str;
	int			start_y, start_m, start_d;
	int			end_y, end_m, end_d;

	datepart_str = text_to_cstring(datepart_text);

	/* Convert dates to year/month/day using j2date */
	j2date(start_date + POSTGRES_EPOCH_JDATE, &start_y, &start_m, &start_d);
	j2date(end_date + POSTGRES_EPOCH_JDATE, &end_y, &end_m, &end_d);

	return datediff_internal(datepart_str,
							 start_y, start_m, start_d,
							 end_y, end_m, end_d);
}

PG_FUNCTION_INFO_V1(datediff_timestamp);

/*
 * datediff_timestamp - TIMESTAMP version of datediff
 *
 * Ignores time component, uses only date portion.
 */
Datum
datediff_timestamp(PG_FUNCTION_ARGS)
{
	text	   *datepart_text = PG_GETARG_TEXT_PP(0);
	Timestamp	start_ts = PG_GETARG_TIMESTAMP(1);
	Timestamp	end_ts = PG_GETARG_TIMESTAMP(2);
	char	   *datepart_str;
	struct pg_tm start_tm, end_tm;
	fsec_t		fsec;

	datepart_str = text_to_cstring(datepart_text);

	/* Decompose timestamps to get date components */
	if (timestamp2tm(start_ts, NULL, &start_tm, &fsec, NULL, NULL) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	if (timestamp2tm(end_ts, NULL, &end_tm, &fsec, NULL, NULL) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	return datediff_internal(datepart_str,
							 start_tm.tm_year, start_tm.tm_mon, start_tm.tm_mday,
							 end_tm.tm_year, end_tm.tm_mon, end_tm.tm_mday);
}

PG_FUNCTION_INFO_V1(datediff_timestamptz);

/*
 * datediff_timestamptz - TIMESTAMPTZ version of datediff
 *
 * Converts to local time then uses date portion.
 */
Datum
datediff_timestamptz(PG_FUNCTION_ARGS)
{
	text	   *datepart_text = PG_GETARG_TEXT_PP(0);
	TimestampTz start_tstz = PG_GETARG_TIMESTAMPTZ(1);
	TimestampTz end_tstz = PG_GETARG_TIMESTAMPTZ(2);
	char	   *datepart_str;
	struct pg_tm start_tm, end_tm;
	fsec_t		fsec;
	int			tz;

	datepart_str = text_to_cstring(datepart_text);

	/* Decompose timestamps with timezone to get date components */
	if (timestamp2tm(start_tstz, &tz, &start_tm, &fsec, NULL, NULL) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	if (timestamp2tm(end_tstz, &tz, &end_tm, &fsec, NULL, NULL) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	return datediff_internal(datepart_str,
							 start_tm.tm_year, start_tm.tm_mon, start_tm.tm_mday,
							 end_tm.tm_year, end_tm.tm_mon, end_tm.tm_mday);
}


/* $PostgreSQL: pgsql/src/interfaces/ecpg/pgtypeslib/interval.c,v 1.36 2006/10/04 00:30:11 momjian Exp $ */

#include "postgres_fe.h"
#include <time.h>
#include <math.h>

#ifdef __FAST_MATH__
#error -ffast-math is known to break this code
#endif

#include "extern.h"
#include "dt.h"
#include "pgtypes_error.h"
#include "pgtypes_interval.h"

/* TrimTrailingZeros()
 * ... resulting from printing numbers with full precision.
 */
static void
TrimTrailingZeros(char *str)
{
	int			len = strlen(str);

	/* chop off trailing zeros... but leave at least 2 fractional digits */
	while (*(str + len - 1) == '0' && *(str + len - 3) != '.')
	{
		len--;
		*(str + len) = '\0';
	}
}

/* DecodeTime()
 * Decode time string which includes delimiters.
 * Only check the lower limit on hours, since this same code
 *	can be used to represent time spans.
 */
static int
DecodeTime(char *str, int fmask, int *tmask, struct tm * tm, fsec_t *fsec)
{
	char	   *cp;

	*tmask = DTK_TIME_M;

	tm->tm_hour = strtol(str, &cp, 10);
	if (*cp != ':')
		return -1;
	str = cp + 1;
	tm->tm_min = strtol(str, &cp, 10);
	if (*cp == '\0')
	{
		tm->tm_sec = 0;
		*fsec = 0;
	}
	else if (*cp != ':')
		return -1;
	else
	{
		str = cp + 1;
		tm->tm_sec = strtol(str, &cp, 10);
		if (*cp == '\0')
			*fsec = 0;
		else if (*cp == '.')
		{
#ifdef HAVE_INT64_TIMESTAMP
			char		fstr[MAXDATELEN + 1];

			/*
			 * OK, we have at most six digits to work with. Let's construct a
			 * string and then do the conversion to an integer.
			 */
			strncpy(fstr, (cp + 1), 7);
			strcpy(fstr + strlen(fstr), "000000");
			*(fstr + 6) = '\0';
			*fsec = strtol(fstr, &cp, 10);
#else
			str = cp;
			*fsec = strtod(str, &cp);
#endif
			if (*cp != '\0')
				return -1;
		}
		else
			return -1;
	}

	/* do a sanity check */
#ifdef HAVE_INT64_TIMESTAMP
	if (tm->tm_hour < 0 || tm->tm_min < 0 || tm->tm_min > 59 ||
		tm->tm_sec < 0 || tm->tm_sec > 59 || *fsec >= USECS_PER_SEC)
		return -1;
#else
	if (tm->tm_hour < 0 || tm->tm_min < 0 || tm->tm_min > 59 ||
		tm->tm_sec < 0 || tm->tm_sec > 59 || *fsec >= 1)
		return -1;
#endif

	return 0;
}	/* DecodeTime() */

/* DecodeInterval()
 * Interpret previously parsed fields for general time interval.
 * Return 0 if decoded and -1 if problems.
 *
 * Allow "date" field DTK_DATE since this could be just
 *	an unsigned floating point number. - thomas 1997-11-16
 *
 * Allow ISO-style time span, with implicit units on number of days
 *	preceding an hh:mm:ss field. - thomas 1998-04-30
 */
int
DecodeInterval(char **field, int *ftype, int nf, int *dtype, struct tm * tm, fsec_t *fsec)
{
	int			is_before = FALSE;

	char	   *cp;
	int			fmask = 0,
				tmask,
				type;
	int			i;
	int			val;
	double		fval;

	*dtype = DTK_DELTA;

	type = IGNORE_DTF;
	tm->tm_year = 0;
	tm->tm_mon = 0;
	tm->tm_mday = 0;
	tm->tm_hour = 0;
	tm->tm_min = 0;
	tm->tm_sec = 0;
	*fsec = 0;

	/* read through list backwards to pick up units before values */
	for (i = nf - 1; i >= 0; i--)
	{
		switch (ftype[i])
		{
			case DTK_TIME:
				if (DecodeTime(field[i], fmask, &tmask, tm, fsec) != 0)
					return -1;
				type = DTK_DAY;
				break;

			case DTK_TZ:

				/*
				 * Timezone is a token with a leading sign character and
				 * otherwise the same as a non-signed time field
				 */

				/*
				 * A single signed number ends up here, but will be rejected
				 * by DecodeTime(). So, work this out to drop through to
				 * DTK_NUMBER, which *can* tolerate this.
				 */
				cp = field[i] + 1;
				while (*cp != '\0' && *cp != ':' && *cp != '.')
					cp++;
				if (*cp == ':' && DecodeTime((field[i] + 1), fmask, &tmask, tm, fsec) == 0)
				{
					if (*field[i] == '-')
					{
						/* flip the sign on all fields */
						tm->tm_hour = -tm->tm_hour;
						tm->tm_min = -tm->tm_min;
						tm->tm_sec = -tm->tm_sec;
						*fsec = -(*fsec);
					}

					/*
					 * Set the next type to be a day, if units are not
					 * specified. This handles the case of '1 +02:03' since we
					 * are reading right to left.
					 */
					type = DTK_DAY;
					tmask = DTK_M(TZ);
					break;
				}
				else if (type == IGNORE_DTF)
				{
					if (*cp == '.')
					{
						/*
						 * Got a decimal point? Then assume some sort of
						 * seconds specification
						 */
						type = DTK_SECOND;
					}
					else if (*cp == '\0')
					{
						/*
						 * Only a signed integer? Then must assume a
						 * timezone-like usage
						 */
						type = DTK_HOUR;
					}
				}
				/* DROP THROUGH */

			case DTK_DATE:
			case DTK_NUMBER:
				val = strtol(field[i], &cp, 10);

				if (type == IGNORE_DTF)
					type = DTK_SECOND;

				if (*cp == '.')
				{
					fval = strtod(cp, &cp);
					if (*cp != '\0')
						return -1;

					if (val < 0)
						fval = -fval;
				}
				else if (*cp == '\0')
					fval = 0;
				else
					return -1;

				tmask = 0;		/* DTK_M(type); */

				switch (type)
				{
					case DTK_MICROSEC:
#ifdef HAVE_INT64_TIMESTAMP
						*fsec += val + fval;
#else
						*fsec += (val + fval) * 1e-6;
#endif
						break;

					case DTK_MILLISEC:
#ifdef HAVE_INT64_TIMESTAMP
						*fsec += (val + fval) * 1000;
#else
						*fsec += (val + fval) * 1e-3;
#endif
						break;

					case DTK_SECOND:
						tm->tm_sec += val;
#ifdef HAVE_INT64_TIMESTAMP
						*fsec += fval * 1000000;
#else
						*fsec += fval;
#endif
						tmask = DTK_M(SECOND);
						break;

					case DTK_MINUTE:
						tm->tm_min += val;
						if (fval != 0)
						{
							int			sec;

							fval *= SECS_PER_MINUTE;
							sec = fval;
							tm->tm_sec += sec;
#ifdef HAVE_INT64_TIMESTAMP
							*fsec += ((fval - sec) * 1000000);
#else
							*fsec += fval - sec;
#endif
						}
						tmask = DTK_M(MINUTE);
						break;

					case DTK_HOUR:
						tm->tm_hour += val;
						if (fval != 0)
						{
							int			sec;

							fval *= SECS_PER_HOUR;
							sec = fval;
							tm->tm_sec += sec;
#ifdef HAVE_INT64_TIMESTAMP
							*fsec += (fval - sec) * 1000000;
#else
							*fsec += fval - sec;
#endif
						}
						tmask = DTK_M(HOUR);
						break;

					case DTK_DAY:
						tm->tm_mday += val;
						if (fval != 0)
						{
							int			sec;

							fval *= SECS_PER_DAY;
							sec = fval;
							tm->tm_sec += sec;
#ifdef HAVE_INT64_TIMESTAMP
							*fsec += (fval - sec) * 1000000;
#else
							*fsec += fval - sec;
#endif
						}
						tmask = (fmask & DTK_M(DAY)) ? 0 : DTK_M(DAY);
						break;

					case DTK_WEEK:
						tm->tm_mday += val * 7;
						if (fval != 0)
						{
							int			extra_days;

							fval *= 7;
							extra_days = (int32) fval;
							tm->tm_mday += extra_days;
							fval -= extra_days;
							if (fval != 0)
							{
								int			sec;

								fval *= SECS_PER_DAY;
								sec = fval;
								tm->tm_sec += sec;
#ifdef HAVE_INT64_TIMESTAMP
								*fsec += (fval - sec) * 1000000;
#else
								*fsec += fval - sec;
#endif
							}
						}
						tmask = (fmask & DTK_M(DAY)) ? 0 : DTK_M(DAY);
						break;

					case DTK_MONTH:
						tm->tm_mon += val;
						if (fval != 0)
						{
							int			day;

							fval *= DAYS_PER_MONTH;
							day = fval;
							tm->tm_mday += day;
							fval -= day;
							if (fval != 0)
							{
								int			sec;

								fval *= SECS_PER_DAY;
								sec = fval;
								tm->tm_sec += sec;
#ifdef HAVE_INT64_TIMESTAMP
								*fsec += (fval - sec) * 1000000;
#else
								*fsec += fval - sec;
#endif
							}
						}
						tmask = DTK_M(MONTH);
						break;

					case DTK_YEAR:
						tm->tm_year += val;
						if (fval != 0)
							tm->tm_mon += fval * MONTHS_PER_YEAR;
						tmask = (fmask & DTK_M(YEAR)) ? 0 : DTK_M(YEAR);
						break;

					case DTK_DECADE:
						tm->tm_year += val * 10;
						if (fval != 0)
							tm->tm_mon += fval * MONTHS_PER_YEAR * 10;
						tmask = (fmask & DTK_M(YEAR)) ? 0 : DTK_M(YEAR);
						break;

					case DTK_CENTURY:
						tm->tm_year += val * 100;
						if (fval != 0)
							tm->tm_mon += fval * MONTHS_PER_YEAR * 100;
						tmask = (fmask & DTK_M(YEAR)) ? 0 : DTK_M(YEAR);
						break;

					case DTK_MILLENNIUM:
						tm->tm_year += val * 1000;
						if (fval != 0)
							tm->tm_mon += fval * MONTHS_PER_YEAR * 1000;
						tmask = (fmask & DTK_M(YEAR)) ? 0 : DTK_M(YEAR);
						break;

					default:
						return -1;
				}
				break;

			case DTK_STRING:
			case DTK_SPECIAL:
				type = DecodeUnits(i, field[i], &val);
				if (type == IGNORE_DTF)
					continue;

				tmask = 0;		/* DTK_M(type); */
				switch (type)
				{
					case UNITS:
						type = val;
						break;

					case AGO:
						is_before = TRUE;
						type = val;
						break;

					case RESERV:
						tmask = (DTK_DATE_M || DTK_TIME_M);
						*dtype = val;
						break;

					default:
						return -1;
				}
				break;

			default:
				return -1;
		}

		if (tmask & fmask)
			return -1;
		fmask |= tmask;
	}

	if (*fsec != 0)
	{
		int			sec;

#ifdef HAVE_INT64_TIMESTAMP
		sec = *fsec / USECS_PER_SEC;
		*fsec -= sec * USECS_PER_SEC;
#else
		TMODULO(*fsec, sec, 1.0);
#endif
		tm->tm_sec += sec;
	}

	if (is_before)
	{
		*fsec = -(*fsec);
		tm->tm_sec = -(tm->tm_sec);
		tm->tm_min = -(tm->tm_min);
		tm->tm_hour = -(tm->tm_hour);
		tm->tm_mday = -(tm->tm_mday);
		tm->tm_mon = -(tm->tm_mon);
		tm->tm_year = -(tm->tm_year);
	}

	/* ensure that at least one time field has been found */
	return (fmask != 0) ? 0 : -1;
}	/* DecodeInterval() */

/* EncodeInterval()
 * Interpret time structure as a delta time and convert to string.
 *
 * Support "traditional Postgres" and ISO-8601 styles.
 * Actually, afaik ISO does not address time interval formatting,
 *	but this looks similar to the spec for absolute date/time.
 * - thomas 1998-04-30
 */
int
EncodeInterval(struct tm * tm, fsec_t fsec, int style, char *str)
{
	int			is_before = FALSE;
	int			is_nonzero = FALSE;
	char	   *cp = str;

	/*
	 * The sign of year and month are guaranteed to match, since they are
	 * stored internally as "month". But we'll need to check for is_before and
	 * is_nonzero when determining the signs of hour/minute/seconds fields.
	 */
	switch (style)
	{
			/* compatible with ISO date formats */
		case USE_ISO_DATES:
			if (tm->tm_year != 0)
			{
				sprintf(cp, "%d year%s",
						tm->tm_year, (tm->tm_year != 1) ? "s" : "");
				cp += strlen(cp);
				is_before = (tm->tm_year < 0);
				is_nonzero = TRUE;
			}

			if (tm->tm_mon != 0)
			{
				sprintf(cp, "%s%s%d mon%s", is_nonzero ? " " : "",
						(is_before && tm->tm_mon > 0) ? "+" : "",
						tm->tm_mon, (tm->tm_mon != 1) ? "s" : "");
				cp += strlen(cp);
				is_before = (tm->tm_mon < 0);
				is_nonzero = TRUE;
			}

			if (tm->tm_mday != 0)
			{
				sprintf(cp, "%s%s%d day%s", is_nonzero ? " " : "",
						(is_before && tm->tm_mday > 0) ? "+" : "",
						tm->tm_mday, (tm->tm_mday != 1) ? "s" : "");
				cp += strlen(cp);
				is_before = (tm->tm_mday < 0);
				is_nonzero = TRUE;
			}
			if (!is_nonzero || tm->tm_hour != 0 || tm->tm_min != 0 ||
				tm->tm_sec != 0 || fsec != 0)
			{
				int			minus = tm->tm_hour < 0 || tm->tm_min < 0 ||
				tm->tm_sec < 0 || fsec < 0;

				sprintf(cp, "%s%s%02d:%02d", (is_nonzero ? " " : ""),
						(minus ? "-" : (is_before ? "+" : "")),
						abs(tm->tm_hour), abs(tm->tm_min));
				cp += strlen(cp);
				/* Mark as "non-zero" since the fields are now filled in */
				is_nonzero = TRUE;

				/* fractional seconds? */
				if (fsec != 0)
				{
#ifdef HAVE_INT64_TIMESTAMP
					sprintf(cp, ":%02d", abs(tm->tm_sec));
					cp += strlen(cp);
					sprintf(cp, ".%06d", Abs(fsec));
#else
					fsec += tm->tm_sec;
					sprintf(cp, ":%012.9f", fabs(fsec));
#endif
					TrimTrailingZeros(cp);
					cp += strlen(cp);
					is_nonzero = TRUE;
				}
				/* otherwise, integer seconds only? */
				else if (tm->tm_sec != 0)
				{
					sprintf(cp, ":%02d", abs(tm->tm_sec));
					cp += strlen(cp);
					is_nonzero = TRUE;
				}
			}
			break;

		case USE_POSTGRES_DATES:
		default:
			strcpy(cp, "@ ");
			cp += strlen(cp);

			if (tm->tm_year != 0)
			{
				int			year = tm->tm_year;

				if (tm->tm_year < 0)
					year = -year;

				sprintf(cp, "%d year%s", year,
						(year != 1) ? "s" : "");
				cp += strlen(cp);
				is_before = (tm->tm_year < 0);
				is_nonzero = TRUE;
			}

			if (tm->tm_mon != 0)
			{
				int			mon = tm->tm_mon;

				if (is_before || (!is_nonzero && tm->tm_mon < 0))
					mon = -mon;

				sprintf(cp, "%s%d mon%s", is_nonzero ? " " : "", mon,
						(mon != 1) ? "s" : "");
				cp += strlen(cp);
				if (!is_nonzero)
					is_before = (tm->tm_mon < 0);
				is_nonzero = TRUE;
			}

			if (tm->tm_mday != 0)
			{
				int			day = tm->tm_mday;

				if (is_before || (!is_nonzero && tm->tm_mday < 0))
					day = -day;

				sprintf(cp, "%s%d day%s", is_nonzero ? " " : "", day,
						(day != 1) ? "s" : "");
				cp += strlen(cp);
				if (!is_nonzero)
					is_before = (tm->tm_mday < 0);
				is_nonzero = TRUE;
			}
			if (tm->tm_hour != 0)
			{
				int			hour = tm->tm_hour;

				if (is_before || (!is_nonzero && tm->tm_hour < 0))
					hour = -hour;

				sprintf(cp, "%s%d hour%s", is_nonzero ? " " : "", hour,
						(hour != 1) ? "s" : "");
				cp += strlen(cp);
				if (!is_nonzero)
					is_before = (tm->tm_hour < 0);
				is_nonzero = TRUE;
			}

			if (tm->tm_min != 0)
			{
				int			min = tm->tm_min;

				if (is_before || (!is_nonzero && tm->tm_min < 0))
					min = -min;

				sprintf(cp, "%s%d min%s", is_nonzero ? " " : "", min,
						(min != 1) ? "s" : "");
				cp += strlen(cp);
				if (!is_nonzero)
					is_before = (tm->tm_min < 0);
				is_nonzero = TRUE;
			}

			/* fractional seconds? */
			if (fsec != 0)
			{
#ifdef HAVE_INT64_TIMESTAMP
				if (is_before || (!is_nonzero && tm->tm_sec < 0))
					tm->tm_sec = -tm->tm_sec;
				sprintf(cp, "%s%d.%02d secs", is_nonzero ? " " : "",
						tm->tm_sec, ((int) fsec) / 10000);
				cp += strlen(cp);
				if (!is_nonzero)
					is_before = (fsec < 0);
#else
				fsec_t		sec;

				fsec += tm->tm_sec;
				sec = fsec;
				if (is_before || (!is_nonzero && fsec < 0))
					sec = -sec;

				sprintf(cp, "%s%.2f secs", is_nonzero ? " " : "", sec);
				cp += strlen(cp);
				if (!is_nonzero)
					is_before = (fsec < 0);
#endif
				is_nonzero = TRUE;

				/* otherwise, integer seconds only? */
			}
			else if (tm->tm_sec != 0)
			{
				int			sec = tm->tm_sec;

				if (is_before || (!is_nonzero && tm->tm_sec < 0))
					sec = -sec;

				sprintf(cp, "%s%d sec%s", is_nonzero ? " " : "", sec,
						(sec != 1) ? "s" : "");
				cp += strlen(cp);
				if (!is_nonzero)
					is_before = (tm->tm_sec < 0);
				is_nonzero = TRUE;
			}
			break;
	}

	/* identically zero? then put in a unitless zero... */
	if (!is_nonzero)
	{
		strcat(cp, "0");
		cp += strlen(cp);
	}

	if (is_before && (style != USE_ISO_DATES))
	{
		strcat(cp, " ago");
		cp += strlen(cp);
	}

	return 0;
}	/* EncodeInterval() */

/* interval2tm()
 * Convert a interval data type to a tm structure.
 */
static int
interval2tm(interval span, struct tm * tm, fsec_t *fsec)
{
#ifdef HAVE_INT64_TIMESTAMP
	int64		time;
#else
	double		time;
#endif

	if (span.month != 0)
	{
		tm->tm_year = span.month / MONTHS_PER_YEAR;
		tm->tm_mon = span.month % MONTHS_PER_YEAR;

	}
	else
	{
		tm->tm_year = 0;
		tm->tm_mon = 0;
	}

	time = span.time;

#ifdef HAVE_INT64_TIMESTAMP
	tm->tm_mday = time / USECS_PER_DAY;
	time -= tm->tm_mday * USECS_PER_DAY;
	tm->tm_hour = time / USECS_PER_HOUR;
	time -= tm->tm_hour * USECS_PER_HOUR;
	tm->tm_min = time / USECS_PER_MINUTE;
	time -= tm->tm_min * USECS_PER_MINUTE;
	tm->tm_sec = time / USECS_PER_SEC;
	*fsec = time - (tm->tm_sec * USECS_PER_SEC);
#else
recalc:
	TMODULO(time, tm->tm_mday, (double) SECS_PER_DAY);
	TMODULO(time, tm->tm_hour, (double) SECS_PER_HOUR);
	TMODULO(time, tm->tm_min, (double) SECS_PER_MINUTE);
	TMODULO(time, tm->tm_sec, 1.0);
	time = TSROUND(time);
	/* roundoff may need to propagate to higher-order fields */
	if (time >= 1.0)
	{
		time = ceil(span.time);
		goto recalc;
	}
	*fsec = time;
#endif

	return 0;
}	/* interval2tm() */

static int
tm2interval(struct tm * tm, fsec_t fsec, interval * span)
{
	span->month = tm->tm_year * MONTHS_PER_YEAR + tm->tm_mon;
#ifdef HAVE_INT64_TIMESTAMP
	span->time = (((((((tm->tm_mday * INT64CONST(24)) +
					   tm->tm_hour) * INT64CONST(60)) +
					 tm->tm_min) * INT64CONST(60)) +
				   tm->tm_sec) * USECS_PER_SEC) + fsec;
#else
	span->time = (((((tm->tm_mday * (double) HOURS_PER_DAY) +
					 tm->tm_hour) * (double) MINS_PER_HOUR) +
				   tm->tm_min) * (double) SECS_PER_MINUTE) +
		tm->tm_sec + fsec;
#endif

	return 0;
}	/* tm2interval() */

interval *
PGTYPESinterval_new(void)
{
	interval   *result;

	result = (interval *) pgtypes_alloc(sizeof(interval));
	/* result can be NULL if we run out of memory */
	return result;
}

void
PGTYPESinterval_free(interval * intvl)
{
	free(intvl);
}

interval *
PGTYPESinterval_from_asc(char *str, char **endptr)
{
	interval   *result = NULL;
	fsec_t		fsec;
	struct tm	tt,
			   *tm = &tt;
	int			dtype;
	int			nf;
	char	   *field[MAXDATEFIELDS];
	int			ftype[MAXDATEFIELDS];
	char		lowstr[MAXDATELEN + MAXDATEFIELDS];
	char	   *realptr;
	char	  **ptr = (endptr != NULL) ? endptr : &realptr;

	tm->tm_year = 0;
	tm->tm_mon = 0;
	tm->tm_mday = 0;
	tm->tm_hour = 0;
	tm->tm_min = 0;
	tm->tm_sec = 0;
	fsec = 0;

	if (strlen(str) >= sizeof(lowstr))
	{
		errno = PGTYPES_INTVL_BAD_INTERVAL;
		return NULL;
	}

	if (ParseDateTime(str, lowstr, field, ftype, MAXDATEFIELDS, &nf, ptr) != 0 ||
		DecodeInterval(field, ftype, nf, &dtype, tm, &fsec) != 0)
	{
		errno = PGTYPES_INTVL_BAD_INTERVAL;
		return NULL;
	}

	result = (interval *) pgtypes_alloc(sizeof(interval));
	if (!result)
		return NULL;

	if (dtype != DTK_DELTA)
	{
		errno = PGTYPES_INTVL_BAD_INTERVAL;
		free(result);
		return NULL;
	}

	if (tm2interval(tm, fsec, result) != 0)
	{
		errno = PGTYPES_INTVL_BAD_INTERVAL;
		free(result);
		return NULL;
	}

	errno = 0;
	return result;
}

char *
PGTYPESinterval_to_asc(interval * span)
{
	struct tm	tt,
			   *tm = &tt;
	fsec_t		fsec;
	char		buf[MAXDATELEN + 1];
	int			DateStyle = 0;

	if (interval2tm(*span, tm, &fsec) != 0)
	{
		errno = PGTYPES_INTVL_BAD_INTERVAL;
		return NULL;
	}

	if (EncodeInterval(tm, fsec, DateStyle, buf) != 0)
	{
		errno = PGTYPES_INTVL_BAD_INTERVAL;
		return NULL;
	}

	return pgtypes_strdup(buf);
}

int
PGTYPESinterval_copy(interval * intvlsrc, interval * intvldest)
{
	intvldest->time = intvlsrc->time;
	intvldest->month = intvlsrc->month;

	return 0;
}

#ifndef DATETIME_FUNCTIONS_H
#define DATETIME_FUNCTIONS_H

TimeADT    *hhmm_in(char *str);
char	   *hhmm_out(TimeADT *time);
TimeADT    *hhmm(TimeADT *time);
TimeADT    *time_difference(TimeADT *time1, TimeADT *time2);
int4		time_hours(TimeADT *time);
int4		time_minutes(TimeADT *time);
int4		time_seconds(TimeADT *time);
int4		as_minutes(TimeADT *time);
int4		as_seconds(TimeADT *time);
int4		date_day(DateADT val);
int4		date_month(DateADT val);
int4		date_year(DateADT val);
TimeADT    *currenttime(void);
DateADT		currentdate(void);

#endif

/*
 * Local Variables:
 *	tab-width: 4
 *	c-indent-level: 4
 *	c-basic-offset: 4
 * End:
 */

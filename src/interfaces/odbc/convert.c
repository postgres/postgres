
/* Module:         convert.c
 *
 * Description:	   This module contains routines related to 
 *                 converting parameters and columns into requested data types.
 *                 Parameters are converted from their SQL_C data types into
 *                 the appropriate postgres type.  Columns are converted from
 *                 their postgres type (SQL type) into the appropriate SQL_C 
 *                 data type.
 *
 * Classes:        n/a
 *
 * API functions:  none
 *
 * Comments:       See "notice.txt" for copyright and license information.
 *
 */

#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <sql.h>
#include <sqlext.h>
#include <time.h>
#include <math.h>
#include "convert.h"
#include "statement.h"
#include "bind.h"
#include "pgtypes.h"

/********		A Guide for date/time/timestamp conversions    **************

			field_type		fCType				Output
			----------		------				----------
			PG_TYPE_DATE	SQL_C_DEFAULT		SQL_C_DATE
			PG_TYPE_DATE	SQL_C_DATE			SQL_C_DATE
			PG_TYPE_DATE	SQL_C_TIMESTAMP		SQL_C_TIMESTAMP		(time = 0 (midnight))
			PG_TYPE_TIME	SQL_C_DEFAULT		SQL_C_TIME
			PG_TYPE_TIME	SQL_C_TIME			SQL_C_TIME
			PG_TYPE_TIME	SQL_C_TIMESTAMP		SQL_C_TIMESTAMP		(date = current date)
			PG_TYPE_ABSTIME	SQL_C_DEFAULT		SQL_C_TIMESTAMP
			PG_TYPE_ABSTIME	SQL_C_DATE			SQL_C_DATE			(time is truncated)
			PG_TYPE_ABSTIME	SQL_C_TIME			SQL_C_TIME			(date is truncated)
			PG_TYPE_ABSTIME	SQL_C_TIMESTAMP		SQL_C_TIMESTAMP		
******************************************************************************/


/*	This is called by SQLFetch() */
int
copy_and_convert_field_bindinfo(Int4 field_type, void *value, BindInfoClass *bic)
{
	return copy_and_convert_field(field_type, value, (Int2)bic->returntype, (PTR)bic->buffer,
                                (SDWORD)bic->buflen, (SDWORD *)bic->used);
}

/*	This is called by SQLGetData() */
int
copy_and_convert_field(Int4 field_type, void *value, Int2 fCType, PTR rgbValue, SDWORD cbValueMax, SDWORD *pcbValue)
{
Int4 len = 0, nf;
char day[4], mon[4], tz[4];
SIMPLE_TIME st;
time_t t = time(NULL);
struct tm *tim;
int bool;


	memset(&st, 0, sizeof(SIMPLE_TIME));

	/*	Initialize current date */
	tim = localtime(&t);
	st.m = tim->tm_mon + 1;
	st.d = tim->tm_mday;
	st.y = tim->tm_year + 1900;

	bool = 0;

	mylog("copy_and_convert: field_type = %d, fctype = %d, value = '%s', cbValueMax=%d\n", field_type, fCType, value, cbValueMax);
    if(value) {

		/********************************************************************
			First convert any specific postgres types into more
			useable data.

			NOTE: Conversions from PG char/varchar of a date/time/timestamp 
			value to SQL_C_DATE,SQL_C_TIME, SQL_C_TIMESTAMP not supported 
		*********************************************************************/
		switch(field_type) {
		/*  $$$ need to add parsing for date/time/timestamp strings in PG_TYPE_CHAR,VARCHAR $$$ */
		case PG_TYPE_DATE:
			sscanf(value, "%2d-%2d-%4d", &st.m, &st.d, &st.y);
			break;

		case PG_TYPE_TIME:
			sscanf(value, "%2d:%2d:%2d", &st.hh, &st.mm, &st.ss);
			break;

		case PG_TYPE_ABSTIME:
		case PG_TYPE_DATETIME:
			if (strnicmp(value, "invalid", 7) != 0) {
				nf = sscanf(value, "%3s %3s %2d %2d:%2d:%2d %4d %3s", &day, &mon, &st.d, &st.hh, &st.mm, &st.ss, &st.y, &tz);

				if (nf == 7 || nf == 8) {
					/* convert month name to month number */
					st.m = monthToNumber(mon);
				}
			} else {	/* The timestamp is invalid so set something conspicuous, like the epoch */
				t = 0;
				tim = localtime(&t);
				st.m = tim->tm_mon + 1;
				st.d = tim->tm_mday;
				st.y = tim->tm_year + 1900;
				st.hh = tim->tm_hour;
				st.mm = tim->tm_min;
				st.ss = tim->tm_sec;
			}
			break;

		case PG_TYPE_BOOL: {		/* change T/F to 1/0 */
			char *s = (char *) value;
			if (s[0] == 'T' || s[0] == 't' || s[0] == '1') 
				bool = 1;
			else
				bool = 0;
			}
			break;

		/* This is for internal use by SQLStatistics() */
		case PG_TYPE_INT28: {
			// this is an array of eight integers
			short *short_array = (short *)rgbValue;

			len = 16;

			sscanf(value, "%hd %hd %hd %hd %hd %hd %hd %hd",
				&short_array[0],
				&short_array[1],
				&short_array[2],
				&short_array[3],
				&short_array[4],
				&short_array[5],
				&short_array[6],
				&short_array[7]);

			/*  There is no corresponding fCType for this. */
			if(pcbValue)
				*pcbValue = len;

			return COPY_OK;		/* dont go any further or the data will be trashed */
							}

		}

		/*  Change default into something useable */
		if (fCType == SQL_C_DEFAULT) {
			fCType = pgtype_to_ctype(field_type);

			mylog("copy_and_convert, SQL_C_DEFAULT: fCType = %d\n", fCType);
		}


        if(fCType == SQL_C_CHAR) {

			/*	Special character formatting as required */
			switch(field_type) {
			case PG_TYPE_DATE:
		        len = 11;
				if (cbValueMax > len)
					sprintf((char *)rgbValue, "%.4d-%.2d-%.2d", st.y, st.m, st.d);
				break;

			case PG_TYPE_TIME:
				len = 9;
				if (cbValueMax > len)
					sprintf((char *)rgbValue, "%.2d:%.2d:%.2d", st.hh, st.mm, st.ss);
				break;

			case PG_TYPE_ABSTIME:
			case PG_TYPE_DATETIME:
				len = 19;
				if (cbValueMax > len)
					sprintf((char *) rgbValue, "%.4d-%.2d-%.2d %.2d:%.2d:%.2d", 
						st.y, st.m, st.d, st.hh, st.mm, st.ss);
				break;

			case PG_TYPE_BOOL:
				len = 1;
				if (cbValueMax > len) {
					strcpy((char *) rgbValue, bool ? "1" : "0");
					mylog("PG_TYPE_BOOL: rgbValue = '%s'\n", rgbValue);
				}
				break;

			case PG_TYPE_BYTEA:		// convert binary data to hex strings (i.e, 255 = "FF")
				len = convert_pgbinary_to_char(value, rgbValue, cbValueMax);
				break;

			default:
				/* convert linefeeds to carriage-return/linefeed */
				convert_linefeeds( (char *) value, rgbValue, cbValueMax);
		        len = strlen(rgbValue);

				mylog("    SQL_C_CHAR, default: len = %d, cbValueMax = %d, rgbValue = '%s'\n", len, cbValueMax, rgbValue);
				break;
			}

        } else {

			/*	for SQL_C_CHAR, its probably ok to leave currency symbols in.  But
				to convert to numeric types, it is necessary to get rid of those.
			*/
			if (field_type == PG_TYPE_MONEY)
				convert_money(value);

			switch(fCType) {
			case SQL_C_DATE:
				len = 6;
				if (cbValueMax >= len) {
					DATE_STRUCT *ds = (DATE_STRUCT *) rgbValue;
					ds->year = st.y;
					ds->month = st.m;
					ds->day = st.d;
				}
				break;

			case SQL_C_TIME:
				len = 6;
				if (cbValueMax >= len) {
					TIME_STRUCT *ts = (TIME_STRUCT *) rgbValue;
					ts->hour = st.hh;
					ts->minute = st.mm;
					ts->second = st.ss;
				}
				break;

			case SQL_C_TIMESTAMP:					
				len = 16;
				if (cbValueMax >= len) {
					TIMESTAMP_STRUCT *ts = (TIMESTAMP_STRUCT *) rgbValue;
					ts->year = st.y;
					ts->month = st.m;
					ts->day = st.d;
					ts->hour = st.hh;
					ts->minute = st.mm;
					ts->second = st.ss;
					ts->fraction = 0;
				}
				break;

			case SQL_C_BIT:
				len = 1;
				if (cbValueMax >= len || field_type == PG_TYPE_BOOL) {
					*((UCHAR *)rgbValue) = (UCHAR) bool;
					mylog("SQL_C_BIT: val = %d, cb = %d, rgb=%d\n", atoi(value), cbValueMax, *((UCHAR *)rgbValue));
				}
				break;

			case SQL_C_STINYINT:
			case SQL_C_TINYINT:
				len = 1;
				if (cbValueMax >= len)
					*((SCHAR *) rgbValue) = atoi(value);
				break;

			case SQL_C_UTINYINT:
				len = 1;
				if (cbValueMax >= len)
					*((UCHAR *) rgbValue) = atoi(value);
				break;

			case SQL_C_FLOAT:
                len = 4;
                if(cbValueMax >= len)
                    *((SFLOAT *)rgbValue) = (float) atof(value);
				break;

			case SQL_C_DOUBLE:
                len = 8;
                if(cbValueMax >= len)
                    *((SDOUBLE *)rgbValue) = atof(value);
				break;

			case SQL_C_SSHORT:
			case SQL_C_SHORT:
				len = 2;
                if(cbValueMax >= len)
                    *((SWORD *)rgbValue) = atoi(value);
                break;

			case SQL_C_USHORT:
				len = 2;
                if(cbValueMax >= len)
                    *((UWORD *)rgbValue) = atoi(value);
                break;

			case SQL_C_SLONG:
			case SQL_C_LONG:
                len = 4;
                if(cbValueMax >= len)
                    *((SDWORD *)rgbValue) = atol(value);
                break;

			case SQL_C_ULONG:
                len = 4;
                if(cbValueMax >= len)
                    *((UDWORD *)rgbValue) = atol(value);
                break;

			case SQL_C_BINARY:	
				//	truncate if necessary
				//	convert octal escapes to bytes
				len = convert_from_pgbinary(value, rgbValue, cbValueMax);
				mylog("SQL_C_BINARY: len = %d\n", len);
				break;
				
			default:
				return COPY_UNSUPPORTED_TYPE;
			}
		}
    } else {
        /* handle a null just by returning SQL_NULL_DATA in pcbValue, */
        /* and doing nothing to the buffer.                           */
        if(pcbValue) {
            *pcbValue = SQL_NULL_DATA;
        }
    }

    // store the length of what was copied, if there's a place for it
    // unless it was a NULL (in which case it was already set above)
    if(pcbValue && value)
        *pcbValue = len;

    if(len > cbValueMax) {
		mylog("!!! COPY_RESULT_TRUNCATED !!!\n");

		//	Don't return truncated because an application
		//	(like Access) will try to call GetData again
		//	to retrieve the rest of the data.  Since we
		//	are not currently ready for this, and the result
		//	is an endless loop, we better just silently
		//	truncate the data.
        // return COPY_RESULT_TRUNCATED;

		*pcbValue = cbValueMax -1;
		return COPY_OK;

    } else {

        return COPY_OK;
    }
}

/*	This function inserts parameters into an SQL statements.
	It will also modify a SELECT statement for use with declare/fetch cursors.
	This function no longer does any dynamic memory allocation!
*/
int
copy_statement_with_parameters(StatementClass *stmt)
{
unsigned int opos, npos;
char param_string[128], tmp[256], cbuf[TEXT_FIELD_SIZE+5];
int param_number;
Int2 param_ctype, param_sqltype;
char *old_statement = stmt->statement;
char *new_statement = stmt->stmt_with_params;
SIMPLE_TIME st;
time_t t = time(NULL);
struct tm *tim;
SDWORD FAR *used;
char *buffer, *buf;


	if ( ! old_statement)
		return SQL_ERROR;


	memset(&st, 0, sizeof(SIMPLE_TIME));

	/*	Initialize current date */
	tim = localtime(&t);
	st.m = tim->tm_mon + 1;
	st.d = tim->tm_mday;
	st.y = tim->tm_year + 1900;



	//	For selects, prepend a declare cursor to the statement
	if (stmt->statement_type == STMT_TYPE_SELECT) {
		sprintf(new_statement, "declare C%u cursor for ", stmt);
		npos = strlen(new_statement);
	}
	else {
		new_statement[0] = '0';
		npos = 0;
	}

    param_number = -1;

    for (opos = 0; opos < strlen(old_statement); opos++) {

		//	Squeeze carriage-returns/linfeed pairs to linefeed only
		if (old_statement[opos] == '\r' && opos+1<strlen(old_statement) && old_statement[opos+1] == '\n') {
			continue;
		}

		//	Handle literals (date, time, timestamp)
		else if (old_statement[opos] == '{') {
			char *esc;
			char *begin = &old_statement[opos + 1];
			char *end = strchr(begin, '}');

			if ( ! end)
				continue;

			*end = '\0';

			esc = convert_escape(begin);
			if (esc) {
				memcpy(&new_statement[npos], esc, strlen(esc));
				npos += strlen(esc);
			}

			opos += end - begin + 2;

			*end = '}';

			continue;
		}

		else if (old_statement[opos] != '?') {		// a regular character
			new_statement[npos++] = old_statement[opos];
			continue;
		}

		/****************************************************/
		/*       Its a '?' parameter alright                */
		/****************************************************/

		param_number++;

	    if (param_number >= stmt->parameters_allocated)
			break;

		/*	Assign correct buffers based on data at exec param or not */
		if ( stmt->parameters[param_number].data_at_exec) {
			used = stmt->parameters[param_number].EXEC_used;
			buffer = stmt->parameters[param_number].EXEC_buffer;
		}
		else {
			used = stmt->parameters[param_number].used;
			buffer = stmt->parameters[param_number].buffer;
		}

		/*	Handle NULL parameter data */
		if (used && *used == SQL_NULL_DATA) {
			strcpy(&new_statement[npos], "NULL");
			npos += 4;
			continue;
		}

		/*	If no buffer, and its not null, then what the hell is it? 
			Just leave it alone then.
		*/
		if ( ! buffer) {
			new_statement[npos++] = '?';
			continue;
		}

		param_ctype = stmt->parameters[param_number].CType;
		param_sqltype = stmt->parameters[param_number].SQLType;
		
		mylog("copy_statement_with_params: from(fcType)=%d, to(fSqlType)=%d\n", 
			param_ctype,
			param_sqltype);
		
		// replace DEFAULT with something we can use
		if(param_ctype == SQL_C_DEFAULT)
			param_ctype = sqltype_to_default_ctype(param_sqltype);

		buf = NULL;
		param_string[0] = '\0';
		cbuf[0] = '\0';

		
		/*	Convert input C type to a neutral format */
		switch(param_ctype) {
		case SQL_C_BINARY:
		case SQL_C_CHAR:
			buf = buffer;
			break;

		case SQL_C_DOUBLE:
			sprintf(param_string, "%f", 
				 *((SDOUBLE *) buffer));
			break;

		case SQL_C_FLOAT:
			sprintf(param_string, "%f", 
				 *((SFLOAT *) buffer));
			break;

		case SQL_C_SLONG:
		case SQL_C_LONG:
			sprintf(param_string, "%ld",
				*((SDWORD *) buffer));
			break;

		case SQL_C_SSHORT:
		case SQL_C_SHORT:
			sprintf(param_string, "%d",
				*((SWORD *) buffer));
			break;

		case SQL_C_STINYINT:
		case SQL_C_TINYINT:
			sprintf(param_string, "%d",
				*((SCHAR *) buffer));
			break;

		case SQL_C_ULONG:
			sprintf(param_string, "%lu",
				*((UDWORD *) buffer));
			break;

		case SQL_C_USHORT:
			sprintf(param_string, "%u",
				*((UWORD *) buffer));
			break;

		case SQL_C_UTINYINT:
			sprintf(param_string, "%u",
				*((UCHAR *) buffer));
			break;

		case SQL_C_BIT: {
			int i = *((UCHAR *) buffer);
			
			sprintf(param_string, "'%s'", i ? "t" : "f");
			break;
						}

		case SQL_C_DATE: {
			DATE_STRUCT *ds = (DATE_STRUCT *) buffer;
			st.m = ds->month;
			st.d = ds->day;
			st.y = ds->year;

			break;
						 }

		case SQL_C_TIME: {
			TIME_STRUCT *ts = (TIME_STRUCT *) buffer;
			st.hh = ts->hour;
			st.mm = ts->minute;
			st.ss = ts->second;

			break;
						 }

		case SQL_C_TIMESTAMP: {
			TIMESTAMP_STRUCT *tss = (TIMESTAMP_STRUCT *) buffer;
			st.m = tss->month;
			st.d = tss->day;
			st.y = tss->year;
			st.hh = tss->hour;
			st.mm = tss->minute;
			st.ss = tss->second;

			mylog("m=%d,d=%d,y=%d,hh=%d,mm=%d,ss=%d\n",
				st.m, st.d, st.y, st.hh, st.mm, st.ss);

			break;

							  }
		default:
			// error
			stmt->errormsg = "Unrecognized C_parameter type in copy_statement_with_parameters";
			stmt->errornumber = STMT_NOT_IMPLEMENTED_ERROR;
			new_statement[npos] = '\0';   // just in case
			return SQL_ERROR;
		}

		/*	Now that the input data is in a neutral format, convert it to
			the desired output format (sqltype)
		*/

		switch(param_sqltype) {
		case SQL_CHAR:
		case SQL_VARCHAR:
		case SQL_LONGVARCHAR:

			new_statement[npos++] = '\'';	/*    Open Quote */

			/* it was a SQL_C_CHAR */
			if (buf) {
				convert_returns(buf, &new_statement[npos], used ? *used : SQL_NTS);
				npos += strlen(&new_statement[npos]);
			}

			/* it was a numeric type */
			else if (param_string[0] != '\0') {	
				strcpy(&new_statement[npos], param_string);
				npos += strlen(param_string);
			}

			/* it was date,time,timestamp -- use m,d,y,hh,mm,ss */
			else {
				char *buf = convert_time(&st);
				strcpy(&new_statement[npos], buf);
				npos += strlen(buf);
			}

			new_statement[npos++] = '\'';	/*    Close Quote */

			break;

		case SQL_DATE:
			if (buf && used) {  /* copy char data to time */
				my_strcpy(cbuf, sizeof(cbuf), buf, *used);
				parse_datetime(cbuf, &st);
			}

			sprintf(tmp, "'%.2d-%.2d-%.4d'", st.m, st.d, st.y);

			strcpy(&new_statement[npos], tmp);
			npos += strlen(tmp);
			break;

		case SQL_TIME:
			if (buf && used) {  /* copy char data to time */
				my_strcpy(cbuf, sizeof(cbuf), buf, *used);
				parse_datetime(cbuf, &st);
			}

			sprintf(tmp, "'%.2d:%.2d:%.2d'", st.hh, st.mm, st.ss);

			strcpy(&new_statement[npos], tmp);
			npos += strlen(tmp);
			break;

		case SQL_TIMESTAMP: {
			char *tbuf;

			if (buf && used) {
				my_strcpy(cbuf, sizeof(cbuf), buf, *used);
				parse_datetime(cbuf, &st);
			}

			tbuf = convert_time(&st);

			sprintf(&new_statement[npos], "'%s'", tbuf);
			npos += strlen(tbuf) + 2;

			break;
							}

		case SQL_BINARY:
		case SQL_VARBINARY:
		case SQL_LONGVARBINARY:		/* non-ascii characters should be converted to octal */

			new_statement[npos++] = '\'';	/*    Open Quote */

			mylog("SQL_LONGVARBINARY: about to call convert_to_pgbinary, *used = %d\n", *used);

			npos += convert_to_pgbinary(buf, &new_statement[npos], *used);

			new_statement[npos++] = '\'';	/*    Close Quote */
			
			break;

		default:		/* a numeric type */
			strcpy(&new_statement[npos], param_string);
			npos += strlen(param_string);
			break;

		}

	}	/* end, for */

	// make sure new_statement is always null-terminated
	new_statement[npos] = '\0';

	return SQL_SUCCESS;
}


//	This function returns a pointer to static memory!
char *
convert_escape(char *value)
{
char key[32], val[256];
static char escape[256];
SIMPLE_TIME st;

	sscanf(value, "%[^'] '%[^']'", key, val);

	mylog("convert_escape: key='%s', val='%s'\n", key, val);

	if ( ! strncmp(key, "d", 1)) {
		sscanf(val, "%4d-%2d-%2d", &st.y, &st.m, &st.d);
		sprintf(escape, "'%.2d-%.2d-%.4d'", st.m, st.d, st.y);
		
	} else if (! strncmp(key, "t", 1)) {
		sprintf(escape, "'%s'", val);

	} else if (! strncmp(key, "ts", 2)) {
		sscanf(val, "%4d-%2d-%2d %2d:%2d:%2d", &st.y, &st.m, &st.d, &st.hh, &st.mm, &st.ss);
		strcpy(escape, convert_time(&st));
	}
	else {
		return NULL;
	}

	return escape;

}


int
monthToNumber(char *mon)
{
int m = 0;

	if ( ! stricmp(mon, "Jan"))
		m = 1;
	else if ( ! stricmp(mon, "Feb"))
		m = 2;
	else if ( ! stricmp(mon, "Mar"))
		m = 3;
	else if ( ! stricmp(mon, "Apr"))
		m = 4;
	else if ( ! stricmp(mon, "May"))
		m = 5;
	else if ( ! stricmp(mon, "Jun"))
		m = 6;
	else if ( ! stricmp(mon, "Jul"))
		m = 7;
	else if ( ! stricmp(mon, "Aug"))
		m = 8;
	else if ( ! stricmp(mon, "Sep"))
		m = 9;
	else if ( ! stricmp(mon, "Oct"))
		m = 10;
	else if ( ! stricmp(mon, "Nov"))
		m = 11;
	else if ( ! stricmp(mon, "Dec"))
		m = 12;

	return m;
}


char *
convert_money(char *s)
{
size_t i = 0, out = 0;

	for (i = 0; i < strlen(s); i++) {
		if (s[i] == '$' || s[i] == ',' || s[i] == ')')
			; // skip these characters
		else if (s[i] == '(')
			s[out++] = '-';
		else
			s[out++] = s[i];
	}
	s[out] = '\0';
	return s;
}

/*	Convert a discrete time into a localized string */
char *
convert_time(SIMPLE_TIME *st)
{
struct tm tim;
static char buf[1024];

mylog("convert_time: m=%d,d=%d,y=%d,hh=%d,mm=%d,ss=%d\n",
	  st->m, st->d, st->y, st->hh, st->mm, st->ss);

	memset(&tim, 0, sizeof(tim));

	tim.tm_mon = st->m - 1;
	tim.tm_mday = st->d;
	tim.tm_year = st->y - 1900;
	tim.tm_hour = st->hh;
	tim.tm_min = st->mm;
	tim.tm_sec = st->ss;

	/*	Dont bother trying to figure out the day of week because
		postgres will determine it correctly.  However, the timezone
		should be taken into account. $$$$
	*/

	//  tim.tm_isdst = _daylight;

	strftime(buf, sizeof(buf), "%b %d %H:%M:%S %Y", 
		&tim);

mylog("convert_time: buf = '%s'\n", buf);

	return buf;
}

/*	This function parses a character string for date/time info and fills in SIMPLE_TIME */
/*	It does not zero out SIMPLE_TIME in case it is desired to initialize it with a value */
char
parse_datetime(char *buf, SIMPLE_TIME *st)
{
int y,m,d,hh,mm,ss;
int nf;
	
	y = m = d = hh = mm = ss = 0;

	if (buf[4] == '-')	/* year first */
		nf = sscanf(buf, "%4d-%2d-%2d %2d:%2d:%2d", &y,&m,&d,&hh,&mm,&ss);
	else
		nf = sscanf(buf, "%2d-%2d-%4d %2d:%2d:%2d", &m,&d,&y,&hh,&mm,&ss);

	if (nf == 5 || nf == 6) {
		st->y = y;
		st->m = m;
		st->d = d;
		st->hh = hh;
		st->mm = mm;
		st->ss = ss;

		return TRUE;
	}

	if (buf[4] == '-')	/* year first */
		nf = sscanf(buf, "%4d-%2d-%2d", &y, &m, &d);
	else
		nf = sscanf(buf, "%2d-%2d-%4d", &m, &d, &y);

	if (nf == 3) {
		st->y = y;
		st->m = m;
		st->d = d;

		return TRUE;
	}

	nf = sscanf(buf, "%2d:%2d:%2d", &hh, &mm, &ss);
	if (nf == 2 || nf == 3) {
		st->hh = hh;
		st->mm = mm;
		st->ss = ss;

		return TRUE;
	}

	return FALSE;
}

/*	Change linefeed to carriage-return/linefeed */
char *
convert_linefeeds(char *si, char *dst, size_t max)
{
size_t i = 0, out = 0;
static char sout[TEXT_FIELD_SIZE+5];
char *p;

	if (dst)
		p = dst;
	else {
		p = sout;
		max = sizeof(sout);
	}

	p[0] = '\0';

	for (i = 0; i < strlen(si) && out < max-2; i++) {
		if (si[i] == '\n') {
			p[out++] = '\r';
			p[out++] = '\n';
		}
		else
			p[out++] = si[i];
	}
	p[out] = '\0';
	return p;
}

/*	Change carriage-return/linefeed to just linefeed */
char *
convert_returns(char *si, char *dst, int used)
{
size_t i = 0, out = 0, max;
static char sout[TEXT_FIELD_SIZE+5];
char *p;

	if (dst)
		p = dst;
	else
		p = sout;

	p[0] = '\0';

	if (used == SQL_NTS)
		max = strlen(si);
	else
		max = used;

	for (i = 0; i < max; i++) {
		if (si[i] == '\r' && i+1 < strlen(si) && si[i+1] == '\n') 
			continue;
		else
			p[out++] = si[i];
	}
	p[out] = '\0';
	return p;
}

int
convert_pgbinary_to_char(char *value, char *rgbValue, int cbValueMax)
{
	return 0;
}

unsigned int
conv_from_octal(unsigned char *s)
{
int i, y=0;

	for (i = 1; i <= 3; i++) {
		y += (s[i] - 48) * (int) pow(8, 3-i);
	}

	return y;

}

//	convert octal escapes to bytes
int
convert_from_pgbinary(unsigned char *value, unsigned char *rgbValue, int cbValueMax)
{
size_t i;
int o=0;
	
	for (i = 0; i < strlen(value); ) {
		if (value[i] == '\\') {
			rgbValue[o] = conv_from_octal(&value[i]);
			i += 4;
		}
		else {
			rgbValue[o] = value[i++];
		}
		mylog("convert_from_pgbinary: i=%d, rgbValue[%d] = %d, %c\n", i, o, rgbValue[o], rgbValue[o]);
		o++;
	}
	return o;
}


char *
conv_to_octal(unsigned char val)
{
int i;
static char x[6];

	x[0] = '\\';
	x[1] = '\\';
	x[5] = '\0';

	for (i = 4; i > 1; i--) {
		x[i] = (val & 7) + 48;
		val >>= 3;
	}

	return x;
}

//	convert non-ascii bytes to octal escape sequences
int
convert_to_pgbinary(unsigned char *in, char *out, int len)
{
int i, o=0;


	for (i = 0; i < len; i++) {
		mylog("convert_to_pgbinary: in[%d] = %d, %c\n", i, in[i], in[i]);
		if (in[i] < 32 || in[i] > 126) {
			strcpy(&out[o], conv_to_octal(in[i])); 
			o += 5;
		}
		else
			out[o++] = in[i];

	}

	mylog("convert_to_pgbinary: returning %d, out='%.*s'\n", o, o, out);

	return o;
}


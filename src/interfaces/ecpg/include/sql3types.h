#ifndef _ECPG_SQL3TYPES_H
#define _ECPG_SQL3TYPES_H

/* SQL3 dynamic type codes
 *
 * Copyright (c) 2000, Christof Petig <christof.petig@wtal.de>
 *
 * $Header: /cvsroot/pgsql/src/interfaces/ecpg/include/sql3types.h,v 1.8 2003/09/20 09:10:09 meskes Exp $
 */

/* chapter 13.1 table 2: Codes used for SQL data types in Dynamic SQL */

enum
{
	SQL3_CHARACTER = 1,
	SQL3_NUMERIC,
	SQL3_DECIMAL,
	SQL3_INTEGER,
	SQL3_SMALLINT,
	SQL3_FLOAT,
	SQL3_REAL,
	SQL3_DOUBLE_PRECISION,
	SQL3_DATE_TIME_TIMESTAMP,
	SQL3_INTERVAL,				/* 10 */
	SQL3_CHARACTER_VARYING = 12,
	SQL3_ENUMERATED,
	SQL3_BIT,
	SQL3_BIT_VARYING,
	SQL3_BOOLEAN,
	SQL3_abstract
	/* the rest is xLOB stuff */
};

/* chapter 13.1 table 3: Codes associated with datetime data types in Dynamic SQL */

enum
{
	SQL3_DDT_DATE = 1,
	SQL3_DDT_TIME,
	SQL3_DDT_TIMESTAMP,
	SQL3_DDT_TIME_WITH_TIME_ZONE,
	SQL3_DDT_TIMESTAMP_WITH_TIME_ZONE,

	SQL3_DDT_ILLEGAL			/* not a datetime data type (not part of
								 * standard) */
};

#endif /* !_ECPG_SQL3TYPES_H */

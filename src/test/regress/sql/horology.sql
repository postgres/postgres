--
-- HOROLOGY
--

--
-- datetime, timespan arithmetic
--

SELECT datetime '1996-03-01' - timespan '1 second' AS "Feb 29";
SELECT datetime '1999-03-01' - timespan '1 second' AS "Feb 28";
SELECT datetime '2000-03-01' - timespan '1 second' AS "Feb 29";
SELECT datetime '1999-12-01' + timespan '1 month - 1 second' AS "Dec 31";

CREATE TABLE TEMP_DATETIME (f1 datetime);

-- get some candidate input values

INSERT INTO TEMP_DATETIME (f1)
  SELECT d1 FROM DATETIME_TBL
  WHERE d1 BETWEEN '13-jun-1957' AND '1-jan-1997'
   OR d1 BETWEEN '1-jan-1999' AND '1-jan-2010';

SELECT '' AS ten, f1 AS datetime
  FROM TEMP_DATETIME
  ORDER BY datetime;

SELECT '' AS hundred, d.f1 AS datetime, t.f1 AS timespan, d.f1 + t.f1 AS plus
  FROM TEMP_DATETIME d, TIMESPAN_TBL t
  ORDER BY plus, datetime, timespan;

SELECT '' AS hundred, d.f1 AS datetime, t.f1 AS timespan, d.f1 - t.f1 AS minus
  FROM TEMP_DATETIME d, TIMESPAN_TBL t
  WHERE isfinite(d.f1)
  ORDER BY minus, datetime, timespan;

SELECT '' AS ten, d.f1 AS datetime, datetime '1980-01-06 00:00 GMT' AS gpstime_zero,
   d.f1 - datetime '1980-01-06 00:00 GMT' AS difference
  FROM TEMP_DATETIME d
  ORDER BY difference;

SELECT '' AS hundred, d1.f1 AS datetime1, d2.f1 AS datetime2, d1.f1 - d2.f1 AS difference
  FROM TEMP_DATETIME d1, TEMP_DATETIME d2
  ORDER BY datetime1, datetime2, difference;

SELECT '' as fifty, d1 as datetime,
  date_part('year', d1) AS year, date_part('month', d1) AS month,
  date_part('day',d1) AS day, date_part('hour', d1) AS hour,
  date_part('minute', d1) AS minute, date_part('second', d1) AS second
  FROM DATETIME_TBL
  WHERE isfinite(d1) and d1 >= '1-jan-1900 GMT'
  ORDER BY datetime;

--
-- abstime, reltime arithmetic
--

SELECT '' AS four, f1 AS abstime,
  date_part('year', f1) AS year, date_part('month', f1) AS month,
  date_part('day',f1) AS day, date_part('hour', f1) AS hour,
  date_part('minute', f1) AS minute, date_part('second', f1) AS second
  FROM ABSTIME_TBL
  WHERE isfinite(f1) and f1 <> abstime 'current'
  ORDER BY abstime;

--
-- conversions
--

SELECT '' AS ten, f1 AS datetime, date( f1) AS date
  FROM TEMP_DATETIME
  WHERE f1 <> datetime 'current'
  ORDER BY date;

SELECT '' AS ten, f1 AS datetime, abstime( f1) AS abstime
  FROM TEMP_DATETIME
  ORDER BY abstime;

SELECT '' AS five, f1 AS abstime, date( f1) AS date
  FROM ABSTIME_TBL
  WHERE isfinite(f1) AND f1 <> abstime 'current'
  ORDER BY date;

SELECT '' AS five, d1 AS datetime, abstime(d1) AS abstime
  FROM DATETIME_TBL WHERE NOT isfinite(d1);

SELECT '' AS three, f1 as abstime, datetime(f1) AS datetime
  FROM ABSTIME_TBL WHERE NOT isfinite(f1);

SELECT '' AS ten, f1 AS timespan, reltime( f1) AS reltime
  FROM TIMESPAN_TBL;

SELECT '' AS six, f1 as reltime, timespan( f1) AS timespan
  FROM RELTIME_TBL;

DROP TABLE TEMP_DATETIME;

--
-- formats
--

SET DateStyle TO 'US,Postgres';

SHOW DateStyle;

SELECT '' AS sixty_two, d1 AS us_postgres FROM DATETIME_TBL;

SELECT '' AS eight, f1 AS us_postgres FROM ABSTIME_TBL;

SET DateStyle TO 'US,ISO';

SELECT '' AS sixty_two, d1 AS us_iso FROM DATETIME_TBL;

SELECT '' AS eight, f1 AS us_iso FROM ABSTIME_TBL;

SET DateStyle TO 'US,SQL';

SHOW DateStyle;

SELECT '' AS sixty_two, d1 AS us_sql FROM DATETIME_TBL;

SELECT '' AS eight, f1 AS us_sql FROM ABSTIME_TBL;

SET DateStyle TO 'European,Postgres';

SHOW DateStyle;

INSERT INTO DATETIME_TBL VALUES('13/06/1957');

SELECT count(*) as one FROM DATETIME_TBL WHERE d1 = 'Jun 13 1957';

SELECT '' AS sixty_three, d1 AS european_postgres FROM DATETIME_TBL;

SELECT '' AS eight, f1 AS european_postgres FROM ABSTIME_TBL;

SET DateStyle TO 'European,ISO';

SHOW DateStyle;

SELECT '' AS sixty_three, d1 AS european_iso FROM DATETIME_TBL;

SELECT '' AS eight, f1 AS european_iso FROM ABSTIME_TBL;

SET DateStyle TO 'European,SQL';

SHOW DateStyle;

SELECT '' AS sixty_three, d1 AS european_sql FROM DATETIME_TBL;

SELECT '' AS eight, f1 AS european_sql FROM ABSTIME_TBL;

RESET DateStyle;

SHOW DateStyle;

--
-- formats
--

SET DateStyle TO 'US,Postgres';

SHOW DateStyle;

SELECT '' AS sixty_two, d1 AS us_postgres FROM DATETIME_TBL;

SELECT '' AS eight, f1 AS us_postgres FROM ABSTIME_TBL;

SET DateStyle TO 'US,ISO';

SELECT '' AS sixty_two, d1 AS us_iso FROM DATETIME_TBL;

SELECT '' AS eight, f1 AS us_iso FROM ABSTIME_TBL;

SET DateStyle TO 'US,SQL';

SHOW DateStyle;

SELECT '' AS sixty_two, d1 AS us_sql FROM DATETIME_TBL;

SELECT '' AS eight, f1 AS us_sql FROM ABSTIME_TBL;

SET DateStyle TO 'European,Postgres';

SHOW DateStyle;

INSERT INTO DATETIME_TBL VALUES('13/06/1957');

SELECT count(*) as one FROM DATETIME_TBL WHERE d1 = 'Jun 13 1957';

SELECT '' AS sixty_three, d1 AS european_postgres FROM DATETIME_TBL;

SELECT '' AS eight, f1 AS european_postgres FROM ABSTIME_TBL;

SET DateStyle TO 'European,ISO';

SHOW DateStyle;

SELECT '' AS sixty_three, d1 AS european_iso FROM DATETIME_TBL;

SELECT '' AS eight, f1 AS european_iso FROM ABSTIME_TBL;

SET DateStyle TO 'European,SQL';

SHOW DateStyle;

SELECT '' AS sixty_three, d1 AS european_sql FROM DATETIME_TBL;

SELECT '' AS eight, f1 AS european_sql FROM ABSTIME_TBL;

RESET DateStyle;

SHOW DateStyle;


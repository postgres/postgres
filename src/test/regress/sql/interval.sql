--
-- INTERVAL
--

SET DATESTYLE = 'ISO';

-- check acceptance of "time zone style"
SELECT INTERVAL '01:00' AS "One hour";
SELECT INTERVAL '+02:00' AS "Two hours";
SELECT INTERVAL '-08:00' AS "Eight hours";
SELECT INTERVAL '-05' AS "Five hours";
SELECT INTERVAL '-1 +02:03' AS "22 hours ago...";
SELECT INTERVAL '-1 days +02:03' AS "22 hours ago...";
SELECT INTERVAL '1.5 weeks' AS "Ten days twelve hours";
SELECT INTERVAL '1.5 months' AS "One month 15 days";
SELECT INTERVAL '10 years -11 month -12 days +13:14' AS "9 years...";

CREATE TABLE INTERVAL_TBL (f1 interval);

INSERT INTO INTERVAL_TBL (f1) VALUES ('@ 1 minute');
INSERT INTO INTERVAL_TBL (f1) VALUES ('@ 5 hour');
INSERT INTO INTERVAL_TBL (f1) VALUES ('@ 10 day');
INSERT INTO INTERVAL_TBL (f1) VALUES ('@ 34 year');
INSERT INTO INTERVAL_TBL (f1) VALUES ('@ 3 months');
INSERT INTO INTERVAL_TBL (f1) VALUES ('@ 14 seconds ago');
INSERT INTO INTERVAL_TBL (f1) VALUES ('1 day 2 hours 3 minutes 4 seconds');
INSERT INTO INTERVAL_TBL (f1) VALUES ('6 years');
INSERT INTO INTERVAL_TBL (f1) VALUES ('5 months');
INSERT INTO INTERVAL_TBL (f1) VALUES ('5 months 12 hours');

-- badly formatted interval
INSERT INTO INTERVAL_TBL (f1) VALUES ('badly formatted interval');
INSERT INTO INTERVAL_TBL (f1) VALUES ('@ 30 eons ago');

-- test interval operators

SELECT '' AS ten, * FROM INTERVAL_TBL;

SELECT '' AS nine, * FROM INTERVAL_TBL
   WHERE INTERVAL_TBL.f1 <> interval '@ 10 days';

SELECT '' AS three, * FROM INTERVAL_TBL
   WHERE INTERVAL_TBL.f1 <= interval '@ 5 hours';

SELECT '' AS three, * FROM INTERVAL_TBL
   WHERE INTERVAL_TBL.f1 < interval '@ 1 day';

SELECT '' AS one, * FROM INTERVAL_TBL
   WHERE INTERVAL_TBL.f1 = interval '@ 34 years';

SELECT '' AS five, * FROM INTERVAL_TBL 
   WHERE INTERVAL_TBL.f1 >= interval '@ 1 month';

SELECT '' AS nine, * FROM INTERVAL_TBL
   WHERE INTERVAL_TBL.f1 > interval '@ 3 seconds ago';

SELECT '' AS fortyfive, r1.*, r2.*
   FROM INTERVAL_TBL r1, INTERVAL_TBL r2
   WHERE r1.f1 > r2.f1
   ORDER BY r1.f1, r2.f1;


-- Test multiplication and division with intervals.
-- Floating point arithmetic rounding errors can lead to unexpected results, 
-- though the code attempts to do the right thing and round up to days and 
-- minutes to avoid results such as '3 days 24:00 hours' or '14:20:60'. 
-- Note that it is expected for some day components to be greater than 29 and 
-- some time components be greater than 23:59:59 due to how intervals are 
-- stored internally.

CREATE TABLE INTERVAL_MULDIV_TBL (span interval);
COPY INTERVAL_MULDIV_TBL FROM STDIN;
41 mon 12 days 360:00
-41 mon -12 days +360:00
-12 days
9 mon -27 days 12:34:56
-3 years 482 days 76:54:32.189
4 mon
14 mon
999 mon 999 days
\.

SELECT span * 0.3 AS product
FROM INTERVAL_MULDIV_TBL;

SELECT span * 8.2 AS product
FROM INTERVAL_MULDIV_TBL;

SELECT span / 10 AS quotient
FROM INTERVAL_MULDIV_TBL;

SELECT span / 100 AS quotient
FROM INTERVAL_MULDIV_TBL;

DROP TABLE INTERVAL_MULDIV_TBL;

SET DATESTYLE = 'postgres';

SELECT '' AS ten, * FROM INTERVAL_TBL;

-- test avg(interval), which is somewhat fragile since people have been
-- known to change the allowed input syntax for type interval without
-- updating pg_aggregate.agginitval

select avg(f1) from interval_tbl;

-- test long interval input
select '4 millenniums 5 centuries 4 decades 1 year 4 months 4 days 17 minutes 31 seconds'::interval;


-- test justify_hours() and justify_days()

SELECT justify_hours(interval '6 months 3 days 52 hours 3 minutes 2 seconds') as "6 mons 5 days 4 hours 3 mins 2 seconds";
SELECT justify_days(interval '6 months 36 days 5 hours 4 minutes 3 seconds') as "7 mons 6 days 5 hours 4 mins 3 seconds";

-- test justify_interval()

SELECT justify_interval(interval '1 month -1 hour') as "1 month -1 hour";

-- test fractional second input, and detection of duplicate units
SET DATESTYLE = 'ISO';
SELECT '1 millisecond'::interval, '1 microsecond'::interval,
       '500 seconds 99 milliseconds 51 microseconds'::interval;
SELECT '3 days 5 milliseconds'::interval;

SELECT '1 second 2 seconds'::interval;              -- error
SELECT '10 milliseconds 20 milliseconds'::interval; -- error
SELECT '5.5 seconds 3 milliseconds'::interval;      -- error
SELECT '1:20:05 5 microseconds'::interval;          -- error
SELECT interval '1-2';  -- SQL year-month literal

-- test SQL-spec syntaxes for restricted field sets
SELECT interval '1' year;
SELECT interval '2' month;
SELECT interval '3' day;
SELECT interval '4' hour;
SELECT interval '5' minute;
SELECT interval '6' second;
SELECT interval '1' year to month;
SELECT interval '1-2' year to month;
SELECT interval '1 2' day to hour;
SELECT interval '1 2:03' day to hour;
SELECT interval '1 2:03:04' day to hour;
SELECT interval '1 2' day to minute;
SELECT interval '1 2:03' day to minute;
SELECT interval '1 2:03:04' day to minute;
SELECT interval '1 2' day to second;
SELECT interval '1 2:03' day to second;
SELECT interval '1 2:03:04' day to second;
SELECT interval '1 2' hour to minute;
SELECT interval '1 2:03' hour to minute;
SELECT interval '1 2:03:04' hour to minute;
SELECT interval '1 2' hour to second;
SELECT interval '1 2:03' hour to second;
SELECT interval '1 2:03:04' hour to second;
SELECT interval '1 2' minute to second;
SELECT interval '1 2:03' minute to second;
SELECT interval '1 2:03:04' minute to second;

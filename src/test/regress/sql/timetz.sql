--
-- TIMETZ
--

CREATE TABLE TIMETZ_TBL (f1 time(2) with time zone);

INSERT INTO TIMETZ_TBL VALUES ('00:01 PDT');
INSERT INTO TIMETZ_TBL VALUES ('01:00 PDT');
INSERT INTO TIMETZ_TBL VALUES ('02:03 PDT');
INSERT INTO TIMETZ_TBL VALUES ('07:07 PST');
INSERT INTO TIMETZ_TBL VALUES ('08:08 EDT');
INSERT INTO TIMETZ_TBL VALUES ('11:59 PDT');
INSERT INTO TIMETZ_TBL VALUES ('12:00 PDT');
INSERT INTO TIMETZ_TBL VALUES ('12:01 PDT');
INSERT INTO TIMETZ_TBL VALUES ('23:59 PDT');
INSERT INTO TIMETZ_TBL VALUES ('11:59:59.99 PM PDT');

INSERT INTO TIMETZ_TBL VALUES ('2003-03-07 15:36:39 America/New_York');
INSERT INTO TIMETZ_TBL VALUES ('2003-07-07 15:36:39 America/New_York');
-- this should fail (the timezone offset is not known)
INSERT INTO TIMETZ_TBL VALUES ('15:36:39 America/New_York');
-- this should fail (timezone not specified without a date)
INSERT INTO TIMETZ_TBL VALUES ('15:36:39 m2');
-- this should fail (dynamic timezone abbreviation without a date)
INSERT INTO TIMETZ_TBL VALUES ('15:36:39 MSK m2');


SELECT f1 AS "Time TZ" FROM TIMETZ_TBL;

SELECT f1 AS "Three" FROM TIMETZ_TBL WHERE f1 < '05:06:07-07';

SELECT f1 AS "Seven" FROM TIMETZ_TBL WHERE f1 > '05:06:07-07';

SELECT f1 AS "None" FROM TIMETZ_TBL WHERE f1 < '00:00-07';

SELECT f1 AS "Ten" FROM TIMETZ_TBL WHERE f1 >= '00:00-07';

-- Check edge cases
SELECT '23:59:59.999999 PDT'::timetz;
SELECT '23:59:59.9999999 PDT'::timetz;  -- rounds up
SELECT '23:59:60 PDT'::timetz;  -- rounds up
SELECT '24:00:00 PDT'::timetz;  -- allowed
SELECT '24:00:00.01 PDT'::timetz;  -- not allowed
SELECT '23:59:60.01 PDT'::timetz;  -- not allowed
SELECT '24:01:00 PDT'::timetz;  -- not allowed
SELECT '25:00:00 PDT'::timetz;  -- not allowed

--
-- TIME simple math
--
-- We now make a distinction between time and intervals,
-- and adding two times together makes no sense at all.
-- Leave in one query to show that it is rejected,
-- and do the rest of the testing in horology.sql
-- where we do mixed-type arithmetic. - thomas 2000-12-02

SELECT f1 + time with time zone '00:01' AS "Illegal" FROM TIMETZ_TBL;

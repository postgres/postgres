--
-- TIME
--

CREATE TABLE TIME_TBL (f1 time, f2 time with time zone);

INSERT INTO TIME_TBL VALUES ('00:00', '00:00 PDT');
INSERT INTO TIME_TBL VALUES ('01:00', '01:00 PDT');
INSERT INTO TIME_TBL VALUES ('02:03', '02:03 PDT');
INSERT INTO TIME_TBL VALUES ('11:59', '11:59 PDT');
INSERT INTO TIME_TBL VALUES ('12:00', '12:00 PDT');
INSERT INTO TIME_TBL VALUES ('12:01', '12:01 PDT');
INSERT INTO TIME_TBL VALUES ('23:59', '23:59 PDT');
INSERT INTO TIME_TBL VALUES ('11:59:59.99 PM', '11:59:59.99 PM PDT');

SELECT f1 AS "Time", f2 AS "Time TZ" FROM TIME_TBL;

SELECT f1 AS "Three" FROM TIME_TBL WHERE f1 < '05:06:07';

SELECT f1 AS "Five" FROM TIME_TBL WHERE f1 > '05:06:07';

SELECT f1 AS "None" FROM TIME_TBL WHERE f1 < '00:00';

SELECT f1 AS "Eight" FROM TIME_TBL WHERE f1 >= '00:00';

--
-- TIME simple math
--
-- We now make a distinction between time and intervals,
-- and adding two times together makes no sense at all.
-- Leave in one query to show that it is rejected,
-- and do the rest of the testing in horology.sql
-- where we do mixed-type arithmetic. - thomas 2000-12-02

SELECT f1 + time '00:01' AS "Illegal" FROM TIME_TBL;

SELECT f2 + time with time zone '00:01' AS "Illegal" FROM TIME_TBL;

--
-- INTERVAL
--

SET DATESTYLE = DEFAULT;

-- check acceptance of "time zone style"
SELECT INTERVAL '01:00' AS "One hour";
SELECT INTERVAL '+02:00' AS "Two hours";
SELECT INTERVAL '-08:00' AS "Eight hours";
SELECT INTERVAL '-05' AS "Five hours";
SELECT INTERVAL '-1 +02:03' AS "22 hours ago...";
SELECT INTERVAL '-1 days +02:03' AS "22 hours ago...";
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

SELECT '' AS ten, INTERVAL_TBL.*;

SELECT '' AS nine, INTERVAL_TBL.*
   WHERE INTERVAL_TBL.f1 <> interval '@ 10 days';

SELECT '' AS three, INTERVAL_TBL.*
   WHERE INTERVAL_TBL.f1 <= interval '@ 5 hours';

SELECT '' AS three, INTERVAL_TBL.*
   WHERE INTERVAL_TBL.f1 < interval '@ 1 day';

SELECT '' AS one, INTERVAL_TBL.*
   WHERE INTERVAL_TBL.f1 = interval '@ 34 years';

SELECT '' AS five, INTERVAL_TBL.* 
   WHERE INTERVAL_TBL.f1 >= interval '@ 1 month';

SELECT '' AS nine, INTERVAL_TBL.*
   WHERE INTERVAL_TBL.f1 > interval '@ 3 seconds ago';

SELECT '' AS fortyfive, r1.*, r2.*
   FROM INTERVAL_TBL r1, INTERVAL_TBL r2
   WHERE r1.f1 > r2.f1
   ORDER BY r1.f1, r2.f1;

SET DATESTYLE = 'postgres';

SELECT '' AS ten, INTERVAL_TBL.*;

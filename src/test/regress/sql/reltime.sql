--
-- RELTIME
--

CREATE TABLE RELTIME_TBL (f1 reltime);

INSERT INTO RELTIME_TBL (f1) VALUES ('@ 1 minute');

INSERT INTO RELTIME_TBL (f1) VALUES ('@ 5 hour');

INSERT INTO RELTIME_TBL (f1) VALUES ('@ 10 day');

INSERT INTO RELTIME_TBL (f1) VALUES ('@ 34 year');

INSERT INTO RELTIME_TBL (f1) VALUES ('@ 3 months');

INSERT INTO RELTIME_TBL (f1) VALUES ('@ 14 seconds ago');


-- badly formatted reltimes
INSERT INTO RELTIME_TBL (f1) VALUES ('badly formatted reltime');

INSERT INTO RELTIME_TBL (f1) VALUES ('@ 30 eons ago');

-- test reltime operators

SELECT '' AS six, RELTIME_TBL.*;

SELECT '' AS five, RELTIME_TBL.*
   WHERE RELTIME_TBL.f1 <> reltime '@ 10 days';

SELECT '' AS three, RELTIME_TBL.*
   WHERE RELTIME_TBL.f1 <= reltime '@ 5 hours';

SELECT '' AS three, RELTIME_TBL.*
   WHERE RELTIME_TBL.f1 < reltime '@ 1 day';

SELECT '' AS one, RELTIME_TBL.*
   WHERE RELTIME_TBL.f1 = reltime '@ 34 years';

SELECT '' AS two, RELTIME_TBL.* 
   WHERE RELTIME_TBL.f1 >= reltime '@ 1 month';

SELECT '' AS five, RELTIME_TBL.*
   WHERE RELTIME_TBL.f1 > reltime '@ 3 seconds ago';

SELECT '' AS fifteen, r1.*, r2.*
   FROM RELTIME_TBL r1, RELTIME_TBL r2
   WHERE r1.f1 > r2.f1
   ORDER BY r1.f1, r2.f1;


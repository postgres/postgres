CREATE TABLE RELTIME_TBL (f1 reltime);

INSERT INTO RELTIME_TBL (f1) VALUES ('@ 1 minute');

INSERT INTO RELTIME_TBL (f1) VALUES ('@ 5 hour');

INSERT INTO RELTIME_TBL (f1) VALUES ('@ 10 day');

INSERT INTO RELTIME_TBL (f1) VALUES ('@ 34 year');

INSERT INTO RELTIME_TBL (f1) VALUES ('@ 3 months');

INSERT INTO RELTIME_TBL (f1) VALUES ('@ 14 seconds ago');


-- badly formatted reltimes:   
INSERT INTO RELTIME_TBL (f1) VALUES ('badly formatted reltime');

INSERT INTO RELTIME_TBL (f1) VALUES ('@ 30 eons ago');

-- test reltime operators

SELECT '' AS eight, RELTIME_TBL.*;

SELECT '' AS five, RELTIME_TBL.*
   WHERE RELTIME_TBL.f1 <> '@ 10 days'::reltime;

SELECT '' AS three, RELTIME_TBL.*
   WHERE RELTIME_TBL.f1 <= '@ 5 hours'::reltime;

SELECT '' AS three, RELTIME_TBL.*
   WHERE RELTIME_TBL.f1 < '@ 1 day'::reltime;

SELECT '' AS one, RELTIME_TBL.*
   WHERE RELTIME_TBL.f1 = '@ 34 years'::reltime;

SELECT '' AS two, RELTIME_TBL.* 
   WHERE RELTIME_TBL.f1 >= '@ 1 month'::reltime;

SELECT '' AS five, RELTIME_TBL.*
   WHERE RELTIME_TBL.f1 > '@ 3 seconds ago'::reltime;

SELECT '' AS fifteen, r1.*, r2.*
   FROM RELTIME_TBL r1, RELTIME_TBL r2
   WHERE r1.f1 > r2.f1;


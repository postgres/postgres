CREATE TABLE TIMESPAN_TBL (f1 timespan);

INSERT INTO TIMESPAN_TBL (f1) VALUES ('@ 1 minute');
INSERT INTO TIMESPAN_TBL (f1) VALUES ('@ 5 hour');
INSERT INTO TIMESPAN_TBL (f1) VALUES ('@ 10 day');
INSERT INTO TIMESPAN_TBL (f1) VALUES ('@ 34 year');
INSERT INTO TIMESPAN_TBL (f1) VALUES ('@ 3 months');
INSERT INTO TIMESPAN_TBL (f1) VALUES ('@ 14 seconds ago');
INSERT INTO TIMESPAN_TBL (f1) VALUES ('1 day 2 hours 3 minutes 4 seconds');
INSERT INTO TIMESPAN_TBL (f1) VALUES ('6 years');
INSERT INTO TIMESPAN_TBL (f1) VALUES ('5 months');
INSERT INTO TIMESPAN_TBL (f1) VALUES ('5 months 12 hours');

-- badly formatted timespan:   
INSERT INTO TIMESPAN_TBL (f1) VALUES ('badly formatted timespan');
INSERT INTO TIMESPAN_TBL (f1) VALUES ('@ 30 eons ago');

-- test timespan operators

SELECT '' AS ten, TIMESPAN_TBL.*;

SELECT '' AS nine, TIMESPAN_TBL.*
   WHERE TIMESPAN_TBL.f1 <> '@ 10 days'::timespan;

SELECT '' AS three, TIMESPAN_TBL.*
   WHERE TIMESPAN_TBL.f1 <= '@ 5 hours'::timespan;

SELECT '' AS three, TIMESPAN_TBL.*
   WHERE TIMESPAN_TBL.f1 < '@ 1 day'::timespan;

SELECT '' AS one, TIMESPAN_TBL.*
   WHERE TIMESPAN_TBL.f1 = '@ 34 years'::timespan;

SELECT '' AS five, TIMESPAN_TBL.* 
   WHERE TIMESPAN_TBL.f1 >= '@ 1 month'::timespan;

SELECT '' AS nine, TIMESPAN_TBL.*
   WHERE TIMESPAN_TBL.f1 > '@ 3 seconds ago'::timespan;

SELECT '' AS fortyfive, r1.*, r2.*
   FROM TIMESPAN_TBL r1, TIMESPAN_TBL r2
   WHERE r1.f1 > r2.f1;


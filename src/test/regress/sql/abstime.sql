-- **** testing built-in time types: abstime, reltime, and tinterval ****

--
-- timezones may vary based not only on location but the operating
-- system.  the main correctness issue is that the OS may not get 
-- daylight savings time right for times prior to Unix epoch (jan 1 1970).
--

CREATE TABLE ABSTIME_TBL (f1 abstime);

INSERT INTO ABSTIME_TBL (f1) VALUES ('Jan 14, 1973 03:14:21');

-- was INSERT INTO ABSTIME_TBL (f1) VALUES ('now'::abstime):
INSERT INTO ABSTIME_TBL (f1) VALUES ('Mon May  1 00:30:30 1995'::abstime);

INSERT INTO ABSTIME_TBL (f1) VALUES ('epoch'::abstime);

INSERT INTO ABSTIME_TBL (f1) VALUES ('current'::abstime);

INSERT INTO ABSTIME_TBL (f1) VALUES ('infinity'::abstime);

INSERT INTO ABSTIME_TBL (f1) VALUES ('-infinity'::abstime);

INSERT INTO ABSTIME_TBL (f1) VALUES ('May 10, 1943 23:59:12');


-- what happens if we specify slightly misformatted abstime? 
INSERT INTO ABSTIME_TBL (f1) VALUES ('Feb 35, 1946 10:00:00');

INSERT INTO ABSTIME_TBL (f1) VALUES ('Feb 28, 1984 25:08:10');


-- badly formatted abstimes:  these should result in invalid abstimes 
INSERT INTO ABSTIME_TBL (f1) VALUES ('bad date format');

INSERT INTO ABSTIME_TBL (f1) VALUES ('Jun 10, 1843');

-- test abstime operators

SELECT '' AS eight, ABSTIME_TBL.*;

SELECT '' AS six, ABSTIME_TBL.*
   WHERE ABSTIME_TBL.f1 < 'Jun 30, 2001'::abstime;

SELECT '' AS six, ABSTIME_TBL.*
   WHERE ABSTIME_TBL.f1 > '-infinity'::abstime;

SELECT '' AS six, ABSTIME_TBL.*
   WHERE 'May 10, 1943 23:59:12'::abstime <> ABSTIME_TBL.f1;

SELECT '' AS one, ABSTIME_TBL.*
   WHERE 'current'::abstime = ABSTIME_TBL.f1;

SELECT '' AS three, ABSTIME_TBL.*
   WHERE 'epoch'::abstime >= ABSTIME_TBL.f1;

SELECT '' AS four, ABSTIME_TBL.*
   WHERE ABSTIME_TBL.f1 <= 'Jan 14, 1973 03:14:21'::abstime;

SELECT '' AS four, ABSTIME_TBL.*
  WHERE ABSTIME_TBL.f1 <?>
	'["Apr 1 1945 00:00:00" "Dec 30 1999 23:00:00"]'::tinterval;

-- these four queries should return the same answer
-- the "infinity" and "-infinity" tuples in ABSTIME_TBL cannot be added and
-- therefore, should not show up in the results.
SELECT '' AS three, ABSTIME_TBL.*
  WHERE  (ABSTIME_TBL.f1 + '@ 3 year'::reltime) -- +3 years
	< 'Jan 14 14:00:00 1977'::abstime;

SELECT '' AS three, ABSTIME_TBL.*
   WHERE  (ABSTIME_TBL.f1 + '@ 3 year ago'::reltime)	-- -3 years
	< 'Jan 14 14:00:00 1971'::abstime;

SELECT '' AS three, ABSTIME_TBL.*
   WHERE  (ABSTIME_TBL.f1 - '@ 3 year'::reltime)        -- -(+3) years
	< 'Jan 14 14:00:00 1971'::abstime;

SELECT '' AS three, ABSTIME_TBL.*
   WHERE  (ABSTIME_TBL.f1 - '@ 3 year ago'::reltime)    -- -(-3) years
        < 'Jan 14 14:00:00 1977'::abstime;


SELECT '' AS ten, ABSTIME_TBL.*, RELTIME_TBL.*
   WHERE (ABSTIME_TBL.f1 + RELTIME_TBL.f1)
	< 'Jan 14 14:00:00 1971'::abstime;


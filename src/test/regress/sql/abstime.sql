--
-- ABSTIME
-- testing built-in time type abstime
-- uses reltime and tinterval
--

--
-- timezones may vary based not only on location but the operating
-- system.  the main correctness issue is that the OS may not get 
-- daylight savings time right for times prior to Unix epoch (jan 1 1970).
--

CREATE TABLE ABSTIME_TBL (f1 abstime);

INSERT INTO ABSTIME_TBL (f1) VALUES ('Jan 14, 1973 03:14:21');

-- was INSERT INTO ABSTIME_TBL (f1) VALUES (abstime 'now'):
INSERT INTO ABSTIME_TBL (f1) VALUES (abstime 'Mon May  1 00:30:30 1995');

INSERT INTO ABSTIME_TBL (f1) VALUES (abstime 'epoch');

INSERT INTO ABSTIME_TBL (f1) VALUES (abstime 'current');

INSERT INTO ABSTIME_TBL (f1) VALUES (abstime 'infinity');

INSERT INTO ABSTIME_TBL (f1) VALUES (abstime '-infinity');

INSERT INTO ABSTIME_TBL (f1) VALUES (abstime 'May 10, 1947 23:59:12');


-- what happens if we specify slightly misformatted abstime? 
INSERT INTO ABSTIME_TBL (f1) VALUES ('Feb 35, 1946 10:00:00');

INSERT INTO ABSTIME_TBL (f1) VALUES ('Feb 28, 1984 25:08:10');


-- badly formatted abstimes:  these should result in invalid abstimes 
INSERT INTO ABSTIME_TBL (f1) VALUES ('bad date format');

INSERT INTO ABSTIME_TBL (f1) VALUES ('Jun 10, 1843');

-- test abstime operators

SELECT '' AS eight, ABSTIME_TBL.*;

SELECT '' AS six, ABSTIME_TBL.*
   WHERE ABSTIME_TBL.f1 < abstime 'Jun 30, 2001';

SELECT '' AS six, ABSTIME_TBL.*
   WHERE ABSTIME_TBL.f1 > abstime '-infinity';

SELECT '' AS six, ABSTIME_TBL.*
   WHERE abstime 'May 10, 1947 23:59:12' <> ABSTIME_TBL.f1;

SELECT '' AS one, ABSTIME_TBL.*
   WHERE abstime 'current' = ABSTIME_TBL.f1;

SELECT '' AS three, ABSTIME_TBL.*
   WHERE abstime 'epoch' >= ABSTIME_TBL.f1;

SELECT '' AS four, ABSTIME_TBL.*
   WHERE ABSTIME_TBL.f1 <= abstime 'Jan 14, 1973 03:14:21';

SELECT '' AS four, ABSTIME_TBL.*
  WHERE ABSTIME_TBL.f1 <?>
	tinterval '["Apr 1 1950 00:00:00" "Dec 30 1999 23:00:00"]';

-- these four queries should return the same answer
-- the "infinity" and "-infinity" tuples in ABSTIME_TBL cannot be added and
-- therefore, should not show up in the results.
SELECT '' AS three, ABSTIME_TBL.*
  WHERE  (ABSTIME_TBL.f1 + reltime '@ 3 year') -- +3 years
	< abstime 'Jan 14 14:00:00 1977';

SELECT '' AS three, ABSTIME_TBL.*
   WHERE  (ABSTIME_TBL.f1 + reltime '@ 3 year ago')	-- -3 years
	< abstime 'Jan 14 14:00:00 1971';

SELECT '' AS three, ABSTIME_TBL.*
   WHERE  (ABSTIME_TBL.f1 - reltime '@ 3 year')        -- -(+3) years
	< abstime 'Jan 14 14:00:00 1971';

SELECT '' AS three, ABSTIME_TBL.*
   WHERE  (ABSTIME_TBL.f1 - reltime '@ 3 year ago')    -- -(-3) years
        < abstime 'Jan 14 14:00:00 1977';

SELECT '' AS ten, ABSTIME_TBL.f1 AS abstime, RELTIME_TBL.f1 AS reltime
   WHERE (ABSTIME_TBL.f1 + RELTIME_TBL.f1)
	< abstime 'Jan 14 14:00:00 1971'
   ORDER BY abstime, reltime;


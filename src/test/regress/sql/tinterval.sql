--
-- TINTERVAL
--

CREATE TABLE TINTERVAL_TBL (f1  tinterval);

-- Should accept any abstime,
-- so do not bother with extensive testing of values

INSERT INTO TINTERVAL_TBL (f1)
   VALUES ('["-infinity" "infinity"]');

INSERT INTO TINTERVAL_TBL (f1)
   VALUES ('["May 10, 1947 23:59:12" "Jan 14, 1973 03:14:21"]');

INSERT INTO TINTERVAL_TBL (f1)
   VALUES ('["Sep 4, 1983 23:59:12" "Oct 4, 1983 23:59:12"]');

INSERT INTO TINTERVAL_TBL (f1)
   VALUES ('["epoch" "Mon May  1 00:30:30 1995"]');

INSERT INTO TINTERVAL_TBL (f1)
   VALUES ('["Feb 15 1990 12:15:03" "2001-09-23 11:12:13"]');


-- badly formatted tintervals 
INSERT INTO TINTERVAL_TBL (f1)
   VALUES ('["bad time specifications" ""]');

INSERT INTO TINTERVAL_TBL (f1)
   VALUES ('["" "infinity"]');

-- test tinterval operators

SELECT '' AS five, TINTERVAL_TBL.*;

-- length ==
SELECT '' AS one, t.*
   FROM TINTERVAL_TBL t
   WHERE t.f1 #= '@ 1 months';

-- length <>
SELECT '' AS three, t.*
   FROM TINTERVAL_TBL t
   WHERE t.f1 #<> '@ 1 months';

-- length <
SELECT '' AS zero, t.*
   FROM TINTERVAL_TBL t
   WHERE t.f1 #< '@ 1 month';

-- length <=
SELECT '' AS one, t.*
   FROM TINTERVAL_TBL t
   WHERE t.f1 #<= '@ 1 month';

-- length >
SELECT '' AS three, t.*
   FROM TINTERVAL_TBL t
   WHERE t.f1 #> '@ 1 year';

-- length >=
SELECT '' AS three, t.*
   FROM TINTERVAL_TBL t
   WHERE t.f1 #>= '@ 3 years';

-- overlaps
SELECT '' AS three, t1.*
   FROM TINTERVAL_TBL t1
   WHERE t1.f1 &&
        tinterval '["Aug 15 14:23:19 1983" "Sep 16 14:23:19 1983"]';

SET geqo TO 'off';

SELECT '' AS five, t1.f1, t2.f1
   FROM TINTERVAL_TBL t1, TINTERVAL_TBL t2
   WHERE t1.f1 && t2.f1 and
         t1.f1 = t2.f1
   ORDER BY t1.f1, t2.f1;

SELECT '' AS fourteen, t1.f1 AS interval1, t2.f1 AS interval2
   FROM TINTERVAL_TBL t1, TINTERVAL_TBL t2
   WHERE t1.f1 && t2.f1 and not t1.f1 = t2.f1
   ORDER BY interval1, interval2;

-- contains
SELECT '' AS five, t1.f1
   FROM TINTERVAL_TBL t1
   WHERE not t1.f1 << 
        tinterval '["Aug 15 14:23:19 1980" "Sep 16 14:23:19 1990"]'
   ORDER BY t1.f1;

-- make time interval
SELECT '' AS three, t1.f1
   FROM TINTERVAL_TBL t1
   WHERE t1.f1 &&
        (abstime 'Aug 15 14:23:19 1983' <#>
         abstime 'Sep 16 14:23:19 1983')
   ORDER BY t1.f1;

RESET geqo;

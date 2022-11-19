--
-- INT2
--

-- int2_tbl was already created and filled in test_setup.sql.
-- Here we just try to insert bad values.

INSERT INTO INT2_TBL(f1) VALUES ('34.5');
INSERT INTO INT2_TBL(f1) VALUES ('100000');
INSERT INTO INT2_TBL(f1) VALUES ('asdf');
INSERT INTO INT2_TBL(f1) VALUES ('    ');
INSERT INTO INT2_TBL(f1) VALUES ('- 1234');
INSERT INTO INT2_TBL(f1) VALUES ('4 444');
INSERT INTO INT2_TBL(f1) VALUES ('123 dt');
INSERT INTO INT2_TBL(f1) VALUES ('');


SELECT * FROM INT2_TBL;

SELECT * FROM INT2_TBL AS f(a, b);

SELECT * FROM (TABLE int2_tbl) AS s (a, b);

SELECT i.* FROM INT2_TBL i WHERE i.f1 <> int2 '0';

SELECT i.* FROM INT2_TBL i WHERE i.f1 <> int4 '0';

SELECT i.* FROM INT2_TBL i WHERE i.f1 = int2 '0';

SELECT i.* FROM INT2_TBL i WHERE i.f1 = int4 '0';

SELECT i.* FROM INT2_TBL i WHERE i.f1 < int2 '0';

SELECT i.* FROM INT2_TBL i WHERE i.f1 < int4 '0';

SELECT i.* FROM INT2_TBL i WHERE i.f1 <= int2 '0';

SELECT i.* FROM INT2_TBL i WHERE i.f1 <= int4 '0';

SELECT i.* FROM INT2_TBL i WHERE i.f1 > int2 '0';

SELECT i.* FROM INT2_TBL i WHERE i.f1 > int4 '0';

SELECT i.* FROM INT2_TBL i WHERE i.f1 >= int2 '0';

SELECT i.* FROM INT2_TBL i WHERE i.f1 >= int4 '0';

-- positive odds
SELECT i.* FROM INT2_TBL i WHERE (i.f1 % int2 '2') = int2 '1';

-- any evens
SELECT i.* FROM INT2_TBL i WHERE (i.f1 % int4 '2') = int2 '0';

SELECT i.f1, i.f1 * int2 '2' AS x FROM INT2_TBL i;

SELECT i.f1, i.f1 * int2 '2' AS x FROM INT2_TBL i
WHERE abs(f1) < 16384;

SELECT i.f1, i.f1 * int4 '2' AS x FROM INT2_TBL i;

SELECT i.f1, i.f1 + int2 '2' AS x FROM INT2_TBL i;

SELECT i.f1, i.f1 + int2 '2' AS x FROM INT2_TBL i
WHERE f1 < 32766;

SELECT i.f1, i.f1 + int4 '2' AS x FROM INT2_TBL i;

SELECT i.f1, i.f1 - int2 '2' AS x FROM INT2_TBL i;

SELECT i.f1, i.f1 - int2 '2' AS x FROM INT2_TBL i
WHERE f1 > -32767;

SELECT i.f1, i.f1 - int4 '2' AS x FROM INT2_TBL i;

SELECT i.f1, i.f1 / int2 '2' AS x FROM INT2_TBL i;

SELECT i.f1, i.f1 / int4 '2' AS x FROM INT2_TBL i;

-- corner cases
SELECT (-1::int2<<15)::text;
SELECT ((-1::int2<<15)+1::int2)::text;

-- check sane handling of INT16_MIN overflow cases
SELECT (-32768)::int2 * (-1)::int2;
SELECT (-32768)::int2 / (-1)::int2;
SELECT (-32768)::int2 % (-1)::int2;

-- check rounding when casting from float
SELECT x, x::int2 AS int2_value
FROM (VALUES (-2.5::float8),
             (-1.5::float8),
             (-0.5::float8),
             (0.0::float8),
             (0.5::float8),
             (1.5::float8),
             (2.5::float8)) t(x);

-- check rounding when casting from numeric
SELECT x, x::int2 AS int2_value
FROM (VALUES (-2.5::numeric),
             (-1.5::numeric),
             (-0.5::numeric),
             (0.0::numeric),
             (0.5::numeric),
             (1.5::numeric),
             (2.5::numeric)) t(x);

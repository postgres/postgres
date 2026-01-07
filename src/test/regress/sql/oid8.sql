--
-- OID8
--

CREATE TABLE OID8_TBL(f1 oid8);

INSERT INTO OID8_TBL(f1) VALUES ('1234');
INSERT INTO OID8_TBL(f1) VALUES ('1235');
INSERT INTO OID8_TBL(f1) VALUES ('987');
INSERT INTO OID8_TBL(f1) VALUES ('-1040');
INSERT INTO OID8_TBL(f1) VALUES ('99999999');
INSERT INTO OID8_TBL(f1) VALUES ('5     ');
INSERT INTO OID8_TBL(f1) VALUES ('   10  ');
INSERT INTO OID8_TBL(f1) VALUES ('123456789012345678');
-- UINT64_MAX
INSERT INTO OID8_TBL(f1) VALUES ('18446744073709551615');
-- leading/trailing hard tab is also allowed
INSERT INTO OID8_TBL(f1) VALUES ('	  15 	  ');

-- bad inputs
INSERT INTO OID8_TBL(f1) VALUES ('');
INSERT INTO OID8_TBL(f1) VALUES ('    ');
INSERT INTO OID8_TBL(f1) VALUES ('asdfasd');
INSERT INTO OID8_TBL(f1) VALUES ('99asdfasd');
INSERT INTO OID8_TBL(f1) VALUES ('5    d');
INSERT INTO OID8_TBL(f1) VALUES ('    5d');
INSERT INTO OID8_TBL(f1) VALUES ('5    5');
INSERT INTO OID8_TBL(f1) VALUES (' - 500');
INSERT INTO OID8_TBL(f1) VALUES ('3908203590239580293850293850329485');
INSERT INTO OID8_TBL(f1) VALUES ('-1204982019841029840928340329840934');
-- UINT64_MAX + 1
INSERT INTO OID8_TBL(f1) VALUES ('18446744073709551616');

SELECT * FROM OID8_TBL;

-- Also try it with non-error-throwing API
SELECT pg_input_is_valid('1234', 'oid8');
SELECT pg_input_is_valid('01XYZ', 'oid8');
SELECT * FROM pg_input_error_info('01XYZ', 'oid8');
SELECT pg_input_is_valid('3908203590239580293850293850329485', 'oid8');
SELECT * FROM pg_input_error_info('-1204982019841029840928340329840934', 'oid8');

-- Operators
SELECT o.* FROM OID8_TBL o WHERE o.f1 = 1234;
SELECT o.* FROM OID8_TBL o WHERE o.f1 <> '1234';
SELECT o.* FROM OID8_TBL o WHERE o.f1 <= '1234';
SELECT o.* FROM OID8_TBL o WHERE o.f1 < '1234';
SELECT o.* FROM OID8_TBL o WHERE o.f1 >= '1234';
SELECT o.* FROM OID8_TBL o WHERE o.f1 > '1234';

-- Casts
SELECT 1::int2::oid8;
SELECT 1::int4::oid8;
SELECT 1::int8::oid8;
SELECT 1::oid8::int8;
SELECT 1::oid::oid8; -- ok
SELECT 1::oid8::oid; -- not ok

-- Aggregates
SELECT min(f1), max(f1) FROM OID8_TBL;

-- Check btree and hash opclasses
EXPLAIN (COSTS OFF)
SELECT DISTINCT (i || '000000000000' || j)::oid8 f
  FROM generate_series(1, 10) i,
       generate_series(1, 10) j,
       generate_series(1, 5) k
  WHERE i <= 10 AND j > 0 AND j <= 10
  ORDER BY f;

SELECT DISTINCT (i || '000000000000' || j)::oid8 f
  FROM generate_series(1, 10) i,
       generate_series(1, 10) j,
       generate_series(1, 5) k
  WHERE i <= 10 AND j > 0 AND j <= 10
  ORDER BY f;

-- 3-way compare for btrees
SELECT btoid8cmp(1::oid8, 2::oid8) < 0 AS val_lower;
SELECT btoid8cmp(2::oid8, 2::oid8) = 0 AS val_equal;
SELECT btoid8cmp(2::oid8, 1::oid8) > 0 AS val_higher;

-- oid8 has btree and hash opclasses
CREATE INDEX on OID8_TBL USING btree(f1);
CREATE INDEX ON OID8_TBL USING hash(f1);

DROP TABLE OID8_TBL;

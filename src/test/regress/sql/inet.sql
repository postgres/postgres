--
-- INET
--

-- prepare the table...

DROP TABLE INET_TBL;
CREATE TABLE INET_TBL (c cidr, i inet);
INSERT INTO INET_TBL (c, i) VALUES ('192.168.1', '192.168.1.226/24');
INSERT INTO INET_TBL (c, i) VALUES ('192.168.1.0/26', '192.168.1.226');
INSERT INTO INET_TBL (c, i) VALUES ('192.168.1', '192.168.1.0/24');
INSERT INTO INET_TBL (c, i) VALUES ('192.168.1', '192.168.1.0/25');
INSERT INTO INET_TBL (c, i) VALUES ('192.168.1', '192.168.1.255/24');
INSERT INTO INET_TBL (c, i) VALUES ('192.168.1', '192.168.1.255/25');
INSERT INTO INET_TBL (c, i) VALUES ('10', '10.1.2.3/8');
INSERT INTO INET_TBL (c, i) VALUES ('10.0.0.0', '10.1.2.3/8');
INSERT INTO INET_TBL (c, i) VALUES ('10.1.2.3', '10.1.2.3/32');
INSERT INTO INET_TBL (c, i) VALUES ('10.1.2', '10.1.2.3/24');
INSERT INTO INET_TBL (c, i) VALUES ('10.1', '10.1.2.3/16');
INSERT INTO INET_TBL (c, i) VALUES ('10', '10.1.2.3/8');
INSERT INTO INET_TBL (c, i) VALUES ('10', '11.1.2.3/8');
INSERT INTO INET_TBL (c, i) VALUES ('10', '9.1.2.3/8');
INSERT INTO INET_TBL (c, i) VALUES ('10:23::f1', '10:23::f1/64');
INSERT INTO INET_TBL (c, i) VALUES ('10:23::8000/113', '10:23::ffff');
INSERT INTO INET_TBL (c, i) VALUES ('::ffff:1.2.3.4', '::4.3.2.1/24');
-- check that CIDR rejects invalid input:
INSERT INTO INET_TBL (c, i) VALUES ('192.168.1.2/30', '192.168.1.226');
INSERT INTO INET_TBL (c, i) VALUES ('1234::1234::1234', '::1.2.3.4');
-- check that CIDR rejects invalid input when converting from text:
INSERT INTO INET_TBL (c, i) VALUES (cidr('192.168.1.2/30'), '192.168.1.226');
INSERT INTO INET_TBL (c, i) VALUES (cidr('ffff:ffff:ffff:ffff::/24'), '::192.168.1.226');
SELECT '' AS ten, c AS cidr, i AS inet FROM INET_TBL;

-- now test some support functions

SELECT '' AS ten, i AS inet, host(i), text(i), family(i) FROM INET_TBL;
SELECT '' AS ten, c AS cidr, broadcast(c),
  i AS inet, broadcast(i) FROM INET_TBL;
SELECT '' AS ten, c AS cidr, network(c) AS "network(cidr)",
  i AS inet, network(i) AS "network(inet)" FROM INET_TBL;
SELECT '' AS ten, c AS cidr, masklen(c) AS "masklen(cidr)",
  i AS inet, masklen(i) AS "masklen(inet)" FROM INET_TBL;

SELECT '' AS four, c AS cidr, masklen(c) AS "masklen(cidr)",
  i AS inet, masklen(i) AS "masklen(inet)" FROM INET_TBL
  WHERE masklen(c) <= 8;

SELECT '' AS six, c AS cidr, i AS inet FROM INET_TBL
  WHERE c = i;

SELECT '' AS ten, i, c,
  i < c AS lt, i <= c AS le, i = c AS eq,
  i >= c AS ge, i > c AS gt, i <> c AS ne,
  i << c AS sb, i <<= c AS sbe,
  i >> c AS sup, i >>= c AS spe,
  i && c AS ovr
  FROM INET_TBL;

SELECT max(i) AS max, min(i) AS min FROM INET_TBL;
SELECT max(c) AS max, min(c) AS min FROM INET_TBL;

-- check the conversion to/from text and set_netmask
SELECT '' AS ten, set_masklen(inet(text(i)), 24) FROM INET_TBL;

-- check that btree index works correctly
CREATE INDEX inet_idx1 ON inet_tbl(i);
SET enable_seqscan TO off;
SELECT * FROM inet_tbl WHERE i<<'192.168.1.0/24'::cidr;
SELECT * FROM inet_tbl WHERE i<<='192.168.1.0/24'::cidr;
SET enable_seqscan TO on;
DROP INDEX inet_idx1;

-- check that gist index works correctly
CREATE INDEX inet_idx2 ON inet_tbl using gist (i inet_ops);
SET enable_seqscan TO off;
SELECT * FROM inet_tbl WHERE i << '192.168.1.0/24'::cidr ORDER BY i;
SELECT * FROM inet_tbl WHERE i <<= '192.168.1.0/24'::cidr ORDER BY i;
SELECT * FROM inet_tbl WHERE i && '192.168.1.0/24'::cidr ORDER BY i;
SELECT * FROM inet_tbl WHERE i >>= '192.168.1.0/24'::cidr ORDER BY i;
SELECT * FROM inet_tbl WHERE i >> '192.168.1.0/24'::cidr ORDER BY i;
SELECT * FROM inet_tbl WHERE i < '192.168.1.0/24'::cidr ORDER BY i;
SELECT * FROM inet_tbl WHERE i <= '192.168.1.0/24'::cidr ORDER BY i;
SELECT * FROM inet_tbl WHERE i = '192.168.1.0/24'::cidr ORDER BY i;
SELECT * FROM inet_tbl WHERE i >= '192.168.1.0/24'::cidr ORDER BY i;
SELECT * FROM inet_tbl WHERE i > '192.168.1.0/24'::cidr ORDER BY i;
SELECT * FROM inet_tbl WHERE i <> '192.168.1.0/24'::cidr ORDER BY i;

-- test index-only scans
EXPLAIN (COSTS OFF)
SELECT i FROM inet_tbl WHERE i << '192.168.1.0/24'::cidr ORDER BY i;
SELECT i FROM inet_tbl WHERE i << '192.168.1.0/24'::cidr ORDER BY i;

SET enable_seqscan TO on;
DROP INDEX inet_idx2;

-- simple tests of inet boolean and arithmetic operators
SELECT i, ~i AS "~i" FROM inet_tbl;
SELECT i, c, i & c AS "and" FROM inet_tbl;
SELECT i, c, i | c AS "or" FROM inet_tbl;
SELECT i, i + 500 AS "i+500" FROM inet_tbl;
SELECT i, i - 500 AS "i-500" FROM inet_tbl;
SELECT i, c, i - c AS "minus" FROM inet_tbl;
SELECT '127.0.0.1'::inet + 257;
SELECT ('127.0.0.1'::inet + 257) - 257;
SELECT '127::1'::inet + 257;
SELECT ('127::1'::inet + 257) - 257;
SELECT '127.0.0.2'::inet  - ('127.0.0.2'::inet + 500);
SELECT '127.0.0.2'::inet  - ('127.0.0.2'::inet - 500);
SELECT '127::2'::inet  - ('127::2'::inet + 500);
SELECT '127::2'::inet  - ('127::2'::inet - 500);
-- these should give overflow errors:
SELECT '127.0.0.1'::inet + 10000000000;
SELECT '127.0.0.1'::inet - 10000000000;
SELECT '126::1'::inet - '127::2'::inet;
SELECT '127::1'::inet - '126::2'::inet;
-- but not these
SELECT '127::1'::inet + 10000000000;
SELECT '127::1'::inet - '127::2'::inet;

-- insert one more row with addressed from different families
INSERT INTO INET_TBL (c, i) VALUES ('10', '10::/8');
-- now, this one should fail
SELECT inet_merge(c, i) FROM INET_TBL;
-- fix it by inet_same_family() condition
SELECT inet_merge(c, i) FROM INET_TBL WHERE inet_same_family(c, i);

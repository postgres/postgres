--
-- INET
--

-- prepare the table...

DROP TABLE INET_TBL;
CREATE TABLE INET_TBL (c cidr, i inet);
INSERT INTO INET_TBL (c, i) VALUES ('192.168.1', '192.168.1.226/24');
INSERT INTO INET_TBL (c, i) VALUES ('192.168.1.0/24', '192.168.1.226');
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
-- check that CIDR rejects invalid input:
INSERT INTO INET_TBL (c, i) VALUES ('192.168.1.2/24', '192.168.1.226');
-- check that CIDR rejects invalid input when converting from text:
INSERT INTO INET_TBL (c, i) VALUES (cidr('192.168.1.2/24'), '192.168.1.226');

SELECT '' AS ten, c AS cidr, i AS inet FROM INET_TBL;

-- now test some support functions

SELECT '' AS ten, i AS inet, host(i), text(i) FROM INET_TBL;
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
  i >> c AS sup, i >>= c AS spe
  FROM INET_TBL;

-- check the conversion to/from text and set_netmask
select '' AS ten, set_masklen(inet(text(i)), 24) FROM INET_TBL;
-- check that index works correctly
create index inet_idx1 on inet_tbl(i);
set enable_seqscan to off;
select * from inet_tbl where i<<'192.168.1.0/24'::cidr;
select * from inet_tbl where i<<='192.168.1.0/24'::cidr;
set enable_seqscan to on;
drop index inet_idx1;


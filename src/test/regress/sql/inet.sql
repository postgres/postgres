-- INET regression tests
--

-- prepare the table...

DROP TABLE INET_TBL;
CREATE TABLE INET_TBL (c cidr, i inet);
INSERT INTO INET_TBL (c, i) VALUES ('192.168.1', '192.168.1.226/24');
INSERT INTO INET_TBL (c, i) VALUES ('192.168.1.2/24', '192.168.1.226');
INSERT INTO INET_TBL (c, i) VALUES ('10', '10.1.2.3/8');
INSERT INTO INET_TBL (c, i) VALUES ('10.0.0.0', '10.1.2.3/8');
INSERT INTO INET_TBL (c, i) VALUES ('10.1.2.3', '10.1.2.3/32');
INSERT INTO INET_TBL (c, i) VALUES ('10.1.2', '10.1.2.3/24');
INSERT INTO INET_TBL (c, i) VALUES ('10.1', '10.1.2.3/16');
INSERT INTO INET_TBL (c, i) VALUES ('10', '10.1.2.3/8');

SELECT '' as eight, c as cidr, i as inet FROM INET_TBL;

-- now test some support functions

SELECT '' as eight, i as inet, host(i) FROM INET_TBL;
SELECT '' as eight, c as cidr, broadcast(c),
  i as inet, broadcast(i) FROM INET_TBL;
SELECT '' as eight, c as cidr, network(c) as "network(cidr)",
  i as inet, network(i) as "network(inet)" FROM INET_TBL;
SELECT '' as eight, c as cidr, masklen(c) as "masklen(cidr)",
  i as inet, masklen(i) as "masklen(inet)" FROM INET_TBL;

SELECT '' as two, c as cidr, masklen(c) as "masklen(cidr)",
  i as inet, masklen(i) as "masklen(inet)" FROM INET_TBL
  WHERE masklen(c) <= 8;

SELECT '' as six, c as cidr, i as inet FROM INET_TBL
  WHERE c = i;


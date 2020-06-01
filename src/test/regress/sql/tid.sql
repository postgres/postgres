-- tests for functions related to TID handling

CREATE TABLE tid_tab (a int);

-- min() and max() for TIDs
INSERT INTO tid_tab VALUES (1), (2);
SELECT min(ctid) FROM tid_tab;
SELECT max(ctid) FROM tid_tab;
TRUNCATE tid_tab;

-- Tests for currtid() and currtid2() with various relation kinds

-- Materialized view
CREATE MATERIALIZED VIEW tid_matview AS SELECT a FROM tid_tab;
SELECT currtid('tid_matview'::regclass::oid, '(0,1)'::tid); -- fails
SELECT currtid2('tid_matview'::text, '(0,1)'::tid); -- fails
INSERT INTO tid_tab VALUES (1);
REFRESH MATERIALIZED VIEW tid_matview;
SELECT currtid('tid_matview'::regclass::oid, '(0,1)'::tid); -- ok
SELECT currtid2('tid_matview'::text, '(0,1)'::tid); -- ok
DROP MATERIALIZED VIEW tid_matview;
TRUNCATE tid_tab;

-- Sequence
CREATE SEQUENCE tid_seq;
SELECT currtid('tid_seq'::regclass::oid, '(0,1)'::tid); -- ok
SELECT currtid2('tid_seq'::text, '(0,1)'::tid); -- ok
DROP SEQUENCE tid_seq;

-- Index, fails with incorrect relation type
CREATE INDEX tid_ind ON tid_tab(a);
SELECT currtid('tid_ind'::regclass::oid, '(0,1)'::tid); -- fails
SELECT currtid2('tid_ind'::text, '(0,1)'::tid); -- fails
DROP INDEX tid_ind;

-- Partitioned table, no storage
CREATE TABLE tid_part (a int) PARTITION BY RANGE (a);
SELECT currtid('tid_part'::regclass::oid, '(0,1)'::tid); -- fails
SELECT currtid2('tid_part'::text, '(0,1)'::tid); -- fails
DROP TABLE tid_part;

-- Views
-- ctid not defined in the view
CREATE VIEW tid_view_no_ctid AS SELECT a FROM tid_tab;
SELECT currtid('tid_view_no_ctid'::regclass::oid, '(0,1)'::tid); -- fails
SELECT currtid2('tid_view_no_ctid'::text, '(0,1)'::tid); -- fails
DROP VIEW tid_view_no_ctid;
-- ctid fetched directly from the source table.
CREATE VIEW tid_view_with_ctid AS SELECT ctid, a FROM tid_tab;
SELECT currtid('tid_view_with_ctid'::regclass::oid, '(0,1)'::tid); -- fails
SELECT currtid2('tid_view_with_ctid'::text, '(0,1)'::tid); -- fails
INSERT INTO tid_tab VALUES (1);
SELECT currtid('tid_view_with_ctid'::regclass::oid, '(0,1)'::tid); -- ok
SELECT currtid2('tid_view_with_ctid'::text, '(0,1)'::tid); -- ok
DROP VIEW tid_view_with_ctid;
TRUNCATE tid_tab;
-- ctid attribute with incorrect data type
CREATE VIEW tid_view_fake_ctid AS SELECT 1 AS ctid, 2 AS a;
SELECT currtid('tid_view_fake_ctid'::regclass::oid, '(0,1)'::tid); -- fails
SELECT currtid2('tid_view_fake_ctid'::text, '(0,1)'::tid); -- fails
DROP VIEW tid_view_fake_ctid;

DROP TABLE tid_tab CASCADE;

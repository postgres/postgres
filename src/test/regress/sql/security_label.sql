--
-- Test for facilities of security label
--

-- initial setups
SET client_min_messages TO 'warning';

DROP ROLE IF EXISTS seclabel_user1;
DROP ROLE IF EXISTS seclabel_user2;

DROP TABLE IF EXISTS seclabel_tbl1;
DROP TABLE IF EXISTS seclabel_tbl2;
DROP TABLE IF EXISTS seclabel_tbl3;

CREATE USER seclabel_user1 WITH CREATEROLE;
CREATE USER seclabel_user2;

CREATE TABLE seclabel_tbl1 (a int, b text);
CREATE TABLE seclabel_tbl2 (x int, y text);
CREATE VIEW seclabel_view1 AS SELECT * FROM seclabel_tbl2;
CREATE FUNCTION seclabel_four() RETURNS integer AS $$SELECT 4$$ language sql;
CREATE DOMAIN seclabel_domain AS text;

ALTER TABLE seclabel_tbl1 OWNER TO seclabel_user1;
ALTER TABLE seclabel_tbl2 OWNER TO seclabel_user2;

RESET client_min_messages;

--
-- Test of SECURITY LABEL statement without a plugin
--
SECURITY LABEL ON TABLE seclabel_tbl1 IS 'classified';			-- fail
SECURITY LABEL FOR 'dummy' ON TABLE seclabel_tbl1 IS 'classified';		-- fail
SECURITY LABEL ON TABLE seclabel_tbl1 IS '...invalid label...';		-- fail
SECURITY LABEL ON TABLE seclabel_tbl3 IS 'unclassified';			-- fail

SECURITY LABEL ON ROLE seclabel_user1 IS 'classified';			-- fail
SECURITY LABEL FOR 'dummy' ON ROLE seclabel_user1 IS 'classified';		-- fail
SECURITY LABEL ON ROLE seclabel_user1 IS '...invalid label...';		-- fail
SECURITY LABEL ON ROLE seclabel_user3 IS 'unclassified';			-- fail

-- clean up objects
DROP FUNCTION seclabel_four();
DROP DOMAIN seclabel_domain;
DROP VIEW seclabel_view1;
DROP TABLE seclabel_tbl1;
DROP TABLE seclabel_tbl2;
DROP USER seclabel_user1;
DROP USER seclabel_user2;

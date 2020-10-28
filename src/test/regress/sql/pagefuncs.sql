--
-- Tests for functions related to relation pages
--

-- Restricted to superusers by default
CREATE ROLE regress_pgfunc_user;
SET ROLE regress_pgfunc_user;
SELECT pg_relation_check_pages('pg_class'); -- error
SELECT pg_relation_check_pages('pg_class', 'main'); -- error
RESET ROLE;
DROP ROLE regress_pgfunc_user;

-- NULL and simple sanity checks
SELECT pg_relation_check_pages(NULL); -- empty result
SELECT pg_relation_check_pages(NULL, NULL); -- empty result
SELECT pg_relation_check_pages('pg_class', 'invalid_fork'); -- error

-- Relation types that are supported
CREATE TABLE pgfunc_test_tab (id int);
CREATE INDEX pgfunc_test_ind ON pgfunc_test_tab(id);
INSERT INTO pgfunc_test_tab VALUES (generate_series(1,1000));
SELECT pg_relation_check_pages('pgfunc_test_tab');
SELECT pg_relation_check_pages('pgfunc_test_ind');
DROP TABLE pgfunc_test_tab;

CREATE MATERIALIZED VIEW pgfunc_test_matview AS SELECT 1;
SELECT pg_relation_check_pages('pgfunc_test_matview');
DROP MATERIALIZED VIEW pgfunc_test_matview;
CREATE SEQUENCE pgfunc_test_seq;
SELECT pg_relation_check_pages('pgfunc_test_seq');
DROP SEQUENCE pgfunc_test_seq;

-- pg_relation_check_pages() returns no results if passed relations that
-- do not support the operation, like relations without storage or temporary
-- relations.
CREATE TEMPORARY TABLE pgfunc_test_temp AS SELECT generate_series(1,10) AS a;
SELECT pg_relation_check_pages('pgfunc_test_temp');
DROP TABLE pgfunc_test_temp;
CREATE VIEW pgfunc_test_view AS SELECT 1;
SELECT pg_relation_check_pages('pgfunc_test_view');
DROP VIEW pgfunc_test_view;

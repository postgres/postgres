-- A test case for https://github.com/percona/pg_tde/pull/21
-- 
CREATE EXTENSION pg_tde;

SELECT pg_tde_add_key_provider_file('file-vault','/tmp/pg_tde_test_keyring.per');
SELECT pg_tde_set_principal_key('test-db-principal-key','file-vault');

DROP TABLE IF EXISTS sbtest1;
CREATE TABLE sbtest1(
	  id SERIAL,
	  k INTEGER DEFAULT '0' NOT NULL,
	  PRIMARY KEY (id)
	) USING :tde_am;

INSERT INTO sbtest1(k) VALUES
(1),
(2),
(3),
(4),
(5),
(6),
(7),
(8),
(9),
(10);
DELETE FROM sbtest1 WHERE id IN (4,5,6);

VACUUM sbtest1;

INSERT INTO sbtest1(k) VALUES
(11),
(12),
(13);

-- Line pointers (lp) point to non-sorted offsets (lp_off):
-- CREATE EXTENSION pageinspect;
-- SELECT lp, lp_off, t_ctid FROM heap_page_items(get_raw_page('sbtest1', 0));
--  lp | lp_off | t_ctid
-- ----+--------+--------
--   1 |   8160 | (0,1)
--   2 |   8128 | (0,2)
--   3 |   8096 | (0,3)
--   4 |   7936 | (0,4)
--   5 |   7904 | (0,5)
--   6 |   7872 | (0,6)
--   7 |   8064 | (0,7)
--   8 |   8032 | (0,8)
--   9 |   8000 | (0,9)
--  10 |   7968 | (0,10)

---- Trigger comapction
delete from sbtest1 where id in (2);
VACUUM sbtest1;

DROP TABLE sbtest1;
DROP EXTENSION pg_tde;

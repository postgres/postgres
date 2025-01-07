CREATE EXTENSION pg_tde;

SELECT pg_tde_add_key_provider_file('file-vault','/tmp/pg_tde_test_keyring.per');
SELECT pg_tde_set_principal_key('test-db-principal-key','file-vault');
\getenv abs_srcdir PG_ABS_SRCDIR

CREATE TABLE tenk1 (
	unique1		int4,
	unique2		int4,
	two			int4,
	four		int4,
	ten			int4,
	twenty		int4,
	hundred		int4,
	thousand	int4,
	twothousand	int4,
	fivethous	int4,
	tenthous	int4,
	odd			int4,
	even		int4,
	stringu1	name,
	stringu2	name,
	string4		name
) using :tde_am;

\set filename :abs_srcdir '/data/tenk.data'
COPY tenk1 FROM :'filename';
VACUUM ANALYZE tenk1;

CREATE INDEX tenk1_unique1 ON tenk1 USING btree(unique1 int4_ops);

CREATE INDEX tenk1_unique2 ON tenk1 USING btree(unique2 int4_ops);

CREATE INDEX tenk1_hundred ON tenk1 USING btree(hundred int4_ops);

CREATE INDEX tenk1_thous_tenthous ON tenk1 (thousand, tenthous);

--
-- regression test: check a case where join_clause_is_movable_into()
-- used to give an imprecise result, causing an assertion failure
--
SELECT count(*)
FROM
  (SELECT t3.tenthous as x1, coalesce(t1.stringu1, t2.stringu1) as x2
   FROM tenk1 t1
   LEFT JOIN tenk1 t2 on t1.unique1 = t2.unique1
   JOIN tenk1 t3 on t1.unique2 = t3.unique2) ss,
  tenk1 t4,
  tenk1 t5
WHERE t4.thousand = t5.unique1 and ss.x1 = t4.tenthous and ss.x2 = t5.stringu1;

--
-- check that we haven't screwed the data
--
SELECT *
FROM
  (SELECT t3.tenthous as x1, coalesce(t1.stringu1, t2.stringu1) as x2
   FROM tenk1 t1
   LEFT JOIN tenk1 t2 on t1.unique1 = t2.unique1
   JOIN tenk1 t3 on t1.unique2 = t3.unique2) ss,
  tenk1 t4,
  tenk1 t5
WHERE t4.thousand = t5.unique1 and ss.x1 = t4.tenthous and ss.x2 = t5.stringu1 LIMIT 20 OFFSET 432;

DROP TABLE tenk1;
DROP EXTENSION pg_tde;

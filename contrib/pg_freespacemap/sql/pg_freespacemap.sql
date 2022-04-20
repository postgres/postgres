CREATE EXTENSION pg_freespacemap;

CREATE TABLE freespace_tab (c1 int) WITH (autovacuum_enabled = off);
CREATE INDEX freespace_brin ON freespace_tab USING brin (c1);
CREATE INDEX freespace_btree ON freespace_tab USING btree (c1);
CREATE INDEX freespace_hash ON freespace_tab USING hash (c1);

-- report all the sizes of the FSMs for all the relation blocks.
WITH rel AS (SELECT oid::regclass AS id FROM pg_class WHERE relname ~ 'freespace')
  SELECT rel.id, fsm.blkno, (fsm.avail > 0) AS is_avail
    FROM rel, LATERAL pg_freespace(rel.id) AS fsm
    ORDER BY 1, 2;

INSERT INTO freespace_tab VALUES (1);
VACUUM freespace_tab;
WITH rel AS (SELECT oid::regclass AS id FROM pg_class WHERE relname ~ 'freespace')
  SELECT rel.id, fsm.blkno, (fsm.avail > 0) AS is_avail
    FROM rel, LATERAL pg_freespace(rel.id) AS fsm
    ORDER BY 1, 2;

DELETE FROM freespace_tab;
VACUUM freespace_tab;
WITH rel AS (SELECT oid::regclass AS id FROM pg_class WHERE relname ~ 'freespace')
  SELECT rel.id, fsm.blkno, (fsm.avail > 0) AS is_avail
    FROM rel, LATERAL pg_freespace(rel.id) AS fsm
    ORDER BY 1, 2;

-- failures with incorrect block number
SELECT * FROM pg_freespace('freespace_tab', -1);
SELECT * FROM pg_freespace('freespace_tab', 4294967295);

DROP TABLE freespace_tab;

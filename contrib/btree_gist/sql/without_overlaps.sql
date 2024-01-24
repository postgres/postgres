-- Core must test WITHOUT OVERLAPS
-- with an int4range + daterange,
-- so here we do some simple tests
-- to make sure int + daterange works too,
-- since that is the expected use-case.
CREATE TABLE temporal_rng (
  id integer,
  valid_at daterange,
  CONSTRAINT temporal_rng_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS)
);
\d temporal_rng
SELECT pg_get_constraintdef(oid) FROM pg_constraint WHERE conname = 'temporal_rng_pk';
SELECT pg_get_indexdef(conindid, 0, true) FROM pg_constraint WHERE conname = 'temporal_rng_pk';

INSERT INTO temporal_rng VALUES
  (1, '[2000-01-01,2001-01-01)');
-- same key, doesn't overlap:
INSERT INTO temporal_rng VALUES
  (1, '[2001-01-01,2002-01-01)');
-- overlaps but different key:
INSERT INTO temporal_rng VALUES
  (2, '[2000-01-01,2001-01-01)');
-- should fail:
INSERT INTO temporal_rng VALUES
  (1, '[2000-06-01,2001-01-01)');

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

-- Foreign key
CREATE TABLE temporal_fk_rng2rng (
  id integer,
  valid_at daterange,
  parent_id integer,
  CONSTRAINT temporal_fk_rng2rng_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS),
  CONSTRAINT temporal_fk_rng2rng_fk FOREIGN KEY (parent_id, PERIOD valid_at)
    REFERENCES temporal_rng (id, PERIOD valid_at)
);
\d temporal_fk_rng2rng
SELECT pg_get_constraintdef(oid) FROM pg_constraint WHERE conname = 'temporal_fk_rng2rng_fk';

-- okay
INSERT INTO temporal_fk_rng2rng VALUES
  (1, '[2000-01-01,2001-01-01)', 1);
-- okay spanning two parent records:
INSERT INTO temporal_fk_rng2rng VALUES
  (2, '[2000-01-01,2002-01-01)', 1);
-- key is missing
INSERT INTO temporal_fk_rng2rng VALUES
  (3, '[2000-01-01,2001-01-01)', 3);
-- key exist but is outside range
INSERT INTO temporal_fk_rng2rng VALUES
  (4, '[2001-01-01,2002-01-01)', 2);
-- key exist but is partly outside range
INSERT INTO temporal_fk_rng2rng VALUES
  (5, '[2000-01-01,2002-01-01)', 2);

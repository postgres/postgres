-- These tests display internal details that would not be stable under
-- debug_parallel_query, so make sure that option is disabled.
SET debug_parallel_query = off;

-- Make sure that we don't print any JIT-related information, as that
-- would also make results unstable.
SET jit = off;

-- These options do not exist, so these queries should all fail.
EXPLAIN (DEBUFF) SELECT 1;
EXPLAIN (DEBUG) SELECT 1;
EXPLAIN (RANGE_TABLE) SELECT 1;

-- Load the module that creates the options.
LOAD 'pg_overexplain';

-- The first option still does not exist, but the others do.
EXPLAIN (DEBUFF) SELECT 1;
EXPLAIN (DEBUG) SELECT 1;
EXPLAIN (RANGE_TABLE) SELECT 1;

-- Create a partitioned table.
CREATE TABLE vegetables (id serial, name text, genus text)
PARTITION BY LIST (genus);
CREATE TABLE daucus PARTITION OF vegetables FOR VALUES IN ('daucus');
CREATE TABLE brassica PARTITION OF vegetables FOR VALUES IN ('brassica');
INSERT INTO vegetables (name, genus)
	VALUES ('carrot', 'daucus'), ('bok choy', 'brassica'),
		   ('brocooli', 'brassica'), ('cauliflower', 'brassica'),
		   ('cabbage', 'brassica'), ('kohlrabi', 'brassica'),
		   ('rutabaga', 'brassica'), ('turnip', 'brassica');
VACUUM ANALYZE vegetables;

-- We filter relation OIDs out of the test output in order to avoid
-- test instability. This is currently only needed for EXPLAIN (DEBUG), not
-- EXPLAIN (RANGE_TABLE). Also suppress actual row counts, which are not
-- stable (e.g. 1/8 is 0.12 on some buildfarm machines and 0.13 on others).
CREATE FUNCTION explain_filter(text) RETURNS SETOF text
LANGUAGE plpgsql AS
$$
DECLARE
    ln text;
BEGIN
    FOR ln IN EXECUTE $1
	LOOP
		ln := regexp_replace(ln, 'Relation OIDs:( \m\d+\M)+',
								 'Relation OIDs: NNN...', 'g');
		ln := regexp_replace(ln, '<Relation-OIDs>( ?\m\d+\M)+</Relation-OIDs>',
								 '<Relation-OIDs>NNN...</Relation-OIDs>', 'g');
		ln := regexp_replace(ln, 'actual rows=\d+\.\d+',
								 'actual rows=N.NN', 'g');
		RETURN NEXT ln;
	END LOOP;
END;
$$;

-- Test with both options together and an aggregate.
SELECT explain_filter($$
EXPLAIN (DEBUG, RANGE_TABLE, COSTS OFF)
SELECT genus, array_agg(name ORDER BY name) FROM vegetables GROUP BY genus
$$);

-- Test a different output format.
SELECT explain_filter($$
EXPLAIN (DEBUG, RANGE_TABLE, FORMAT XML, COSTS OFF)
SELECT genus, array_agg(name ORDER BY name) FROM vegetables GROUP BY genus
$$);

-- Test just the DEBUG option. Verify that it shows information about
-- disabled nodes, parallel safety, and the parallelModeNeeded flag.
SET enable_seqscan = false;
SET debug_parallel_query = true;
SELECT explain_filter($$
EXPLAIN (DEBUG, COSTS OFF)
SELECT genus, array_agg(name ORDER BY name) FROM vegetables GROUP BY genus
$$);
SET debug_parallel_query = false;
RESET enable_seqscan;

-- Test the DEBUG option with a non-SELECT query, and also verify that the
-- hasReturning flag is shown.
SELECT explain_filter($$
EXPLAIN (DEBUG, COSTS OFF)
INSERT INTO vegetables (name, genus)
	VALUES ('Brotero''s carrot', 'brassica') RETURNING id
$$);

-- Create an index, and then attempt to force a nested loop with inner index
-- scan so that we can see parameter-related information. Also, let's try
-- actually running the query, but try to suppress potentially variable output.
CREATE INDEX ON vegetables (id);
ANALYZE vegetables;
SET enable_hashjoin = false;
SET enable_material = false;
SET enable_mergejoin = false;
SET enable_seqscan = false;
SELECT explain_filter($$
EXPLAIN (BUFFERS OFF, COSTS OFF, SUMMARY OFF, TIMING OFF, ANALYZE, DEBUG)
SELECT * FROM vegetables v1, vegetables v2 WHERE v1.id = v2.id;
$$);
RESET enable_hashjoin;
RESET enable_material;
RESET enable_mergejoin;
RESET enable_seqscan;

-- Test the RANGE_TABLE option with a case that allows partition pruning.
EXPLAIN (RANGE_TABLE, COSTS OFF)
SELECT * FROM vegetables WHERE genus = 'daucus';

-- Also test a case that involves a write.
EXPLAIN (RANGE_TABLE, COSTS OFF)
INSERT INTO vegetables (name, genus) VALUES ('broccoflower', 'brassica');

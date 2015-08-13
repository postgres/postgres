# This test verifies that values inserted in transactions still in progress
# are considered during concurrent range summarization (either using the
# brin_summarize_new_values function or regular VACUUM).

setup
{
    CREATE TABLE brin_iso (
        value int
    ) WITH (fillfactor=10);
    CREATE INDEX brinidx ON brin_iso USING brin (value) WITH (pages_per_range=1);
    -- this fills the first page
    DO $$
    DECLARE curtid tid;
    BEGIN
      LOOP
        INSERT INTO brin_iso VALUES (1) RETURNING ctid INTO curtid;
        EXIT WHEN curtid > tid '(1, 0)';
      END LOOP;
    END;
    $$;
    CREATE EXTENSION IF NOT EXISTS pageinspect;
}

teardown
{
    DROP TABLE brin_iso;
}

session "s1"
step "s1b"		{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step "s1i"		{ INSERT INTO brin_iso VALUES (1000); }
step "s1c"		{ COMMIT; }

session "s2"
step "s2b"		{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1; }
step "s2summ"	{ SELECT brin_summarize_new_values('brinidx'::regclass); }
step "s2c"		{ COMMIT; }

step "s2vacuum"	{ VACUUM brin_iso; }

step "s2check"	{ SELECT * FROM brin_page_items(get_raw_page('brinidx', 2), 'brinidx'::regclass); }

permutation "s2check" "s1b" "s2b" "s1i" "s2summ" "s1c" "s2c" "s2check"
permutation "s2check" "s1b" "s1i" "s2vacuum" "s1c" "s2check"

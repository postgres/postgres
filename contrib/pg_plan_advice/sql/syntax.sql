LOAD 'pg_plan_advice';

-- An empty string is allowed. Empty target lists are allowed for most advice
-- tags, but not for JOIN_ORDER. "Supplied Plan Advice" should be omitted in
-- text format when there is no actual advice, but not in non-text format.
SET pg_plan_advice.advice = '';
EXPLAIN (COSTS OFF) SELECT 1;
SET pg_plan_advice.advice = 'SEQ_SCAN()';
EXPLAIN (COSTS OFF) SELECT 1;
SET pg_plan_advice.advice = 'NESTED_LOOP_PLAIN()';
EXPLAIN (COSTS OFF, FORMAT JSON) SELECT 1;
SET pg_plan_advice.advice = 'JOIN_ORDER()';

-- Test assorted variations in capitalization, whitespace, and which parts of
-- the relation identifier are included. These should all work.
SET pg_plan_advice.advice = 'SEQ_SCAN(x)';
EXPLAIN (COSTS OFF) SELECT 1;
SET pg_plan_advice.advice = 'seq_scan(x@y)';
EXPLAIN (COSTS OFF) SELECT 1;
SET pg_plan_advice.advice = 'SEQ_scan(x#2)';
EXPLAIN (COSTS OFF) SELECT 1;
SET pg_plan_advice.advice = 'SEQ_SCAN (x/y)';
EXPLAIN (COSTS OFF) SELECT 1;
SET pg_plan_advice.advice = '  SEQ_SCAN ( x / y . z )  ';
EXPLAIN (COSTS OFF) SELECT 1;
SET pg_plan_advice.advice = 'SEQ_SCAN("x"#2/"y"."z"@"t")';
EXPLAIN (COSTS OFF) SELECT 1;

-- Syntax errors.
SET pg_plan_advice.advice = 'SEQUENTIAL_SCAN(x)';
SET pg_plan_advice.advice = 'SEQ_SCAN';
SET pg_plan_advice.advice = 'SEQ_SCAN(';
SET pg_plan_advice.advice = 'SEQ_SCAN("';
SET pg_plan_advice.advice = 'SEQ_SCAN("")';
SET pg_plan_advice.advice = 'SEQ_SCAN("a"';
SET pg_plan_advice.advice = 'SEQ_SCAN(#';
SET pg_plan_advice.advice = '()';
SET pg_plan_advice.advice = '123';

-- Tags like SEQ_SCAN and NO_GATHER don't allow sublists at all; other tags,
-- except for JOIN_ORDER, allow at most one level of sublist. Hence, these
-- examples should error out.
SET pg_plan_advice.advice = 'SEQ_SCAN((x))';
SET pg_plan_advice.advice = 'GATHER(((x)))';

-- Legal comments.
SET pg_plan_advice.advice = '/**/';
EXPLAIN (COSTS OFF) SELECT 1;
SET pg_plan_advice.advice = 'HASH_JOIN(_)/***/';
EXPLAIN (COSTS OFF) SELECT 1;
SET pg_plan_advice.advice = '/* comment */ HASH_JOIN(/*x*/y)';
EXPLAIN (COSTS OFF) SELECT 1;
SET pg_plan_advice.advice = '/* comment */ HASH_JOIN(y//*x*/z)';
EXPLAIN (COSTS OFF) SELECT 1;

-- Unterminated comments.
SET pg_plan_advice.advice = '/*';
SET pg_plan_advice.advice = 'JOIN_ORDER("fOO") /* oops';

-- Nested comments are not supported, so the first of these is legal and
-- the second is not.
SET pg_plan_advice.advice = '/*/*/';
EXPLAIN (COSTS OFF) SELECT 1;
SET pg_plan_advice.advice = '/*/* stuff */*/';

-- Foreign join requires multiple relation identifiers.
SET pg_plan_advice.advice = 'FOREIGN_JOIN(a)';
SET pg_plan_advice.advice = 'FOREIGN_JOIN((a))';

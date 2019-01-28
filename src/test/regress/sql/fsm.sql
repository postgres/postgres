--
-- Free Space Map test
--

CREATE TABLE fsm_check_size (num int, str text);

-- Fill 3 blocks with as many large records as will fit
-- No FSM
INSERT INTO fsm_check_size SELECT i, rpad('', 1024, 'a')
FROM generate_series(1,7*3) i;
VACUUM fsm_check_size;
SELECT pg_relation_size('fsm_check_size', 'main') AS heap_size,
pg_relation_size('fsm_check_size', 'fsm') AS fsm_size;

-- Clear some space on block 0
DELETE FROM fsm_check_size WHERE num <= 5;
VACUUM fsm_check_size;

-- Insert small record in block 2 to set the cached smgr targetBlock
INSERT INTO fsm_check_size VALUES(99, 'b');

-- Insert large record and make sure it goes in block 0 rather than
-- causing the relation to extend
INSERT INTO fsm_check_size VALUES (101, rpad('', 1024, 'a'));
SELECT pg_relation_size('fsm_check_size', 'main') AS heap_size,
pg_relation_size('fsm_check_size', 'fsm') AS fsm_size;

-- Extend table with enough blocks to exceed the FSM threshold
-- FSM is created and extended to 3 blocks
INSERT INTO fsm_check_size SELECT i, 'c' FROM generate_series(200,1200) i;
VACUUM fsm_check_size;
SELECT pg_relation_size('fsm_check_size', 'fsm') AS fsm_size;

-- Truncate heap to 1 block
-- No change in FSM
DELETE FROM fsm_check_size WHERE num > 7;
VACUUM fsm_check_size;
SELECT pg_relation_size('fsm_check_size', 'fsm') AS fsm_size;

-- Truncate heap to 0 blocks
-- FSM now truncated to 2 blocks
DELETE FROM fsm_check_size;
VACUUM fsm_check_size;
SELECT pg_relation_size('fsm_check_size', 'fsm') AS fsm_size;

-- Add long random string to extend TOAST table to 1 block
INSERT INTO fsm_check_size
VALUES(0, (SELECT string_agg(md5(chr(i)), '')
		   FROM generate_series(1,100) i));
VACUUM fsm_check_size;
SELECT pg_relation_size(reltoastrelid, 'main') AS toast_size,
pg_relation_size(reltoastrelid, 'fsm') AS toast_fsm_size
FROM pg_class WHERE relname = 'fsm_check_size';

DROP TABLE fsm_check_size;

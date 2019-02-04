--
-- Free Space Map test
--

CREATE TABLE fsm_check_size (num int, str text);

-- With one block, there should be no FSM
INSERT INTO fsm_check_size VALUES(1, 'a');

VACUUM fsm_check_size;
SELECT pg_relation_size('fsm_check_size', 'main') AS heap_size,
pg_relation_size('fsm_check_size', 'fsm') AS fsm_size;

-- Extend table with enough blocks to exceed the FSM threshold
DO $$
DECLARE curtid tid;
num int;
BEGIN
num = 11;
  LOOP
    INSERT INTO fsm_check_size VALUES (num, 'b') RETURNING ctid INTO curtid;
    EXIT WHEN curtid >= tid '(4, 0)';
    num = num + 1;
  END LOOP;
END;
$$;

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

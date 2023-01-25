CREATE TABLE test (a INT);
SELECT 1 FROM pg_switch_wal();

DO $$
DECLARE
	archived bool;
	loops int := 0;
BEGIN
	LOOP
		archived := count(*) > 0 FROM pg_ls_dir('.', false, false) a
			WHERE a ~ '^[0-9A-F]{24}$';
		IF archived OR loops > 120 * 10 THEN EXIT; END IF;
		PERFORM pg_sleep(0.1);
		loops := loops + 1;
	END LOOP;
END
$$;

SELECT count(*) > 0 FROM pg_ls_dir('.', false, false) a
	WHERE a ~ '^[0-9A-F]{24}$';

DROP TABLE test;

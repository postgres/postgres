setup
{
	CREATE TABLE t1 (a serial, b integer, c text);
	INSERT INTO t1 (b, c) VALUES (generate_series(1,10000), 'starting values');

	CREATE OR REPLACE FUNCTION insert_1k(iterations int) RETURNS boolean AS $$
	DECLARE
		counter integer;
	BEGIN
		FOR counter IN 1..$1 LOOP
			INSERT INTO t1 (b, c) VALUES (
				generate_series(1, 1000),
				array_to_string(array(select chr(97 + (random() * 25)::int) from generate_series(1,250)), '')
			);
			PERFORM pg_sleep(0.1);
		END LOOP;
		RETURN True;
	END;
	$$ LANGUAGE plpgsql;
	
	CREATE OR REPLACE FUNCTION test_checksums_on() RETURNS boolean AS $$
	DECLARE
		enabled boolean;
	BEGIN
		LOOP
			SELECT setting = 'on' INTO enabled FROM pg_catalog.pg_settings WHERE name = 'data_checksums';
			IF enabled THEN
				EXIT;
			END IF;
			PERFORM pg_sleep(1);
		END LOOP;
		RETURN enabled;
	END;
	$$ LANGUAGE plpgsql;
	
	CREATE OR REPLACE FUNCTION reader_loop() RETURNS boolean AS $$
	DECLARE
		counter integer;
	BEGIN
		FOR counter IN 1..30 LOOP
			PERFORM count(a) FROM t1;
			PERFORM pg_sleep(0.2);
		END LOOP;
		RETURN True;
	END;
	$$ LANGUAGE plpgsql;
}

teardown
{
	DROP FUNCTION reader_loop();
	DROP FUNCTION test_checksums_on();
	DROP FUNCTION insert_1k(int);

	DROP TABLE t1;
}

session "writer"
step "w_insert100k"				{ SELECT insert_1k(100); }

session "reader"
step "r_seqread"				{ SELECT * FROM reader_loop(); }

session "checksums"
step "c_verify_checksums_off"	{ SELECT setting = 'off' FROM pg_catalog.pg_settings WHERE name = 'data_checksums'; }
step "c_enable_checksums"		{ SELECT pg_enable_data_checksums(); }
step "c_wait_for_checksums"		{ SELECT test_checksums_on(); }
step "c_verify_checksums_on"	{ SELECT setting = 'on' FROM pg_catalog.pg_settings WHERE name = 'data_checksums'; }

permutation "c_verify_checksums_off" "w_insert100k" "r_seqread" "c_enable_checksums" "c_wait_for_checksums" "c_verify_checksums_on"

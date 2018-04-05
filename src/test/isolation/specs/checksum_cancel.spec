setup
{
	CREATE TABLE t1 (a serial, b integer, c text);
	INSERT INTO t1 (b, c) VALUES (generate_series(1,10000), 'starting values');

	CREATE OR REPLACE FUNCTION test_checksums_off() RETURNS boolean AS $$
	DECLARE
		enabled boolean;
	BEGIN
		PERFORM pg_sleep(1);
		SELECT setting = 'off' INTO enabled FROM pg_catalog.pg_settings WHERE name = 'data_checksums';
		RETURN enabled;
	END;
	$$ LANGUAGE plpgsql;
	
	CREATE OR REPLACE FUNCTION reader_loop() RETURNS boolean AS $$
	DECLARE
		counter integer;
		enabled boolean;
	BEGIN
		FOR counter IN 1..100 LOOP
			PERFORM count(a) FROM t1;
		END LOOP;
		RETURN True;
	END;
	$$ LANGUAGE plpgsql;
}

teardown
{
	DROP FUNCTION reader_loop();
	DROP FUNCTION test_checksums_off();

	DROP TABLE t1;
}

session "reader"
step "r_seqread"						{ SELECT * FROM reader_loop(); }

session "checksums"
step "c_verify_checksums_off"			{ SELECT setting = 'off' FROM pg_catalog.pg_settings WHERE name = 'data_checksums'; }
step "c_enable_checksums"				{ SELECT pg_enable_data_checksums(1000); }
step "c_disable_checksums"				{ SELECT pg_disable_data_checksums(); }
step "c_verify_checksums_inprogress"	{ SELECT setting = 'inprogress' FROM pg_catalog.pg_settings WHERE name = 'data_checksums'; }
step "c_wait_checksums_off"				{ SELECT test_checksums_off(); }

permutation "c_verify_checksums_off" "r_seqread" "c_enable_checksums" "c_verify_checksums_inprogress" "c_disable_checksums" "c_wait_checksums_off"

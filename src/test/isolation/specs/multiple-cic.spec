# Test multiple CREATE INDEX CONCURRENTLY working simultaneously

setup
{
  CREATE TABLE mcic_one (
	id int
  );
  CREATE TABLE mcic_two (
	id int
  );
  CREATE FUNCTION lck_shr(bigint) RETURNS bool IMMUTABLE LANGUAGE plpgsql AS $$
     BEGIN PERFORM pg_advisory_lock_shared($1); RETURN true; END;
  $$;
  CREATE FUNCTION unlck() RETURNS bool IMMUTABLE LANGUAGE plpgsql AS $$
     BEGIN PERFORM pg_advisory_unlock_all(); RETURN true; END;
  $$;
}
teardown
{
  DROP TABLE mcic_one, mcic_two;
  DROP FUNCTION lck_shr(bigint);
  DROP FUNCTION unlck();
}

session s1
step s1i	{
		CREATE INDEX CONCURRENTLY mcic_one_pkey ON mcic_one (id)
		WHERE lck_shr(281457);
	}
teardown	{ SELECT unlck(); }


session s2
step s2l	{ SELECT pg_advisory_lock(281457); }
step s2i	{
		CREATE INDEX CONCURRENTLY mcic_two_pkey ON mcic_two (id)
		WHERE unlck();
	}

# (*) marker ensures that s2i is reported as "waiting", even if it
# completes very quickly

permutation s2l s1i s2i(*)

# Test ALTER DOMAIN VALIDATE CONSTRAINT waits for already-running DML.

setup
{
	CREATE DOMAIN alter_domain_validate_d AS int;
	CREATE TABLE alter_domain_validate_t (a alter_domain_validate_d);
}

teardown
{
	DROP TABLE alter_domain_validate_t;
	DROP DOMAIN alter_domain_validate_d;
}

session s1
step s1_lock		{ DO $$ BEGIN PERFORM pg_advisory_lock(8888); END $$; }
step s1_unlock		{ DO $$ BEGIN PERFORM pg_advisory_unlock(8888); END $$; }

session s2
# CoerceToDomain initializes the domain constraint list during executor
# startup, before this CTE waits on the advisory lock.
step s2_insert		{ WITH wait AS MATERIALIZED (SELECT pg_advisory_lock(8888)) INSERT INTO alter_domain_validate_t SELECT (-1)::alter_domain_validate_d FROM wait; }

session s3
step s3_add			{ ALTER DOMAIN alter_domain_validate_d ADD CONSTRAINT alter_domain_validate_d_pos CHECK (VALUE > 0) NOT VALID; }
step s3_validate	{ ALTER DOMAIN alter_domain_validate_d VALIDATE CONSTRAINT alter_domain_validate_d_pos; }
step s3_validated	{ SELECT convalidated FROM pg_constraint WHERE conname = 'alter_domain_validate_d_pos'; }
step s3_check		{ SELECT count(*) FROM alter_domain_validate_t; }

permutation s1_lock s2_insert s3_add s3_validate s1_unlock s3_validated s3_check

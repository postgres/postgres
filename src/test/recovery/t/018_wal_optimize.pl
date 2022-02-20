# Test WAL replay when some operation has skipped WAL.
#
# These tests exercise code that once violated the mandate described in
# src/backend/access/transam/README section "Skipping WAL for New
# RelFileNode".  The tests work by committing some transactions, initiating an
# immediate shutdown, and confirming that the expected data survives recovery.
# For many years, individual commands made the decision to skip WAL, hence the
# frequent appearance of COPY in these tests.
use strict;
use warnings;

use PostgresNode;
use TestLib;
use Test::More tests => 38;

sub check_orphan_relfilenodes
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($node, $test_name) = @_;

	my $db_oid = $node->safe_psql('postgres',
		"SELECT oid FROM pg_database WHERE datname = 'postgres'");
	my $prefix               = "base/$db_oid/";
	my $filepaths_referenced = $node->safe_psql(
		'postgres', "
	   SELECT pg_relation_filepath(oid) FROM pg_class
	   WHERE reltablespace = 0 AND relpersistence <> 't' AND
	   pg_relation_filepath(oid) IS NOT NULL;");
	is_deeply(
		[
			sort(map { "$prefix$_" }
				  grep(/^[0-9]+$/, slurp_dir($node->data_dir . "/$prefix")))
		],
		[ sort split /\n/, $filepaths_referenced ],
		$test_name);
	return;
}

# We run this same test suite for both wal_level=minimal and replica.
sub run_wal_optimize
{
	my $wal_level = shift;

	my $node = get_new_node("node_$wal_level");
	$node->init;
	$node->append_conf(
		'postgresql.conf', qq(
wal_level = $wal_level
max_prepared_transactions = 1
wal_log_hints = on
wal_skip_threshold = 0
#wal_debug = on
));
	$node->start;

	# Setup
	my $tablespace_dir = $node->basedir . '/tablespace_other';
	mkdir($tablespace_dir);
	my $result;

	# Test redo of CREATE TABLESPACE.
	$node->safe_psql(
		'postgres', "
		CREATE TABLE moved (id int);
		INSERT INTO moved VALUES (1);
		CREATE TABLESPACE other LOCATION '$tablespace_dir';
		BEGIN;
		ALTER TABLE moved SET TABLESPACE other;
		CREATE TABLE originated (id int);
		INSERT INTO originated VALUES (1);
		CREATE UNIQUE INDEX ON originated(id) TABLESPACE other;
		COMMIT;");
	$node->stop('immediate');
	$node->start;
	$result = $node->safe_psql('postgres', "SELECT count(*) FROM moved;");
	is($result, qq(1), "wal_level = $wal_level, CREATE+SET TABLESPACE");
	$result = $node->safe_psql(
		'postgres', "
		INSERT INTO originated VALUES (1) ON CONFLICT (id)
		  DO UPDATE set id = originated.id + 1
		  RETURNING id;");
	is($result, qq(2),
		"wal_level = $wal_level, CREATE TABLESPACE, CREATE INDEX");

	# Test direct truncation optimization.  No tuples.
	$node->safe_psql(
		'postgres', "
		BEGIN;
		CREATE TABLE trunc (id serial PRIMARY KEY);
		TRUNCATE trunc;
		COMMIT;");
	$node->stop('immediate');
	$node->start;
	$result = $node->safe_psql('postgres', "SELECT count(*) FROM trunc;");
	is($result, qq(0), "wal_level = $wal_level, TRUNCATE with empty table");

	# Test truncation with inserted tuples within the same transaction.
	# Tuples inserted after the truncation should be seen.
	$node->safe_psql(
		'postgres', "
		BEGIN;
		CREATE TABLE trunc_ins (id serial PRIMARY KEY);
		INSERT INTO trunc_ins VALUES (DEFAULT);
		TRUNCATE trunc_ins;
		INSERT INTO trunc_ins VALUES (DEFAULT);
		COMMIT;");
	$node->stop('immediate');
	$node->start;
	$result = $node->safe_psql('postgres',
		"SELECT count(*), min(id) FROM trunc_ins;");
	is($result, qq(1|2), "wal_level = $wal_level, TRUNCATE INSERT");

	# Same for prepared transaction.
	# Tuples inserted after the truncation should be seen.
	$node->safe_psql(
		'postgres', "
		BEGIN;
		CREATE TABLE twophase (id serial PRIMARY KEY);
		INSERT INTO twophase VALUES (DEFAULT);
		TRUNCATE twophase;
		INSERT INTO twophase VALUES (DEFAULT);
		PREPARE TRANSACTION 't';
		COMMIT PREPARED 't';");
	$node->stop('immediate');
	$node->start;
	$result = $node->safe_psql('postgres',
		"SELECT count(*), min(id) FROM trunc_ins;");
	is($result, qq(1|2), "wal_level = $wal_level, TRUNCATE INSERT PREPARE");

	# Writing WAL at end of xact, instead of syncing.
	$node->safe_psql(
		'postgres', "
		SET wal_skip_threshold = '1GB';
		BEGIN;
		CREATE TABLE noskip (id serial PRIMARY KEY);
		INSERT INTO noskip (SELECT FROM generate_series(1, 20000) a) ;
		COMMIT;");
	$node->stop('immediate');
	$node->start;
	$result = $node->safe_psql('postgres', "SELECT count(*) FROM noskip;");
	is($result, qq(20000), "wal_level = $wal_level, end-of-xact WAL");

	# Data file for COPY query in subsequent tests
	my $basedir   = $node->basedir;
	my $copy_file = "$basedir/copy_data.txt";
	TestLib::append_to_file(
		$copy_file, qq(20000,30000
20001,30001
20002,30002));

	# Test truncation with inserted tuples using both INSERT and COPY.  Tuples
	# inserted after the truncation should be seen.
	$node->safe_psql(
		'postgres', "
		BEGIN;
		CREATE TABLE ins_trunc (id serial PRIMARY KEY, id2 int);
		INSERT INTO ins_trunc VALUES (DEFAULT, generate_series(1,10000));
		TRUNCATE ins_trunc;
		INSERT INTO ins_trunc (id, id2) VALUES (DEFAULT, 10000);
		COPY ins_trunc FROM '$copy_file' DELIMITER ',';
		INSERT INTO ins_trunc (id, id2) VALUES (DEFAULT, 10000);
		COMMIT;");
	$node->stop('immediate');
	$node->start;
	$result = $node->safe_psql('postgres', "SELECT count(*) FROM ins_trunc;");
	is($result, qq(5), "wal_level = $wal_level, TRUNCATE COPY INSERT");

	# Test truncation with inserted tuples using COPY.  Tuples copied after
	# the truncation should be seen.
	$node->safe_psql(
		'postgres', "
		BEGIN;
		CREATE TABLE trunc_copy (id serial PRIMARY KEY, id2 int);
		INSERT INTO trunc_copy VALUES (DEFAULT, generate_series(1,3000));
		TRUNCATE trunc_copy;
		COPY trunc_copy FROM '$copy_file' DELIMITER ',';
		COMMIT;");
	$node->stop('immediate');
	$node->start;
	$result =
	  $node->safe_psql('postgres', "SELECT count(*) FROM trunc_copy;");
	is($result, qq(3), "wal_level = $wal_level, TRUNCATE COPY");

	# Like previous test, but rollback SET TABLESPACE in a subtransaction.
	$node->safe_psql(
		'postgres', "
		BEGIN;
		CREATE TABLE spc_abort (id serial PRIMARY KEY, id2 int);
		INSERT INTO spc_abort VALUES (DEFAULT, generate_series(1,3000));
		TRUNCATE spc_abort;
		SAVEPOINT s;
		  ALTER TABLE spc_abort SET TABLESPACE other; ROLLBACK TO s;
		COPY spc_abort FROM '$copy_file' DELIMITER ',';
		COMMIT;");
	$node->stop('immediate');
	$node->start;
	$result = $node->safe_psql('postgres', "SELECT count(*) FROM spc_abort;");
	is($result, qq(3),
		"wal_level = $wal_level, SET TABLESPACE abort subtransaction");

	# in different subtransaction patterns
	$node->safe_psql(
		'postgres', "
		BEGIN;
		CREATE TABLE spc_commit (id serial PRIMARY KEY, id2 int);
		INSERT INTO spc_commit VALUES (DEFAULT, generate_series(1,3000));
		TRUNCATE spc_commit;
		SAVEPOINT s; ALTER TABLE spc_commit SET TABLESPACE other; RELEASE s;
		COPY spc_commit FROM '$copy_file' DELIMITER ',';
		COMMIT;");
	$node->stop('immediate');
	$node->start;
	$result =
	  $node->safe_psql('postgres', "SELECT count(*) FROM spc_commit;");
	is($result, qq(3),
		"wal_level = $wal_level, SET TABLESPACE commit subtransaction");

	$node->safe_psql(
		'postgres', "
		BEGIN;
		CREATE TABLE spc_nest (id serial PRIMARY KEY, id2 int);
		INSERT INTO spc_nest VALUES (DEFAULT, generate_series(1,3000));
		TRUNCATE spc_nest;
		SAVEPOINT s;
			ALTER TABLE spc_nest SET TABLESPACE other;
			SAVEPOINT s2;
				ALTER TABLE spc_nest SET TABLESPACE pg_default;
			ROLLBACK TO s2;
			SAVEPOINT s2;
				ALTER TABLE spc_nest SET TABLESPACE pg_default;
			RELEASE s2;
		ROLLBACK TO s;
		COPY spc_nest FROM '$copy_file' DELIMITER ',';
		COMMIT;");
	$node->stop('immediate');
	$node->start;
	$result = $node->safe_psql('postgres', "SELECT count(*) FROM spc_nest;");
	is($result, qq(3),
		"wal_level = $wal_level, SET TABLESPACE nested subtransaction");

	$node->safe_psql(
		'postgres', "
		CREATE TABLE spc_hint (id int);
		INSERT INTO spc_hint VALUES (1);
		BEGIN;
		ALTER TABLE spc_hint SET TABLESPACE other;
		CHECKPOINT;
		SELECT * FROM spc_hint;  -- set hint bit
		INSERT INTO spc_hint VALUES (2);
		COMMIT;");
	$node->stop('immediate');
	$node->start;
	$result = $node->safe_psql('postgres', "SELECT count(*) FROM spc_hint;");
	is($result, qq(2), "wal_level = $wal_level, SET TABLESPACE, hint bit");

	$node->safe_psql(
		'postgres', "
		BEGIN;
		CREATE TABLE idx_hint (c int PRIMARY KEY);
		SAVEPOINT q; INSERT INTO idx_hint VALUES (1); ROLLBACK TO q;
		CHECKPOINT;
		INSERT INTO idx_hint VALUES (1);  -- set index hint bit
		INSERT INTO idx_hint VALUES (2);
		COMMIT;");
	$node->stop('immediate');
	$node->start;
	$result = $node->psql('postgres',);
	my ($ret, $stdout, $stderr) =
	  $node->psql('postgres', "INSERT INTO idx_hint VALUES (2);");
	is($ret, qq(3), "wal_level = $wal_level, unique index LP_DEAD");
	like(
		$stderr,
		qr/violates unique/,
		"wal_level = $wal_level, unique index LP_DEAD message");

	# UPDATE touches two buffers for one row.
	$node->safe_psql(
		'postgres', "
		BEGIN;
		CREATE TABLE upd (id serial PRIMARY KEY, id2 int);
		INSERT INTO upd (id, id2) VALUES (DEFAULT, generate_series(1,10000));
		COPY upd FROM '$copy_file' DELIMITER ',';
		UPDATE upd SET id2 = id2 + 1;
		DELETE FROM upd;
		COMMIT;");
	$node->stop('immediate');
	$node->start;
	$result = $node->safe_psql('postgres', "SELECT count(*) FROM upd;");
	is($result, qq(0),
		"wal_level = $wal_level, UPDATE touches two buffers for one row");

	# Test consistency of COPY with INSERT for table created in the same
	# transaction.
	$node->safe_psql(
		'postgres', "
		BEGIN;
		CREATE TABLE ins_copy (id serial PRIMARY KEY, id2 int);
		INSERT INTO ins_copy VALUES (DEFAULT, 1);
		COPY ins_copy FROM '$copy_file' DELIMITER ',';
		COMMIT;");
	$node->stop('immediate');
	$node->start;
	$result = $node->safe_psql('postgres', "SELECT count(*) FROM ins_copy;");
	is($result, qq(4), "wal_level = $wal_level, INSERT COPY");

	# Test consistency of COPY that inserts more to the same table using
	# triggers.  If the INSERTS from the trigger go to the same block data
	# is copied to, and the INSERTs are WAL-logged, WAL replay will fail when
	# it tries to replay the WAL record but the "before" image doesn't match,
	# because not all changes were WAL-logged.
	$node->safe_psql(
		'postgres', "
		BEGIN;
		CREATE TABLE ins_trig (id serial PRIMARY KEY, id2 text);
		CREATE FUNCTION ins_trig_before_row_trig() RETURNS trigger
		  LANGUAGE plpgsql as \$\$
		  BEGIN
			IF new.id2 NOT LIKE 'triggered%' THEN
			  INSERT INTO ins_trig
				VALUES (DEFAULT, 'triggered row before' || NEW.id2);
			END IF;
			RETURN NEW;
		  END; \$\$;
		CREATE FUNCTION ins_trig_after_row_trig() RETURNS trigger
		  LANGUAGE plpgsql as \$\$
		  BEGIN
			IF new.id2 NOT LIKE 'triggered%' THEN
			  INSERT INTO ins_trig
				VALUES (DEFAULT, 'triggered row after' || NEW.id2);
			END IF;
			RETURN NEW;
		  END; \$\$;
		CREATE TRIGGER ins_trig_before_row_insert
		  BEFORE INSERT ON ins_trig
		  FOR EACH ROW EXECUTE PROCEDURE ins_trig_before_row_trig();
		CREATE TRIGGER ins_trig_after_row_insert
		  AFTER INSERT ON ins_trig
		  FOR EACH ROW EXECUTE PROCEDURE ins_trig_after_row_trig();
		COPY ins_trig FROM '$copy_file' DELIMITER ',';
		COMMIT;");
	$node->stop('immediate');
	$node->start;
	$result = $node->safe_psql('postgres', "SELECT count(*) FROM ins_trig;");
	is($result, qq(9), "wal_level = $wal_level, COPY with INSERT triggers");

	# Test consistency of INSERT, COPY and TRUNCATE in same transaction block
	# with TRUNCATE triggers.
	$node->safe_psql(
		'postgres', "
		BEGIN;
		CREATE TABLE trunc_trig (id serial PRIMARY KEY, id2 text);
		CREATE FUNCTION trunc_trig_before_stat_trig() RETURNS trigger
		  LANGUAGE plpgsql as \$\$
		  BEGIN
			INSERT INTO trunc_trig VALUES (DEFAULT, 'triggered stat before');
			RETURN NULL;
		  END; \$\$;
		CREATE FUNCTION trunc_trig_after_stat_trig() RETURNS trigger
		  LANGUAGE plpgsql as \$\$
		  BEGIN
			INSERT INTO trunc_trig VALUES (DEFAULT, 'triggered stat before');
			RETURN NULL;
		  END; \$\$;
		CREATE TRIGGER trunc_trig_before_stat_truncate
		  BEFORE TRUNCATE ON trunc_trig
		  FOR EACH STATEMENT EXECUTE PROCEDURE trunc_trig_before_stat_trig();
		CREATE TRIGGER trunc_trig_after_stat_truncate
		  AFTER TRUNCATE ON trunc_trig
		  FOR EACH STATEMENT EXECUTE PROCEDURE trunc_trig_after_stat_trig();
		INSERT INTO trunc_trig VALUES (DEFAULT, 1);
		TRUNCATE trunc_trig;
		COPY trunc_trig FROM '$copy_file' DELIMITER ',';
		COMMIT;");
	$node->stop('immediate');
	$node->start;
	$result =
	  $node->safe_psql('postgres', "SELECT count(*) FROM trunc_trig;");
	is($result, qq(4),
		"wal_level = $wal_level, TRUNCATE COPY with TRUNCATE triggers");

	# Test redo of temp table creation.
	$node->safe_psql(
		'postgres', "
		CREATE TEMP TABLE temp (id serial PRIMARY KEY, id2 text);");
	$node->stop('immediate');
	$node->start;
	check_orphan_relfilenodes($node,
		"wal_level = $wal_level, no orphan relfilenode remains");

	return;
}

# Run same test suite for multiple wal_level values.
run_wal_optimize("minimal");
run_wal_optimize("replica");

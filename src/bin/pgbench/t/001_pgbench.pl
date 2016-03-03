use strict;
use warnings;

use PostgresNode;
use TestLib;
use Test::More tests => 3;

# Test concurrent insertion into table with UNIQUE oid column.  DDL expects
# GetNewOidWithIndex() to successfully avoid violating uniqueness for indexes
# like pg_class_oid_index and pg_proc_oid_index.  This indirectly exercises
# LWLock and spinlock concurrency.  This test makes a 5-MiB table.
my $node = get_new_node('main');
$node->init;
$node->start;
$node->safe_psql('postgres',
	    'CREATE UNLOGGED TABLE oid_tbl () WITH OIDS; '
	  . 'ALTER TABLE oid_tbl ADD UNIQUE (oid);');
my $script = $node->basedir . '/pgbench_script';
append_to_file($script,
	'INSERT INTO oid_tbl SELECT FROM generate_series(1,1000);');
$node->command_like(
	[   qw(pgbench --no-vacuum --client=5 --protocol=prepared
		  --transactions=25 --file), $script ],
	qr{processed: 125/125},
	'concurrent OID generation');

#!/usr/bin/perl

use strict;
use warnings;
use File::Basename;
use Fcntl 'SEEK_CUR';
use Test::More;
use lib 't';
use pgtde;

PGTDE::setup_files_dir(basename($0));

unlink('/tmp/pg_tde_test_key_validation.per');

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf('postgresql.conf', "shared_preload_libraries = 'pg_tde'");
$node->start;

PGTDE::psql($node, 'postgres', 'CREATE EXTENSION pg_tde;');
PGTDE::psql($node, 'postgres',
	"SELECT pg_tde_add_database_key_provider_file('test-file-provider', '/tmp/pg_tde_test_key_validation.per');"
);
PGTDE::psql($node, 'postgres',
	"SELECT pg_tde_create_key_using_database_key_provider('key1', 'test-file-provider');"
);
PGTDE::psql($node, 'postgres',
	"SELECT pg_tde_create_key_using_database_key_provider('key2', 'test-file-provider');"
);


corrupt_key_file('/tmp/pg_tde_test_key_validation.per');


PGTDE::psql($node, 'postgres',
	"SELECT pg_tde_set_key_using_database_key_provider('key1', 'test-file-provider');"
);
PGTDE::psql($node, 'postgres',
	"SELECT pg_tde_set_key_using_database_key_provider('key2', 'test-file-provider');"
);

sub corrupt_key_file
{
	my ($keyfile) = @_;

	my $fh;
	open($fh, '+<', $keyfile)
	  or BAIL_OUT("open failed: $!");
	binmode $fh;

	# Corrupt the first page of the key file  by zeroing key data length.
	# Offset is TDE_KEY_NAME_LEN + MAX_KEY_DATA_SIZE. See keyring_api.h for details.
	sysseek($fh, 256 + 32, 0)
	  or BAIL_OUT("sysseek failed: $!");
	syswrite($fh, pack("L*", 0x00000000)) or BAIL_OUT("syswrite failed: $!");

	# Corrupt the second page of the key file by setting incorrect key length.
	# Offset is TDE_KEY_NAME_LEN + MAX_KEY_DATA_SIZE. See keyring_api.h for details.
	sysseek($fh, 256 + 32, SEEK_CUR)
	  or BAIL_OUT("sysseek failed: $!");
	syswrite($fh, pack("L*", 0xFFFFFFFF)) or BAIL_OUT("syswrite failed: $!");


	close($fh)
	  or BAIL_OUT("close failed: $!");
}

$node->stop;

# Compare the expected and out file
my $compare = PGTDE->compare_results();

is($compare, 0,
	"Compare Files: $PGTDE::expected_filename_with_path and $PGTDE::out_filename_with_path files."
);

done_testing();

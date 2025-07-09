use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use JSON;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf('postgresql.conf', q{shared_preload_libraries = 'pg_tde'});
$node->start;

$node->safe_psql('postgres', q{CREATE EXTENSION pg_tde});
$node->safe_psql('postgres',
	q{SELECT pg_tde_add_global_key_provider_file('global-provider', '/tmp/pg_tde_change_key_provider-global')}
);
$node->safe_psql('postgres',
	q{SELECT pg_tde_add_database_key_provider_file('database-provider', '/tmp/pg_tde_change_key_provider-database')}
);
my $db_oid = $node->safe_psql('postgres',
	q{SELECT oid FROM pg_catalog.pg_database WHERE datname = 'postgres'});
my $options;

my $token_file = "${PostgreSQL::Test::Utils::tmp_check}/vault_token";
append_to_file($token_file, 'DUMMY');

$node->stop;

command_like(
	[
		'pg_tde_change_key_provider',
		'-D' => $node->data_dir,
		$db_oid,
		'database-provider',
		'file',
		'/tmp/pg_tde_change_key_provider-database-2',
	],
	qr/Key provider updated successfully!/,
	'updates key provider to file type');

$node->start;

is( $node->safe_psql(
		'postgres',
		q{SELECT type FROM pg_tde_list_all_database_key_providers() WHERE name = 'database-provider'}
	),
	'file',
	'provider type is set to file');

$options = decode_json(
	$node->safe_psql(
		'postgres',
		q{SELECT options FROM pg_tde_list_all_database_key_providers() WHERE name = 'database-provider'}
	));
is( $options->{path},
	'/tmp/pg_tde_change_key_provider-database-2',
	'path is set correctly for file provider');

$node->stop;

command_like(
	[
		'pg_tde_change_key_provider',
		'-D' => $node->data_dir,
		$db_oid,
		'database-provider',
		'vault-v2',
		'https://vault-server.example:8200/',
		'mount-path',
		$token_file,
		'/tmp/ca_path',
	],
	qr/Key provider updated successfully!/,
	'updates key provider to vault-v2 type with https');

$node->start;

is( $node->safe_psql(
		'postgres',
		q{SELECT type FROM pg_tde_list_all_database_key_providers() WHERE name = 'database-provider'}
	),
	'vault-v2',
	'provider type is set to vault-v2');

$options = decode_json(
	$node->safe_psql(
		'postgres',
		q{SELECT options FROM pg_tde_list_all_database_key_providers() WHERE name = 'database-provider'}
	));
is( $options->{url},
	'https://vault-server.example:8200/',
	'url is set correctly for vault-v2 provider');
is($options->{mountPath}, 'mount-path',
	'mount path is set correctly for vault-v2 provider');
is($options->{tokenPath}, $token_file,
	'tokenPath is set correctly for vault-v2 provider');
is($options->{caPath}, '/tmp/ca_path',
	'CA path is set correctly for vault-v2 provider');

$node->stop;

command_like(
	[
		'pg_tde_change_key_provider',
		'-D' => $node->data_dir,
		$db_oid,
		'database-provider',
		'vault-v2',
		'http://vault-server.example:8200/',
		'mount-path-2',
		$token_file,
	],
	qr/Key provider updated successfully!/,
	'updates key provider to vault-v2 type with http');

$node->start;

is( $node->safe_psql(
		'postgres',
		q{SELECT type FROM pg_tde_list_all_database_key_providers() WHERE name = 'database-provider'}
	),
	'vault-v2',
	'provider type is set to vault-v2');

$options = decode_json(
	$node->safe_psql(
		'postgres',
		q{SELECT options FROM pg_tde_list_all_database_key_providers() WHERE name = 'database-provider'}
	));
is( $options->{url},
	'http://vault-server.example:8200/',
	'url is set correctly for vault-v2 provider');
is($options->{mountPath}, 'mount-path-2',
	'mount path is set correctly for vault-v2 provider');
is($options->{tokenPath}, $token_file,
	'tokenPath is set correctly for vault-v2 provider');
is($options->{caPath}, '', 'CA path is set correctly for vault-v2 provider');

$node->stop;

command_like(
	[
		'pg_tde_change_key_provider',
		'-D' => $node->data_dir,
		$db_oid,
		'database-provider',
		'kmip',
		'kmip-server.example',
		'12345',
		'/tmp/cert_path',
		'/tmp/key_path',
		'/tmp/ca_path',
	],
	qr/Key provider updated successfully!/,
	'updates key provider to kmip type');

$node->start;

is( $node->safe_psql(
		'postgres',
		q{SELECT type FROM pg_tde_list_all_database_key_providers() WHERE name = 'database-provider'}
	),
	'kmip',
	'provider type is set to kmip');

$options = decode_json(
	$node->safe_psql(
		'postgres',
		q{SELECT options FROM pg_tde_list_all_database_key_providers() WHERE name = 'database-provider'}
	));
is($options->{host}, 'kmip-server.example',
	'host is set correctly for kmip provider');
is($options->{port}, '12345', 'port is set correctly for kmip provider');
is($options->{certPath}, '/tmp/cert_path',
	'client cert path is set correctly for kmip provider');
is($options->{keyPath}, '/tmp/key_path',
	'client cert key path is set correctly for kmip provider');
is($options->{caPath}, '/tmp/ca_path',
	'CA path is set correctly for kmip provider');

$node->stop;

command_like(
	[
		'pg_tde_change_key_provider',
		'-D' => $node->data_dir,
		'1664',
		'global-provider',
		'vault-v2',
		'http://vault-server.example:8200/',
		'mount-path',
		$token_file,
		'/tmp/ca_path',
	],
	qr/Key provider updated successfully!/,
	'updates key provider to vault-v2 type for global provider');

$node->start;

is( $node->safe_psql(
		'postgres',
		q{SELECT type FROM pg_tde_list_all_global_key_providers() WHERE name = 'global-provider'}
	),
	'vault-v2',
	'provider type is set to vault-v2 for global provider');

$options = decode_json(
	$node->safe_psql(
		'postgres',
		q{SELECT options FROM pg_tde_list_all_global_key_providers() WHERE name = 'global-provider'}
	));
is( $options->{url},
	'http://vault-server.example:8200/',
	'options are updated for global provider');

$node->stop;

command_fails_like(
	[
		'pg_tde_change_key_provider',
		'-D' => '/non/existing/path',
		'1664',
		'global-provider',
		'file',
		'/tmp/file',
	],
	qr{pg_tde_change_key_provider: error: could not open file "/non/existing/path/global/pg_control" for reading: No such file or directory},
	'gives error on incorrect data dir');

$node->start;
command_fails_like(
	[
		'pg_tde_change_key_provider',
		'-D' => $node->data_dir,
		'1664',
		'global-provider',
		'file',
		'/tmp/file',
	],
	qr/pg_tde_change_key_provider: error: cluster must be shut down/,
	'gives error on if cluster is running');
$node->stop;

command_fails_like(
	[
		'pg_tde_change_key_provider',
		'-D' => $node->data_dir,
		'12345678',
		'global-provider',
		'file',
		'/tmp/file',
	],
	qr/error: could not open tde file "[^"]+": No such file or directory/,
	'gives error on unknown database oid');

command_fails_like(
	[
		'pg_tde_change_key_provider',
		'-D' => $node->data_dir,
		'1664',
		'incorrect-global-provider',
		'file',
		'/tmp/file',
	],
	qr/error: provder "incorrect-global-provider" not found for database 1664/,
	'gives error on unknown key provider');

command_fails_like(
	[
		'pg_tde_change_key_provider',
		'-D' => $node->data_dir,
		'1664',
		'global-provider',
		'incorrect-provider-type',
	],
	qr/error: unknown provider type "incorrect-provider-type"/,
	'gives error on unknown provider type');

command_fails_like(
	[
		'pg_tde_change_key_provider',
		'-D' => $node->data_dir,
		'1664',
		'global-provider',
		'file',
	],
	qr/error: wrong number of arguments for "file"/,
	'gives error on missing arguments for file provider');

command_fails_like(
	[
		'pg_tde_change_key_provider',
		'-D' => $node->data_dir,
		'1664',
		'global-provider',
		'kmip',
	],
	qr/error: wrong number of arguments for "kmip"/,
	'gives error on missing arguments for kmip provider');

command_fails_like(
	[
		'pg_tde_change_key_provider',
		'-D' => $node->data_dir,
		'1664',
		'global-provider',
		'vault-v2',
	],
	qr/error: wrong number of arguments for "vault-v2"/,
	'gives error on missing arguments for vault-v2 provider');

done_testing();

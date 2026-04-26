
# Copyright (c) 2021-2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use File::Copy;

use PostgreSQL::Test::Utils;
use Test::More;
use PostgreSQL::Test::Cluster;

unless (($ENV{with_ssl} || "") eq 'openssl')
{
	plan skip_all => 'OpenSSL not supported by this build';
}

my $libressl = not check_pg_config("#define HAVE_SSL_CTX_SET_CERT_CB 1");

my $rot13pass = "SbbOnE1";

# see the Makefile for how the certificate and key have been generated

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf('postgresql.conf',
	"ssl_passphrase.passphrase = '$rot13pass'");
$node->append_conf('postgresql.conf',
	"shared_preload_libraries = 'ssl_passphrase_func'");
$node->append_conf('postgresql.conf', "ssl = 'on'");

my $ddir = $node->data_dir;

# install certificate and protected key
copy("server.crt", $ddir);
copy("server.key", $ddir);
chmod 0600, "$ddir/server.key" or die $!;

$node->start;

# if the server is running we must have successfully transformed the passphrase
ok(-e "$ddir/postmaster.pid", "postgres started");

$node->stop('fast');

# should get a warning if ssl_passphrase_command is set
my $log = $node->rotate_logfile();

$node->append_conf('postgresql.conf',
	"ssl_passphrase_command = 'echo spl0tz'");

$node->start;

$node->stop('fast');

my $log_contents = slurp_file($log);

like(
	$log_contents,
	qr/WARNING.*"ssl_passphrase_command" setting ignored by ssl_passphrase_func module/,
	"ssl_passphrase_command set warning");

# set the wrong passphrase
$node->append_conf('postgresql.conf', "ssl_passphrase.passphrase = 'blurfl'");

# try to start the server again
my $ret = PostgreSQL::Test::Utils::system_log(
	'pg_ctl',
	'--pgdata' => $node->data_dir,
	'--log' => $node->logfile,
	'start');


# with a bad passphrase the server should not start
ok($ret, "pg_ctl fails with bad passphrase");
ok(!-e "$ddir/postmaster.pid", "postgres not started with bad passphrase");

# just in case
$node->stop('fast');

# Make sure the hook is bypassed when SNI is enabled.
SKIP:
{
	skip 'SNI not supported with LibreSSL', 2 if ($libressl);

	$node->append_conf(
		'postgresql.conf', qq{
ssl_passphrase_command = 'echo FooBaR1'
ssl_sni = on
});
	$node->append_conf(
		'pg_hosts.conf', qq{
example.org "$ddir/server.crt" "$ddir/server.key" "" "echo FooBaR1" on
example.com "$ddir/server.crt" "$ddir/server.key" "" "echo FooBaR1" on
});

	# If the servers starts and runs, the bad ssl_passphrase.passphrase was
	# correctly ignored.
	$node->start;
	ok(-e "$ddir/postmaster.pid", "postgres started after SNI");

	$node->stop('fast');
	$log_contents = slurp_file($log);
	like(
		$log_contents,
		qr/WARNING.*SNI is enabled; installed TLS init hook will be ignored/,
		"server warns that init hook and SNI are incompatible");
	# Ensure that the warning was printed once and not once per host line
	my $count =()= $log_contents =~ m/installed TLS init hook will be ignored/;
	is($count, 1, 'Only one WARNING');
}

done_testing();

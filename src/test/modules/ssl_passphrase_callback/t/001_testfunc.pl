use strict;
use warnings;

use File::Copy;

use TestLib;
use Test::More;
use PostgresNode;

unless ($ENV{with_ssl} eq 'openssl')
{
	plan skip_all => 'OpenSSL not supported by this build';
}

my $clearpass = "FooBaR1";
my $rot13pass = "SbbOnE1";

# see the Makefile for how the certificate and key have been generated

my $node = get_new_node('main');
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
chmod 0600, "$ddir/server.key";

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
	qr/WARNING.*ssl_passphrase_command setting ignored by ssl_passphrase_func module/,
	"ssl_passphrase_command set warning");

# set the wrong passphrase
$node->append_conf('postgresql.conf', "ssl_passphrase.passphrase = 'blurfl'");

# try to start the server again
my $ret = TestLib::system_log('pg_ctl', '-D', $node->data_dir, '-l',
	$node->logfile, 'start');


# with a bad passphrase the server should not start
ok($ret,                       "pg_ctl fails with bad passphrase");
ok(!-e "$ddir/postmaster.pid", "postgres not started with bad passphrase");

# just in case
$node->stop('fast');

done_testing();

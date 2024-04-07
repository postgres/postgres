# Sets up a KDC and then runs a variety of tests to make sure that the
# GSSAPI/Kerberos authentication and encryption are working properly,
# that the options in pg_hba.conf and pg_ident.conf are handled correctly,
# and that the server-side pg_stat_gssapi view reports what we expect to
# see for each test.
#
# Since this requires setting up a full KDC, it doesn't make much sense
# to have multiple test scripts (since they'd have to also create their
# own KDC and that could cause race conditions or other problems)- so
# just add whatever other tests are needed to here.
#
# See the README for additional information.

use strict;
use warnings;
use TestLib;
use PostgresNode;
use Test::More;

if ($ENV{with_gssapi} eq 'yes')
{
	plan tests => 18;
}
else
{
	plan skip_all => 'GSSAPI/Kerberos not supported by this build';
}

my ($krb5_bin_dir, $krb5_sbin_dir);

if ($^O eq 'darwin' && -d "/opt/homebrew" )
{
	# typical paths for Homebrew on ARM
	$krb5_bin_dir  = '/opt/homebrew/opt/krb5/bin';
	$krb5_sbin_dir = '/opt/homebrew/opt/krb5/sbin';
}
elsif ($^O eq 'darwin')
{
	# typical paths for Homebrew on Intel
	$krb5_bin_dir  = '/usr/local/opt/krb5/bin';
	$krb5_sbin_dir = '/usr/local/opt/krb5/sbin';
}
elsif ($^O eq 'freebsd')
{
	$krb5_bin_dir  = '/usr/local/bin';
	$krb5_sbin_dir = '/usr/local/sbin';
}
elsif ($^O eq 'linux')
{
	$krb5_sbin_dir = '/usr/sbin';
}

my $krb5_config  = 'krb5-config';
my $kinit        = 'kinit';
my $kdb5_util    = 'kdb5_util';
my $kadmin_local = 'kadmin.local';
my $krb5kdc      = 'krb5kdc';

if ($krb5_bin_dir && -d $krb5_bin_dir)
{
	$krb5_config = $krb5_bin_dir . '/' . $krb5_config;
	$kinit       = $krb5_bin_dir . '/' . $kinit;
}
if ($krb5_sbin_dir && -d $krb5_sbin_dir)
{
	$kdb5_util    = $krb5_sbin_dir . '/' . $kdb5_util;
	$kadmin_local = $krb5_sbin_dir . '/' . $kadmin_local;
	$krb5kdc      = $krb5_sbin_dir . '/' . $krb5kdc;
}

my $host     = 'auth-test-localhost.postgresql.example.com';
my $hostaddr = '127.0.0.1';
my $realm    = 'EXAMPLE.COM';

my $krb5_conf   = "${TestLib::tmp_check}/krb5.conf";
my $kdc_conf    = "${TestLib::tmp_check}/kdc.conf";
my $krb5_cache  = "${TestLib::tmp_check}/krb5cc";
my $krb5_log    = "${TestLib::log_path}/krb5libs.log";
my $kdc_log     = "${TestLib::log_path}/krb5kdc.log";
my $kdc_port    = get_free_port();
my $kdc_datadir = "${TestLib::tmp_check}/krb5kdc";
my $kdc_pidfile = "${TestLib::tmp_check}/krb5kdc.pid";
my $keytab      = "${TestLib::tmp_check}/krb5.keytab";

note "setting up Kerberos";

my ($stdout, $krb5_version);
run_log [ $krb5_config, '--version' ], '>', \$stdout
  or BAIL_OUT("could not execute krb5-config");
BAIL_OUT("Heimdal is not supported") if $stdout =~ m/heimdal/;
$stdout =~ m/Kerberos 5 release ([0-9]+\.[0-9]+)/
  or BAIL_OUT("could not get Kerberos version");
$krb5_version = $1;

# Build the krb5.conf to use.
#
# Explicitly specify the default (test) realm and the KDC for
# that realm to avoid the Kerberos library trying to look up
# that information in DNS, and also because we're using a
# non-standard KDC port.
#
# Also explicitly disable DNS lookups since this isn't really
# our domain and we shouldn't be causing random DNS requests
# to be sent out (not to mention that broken DNS environments
# can cause the tests to take an extra long time and timeout).
#
# Reverse DNS is explicitly disabled to avoid any issue with a
# captive portal or other cases where the reverse DNS succeeds
# and the Kerberos library uses that as the canonical name of
# the host and then tries to acquire a cross-realm ticket.
append_to_file(
	$krb5_conf,
	qq![logging]
default = FILE:$krb5_log
kdc = FILE:$kdc_log

[libdefaults]
dns_lookup_realm = false
dns_lookup_kdc = false
default_realm = $realm
rdns = false

[realms]
$realm = {
    kdc = $hostaddr:$kdc_port
}!);

append_to_file(
	$kdc_conf,
	qq![kdcdefaults]
!);

# For new-enough versions of krb5, use the _listen settings rather
# than the _ports settings so that we can bind to localhost only.
if ($krb5_version >= 1.15)
{
	append_to_file(
		$kdc_conf,
		qq!kdc_listen = $hostaddr:$kdc_port
kdc_tcp_listen = $hostaddr:$kdc_port
!);
}
else
{
	append_to_file(
		$kdc_conf,
		qq!kdc_ports = $kdc_port
kdc_tcp_ports = $kdc_port
!);
}
append_to_file(
	$kdc_conf,
	qq!
[realms]
$realm = {
    database_name = $kdc_datadir/principal
    admin_keytab = FILE:$kdc_datadir/kadm5.keytab
    acl_file = $kdc_datadir/kadm5.acl
    key_stash_file = $kdc_datadir/_k5.$realm
}!);

mkdir $kdc_datadir or die;

# Ensure that we use test's config and cache files, not global ones.
$ENV{'KRB5_CONFIG'}      = $krb5_conf;
$ENV{'KRB5_KDC_PROFILE'} = $kdc_conf;
$ENV{'KRB5CCNAME'}       = $krb5_cache;

my $service_principal = "$ENV{with_krb_srvnam}/$host";

system_or_bail $kdb5_util, 'create', '-s', '-P', 'secret0';

my $test1_password = 'secret1';
system_or_bail $kadmin_local, '-q', "addprinc -pw $test1_password test1";

system_or_bail $kadmin_local, '-q', "addprinc -randkey $service_principal";
system_or_bail $kadmin_local, '-q', "ktadd -k $keytab $service_principal";

system_or_bail $krb5kdc, '-P', $kdc_pidfile;

END
{
	# take care not to change the script's exit value
	my $exit_code = $?;

	kill 'INT', `cat $kdc_pidfile` if defined($kdc_pidfile) && -f $kdc_pidfile;

	$? = $exit_code;
}

note "setting up PostgreSQL instance";

my $node = get_new_node('node');
$node->init;
$node->append_conf('postgresql.conf', "listen_addresses = '$hostaddr'");
$node->append_conf('postgresql.conf', "krb_server_keyfile = '$keytab'");
$node->start;

$node->safe_psql('postgres', 'CREATE USER test1;');

note "running tests";

# Test connection success or failure, and if success, that query returns true.
sub test_access
{
	my ($node, $role, $query, $expected_res, $gssencmode, $test_name) = @_;

	# need to connect over TCP/IP for Kerberos
	my ($res, $stdoutres, $stderrres) = $node->psql(
		'postgres',
		undef,
		extra_params => [
			'-XAtd',
			$node->connstr('postgres')
			  . " host=$host hostaddr=$hostaddr $gssencmode",
			'-U',
			$role,
			'-c',
			$query
		]);

	# If we get a query result back, it should be true.
	if ($res == $expected_res and $res eq 0)
	{
		is($stdoutres, "t", $test_name);
	}
	else
	{
		is($res, $expected_res, $test_name);
	}
	return;
}

# As above, but test for an arbitrary query result.
sub test_query
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($node, $role, $query, $expected, $gssencmode, $test_name) = @_;

	# need to connect over TCP/IP for Kerberos
	my ($res, $stdoutres, $stderrres) = $node->psql(
		'postgres',
		"$query",
		extra_params => [
			'-XAtd',
			$node->connstr('postgres')
			  . " host=$host hostaddr=$hostaddr $gssencmode",
			'-U',
			$role
		]);

	is($res, 0, $test_name);
	like($stdoutres, $expected, $test_name);
	is($stderrres, "", $test_name);
	return;
}

unlink($node->data_dir . '/pg_hba.conf');
$node->append_conf('pg_hba.conf',
	qq{host all all $hostaddr/32 gss map=mymap});
$node->restart;

test_access($node, 'test1', 'SELECT true', 2, '', 'fails without ticket');

run_log [ $kinit, 'test1' ], \$test1_password or BAIL_OUT($?);

test_access($node, 'test1', 'SELECT true', 2, '', 'fails without mapping');

$node->append_conf('pg_ident.conf', qq{mymap  /^(.*)\@$realm\$  \\1});
$node->restart;

test_access(
	$node,
	'test1',
	'SELECT gss_authenticated AND encrypted from pg_stat_gssapi where pid = pg_backend_pid();',
	0,
	'',
	'succeeds with mapping with default gssencmode and host hba');
test_access(
	$node,
	"test1",
	'SELECT gss_authenticated AND encrypted from pg_stat_gssapi where pid = pg_backend_pid();',
	0,
	"gssencmode=prefer",
	"succeeds with GSS-encrypted access preferred with host hba");
test_access(
	$node,
	"test1",
	'SELECT gss_authenticated AND encrypted from pg_stat_gssapi where pid = pg_backend_pid();',
	0,
	"gssencmode=require",
	"succeeds with GSS-encrypted access required with host hba");

# Test that we can transport a reasonable amount of data.
test_query(
	$node,
	"test1",
	'SELECT * FROM generate_series(1, 100000);',
	qr/^1\n.*\n1024\n.*\n9999\n.*\n100000$/s,
	"gssencmode=require",
	"receiving 100K lines works");

test_query(
	$node,
	"test1",
	"CREATE TABLE mytab (f1 int primary key);\n"
	  . "COPY mytab FROM STDIN;\n"
	  . join("\n", (1 .. 100000))
	  . "\n\\.\n"
	  . "SELECT COUNT(*) FROM mytab;",
	qr/^100000$/s,
	"gssencmode=require",
	"sending 100K lines works");

unlink($node->data_dir . '/pg_hba.conf');
$node->append_conf('pg_hba.conf',
	qq{hostgssenc all all $hostaddr/32 gss map=mymap});
$node->restart;

test_access(
	$node,
	"test1",
	'SELECT gss_authenticated AND encrypted from pg_stat_gssapi where pid = pg_backend_pid();',
	0,
	"gssencmode=prefer",
	"succeeds with GSS-encrypted access preferred and hostgssenc hba");
test_access(
	$node,
	"test1",
	'SELECT gss_authenticated AND encrypted from pg_stat_gssapi where pid = pg_backend_pid();',
	0,
	"gssencmode=require",
	"succeeds with GSS-encrypted access required and hostgssenc hba");
test_access($node, "test1", 'SELECT true', 2, "gssencmode=disable",
	"fails with GSS encryption disabled and hostgssenc hba");

unlink($node->data_dir . '/pg_hba.conf');
$node->append_conf('pg_hba.conf',
	qq{hostnogssenc all all $hostaddr/32 gss map=mymap});
$node->restart;

test_access(
	$node,
	"test1",
	'SELECT gss_authenticated and not encrypted from pg_stat_gssapi where pid = pg_backend_pid();',
	0,
	"gssencmode=prefer",
	"succeeds with GSS-encrypted access preferred and hostnogssenc hba, but no encryption"
);
test_access($node, "test1", 'SELECT true', 2, "gssencmode=require",
	"fails with GSS-encrypted access required and hostnogssenc hba");
test_access(
	$node,
	"test1",
	'SELECT gss_authenticated and not encrypted from pg_stat_gssapi where pid = pg_backend_pid();',
	0,
	"gssencmode=disable",
	"succeeds with GSS encryption disabled and hostnogssenc hba");

truncate($node->data_dir . '/pg_ident.conf', 0);
unlink($node->data_dir . '/pg_hba.conf');
$node->append_conf('pg_hba.conf',
	qq{host all all $hostaddr/32 gss include_realm=0});
$node->restart;

test_access(
	$node,
	'test1',
	'SELECT gss_authenticated AND encrypted from pg_stat_gssapi where pid = pg_backend_pid();',
	0,
	'',
	'succeeds with include_realm=0 and defaults');

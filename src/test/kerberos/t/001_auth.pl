use strict;
use warnings;
use TestLib;
use PostgresNode;
use Test::More;

if ($ENV{with_gssapi} eq 'yes')
{
	plan tests => 4;
}
else
{
	plan skip_all => 'GSSAPI/Kerberos not supported by this build';
}

my ($krb5_bin_dir, $krb5_sbin_dir);

if ($^O eq 'darwin')
{
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

my $realm = 'EXAMPLE.COM';

my $krb5_conf   = "${TestLib::tmp_check}/krb5.conf";
my $kdc_conf    = "${TestLib::tmp_check}/kdc.conf";
my $krb5_log    = "${TestLib::tmp_check}/krb5libs.log";
my $kdc_log     = "${TestLib::tmp_check}/krb5kdc.log";
my $kdc_port    = int(rand() * 16384) + 49152;
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

append_to_file(
	$krb5_conf,
	qq![logging]
default = FILE:$krb5_log
kdc = FILE:$kdc_log

[libdefaults]
default_realm = $realm

[realms]
$realm = {
    kdc = localhost:$kdc_port
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
		qq!kdc_listen = localhost:$kdc_port
kdc_tcp_listen = localhost:$kdc_port
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

$ENV{'KRB5_CONFIG'}      = $krb5_conf;
$ENV{'KRB5_KDC_PROFILE'} = $kdc_conf;

my $service_principal = "$ENV{with_krb_srvnam}/localhost";

system_or_bail $kdb5_util, 'create', '-s', '-P', 'secret0';

my $test1_password = 'secret1';
system_or_bail $kadmin_local, '-q', "addprinc -pw $test1_password test1";

system_or_bail $kadmin_local, '-q', "addprinc -randkey $service_principal";
system_or_bail $kadmin_local, '-q', "ktadd -k $keytab $service_principal";

system_or_bail $krb5kdc, '-P', $kdc_pidfile;

END
{
	kill 'INT', `cat $kdc_pidfile` if -f $kdc_pidfile;
}

note "setting up PostgreSQL instance";

my $node = get_new_node('node');
$node->init;
$node->append_conf('postgresql.conf', "listen_addresses = 'localhost'");
$node->append_conf('postgresql.conf', "krb_server_keyfile = '$keytab'");
$node->start;

$node->safe_psql('postgres', 'CREATE USER test1;');

note "running tests";

sub test_access
{
	my ($node, $role, $expected_res, $test_name) = @_;

	# need to connect over TCP/IP for Kerberos
	my $res = $node->psql(
		'postgres',
		'SELECT 1',
		extra_params => [
			'-d', $node->connstr('postgres') . ' host=localhost',
			'-U', $role
		]);
	is($res, $expected_res, $test_name);
	return;
}

unlink($node->data_dir . '/pg_hba.conf');
$node->append_conf('pg_hba.conf', qq{host all all localhost gss map=mymap});
$node->restart;

test_access($node, 'test1', 2, 'fails without ticket');

run_log [ $kinit, 'test1' ], \$test1_password or BAIL_OUT($?);

test_access($node, 'test1', 2, 'fails without mapping');

$node->append_conf('pg_ident.conf', qq{mymap  /^(.*)\@$realm\$  \\1});
$node->restart;

test_access($node, 'test1', 0, 'succeeds with mapping');

truncate($node->data_dir . '/pg_ident.conf', 0);
unlink($node->data_dir . '/pg_hba.conf');
$node->append_conf('pg_hba.conf',
	qq{host all all localhost gss include_realm=0});
$node->restart;

test_access($node, 'test1', 0, 'succeeds with include_realm=0');

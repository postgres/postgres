
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

# Sets up a stand-alone KDC for testing PostgreSQL GSSAPI / Kerberos
# functionality.

package PostgreSQL::Test::Kerberos;

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Utils;

our (
	$krb5_bin_dir, $krb5_sbin_dir, $krb5_config, $kinit,
	$klist, $kdb5_util, $kadmin_local, $krb5kdc,
	$krb5_conf, $kdc_conf, $krb5_cache, $krb5_log,
	$kdc_log, $kdc_port, $kdc_datadir, $kdc_pidfile,
	$keytab);

INIT
{
	if ($^O eq 'darwin' && -d "/opt/homebrew")
	{
		# typical paths for Homebrew on ARM
		$krb5_bin_dir = '/opt/homebrew/opt/krb5/bin';
		$krb5_sbin_dir = '/opt/homebrew/opt/krb5/sbin';
	}
	elsif ($^O eq 'darwin')
	{
		# typical paths for Homebrew on Intel
		$krb5_bin_dir = '/usr/local/opt/krb5/bin';
		$krb5_sbin_dir = '/usr/local/opt/krb5/sbin';
	}
	elsif ($^O eq 'freebsd')
	{
		$krb5_bin_dir = '/usr/local/bin';
		$krb5_sbin_dir = '/usr/local/sbin';
	}
	elsif ($^O eq 'linux')
	{
		$krb5_sbin_dir = '/usr/sbin';
	}

	$krb5_config = 'krb5-config';
	$kinit = 'kinit';
	$klist = 'klist';
	$kdb5_util = 'kdb5_util';
	$kadmin_local = 'kadmin.local';
	$krb5kdc = 'krb5kdc';

	if ($krb5_bin_dir && -d $krb5_bin_dir)
	{
		$krb5_config = $krb5_bin_dir . '/' . $krb5_config;
		$kinit = $krb5_bin_dir . '/' . $kinit;
		$klist = $krb5_bin_dir . '/' . $klist;
	}
	if ($krb5_sbin_dir && -d $krb5_sbin_dir)
	{
		$kdb5_util = $krb5_sbin_dir . '/' . $kdb5_util;
		$kadmin_local = $krb5_sbin_dir . '/' . $kadmin_local;
		$krb5kdc = $krb5_sbin_dir . '/' . $krb5kdc;
	}

	$krb5_conf = "${PostgreSQL::Test::Utils::tmp_check}/krb5.conf";
	$kdc_conf = "${PostgreSQL::Test::Utils::tmp_check}/kdc.conf";
	$krb5_cache = "${PostgreSQL::Test::Utils::tmp_check}/krb5cc";
	$krb5_log = "${PostgreSQL::Test::Utils::log_path}/krb5libs.log";
	$kdc_log = "${PostgreSQL::Test::Utils::log_path}/krb5kdc.log";
	$kdc_port = PostgreSQL::Test::Cluster::get_free_port();
	$kdc_datadir = "${PostgreSQL::Test::Utils::tmp_check}/krb5kdc";
	$kdc_pidfile = "${PostgreSQL::Test::Utils::tmp_check}/krb5kdc.pid";
	$keytab = "${PostgreSQL::Test::Utils::tmp_check}/krb5.keytab";
}

=pod

=item PostgreSQL::Test::Kerberos->new(host, hostaddr, realm, %params)

Sets up a new Kerberos realm and KDC.  This function assigns a free
port for the KDC.  The KDC will be shut down automatically when the
test script exits.

=over

=item host => 'auth-test-localhost.postgresql.example.com'

Hostname to use in the service principal.

=item hostaddr => '127.0.0.1'

Network interface the KDC will listen on.

=item realm => 'EXAMPLE.COM'

Name of the Kerberos realm.

=back

=cut

sub new
{
	my $class = shift;
	my ($host, $hostaddr, $realm) = @_;

	my ($stdout, $krb5_version);
	run_log [ $krb5_config, '--version' ], '>' => \$stdout
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
forwardable = false
rdns = false

[realms]
$realm = {
    kdc = $hostaddr:$kdc_port
}
!);

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

	mkdir $kdc_datadir
	  or BAIL_OUT("could not create directory \"$kdc_datadir\"");

	# Ensure that we use test's config and cache files, not global ones.
	$ENV{'KRB5_CONFIG'} = $krb5_conf;
	$ENV{'KRB5_KDC_PROFILE'} = $kdc_conf;
	$ENV{'KRB5CCNAME'} = $krb5_cache;

	my $service_principal = "$ENV{with_krb_srvnam}/$host";

	system_or_bail $kdb5_util, 'create', '-s', '-P', 'secret0';

	system_or_bail $kadmin_local, '-q',
	  "addprinc -randkey $service_principal";
	system_or_bail $kadmin_local, '-q', "ktadd -k $keytab $service_principal";

	system_or_bail $krb5kdc, '-P', $kdc_pidfile;

	my $self = {};
	$self->{keytab} = $keytab;

	bless $self, $class;

	return $self;
}

sub create_principal
{
	my ($self, $principal, $password) = @_;

	system_or_bail $kadmin_local, '-q', "addprinc -pw $password $principal";
}

sub create_ticket
{
	my ($self, $principal, $password, %params) = @_;

	my @cmd = ($kinit, $principal);

	push @cmd, '-f' if ($params{forwardable});

	run_log [@cmd], \$password or BAIL_OUT($?);
	run_log [ $klist, '-f' ] or BAIL_OUT($?);
}

END
{
	# take care not to change the script's exit value
	my $exit_code = $?;

	kill 'INT', `cat $kdc_pidfile`
	  if defined($kdc_pidfile) && -f $kdc_pidfile;

	$? = $exit_code;
}

1;


############################################################################
#
# LdapServer.pm
#
# Module to set up an LDAP server for testing pg_hba.conf ldap authentication
#
# Copyright (c) 2023-2025, PostgreSQL Global Development Group
#
############################################################################

=pod

=head1 NAME

LdapServer - class for an LDAP server for testing pg_hba.conf authentication

=head1 SYNOPSIS

  use LdapServer;

  # have we found openldap binaries suitable for setting up a server?
  my $ldap_binaries_found = $LdapServer::setup;

  # create a server with the given root password and auth type
  # (users or anonymous)
  my $server = LdapServer->new($root_password, $auth_type);

  # Add the contents of an LDIF file to the server
  $server->ldapadd_file ($path_to_ldif_data);

  # set the Ldap password for a user
  $server->ldapsetpw($user, $password);

  # get details of some settings for the server
  my @properties = $server->prop($propname1, $propname2, ...);

=head1 DESCRIPTION

  LdapServer tests in its INIT phase for the presence of suitable openldap
  binaries. Its constructor method sets up and runs an LDAP server, and any
  servers that are set up are terminated during its END phase.

=cut

package LdapServer;

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Utils;
use Test::More;

use File::Copy;
use File::Basename;

# private variables
my ($slapd, $ldap_schema_dir, @servers);

# visible variables
our ($setup, $setup_error);

INIT
{
	# Find the OpenLDAP server binary and directory containing schema
	# definition files.  On success, $setup is set to 1. On failure,
	# it's set to 0, and an error message is set in $setup_error.
	$setup = 1;
	if ($^O eq 'darwin')
	{
		if (-d '/opt/homebrew/opt/openldap')
		{
			# typical paths for Homebrew on ARM
			$slapd = '/opt/homebrew/opt/openldap/libexec/slapd';
			$ldap_schema_dir = '/opt/homebrew/etc/openldap/schema';
		}
		elsif (-d '/usr/local/opt/openldap')
		{
			# typical paths for Homebrew on Intel
			$slapd = '/usr/local/opt/openldap/libexec/slapd';
			$ldap_schema_dir = '/usr/local/etc/openldap/schema';
		}
		elsif (-d '/opt/local/etc/openldap')
		{
			# typical paths for MacPorts
			$slapd = '/opt/local/libexec/slapd';
			$ldap_schema_dir = '/opt/local/etc/openldap/schema';
		}
		else
		{
			$setup_error = "OpenLDAP server installation not found";
			$setup = 0;
		}
	}
	elsif ($^O eq 'linux')
	{
		if (-d '/etc/ldap/schema')
		{
			$slapd = '/usr/sbin/slapd';
			$ldap_schema_dir = '/etc/ldap/schema';
		}
		elsif (-d '/etc/openldap/schema')
		{
			$slapd = '/usr/sbin/slapd';
			$ldap_schema_dir = '/etc/openldap/schema';
		}
		else
		{
			$setup_error = "OpenLDAP server installation not found";
			$setup = 0;
		}
	}
	elsif ($^O eq 'freebsd')
	{
		if (-d '/usr/local/etc/openldap/schema')
		{
			$slapd = '/usr/local/libexec/slapd';
			$ldap_schema_dir = '/usr/local/etc/openldap/schema';
		}
		else
		{
			$setup_error = "OpenLDAP server installation not found";
			$setup = 0;
		}
	}
	elsif ($^O eq 'openbsd')
	{
		if (-d '/usr/local/share/examples/openldap/schema')
		{
			$slapd = '/usr/local/libexec/slapd';
			$ldap_schema_dir = '/usr/local/share/examples/openldap/schema';
		}
		else
		{
			$setup_error = "OpenLDAP server installation not found";
			$setup = 0;
		}
	}
	else
	{
		$setup_error = "ldap tests not supported on $^O";
		$setup = 0;
	}
}

END
{
	# take care not to change the script's exit value
	my $exit_code = $?;

	foreach my $server (@servers)
	{
		next unless -f $server->{pidfile};
		my $pid = slurp_file($server->{pidfile});
		chomp $pid;
		kill 'INT', $pid;
	}

	$? = $exit_code;
}

=pod

=head1 METHODS

=over

=item LdapServer->new($rootpw, $auth_type)

Create a new LDAP server.

The rootpw can be used when authenticating with the ldapbindpasswd option.

The auth_type is either 'users' or 'anonymous'.

=back

=cut

sub new
{
	die "no suitable binaries found" unless $setup;

	my $class = shift;
	my $rootpw = shift;
	my $authtype = shift;    # 'users' or 'anonymous'
	my $testname = basename((caller)[1], '.pl');
	my $self = {};

	my $test_temp = PostgreSQL::Test::Utils::tempdir("ldap-$testname");

	my $ldap_datadir = "$test_temp/openldap-data";
	my $slapd_certs = "$test_temp/slapd-certs";
	my $slapd_pidfile = "$test_temp/slapd.pid";
	my $slapd_conf = "$test_temp/slapd.conf";
	my $slapd_logfile =
	  "${PostgreSQL::Test::Utils::log_path}/slapd-$testname.log";
	my $ldap_server = 'localhost';
	my $ldap_port = PostgreSQL::Test::Cluster::get_free_port();
	my $ldaps_port = PostgreSQL::Test::Cluster::get_free_port();
	my $ldap_url = "ldap://$ldap_server:$ldap_port";
	my $ldaps_url = "ldaps://$ldap_server:$ldaps_port";
	my $ldap_basedn = 'dc=example,dc=net';
	my $ldap_rootdn = 'cn=Manager,dc=example,dc=net';
	my $ldap_rootpw = $rootpw;
	my $ldap_pwfile = "$test_temp/ldappassword";

	(my $conf = <<"EOC") =~ s/^\t\t//gm;
		include $ldap_schema_dir/core.schema
		include $ldap_schema_dir/cosine.schema
		include $ldap_schema_dir/nis.schema
		include $ldap_schema_dir/inetorgperson.schema

		pidfile $slapd_pidfile
		logfile $slapd_logfile

		access to *
		        by * read
		        by $authtype auth

		database ldif
		directory $ldap_datadir

		TLSCACertificateFile $slapd_certs/ca.crt
		TLSCertificateFile $slapd_certs/server.crt
		TLSCertificateKeyFile $slapd_certs/server.key

		suffix "dc=example,dc=net"
		rootdn "$ldap_rootdn"
		rootpw "$ldap_rootpw"
EOC
	append_to_file($slapd_conf, $conf);

	mkdir $ldap_datadir or die "making $ldap_datadir: $!";
	mkdir $slapd_certs or die "making $slapd_certs: $!";

	my $certdir = dirname(__FILE__) . "/../ssl/ssl";

	copy "$certdir/server_ca.crt", "$slapd_certs/ca.crt"
	  || die "copying ca.crt: $!";
	# check we actually have the file, as copy() sometimes gives a false success
	-f "$slapd_certs/ca.crt" || die "copying ca.crt (error unknown)";
	copy "$certdir/server-cn-only.crt", "$slapd_certs/server.crt"
	  || die "copying server.crt: $!";
	copy "$certdir/server-cn-only.key", "$slapd_certs/server.key"
	  || die "copying server.key: $!";

	append_to_file($ldap_pwfile, $ldap_rootpw);
	chmod 0600, $ldap_pwfile or die "chmod on $ldap_pwfile";

	# -s0 prevents log messages ending up in syslog
	system_or_bail $slapd, '-f', $slapd_conf, '-s0', '-h',
	  "$ldap_url $ldaps_url";

	# wait until slapd accepts requests
	my $retries = 0;
	while (1)
	{
		last
		  if (
			system_log(
				"ldapsearch", "-sbase",
				"-H", $ldap_url,
				"-b", $ldap_basedn,
				"-D", $ldap_rootdn,
				"-y", $ldap_pwfile,
				"-n", "'objectclass=*'") == 0);
		die "cannot connect to slapd" if ++$retries >= 300;
		note "waiting for slapd to accept requests...";
		Time::HiRes::usleep(1000000);
	}

	$self->{pidfile} = $slapd_pidfile;
	$self->{pwfile} = $ldap_pwfile;
	$self->{url} = $ldap_url;
	$self->{s_url} = $ldaps_url;
	$self->{server} = $ldap_server;
	$self->{port} = $ldap_port;
	$self->{s_port} = $ldaps_port;
	$self->{basedn} = $ldap_basedn;
	$self->{rootdn} = $ldap_rootdn;

	bless $self, $class;
	push @servers, $self;
	return $self;
}

# private routine to set up the environment for methods below
sub _ldapenv
{
	my $self = shift;
	my %env = %ENV;
	$env{'LDAPURI'} = $self->{url};
	$env{'LDAPBINDDN'} = $self->{rootdn};
	return %env;
}

=pod

=over

=item ldapadd_file(filename)

filename is the path to a file containing LDIF data which is added to the LDAP
server.

=back

=cut

sub ldapadd_file
{
	my $self = shift;
	my $file = shift;

	local %ENV = $self->_ldapenv;

	system_or_bail 'ldapadd', '-x', '-y', $self->{pwfile}, '-f', $file;
}

=pod

=over

=item ldapsetpw(user, password)

Set the user's password in the LDAP server

=back

=cut

sub ldapsetpw
{
	my $self = shift;
	my $user = shift;
	my $password = shift;

	local %ENV = $self->_ldapenv;

	system_or_bail 'ldappasswd', '-x', '-y', $self->{pwfile}, '-s', $password,
	  $user;
}

=pod

=over

=item prop(name1, ...)

Returns the list of values for the specified properties of the instance, such
as 'url', 'port', 'basedn'.

=back

=cut

sub prop
{
	my $self = shift;
	my @settings;
	push @settings, $self->{$_} foreach (@_);
	return @settings;
}

1;

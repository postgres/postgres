
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

=pod

=head1 NAME

PostgreSQL::Test::Cluster - class representing PostgreSQL server instance

=head1 SYNOPSIS

  use PostgreSQL::Test::Cluster;

  my $node = PostgreSQL::Test::Cluster->new('mynode');

  # Create a data directory with initdb
  $node->init();

  # Start the PostgreSQL server
  $node->start();

  # Add a setting and restart
  $node->append_conf('postgresql.conf', 'hot_standby = on');
  $node->restart();

  # Modify or delete an existing setting
  $node->adjust_conf('postgresql.conf', 'max_wal_senders', '10');

  # get pg_config settings
  # all the settings in one string
  $pgconfig = $node->config_data;
  # all the settings as a map
  %config_map = ($node->config_data);
  # specified settings
  ($incdir, $sharedir) = $node->config_data(qw(--includedir --sharedir));

  # run a query with psql, like:
  #   echo 'SELECT 1' | psql -qAXt postgres -v ON_ERROR_STOP=1
  $psql_stdout = $node->safe_psql('postgres', 'SELECT 1');

  # Run psql with a timeout, capturing stdout and stderr
  # as well as the psql exit code. Pass some extra psql
  # options. If there's an error from psql raise an exception.
  my ($stdout, $stderr, $timed_out);
  my $cmdret = $node->psql('postgres', 'SELECT pg_sleep(600)',
	  stdout => \$stdout, stderr => \$stderr,
	  timeout => $PostgreSQL::Test::Utils::timeout_default,
	  timed_out => \$timed_out,
	  extra_params => ['--single-transaction'],
	  on_error_die => 1)
  print "Sleep timed out" if $timed_out;

  # Similar thing, more convenient in common cases
  my ($cmdret, $stdout, $stderr) =
      $node->psql('postgres', 'SELECT 1');

  # run query every second until it returns 't'
  # or times out
  $node->poll_query_until('postgres', q|SELECT random() < 0.1;|')
    or die "timed out";

  # Do an online pg_basebackup
  my $ret = $node->backup('testbackup1');

  # Take a backup of a running server
  my $ret = $node->backup_fs_hot('testbackup2');

  # Take a backup of a stopped server
  $node->stop;
  my $ret = $node->backup_fs_cold('testbackup3')

  # Restore it to create a new independent node (not a replica)
  my $other_node = PostgreSQL::Test::Cluster->new('mycopy');
  $other_node->init_from_backup($node, 'testbackup');
  $other_node->start;

  # Stop the server
  $node->stop('fast');

  # Find a free, unprivileged TCP port to bind some other service to
  my $port = PostgreSQL::Test::Cluster::get_free_port();

=head1 DESCRIPTION

PostgreSQL::Test::Cluster contains a set of routines able to work on a PostgreSQL node,
allowing to start, stop, backup and initialize it with various options.
The set of nodes managed by a given test is also managed by this module.

In addition to node management, PostgreSQL::Test::Cluster instances have some wrappers
around Test::More functions to run commands with an environment set up to
point to the instance.

The IPC::Run module is required.

=cut

package PostgreSQL::Test::Cluster;

use strict;
use warnings;

use Carp;
use Config;
use Fcntl qw(:mode :flock :seek :DEFAULT);
use File::Basename;
use File::Path qw(rmtree mkpath);
use File::Spec;
use File::stat qw(stat);
use File::Temp ();
use IPC::Run;
use PostgreSQL::Version;
use PostgreSQL::Test::RecursiveCopy;
use Socket;
use Test::More;
use PostgreSQL::Test::Utils ();
use Time::HiRes qw(usleep);
use Scalar::Util qw(blessed);

our ($use_tcp, $test_localhost, $test_pghost, $last_host_assigned,
	$last_port_assigned, @all_nodes, $died, $portdir);

# the minimum version we believe to be compatible with this package without
# subclassing.
our $min_compat = 12;

# list of file reservations made by get_free_port
my @port_reservation_files;

INIT
{

	# Set PGHOST for backward compatibility.  This doesn't work for own_host
	# nodes, so prefer to not rely on this when writing new tests.
	$use_tcp            = !$PostgreSQL::Test::Utils::use_unix_sockets;
	$test_localhost     = "127.0.0.1";
	$last_host_assigned = 1;
	if ($use_tcp)
	{
		$test_pghost = $test_localhost;
	}
	else
	{
		# On windows, replace windows-style \ path separators with / when
		# putting socket directories either in postgresql.conf or libpq
		# connection strings, otherwise they are interpreted as escapes.
		$test_pghost = PostgreSQL::Test::Utils::tempdir_short;
		$test_pghost =~ s!\\!/!g if $PostgreSQL::Test::Utils::windows_os;
	}
	$ENV{PGHOST}     = $test_pghost;
	$ENV{PGDATABASE} = 'postgres';

	# Tracking of last port value assigned to accelerate free port lookup.
	$last_port_assigned = int(rand() * 16384) + 49152;

	# Set the port lock directory

	# If we're told to use a directory (e.g. from a buildfarm client)
	# explicitly, use that
	$portdir = $ENV{PG_TEST_PORT_DIR};
	# Otherwise, try to use a directory at the top of the build tree
	# or as a last resort use the tmp_check directory
	my $build_dir = $ENV{top_builddir}
	  || $PostgreSQL::Test::Utils::tmp_check ;
	$portdir ||= "$build_dir/portlock";
	$portdir =~ s!\\!/!g;
	# Make sure the directory exists
	mkpath($portdir) unless -d $portdir;
}

=pod

=head1 METHODS

=over

=item $node->port()

Get the port number assigned to the host. This won't necessarily be a TCP port
open on the local host since we prefer to use unix sockets if possible.

Use $node->connstr() if you want a connection string.

=cut

sub port
{
	my ($self) = @_;
	return $self->{_port};
}

=pod

=item $node->host()

Return the host (like PGHOST) for this instance. May be a UNIX socket path.

Use $node->connstr() if you want a connection string.

=cut

sub host
{
	my ($self) = @_;
	return $self->{_host};
}

=pod

=item $node->basedir()

The directory all the node's files will be within - datadir, archive directory,
backups, etc.

=cut

sub basedir
{
	my ($self) = @_;
	return $self->{_basedir};
}

=pod

=item $node->name()

The name assigned to the node at creation time.

=cut

sub name
{
	my ($self) = @_;
	return $self->{_name};
}

=pod

=item $node->logfile()

Path to the PostgreSQL log file for this instance.

=cut

sub logfile
{
	my ($self) = @_;
	return $self->{_logfile};
}

=pod

=item $node->connstr()

Get a libpq connection string that will establish a connection to
this node. Suitable for passing to psql, DBD::Pg, etc.

=cut

sub connstr
{
	my ($self, $dbname) = @_;
	my $pgport = $self->port;
	my $pghost = $self->host;
	if (!defined($dbname))
	{
		return "port=$pgport host=$pghost";
	}

	# Escape properly the database string before using it, only
	# single quotes and backslashes need to be treated this way.
	$dbname =~ s#\\#\\\\#g;
	$dbname =~ s#\'#\\\'#g;

	return "port=$pgport host=$pghost dbname='$dbname'";
}

=pod

=item $node->group_access()

Does the data dir allow group access?

=cut

sub group_access
{
	my ($self) = @_;

	my $dir_stat = stat($self->data_dir);

	defined($dir_stat)
	  or die('unable to stat ' . $self->data_dir);

	return (S_IMODE($dir_stat->mode) == 0750);
}

=pod

=item $node->data_dir()

Returns the path to the data directory. postgresql.conf and pg_hba.conf are
always here.

=cut

sub data_dir
{
	my ($self) = @_;
	my $res = $self->basedir;
	return "$res/pgdata";
}

=pod

=item $node->archive_dir()

If archiving is enabled, WAL files go here.

=cut

sub archive_dir
{
	my ($self) = @_;
	my $basedir = $self->basedir;
	return "$basedir/archives";
}

=pod

=item $node->backup_dir()

The output path for backups taken with $node->backup()

=cut

sub backup_dir
{
	my ($self) = @_;
	my $basedir = $self->basedir;
	return "$basedir/backup";
}

=pod

=item $node->install_path()

The configured install path (if any) for the node.

=cut

sub install_path
{
	my ($self) = @_;
	return $self->{_install_path};
}

=pod

=item $node->pg_version()

The version number for the node, from PostgreSQL::Version.

=cut

sub pg_version
{
	my ($self) = @_;
	return $self->{_pg_version};
}

=pod

=item $node->config_data( option ...)

Return configuration data from pg_config, using options (if supplied).
The options will be things like '--sharedir'.

If no options are supplied, return a string in scalar context or a map in
array context.

If options are supplied, return the list of values.

=cut

sub config_data
{
	my ($self, @options) = @_;
	local %ENV = $self->_get_env();

	my ($stdout, $stderr);
	my $result =
	  IPC::Run::run [ $self->installed_command('pg_config'), @options ],
	  '>', \$stdout, '2>', \$stderr
	  or die "could not execute pg_config";
	# standardize line endings
	$stdout =~ s/\r(?=\n)//g;
	# no options, scalar context: just hand back the output
	return $stdout unless (wantarray || @options);
	chomp($stdout);
	# exactly one option: hand back the output (minus LF)
	return $stdout if (@options == 1);
	my @lines = split(/\n/, $stdout);
	# more than one option: hand back the list of values;
	return @lines if (@options);
	# no options, array context: return a map
	my @map;
	foreach my $line (@lines)
	{
		my ($k,$v) = split (/ = /,$line,2);
		push(@map, $k, $v);
	}
	return @map;
}

=pod

=item $node->info()

Return a string containing human-readable diagnostic information (paths, etc)
about this node.

=cut

sub info
{
	my ($self) = @_;
	my $_info = '';
	open my $fh, '>', \$_info or die;
	print $fh "Name: " . $self->name . "\n";
	print $fh "Version: " . $self->{_pg_version} . "\n"
	  if $self->{_pg_version};
	print $fh "Data directory: " . $self->data_dir . "\n";
	print $fh "Backup directory: " . $self->backup_dir . "\n";
	print $fh "Archive directory: " . $self->archive_dir . "\n";
	print $fh "Connection string: " . $self->connstr . "\n";
	print $fh "Log file: " . $self->logfile . "\n";
	print $fh "Install Path: ", $self->{_install_path} . "\n"
	  if $self->{_install_path};
	close $fh or die;
	return $_info;
}

=pod

=item $node->dump_info()

Print $node->info()

=cut

sub dump_info
{
	my ($self) = @_;
	print $self->info;
	return;
}


# Internal method to set up trusted pg_hba.conf for replication.  Not
# documented because you shouldn't use it, it's called automatically if needed.
sub set_replication_conf
{
	my ($self) = @_;
	my $pgdata = $self->data_dir;

	$self->host eq $test_pghost
	  or croak "set_replication_conf only works with the default host";

	open my $hba, '>>', "$pgdata/pg_hba.conf";
	print $hba
	  "\n# Allow replication (set up by PostgreSQL::Test::Cluster.pm)\n";
	if ($PostgreSQL::Test::Utils::windows_os
		&& !$PostgreSQL::Test::Utils::use_unix_sockets)
	{
		print $hba
		  "host replication all $test_localhost/32 sspi include_realm=1 map=regress\n";
	}
	close $hba;
	return;
}

=pod

=item $node->init(...)

Initialize a new cluster for testing.

Authentication is set up so that only the current OS user can access the
cluster. On Unix, we use Unix domain socket connections, with the socket in
a directory that's only accessible to the current user to ensure that.
On Windows, we use SSPI authentication to ensure the same (by pg_regress
--config-auth).

WAL archiving can be enabled on this node by passing the keyword parameter
has_archiving => 1. This is disabled by default.

postgresql.conf can be set up for replication by passing the keyword
parameter allows_streaming => 'logical' or 'physical' (passing 1 will also
suffice for physical replication) depending on type of replication that
should be enabled. This is disabled by default.

The new node is set up in a fast but unsafe configuration where fsync is
disabled.

=cut

sub init
{
	my ($self, %params) = @_;
	my $port   = $self->port;
	my $pgdata = $self->data_dir;
	my $host   = $self->host;

	local %ENV = $self->_get_env();

	$params{allows_streaming} = 0 unless defined $params{allows_streaming};
	$params{has_archiving}    = 0 unless defined $params{has_archiving};

	mkdir $self->backup_dir;
	mkdir $self->archive_dir;

	PostgreSQL::Test::Utils::system_or_bail('initdb', '-D', $pgdata, '-A',
		'trust', '-N', @{ $params{extra} });
	PostgreSQL::Test::Utils::system_or_bail($ENV{PG_REGRESS},
		'--config-auth', $pgdata, @{ $params{auth_extra} });

	open my $conf, '>>', "$pgdata/postgresql.conf";
	print $conf "\n# Added by PostgreSQL::Test::Cluster.pm\n";
	print $conf "fsync = off\n";
	print $conf "restart_after_crash = off\n";
	print $conf "log_line_prefix = '%m [%p] %q%a '\n";
	print $conf "log_statement = all\n";
	print $conf "log_replication_commands = on\n";
	print $conf "wal_retrieve_retry_interval = '500ms'\n";

	# If a setting tends to affect whether tests pass or fail, print it after
	# TEMP_CONFIG.  Otherwise, print it before TEMP_CONFIG, thereby permitting
	# overrides.  Settings that merely improve performance or ease debugging
	# belong before TEMP_CONFIG.
	print $conf PostgreSQL::Test::Utils::slurp_file($ENV{TEMP_CONFIG})
	  if defined $ENV{TEMP_CONFIG};

	if ($params{allows_streaming})
	{
		if ($params{allows_streaming} eq "logical")
		{
			print $conf "wal_level = logical\n";
		}
		else
		{
			print $conf "wal_level = replica\n";
		}
		print $conf "max_wal_senders = 10\n";
		print $conf "max_replication_slots = 10\n";
		print $conf "wal_log_hints = on\n";
		print $conf "hot_standby = on\n";
		# conservative settings to ensure we can run multiple postmasters:
		print $conf "shared_buffers = 1MB\n";
		print $conf "max_connections = 10\n";
		# limit disk space consumption, too:
		print $conf "max_wal_size = 128MB\n";
	}
	else
	{
		print $conf "wal_level = minimal\n";
		print $conf "max_wal_senders = 0\n";
	}

	print $conf "port = $port\n";
	if ($use_tcp)
	{
		print $conf "unix_socket_directories = ''\n";
		print $conf "listen_addresses = '$host'\n";
	}
	else
	{
		print $conf "unix_socket_directories = '$host'\n";
		print $conf "listen_addresses = ''\n";
	}
	close $conf;

	chmod($self->group_access ? 0640 : 0600, "$pgdata/postgresql.conf")
	  or die("unable to set permissions for $pgdata/postgresql.conf");

	$self->set_replication_conf if $params{allows_streaming};
	$self->enable_archiving     if $params{has_archiving};
	return;
}

=pod

=item $node->append_conf(filename, str)

A shortcut method to append to files like pg_hba.conf and postgresql.conf.

Does no validation or sanity checking. Does not reload the configuration
after writing.

A newline is automatically appended to the string.

=cut

sub append_conf
{
	my ($self, $filename, $str) = @_;

	my $conffile = $self->data_dir . '/' . $filename;

	PostgreSQL::Test::Utils::append_to_file($conffile, $str . "\n");

	chmod($self->group_access() ? 0640 : 0600, $conffile)
	  or die("unable to set permissions for $conffile");

	return;
}

=pod

=item $node->adjust_conf(filename, setting, value, skip_equals)

Modify the named config file setting with the value. If the value is undefined,
instead delete the setting. If the setting is not present no action is taken.

This will write "$setting = $value\n" in place of the existing line,
unless skip_equals is true, in which case it will write
"$setting $value\n". If the value needs to be quoted it is the caller's
responsibility to do that.

=cut

sub adjust_conf
{
	my ($self, $filename, $setting, $value, $skip_equals) = @_;

	my $conffile = $self->data_dir . '/' . $filename;

	my $contents = PostgreSQL::Test::Utils::slurp_file($conffile);
	my @lines = split(/\n/, $contents);
	my @result;
	my $eq = $skip_equals ? '' : '= ';
	foreach my $line (@lines)
	{
		if ($line !~ /^$setting\W/)
		{
			push(@result, "$line\n");
		}
		elsif (defined $value)
		{
			push(@result, "$setting $eq$value\n");
		}
	}
	open my $fh, ">", $conffile
	  or croak "could not write \"$conffile\": $!";
	print $fh @result;
	close $fh;

	chmod($self->group_access() ? 0640 : 0600, $conffile)
	  or die("unable to set permissions for $conffile");
}

=pod

=item $node->backup(backup_name)

Create a hot backup with B<pg_basebackup> in subdirectory B<backup_name> of
B<< $node->backup_dir >>, including the WAL.

By default, WAL files are fetched at the end of the backup, not streamed.
You can adjust that and other things by passing an array of additional
B<pg_basebackup> command line options in the keyword parameter backup_options.

You'll have to configure a suitable B<max_wal_senders> on the
target server since it isn't done by default.

=cut

sub backup
{
	my ($self, $backup_name, %params) = @_;
	my $backup_path = $self->backup_dir . '/' . $backup_name;
	my $name        = $self->name;

	local %ENV = $self->_get_env();

	print "# Taking pg_basebackup $backup_name from node \"$name\"\n";
	PostgreSQL::Test::Utils::system_or_bail(
		'pg_basebackup', '-D',
		$backup_path,    '-h',
		$self->host,     '-p',
		$self->port,     '--checkpoint',
		'fast',          '--no-sync',
		@{ $params{backup_options} });
	print "# Backup finished\n";
	return;
}

=item $node->backup_fs_cold(backup_name)

Create a backup with a filesystem level copy in subdirectory B<backup_name> of
B<< $node->backup_dir >>, including WAL. The server must be
stopped as no attempt to handle concurrent writes is made.

Use B<backup> or B<backup_fs_hot> if you want to back up a running server.

=cut

sub backup_fs_cold
{
	my ($self, $backup_name) = @_;

	PostgreSQL::Test::RecursiveCopy::copypath(
		$self->data_dir,
		$self->backup_dir . '/' . $backup_name,
		filterfn => sub {
			my $src = shift;
			return ($src ne 'log' and $src ne 'postmaster.pid');
		});

	return;
}

=pod

=item $node->init_from_backup(root_node, backup_name)

Initialize a node from a backup, which may come from this node or a different
node. root_node must be a PostgreSQL::Test::Cluster reference, backup_name the string name
of a backup previously created on that node with $node->backup.

Does not start the node after initializing it.

By default, the backup is assumed to be plain format.  To restore from
a tar-format backup, pass the name of the tar program to use in the
keyword parameter tar_program.  Note that tablespace tar files aren't
handled here.

Streaming replication can be enabled on this node by passing the keyword
parameter has_streaming => 1. This is disabled by default.

Restoring WAL segments from archives using restore_command can be enabled
by passing the keyword parameter has_restoring => 1. This is disabled by
default.

If has_restoring is used, standby mode is used by default.  To use
recovery mode instead, pass the keyword parameter standby => 0.

The backup is copied, leaving the original unmodified. pg_hba.conf is
unconditionally set to enable replication connections.

=cut

sub init_from_backup
{
	my ($self, $root_node, $backup_name, %params) = @_;
	my $backup_path = $root_node->backup_dir . '/' . $backup_name;
	my $host        = $self->host;
	my $port        = $self->port;
	my $node_name   = $self->name;
	my $root_name   = $root_node->name;

	$params{has_streaming} = 0 unless defined $params{has_streaming};
	$params{has_restoring} = 0 unless defined $params{has_restoring};
	$params{standby}       = 1 unless defined $params{standby};

	print
	  "# Initializing node \"$node_name\" from backup \"$backup_name\" of node \"$root_name\"\n";
	croak "Backup \"$backup_name\" does not exist at $backup_path"
	  unless -d $backup_path;

	mkdir $self->backup_dir;
	mkdir $self->archive_dir;

	my $data_path = $self->data_dir;
	if (defined $params{tar_program})
	{
		mkdir($data_path);
		PostgreSQL::Test::Utils::system_or_bail($params{tar_program}, 'xf',
			$backup_path . '/base.tar',
			'-C', $data_path);
		PostgreSQL::Test::Utils::system_or_bail(
			$params{tar_program},         'xf',
			$backup_path . '/pg_wal.tar', '-C',
			$data_path . '/pg_wal');
	}
	else
	{
		rmdir($data_path);
		PostgreSQL::Test::RecursiveCopy::copypath($backup_path, $data_path);
	}
	chmod(0700, $data_path);

	# Base configuration for this node
	$self->append_conf(
		'postgresql.conf',
		qq(
port = $port
));
	if ($use_tcp)
	{
		$self->append_conf('postgresql.conf', "listen_addresses = '$host'");
	}
	else
	{
		$self->append_conf('postgresql.conf',
			"unix_socket_directories = '$host'");
	}
	$self->enable_streaming($root_node) if $params{has_streaming};
	$self->enable_restoring($root_node, $params{standby})
	  if $params{has_restoring};
	return;
}

=pod

=item $node->rotate_logfile()

Switch to a new PostgreSQL log file.  This does not alter any running
PostgreSQL process.  Subsequent method calls, including pg_ctl invocations,
will use the new name.  Return the new name.

=cut

sub rotate_logfile
{
	my ($self) = @_;
	$self->{_logfile} = sprintf('%s_%d.log',
		$self->{_logfile_base},
		++$self->{_logfile_generation});
	return $self->{_logfile};
}

=pod

=item $node->start(%params) => success_or_failure

Wrapper for pg_ctl start

Start the node and wait until it is ready to accept connections.

=over

=item fail_ok => 1

By default, failure terminates the entire F<prove> invocation.  If given,
instead return a true or false value to indicate success or failure.

=back

=cut

sub start
{
	my ($self, %params) = @_;
	my $port   = $self->port;
	my $pgdata = $self->data_dir;
	my $name   = $self->name;
	my $ret;

	BAIL_OUT("node \"$name\" is already running") if defined $self->{_pid};

	print("### Starting node \"$name\"\n");

	# Temporarily unset PGAPPNAME so that the server doesn't
	# inherit it.  Otherwise this could affect libpqwalreceiver
	# connections in confusing ways.
	local %ENV = $self->_get_env(PGAPPNAME => undef);

	# Note: We set the cluster_name here, not in postgresql.conf (in
	# sub init) so that it does not get copied to standbys.
	# -w is now the default but having it here does no harm and helps
	# compatibility with older versions.
	$ret = PostgreSQL::Test::Utils::system_log(
		'pg_ctl', '-w',           '-D', $self->data_dir,
		'-l',     $self->logfile, '-o', "--cluster-name=$name",
		'start');

	if ($ret != 0)
	{
		print "# pg_ctl start failed; logfile:\n";
		print PostgreSQL::Test::Utils::slurp_file($self->logfile);

		# pg_ctl could have timed out, so check to see if there's a pid file;
		# otherwise our END block will fail to shut down the new postmaster.
		$self->_update_pid(-1);

		BAIL_OUT("pg_ctl start failed") unless $params{fail_ok};
		return 0;
	}

	$self->_update_pid(1);
	return 1;
}

=pod

=item $node->kill9()

Send SIGKILL (signal 9) to the postmaster.

Note: if the node is already known stopped, this does nothing.
However, if we think it's running and it's not, it's important for
this to fail.  Otherwise, tests might fail to detect server crashes.

=cut

sub kill9
{
	my ($self) = @_;
	my $name = $self->name;
	return unless defined $self->{_pid};

	local %ENV = $self->_get_env();

	print "### Killing node \"$name\" using signal 9\n";
	kill(9, $self->{_pid});
	$self->{_pid} = undef;
	return;
}

=pod

=item $node->stop(mode)

Stop the node using pg_ctl -m $mode and wait for it to stop.

Note: if the node is already known stopped, this does nothing.
However, if we think it's running and it's not, it's important for
this to fail.  Otherwise, tests might fail to detect server crashes.

With optional extra param fail_ok => 1, returns 0 for failure
instead of bailing out.

=cut

sub stop
{
	my ($self, $mode, %params) = @_;
	my $pgdata = $self->data_dir;
	my $name   = $self->name;
	my $ret;

	local %ENV = $self->_get_env();

	$mode = 'fast' unless defined $mode;
	return 1 unless defined $self->{_pid};

	print "### Stopping node \"$name\" using mode $mode\n";
	$ret = PostgreSQL::Test::Utils::system_log('pg_ctl', '-D', $pgdata,
		'-m', $mode, 'stop');

	if ($ret != 0)
	{
		print "# pg_ctl stop failed: $ret\n";

		# Check to see if we still have a postmaster or not.
		$self->_update_pid(-1);

		BAIL_OUT("pg_ctl stop failed") unless $params{fail_ok};
		return 0;
	}

	$self->_update_pid(0);
	return 1;
}

=pod

=item $node->reload()

Reload configuration parameters on the node.

=cut

sub reload
{
	my ($self) = @_;
	my $port   = $self->port;
	my $pgdata = $self->data_dir;
	my $name   = $self->name;

	local %ENV = $self->_get_env();

	print "### Reloading node \"$name\"\n";
	PostgreSQL::Test::Utils::system_or_bail('pg_ctl', '-D', $pgdata,
		'reload');
	return;
}

=pod

=item $node->restart()

Wrapper for pg_ctl restart

=cut

sub restart
{
	my ($self)  = @_;
	my $port    = $self->port;
	my $pgdata  = $self->data_dir;
	my $logfile = $self->logfile;
	my $name    = $self->name;

	local %ENV = $self->_get_env(PGAPPNAME => undef);

	print "### Restarting node \"$name\"\n";

	# -w is now the default but having it here does no harm and helps
	# compatibility with older versions.
	PostgreSQL::Test::Utils::system_or_bail('pg_ctl', '-w', '-D', $pgdata,
		'-l', $logfile, 'restart');

	$self->_update_pid(1);
	return;
}

=pod

=item $node->promote()

Wrapper for pg_ctl promote

=cut

sub promote
{
	my ($self)  = @_;
	my $port    = $self->port;
	my $pgdata  = $self->data_dir;
	my $logfile = $self->logfile;
	my $name    = $self->name;

	local %ENV = $self->_get_env();

	print "### Promoting node \"$name\"\n";
	PostgreSQL::Test::Utils::system_or_bail('pg_ctl', '-D', $pgdata, '-l',
		$logfile, 'promote');
	return;
}

=pod

=item $node->logrotate()

Wrapper for pg_ctl logrotate

=cut

sub logrotate
{
	my ($self)  = @_;
	my $port    = $self->port;
	my $pgdata  = $self->data_dir;
	my $logfile = $self->logfile;
	my $name    = $self->name;

	local %ENV = $self->_get_env();

	print "### Rotating log in node \"$name\"\n";
	PostgreSQL::Test::Utils::system_or_bail('pg_ctl', '-D', $pgdata, '-l',
		$logfile, 'logrotate');
	return;
}

# Internal routine to enable streaming replication on a standby node.
sub enable_streaming
{
	my ($self, $root_node) = @_;
	my $root_connstr = $root_node->connstr;
	my $name         = $self->name;

	print "### Enabling streaming replication for node \"$name\"\n";
	$self->append_conf(
		$self->_recovery_file, qq(
primary_conninfo='$root_connstr'
));
	$self->set_standby_mode();
	return;
}

# Internal routine to enable archive recovery command on a standby node
sub enable_restoring
{
	my ($self, $root_node, $standby) = @_;
	my $path = $root_node->archive_dir;
	my $name = $self->name;

	print "### Enabling WAL restore for node \"$name\"\n";

	# On Windows, the path specified in the restore command needs to use
	# double back-slashes to work properly and to be able to detect properly
	# the file targeted by the copy command, so the directory value used
	# in this routine, using only one back-slash, need to be properly changed
	# first. Paths also need to be double-quoted to prevent failures where
	# the path contains spaces.
	$path =~ s{\\}{\\\\}g if ($PostgreSQL::Test::Utils::windows_os);
	my $copy_command =
	  $PostgreSQL::Test::Utils::windows_os
	  ? qq{copy "$path\\\\%f" "%p"}
	  : qq{cp "$path/%f" "%p"};

	$self->append_conf(
		$self->_recovery_file, qq(
restore_command = '$copy_command'
));
	if ($standby)
	{
		$self->set_standby_mode();
	}
	else
	{
		$self->set_recovery_mode();
	}
	return;
}

sub _recovery_file { return "postgresql.conf"; }

=pod

=item $node->set_recovery_mode()

Place recovery.signal file.

=cut

sub set_recovery_mode
{
	my ($self) = @_;

	$self->append_conf('recovery.signal', '');
	return;
}

=pod

=item $node->set_standby_mode()

Place standby.signal file.

=cut

sub set_standby_mode
{
	my ($self) = @_;

	$self->append_conf('standby.signal', '');
	return;
}

# Internal routine to enable archiving
sub enable_archiving
{
	my ($self) = @_;
	my $path   = $self->archive_dir;
	my $name   = $self->name;

	print "### Enabling WAL archiving for node \"$name\"\n";

	# On Windows, the path specified in the restore command needs to use
	# double back-slashes to work properly and to be able to detect properly
	# the file targeted by the copy command, so the directory value used
	# in this routine, using only one back-slash, need to be properly changed
	# first. Paths also need to be double-quoted to prevent failures where
	# the path contains spaces.
	$path =~ s{\\}{\\\\}g if ($PostgreSQL::Test::Utils::windows_os);
	my $copy_command =
	  $PostgreSQL::Test::Utils::windows_os
	  ? qq{copy "%p" "$path\\\\%f"}
	  : qq{cp "%p" "$path/%f"};

	# Enable archive_mode and archive_command on node
	$self->append_conf(
		'postgresql.conf', qq(
archive_mode = on
archive_command = '$copy_command'
));
	return;
}

# Internal method to update $self->{_pid}
# $is_running = 1: pid file should be there
# $is_running = 0: pid file should NOT be there
# $is_running = -1: we aren't sure
sub _update_pid
{
	my ($self, $is_running) = @_;
	my $name = $self->name;

	# If we can open the PID file, read its first line and that's the PID we
	# want.
	if (open my $pidfile, '<', $self->data_dir . "/postmaster.pid")
	{
		chomp($self->{_pid} = <$pidfile>);
		close $pidfile;

		# If we aren't sure what to expect, validate the PID using kill().
		# This protects against stale PID files left by crashed postmasters.
		if ($is_running == -1 && kill(0, $self->{_pid}) == 0)
		{
			print
			  "# Stale postmaster.pid file for node \"$name\": PID $self->{_pid} no longer exists\n";
			$self->{_pid} = undef;
			return;
		}

		print "# Postmaster PID for node \"$name\" is $self->{_pid}\n";

		# If we found a pidfile when there shouldn't be one, complain.
		BAIL_OUT("postmaster.pid unexpectedly present") if $is_running == 0;
		return;
	}

	$self->{_pid} = undef;
	print "# No postmaster PID for node \"$name\"\n";

	# Complain if we expected to find a pidfile.
	BAIL_OUT("postmaster.pid unexpectedly not present") if $is_running == 1;
	return;
}

=pod

=item PostgreSQL::Test::Cluster->new(node_name, %params)

Build a new object of class C<PostgreSQL::Test::Cluster> (or of a subclass, if you have
one), assigning a free port number.  Remembers the node, to prevent its port
number from being reused for another node, and to ensure that it gets
shut down when the test script exits.

=over

=item port => [1,65535]

By default, this function assigns a port number to each node.  Specify this to
force a particular port number.  The caller is responsible for evaluating
potential conflicts and privilege requirements.

=item own_host => 1

By default, all nodes use the same PGHOST value.  If specified, generate a
PGHOST specific to this node.  This allows multiple nodes to use the same
port.

=item install_path => '/path/to/postgres/installation'

Using this parameter is it possible to have nodes pointing to different
installations, for testing different versions together or the same version
with different build parameters. The provided path must be the parent of the
installation's 'bin' and 'lib' directories. In the common case where this is
not provided, Postgres binaries will be found in the caller's PATH.

=back

=cut

sub new
{
	my $class = shift;
	my ($name, %params) = @_;

	# Select a port.
	my $port;
	if (defined $params{port})
	{
		$port = $params{port};
	}
	else
	{
		# When selecting a port, we look for an unassigned TCP port number,
		# even if we intend to use only Unix-domain sockets.  This is clearly
		# necessary on $use_tcp (Windows) configurations, and it seems like a
		# good idea on Unixen as well.
		$port = get_free_port();
	}

	# Select a host.
	my $host = $test_pghost;
	if ($params{own_host})
	{
		if ($use_tcp)
		{
			$last_host_assigned++;
			$last_host_assigned > 254 and BAIL_OUT("too many own_host nodes");
			$host = '127.0.0.' . $last_host_assigned;
		}
		else
		{
			$host = "$test_pghost/$name"; # Assume $name =~ /^[-_a-zA-Z0-9]+$/
			mkdir $host;
		}
	}

	my $testname = basename($0);
	$testname =~ s/\.[^.]+$//;
	my $node = {
		_port => $port,
		_host => $host,
		_basedir =>
		  "$PostgreSQL::Test::Utils::tmp_check/t_${testname}_${name}_data",
		_name               => $name,
		_logfile_generation => 0,
		_logfile_base =>
		  "$PostgreSQL::Test::Utils::log_path/${testname}_${name}",
		_logfile =>
		  "$PostgreSQL::Test::Utils::log_path/${testname}_${name}.log"
	};

	if ($params{install_path})
	{
		$node->{_install_path} = $params{install_path};
	}

	bless $node, $class;
	mkdir $node->{_basedir}
	  or
	  BAIL_OUT("could not create data directory \"$node->{_basedir}\": $!");

	$node->dump_info;

	$node->_set_pg_version;

	my $ver = $node->{_pg_version};

	# Use a subclass as defined below (or elsewhere) if this version
	# isn't fully compatible. Warn if the version is too old and thus we don't
	# have a subclass of this class.
	if (ref $ver && $ver < $min_compat)
	{
		my $maj = $ver->major(separator => '_');
		my $subclass = $class . "::V_$maj";
		if ($subclass->isa($class))
		{
			bless $node, $subclass;
		}
		else
		{
			carp
			  "PostgreSQL::Test::Cluster isn't fully compatible with version $ver";
		}
	}

	# Add node to list of nodes
	push(@all_nodes, $node);

	return $node;
}

# Private routine to run the pg_config binary found in our environment (or in
# our install_path, if we have one), and set the version from it
#
sub _set_pg_version
{
	my ($self)    = @_;
	my $inst      = $self->{_install_path};
	my $pg_config = "pg_config";

	if (defined $inst)
	{
		# If the _install_path is invalid, our PATH variables might find an
		# unrelated pg_config executable elsewhere.  Sanity check the
		# directory.
		BAIL_OUT("directory not found: $inst")
		  unless -d $inst;

		# If the directory exists but is not the root of a postgresql
		# installation, or if the user configured using
		# --bindir=$SOMEWHERE_ELSE, we're not going to find pg_config, so
		# complain about that, too.
		$pg_config = "$inst/bin/pg_config";
		BAIL_OUT("pg_config not found: $pg_config")
		  unless -e $pg_config
		  or ($PostgreSQL::Test::Utils::windows_os and -e "$pg_config.exe");
		BAIL_OUT("pg_config not executable: $pg_config")
		  unless $PostgreSQL::Test::Utils::windows_os or -x $pg_config;

		# Leave $pg_config install_path qualified, to be sure we get the right
		# version information, below, or die trying
	}

	local %ENV = $self->_get_env();

	# We only want the version field
	my $version_line = qx{$pg_config --version};
	BAIL_OUT("$pg_config failed: $!") if $?;

	$self->{_pg_version} = PostgreSQL::Version->new($version_line);

	BAIL_OUT("could not parse pg_config --version output: $version_line")
	  unless defined $self->{_pg_version};
}

# Private routine to return a copy of the environment with the PATH and
# (DY)LD_LIBRARY_PATH correctly set when there is an install path set for
# the node.
#
# Routines that call Postgres binaries need to call this routine like this:
#
#    local %ENV = $self->_get_env([%extra_settings]);
#
# A copy of the environment is taken and node's host and port settings are
# added as PGHOST and PGPORT, then the extra settings (if any) are applied.
# Any setting in %extra_settings with a value that is undefined is deleted;
# the remainder are set. Then the PATH and (DY)LD_LIBRARY_PATH are adjusted
# if the node's install path is set, and the copy environment is returned.
#
# The install path set in new() needs to be a directory containing
# bin and lib subdirectories as in a standard PostgreSQL installation, so this
# can't be used with installations where the bin and lib directories don't have
# a common parent directory.
sub _get_env
{
	my $self = shift;
	my %inst_env = (%ENV, PGHOST => $self->{_host}, PGPORT => $self->{_port});
	# the remaining arguments are modifications to make to the environment
	my %mods = (@_);
	while (my ($k, $v) = each %mods)
	{
		if (defined $v)
		{
			$inst_env{$k} = "$v";
		}
		else
		{
			delete $inst_env{$k};
		}
	}
	# now fix up the new environment for the install path
	my $inst = $self->{_install_path};
	if ($inst)
	{
		if ($PostgreSQL::Test::Utils::windows_os)
		{
			# Windows picks up DLLs from the PATH rather than *LD_LIBRARY_PATH
			# choose the right path separator
			if ($Config{osname} eq 'MSWin32')
			{
				$inst_env{PATH} = "$inst/bin;$inst/lib;$ENV{PATH}";
			}
			else
			{
				$inst_env{PATH} = "$inst/bin:$inst/lib:$ENV{PATH}";
			}
		}
		else
		{
			my $dylib_name =
			  $Config{osname} eq 'darwin'
			  ? "DYLD_LIBRARY_PATH"
			  : "LD_LIBRARY_PATH";
			$inst_env{PATH} = "$inst/bin:$ENV{PATH}";
			if (exists $ENV{$dylib_name})
			{
				$inst_env{$dylib_name} = "$inst/lib:$ENV{$dylib_name}";
			}
			else
			{
				$inst_env{$dylib_name} = "$inst/lib";
			}
		}
	}
	return (%inst_env);
}

# Private routine to get an installation path qualified command.
#
# IPC::Run maintains a cache, %cmd_cache, mapping commands to paths.  Tests
# which use nodes spanning more than one postgres installation path need to
# avoid confusing which installation's binaries get run.  Setting $ENV{PATH} is
# insufficient, as IPC::Run does not check to see if the path has changed since
# caching a command.
sub installed_command
{
	my ($self, $cmd) = @_;

	# Nodes using alternate installation locations use their installation's
	# bin/ directory explicitly
	return join('/', $self->{_install_path}, 'bin', $cmd)
	  if defined $self->{_install_path};

	# Nodes implicitly using the default installation location rely on IPC::Run
	# to find the right binary, which should not cause %cmd_cache confusion,
	# because no nodes with other installation paths do it that way.
	return $cmd;
}

=pod

=item get_free_port()

Locate an unprivileged (high) TCP port that's not currently bound to
anything.  This is used by C<new()>, and also by some test cases that need to
start other, non-Postgres servers.

Ports assigned to existing PostgreSQL::Test::Cluster objects are automatically
excluded, even if those servers are not currently running.

The port number is reserved so that other concurrent test programs will not
try to use the same port.

Note: this is not an instance method. As it's not exported it should be
called from outside the module as C<PostgreSQL::Test::Cluster::get_free_port()>.

=cut

sub get_free_port
{
	my $found = 0;
	my $port  = $last_port_assigned;

	while ($found == 0)
	{

		# advance $port, wrapping correctly around range end
		$port = 49152 if ++$port >= 65536;
		print "# Checking port $port\n";

		# Check first that candidate port number is not included in
		# the list of already-registered nodes.
		$found = 1;
		foreach my $node (@all_nodes)
		{
			$found = 0 if ($node->port == $port);
		}

		# Check to see if anything else is listening on this TCP port.
		# Seek a port available for all possible listen_addresses values,
		# so callers can harness this port for the widest range of purposes.
		# The 0.0.0.0 test achieves that for MSYS, which automatically sets
		# SO_EXCLUSIVEADDRUSE.  Testing 0.0.0.0 is insufficient for Windows
		# native Perl (https://stackoverflow.com/a/14388707), so we also
		# have to test individual addresses.  Doing that for 127.0.0/24
		# addresses other than 127.0.0.1 might fail with EADDRNOTAVAIL on
		# non-Linux, non-Windows kernels.
		#
		# Thus, 0.0.0.0 and individual 127.0.0/24 addresses are tested
		# only on Windows and only when TCP usage is requested.
		if ($found == 1)
		{
			foreach my $addr (qw(127.0.0.1),
				($use_tcp && $PostgreSQL::Test::Utils::windows_os)
				  ? qw(127.0.0.2 127.0.0.3 0.0.0.0)
				  : ())
			{
				if (!can_bind($addr, $port))
				{
					$found = 0;
					last;
				}
			}
			$found = _reserve_port($port) if $found;
		}
	}

	print "# Found port $port\n";

	# Update port for next time
	$last_port_assigned = $port;

	return $port;
}

# Internal routine to check whether a host:port is available to bind
sub can_bind
{
	my ($host, $port) = @_;
	my $iaddr = inet_aton($host);
	my $paddr = sockaddr_in($port, $iaddr);
	my $proto = getprotobyname("tcp");

	socket(SOCK, PF_INET, SOCK_STREAM, $proto)
	  or die "socket failed: $!";

	# As in postmaster, don't use SO_REUSEADDR on Windows
	setsockopt(SOCK, SOL_SOCKET, SO_REUSEADDR, pack("l", 1))
	  unless $PostgreSQL::Test::Utils::windows_os;
	my $ret = bind(SOCK, $paddr) && listen(SOCK, SOMAXCONN);
	close(SOCK);
	return $ret;
}

# Internal routine to reserve a port number
# Returns 1 if successful, 0 if port is already reserved.
sub _reserve_port
{
	my $port = shift;
	# open in rw mode so we don't have to reopen it and lose the lock
	my $filename = "$portdir/$port.rsv";
	sysopen(my $portfile, $filename, O_RDWR|O_CREAT)
	  || die "opening port file $filename: $!";
	# take an exclusive lock to avoid concurrent access
	flock($portfile, LOCK_EX) || die "locking port file $filename: $!";
	# see if someone else has or had a reservation of this port
	my $pid = <$portfile> || "0";
	chomp $pid;
	if ($pid +0 > 0)
	{
		if (kill 0, $pid)
		{
			# process exists and is owned by us, so we can't reserve this port
			flock($portfile, LOCK_UN);
			close($portfile);
			return 0;
		}
	}
	# All good, go ahead and reserve the port
	seek($portfile, 0, SEEK_SET);
	# print the pid with a fixed width so we don't leave any trailing junk
	print $portfile sprintf("%10d\n",$$);
	flock($portfile, LOCK_UN);
	close($portfile);
	push(@port_reservation_files, $filename);
	return 1;
}

# Automatically shut down any still-running nodes (in the same order the nodes
# were created in) when the test script exits.
END
{

	# take care not to change the script's exit value
	my $exit_code = $?;

	foreach my $node (@all_nodes)
	{
		$node->teardown_node;

		# skip clean if we are requested to retain the basedir
		next if defined $ENV{'PG_TEST_NOCLEAN'};

		# clean basedir on clean test invocation
		$node->clean_node
		  if $exit_code == 0 && PostgreSQL::Test::Utils::all_tests_passing();
	}

	unlink @port_reservation_files;

	$? = $exit_code;
}

=pod

=item $node->teardown_node()

Do an immediate stop of the node

=cut

sub teardown_node
{
	my $self = shift;

	$self->stop('immediate');
	return;
}

=pod

=item $node->clean_node()

Remove the base directory of the node if the node has been stopped.

=cut

sub clean_node
{
	my $self = shift;

	rmtree $self->{_basedir} unless defined $self->{_pid};
	return;
}

=pod

=item $node->safe_psql($dbname, $sql) => stdout

Invoke B<psql> to run B<sql> on B<dbname> and return its stdout on success.
Die if the SQL produces an error. Runs with B<ON_ERROR_STOP> set.

Takes optional extra params like timeout and timed_out parameters with the same
options as psql.

=cut

sub safe_psql
{
	my ($self, $dbname, $sql, %params) = @_;

	local %ENV = $self->_get_env();

	my ($stdout, $stderr);

	my $ret = $self->psql(
		$dbname, $sql,
		%params,
		stdout        => \$stdout,
		stderr        => \$stderr,
		on_error_die  => 1,
		on_error_stop => 1);

	# psql can emit stderr from NOTICEs etc
	if ($stderr ne "")
	{
		print "#### Begin standard error\n";
		print $stderr;
		print "\n#### End standard error\n";
	}

	return $stdout;
}

=pod

=item $node->psql($dbname, $sql, %params) => psql_retval

Invoke B<psql> to execute B<$sql> on B<$dbname> and return the return value
from B<psql>, which is run with on_error_stop by default so that it will
stop running sql and return 3 if the passed SQL results in an error.

As a convenience, if B<psql> is called in array context it returns an
array containing ($retval, $stdout, $stderr).

psql is invoked in tuples-only unaligned mode with reading of B<.psqlrc>
disabled.  That may be overridden by passing extra psql parameters.

stdout and stderr are transformed to UNIX line endings if on Windows. Any
trailing newline is removed.

Dies on failure to invoke psql but not if psql exits with a nonzero
return code (unless on_error_die specified).

If psql exits because of a signal, an exception is raised.

=over

=item stdout => \$stdout

B<stdout>, if given, must be a scalar reference to which standard output is
written.  If not given, standard output is not redirected and will be printed
unless B<psql> is called in array context, in which case it's captured and
returned.

=item stderr => \$stderr

Same as B<stdout> but gets standard error. If the same scalar is passed for
both B<stdout> and B<stderr> the results may be interleaved unpredictably.

=item on_error_stop => 1

By default, the B<psql> method invokes the B<psql> program with ON_ERROR_STOP=1
set, so SQL execution is stopped at the first error and exit code 3 is
returned.  Set B<on_error_stop> to 0 to ignore errors instead.

=item on_error_die => 0

By default, this method returns psql's result code. Pass on_error_die to
instead die with an informative message.

=item timeout => 'interval'

Set a timeout for the psql call as an interval accepted by B<IPC::Run::timer>
(integer seconds is fine).  This method raises an exception on timeout, unless
the B<timed_out> parameter is also given.

=item timed_out => \$timed_out

If B<timeout> is set and this parameter is given, the scalar it references
is set to true if the psql call times out.

=item connstr => B<value>

If set, use this as the connection string for the connection to the
backend.

=item replication => B<value>

If set, add B<replication=value> to the conninfo string.
Passing the literal value C<database> results in a logical replication
connection.

=item extra_params => ['--single-transaction']

If given, it must be an array reference containing additional parameters to B<psql>.

=back

e.g.

	my ($stdout, $stderr, $timed_out);
	my $cmdret = $node->psql('postgres', 'SELECT pg_sleep(600)',
		stdout => \$stdout, stderr => \$stderr,
		timeout => $PostgreSQL::Test::Utils::timeout_default,
		timed_out => \$timed_out,
		extra_params => ['--single-transaction'])

will set $cmdret to undef and $timed_out to a true value.

	$node->psql('postgres', $sql, on_error_die => 1);

dies with an informative message if $sql fails.

=cut

sub psql
{
	my ($self, $dbname, $sql, %params) = @_;

	local %ENV = $self->_get_env();

	my $stdout            = $params{stdout};
	my $stderr            = $params{stderr};
	my $replication       = $params{replication};
	my $timeout           = undef;
	my $timeout_exception = 'psql timed out';

	# Build the connection string.
	my $psql_connstr;
	if (defined $params{connstr})
	{
		$psql_connstr = $params{connstr};
	}
	else
	{
		$psql_connstr = $self->connstr($dbname);
	}
	$psql_connstr .= defined $replication ? " replication=$replication" : "";

	my @psql_params = (
		$self->installed_command('psql'),
		'-XAtq', '-d', $psql_connstr, '-f', '-');

	# If the caller wants an array and hasn't passed stdout/stderr
	# references, allocate temporary ones to capture them so we
	# can return them. Otherwise we won't redirect them at all.
	if (wantarray)
	{
		if (!defined($stdout))
		{
			my $temp_stdout = "";
			$stdout = \$temp_stdout;
		}
		if (!defined($stderr))
		{
			my $temp_stderr = "";
			$stderr = \$temp_stderr;
		}
	}

	$params{on_error_stop} = 1 unless defined $params{on_error_stop};
	$params{on_error_die}  = 0 unless defined $params{on_error_die};

	push @psql_params, '-v', 'ON_ERROR_STOP=1' if $params{on_error_stop};
	push @psql_params, @{ $params{extra_params} }
	  if defined $params{extra_params};

	$timeout =
	  IPC::Run::timeout($params{timeout}, exception => $timeout_exception)
	  if (defined($params{timeout}));

	${ $params{timed_out} } = 0 if defined $params{timed_out};

	# IPC::Run would otherwise append to existing contents:
	$$stdout = "" if ref($stdout);
	$$stderr = "" if ref($stderr);

	my $ret;

	# Run psql and capture any possible exceptions.  If the exception is
	# because of a timeout and the caller requested to handle that, just return
	# and set the flag.  Otherwise, and for any other exception, rethrow.
	#
	# For background, see
	# https://metacpan.org/pod/release/ETHER/Try-Tiny-0.24/lib/Try/Tiny.pm
	do
	{
		local $@;
		eval {
			my @ipcrun_opts = (\@psql_params, '<', \$sql);
			push @ipcrun_opts, '>',  $stdout if defined $stdout;
			push @ipcrun_opts, '2>', $stderr if defined $stderr;
			push @ipcrun_opts, $timeout if defined $timeout;

			IPC::Run::run @ipcrun_opts;
			$ret = $?;
		};
		my $exc_save = $@;
		if ($exc_save)
		{

			# IPC::Run::run threw an exception. re-throw unless it's a
			# timeout, which we'll handle by testing is_expired
			die $exc_save
			  if (blessed($exc_save)
				|| $exc_save !~ /^\Q$timeout_exception\E/);

			$ret = undef;

			die "Got timeout exception '$exc_save' but timer not expired?!"
			  unless $timeout->is_expired;

			if (defined($params{timed_out}))
			{
				${ $params{timed_out} } = 1;
			}
			else
			{
				die "psql timed out: stderr: '$$stderr'\n"
				  . "while running '@psql_params'";
			}
		}
	};

	if (defined $$stdout)
	{
		chomp $$stdout;
	}

	if (defined $$stderr)
	{
		chomp $$stderr;
	}

	# See http://perldoc.perl.org/perlvar.html#%24CHILD_ERROR
	# We don't use IPC::Run::Simple to limit dependencies.
	#
	# We always die on signal.
	my $core = $ret & 128 ? " (core dumped)" : "";
	die "psql exited with signal "
	  . ($ret & 127)
	  . "$core: '$$stderr' while running '@psql_params'"
	  if $ret & 127;
	$ret = $ret >> 8;

	if ($ret && $params{on_error_die})
	{
		die "psql error: stderr: '$$stderr'\nwhile running '@psql_params'"
		  if $ret == 1;
		die "connection error: '$$stderr'\nwhile running '@psql_params'"
		  if $ret == 2;
		die
		  "error running SQL: '$$stderr'\nwhile running '@psql_params' with sql '$sql'"
		  if $ret == 3;
		die "psql returns $ret: '$$stderr'\nwhile running '@psql_params'";
	}

	if (wantarray)
	{
		return ($ret, $$stdout, $$stderr);
	}
	else
	{
		return $ret;
	}
}

=pod

=item $node->background_psql($dbname, \$stdin, \$stdout, $timer, %params) => harness

Invoke B<psql> on B<$dbname> and return an IPC::Run harness object, which the
caller may use to send input to B<psql>.  The process's stdin is sourced from
the $stdin scalar reference, and its stdout and stderr go to the $stdout
scalar reference.  This allows the caller to act on other parts of the system
while idling this backend.

The specified timer object is attached to the harness, as well.  It's caller's
responsibility to set the timeout length (usually
$PostgreSQL::Test::Utils::timeout_default), and to restart the timer after
each command if the timeout is per-command.

psql is invoked in tuples-only unaligned mode with reading of B<.psqlrc>
disabled.  That may be overridden by passing extra psql parameters.

Dies on failure to invoke psql, or if psql fails to connect.  Errors occurring
later are the caller's problem.  psql runs with on_error_stop by default so
that it will stop running sql and return 3 if passed SQL results in an error.

Be sure to "finish" the harness when done with it.

=over

=item on_error_stop => 1

By default, the B<psql> method invokes the B<psql> program with ON_ERROR_STOP=1
set, so SQL execution is stopped at the first error and exit code 3 is
returned.  Set B<on_error_stop> to 0 to ignore errors instead.

=item replication => B<value>

If set, add B<replication=value> to the conninfo string.
Passing the literal value C<database> results in a logical replication
connection.

=item extra_params => ['--single-transaction']

If given, it must be an array reference containing additional parameters to B<psql>.

=back

=cut

sub background_psql
{
	my ($self, $dbname, $stdin, $stdout, $timer, %params) = @_;

	local %ENV = $self->_get_env();

	my $replication = $params{replication};

	my @psql_params = (
		$self->installed_command('psql'),
		'-XAtq',
		'-d',
		$self->connstr($dbname)
		  . (defined $replication ? " replication=$replication" : ""),
		'-f',
		'-');

	$params{on_error_stop} = 1 unless defined $params{on_error_stop};

	push @psql_params, '-v', 'ON_ERROR_STOP=1' if $params{on_error_stop};
	push @psql_params, @{ $params{extra_params} }
	  if defined $params{extra_params};

	# Ensure there is no data waiting to be sent:
	$$stdin = "" if ref($stdin);
	# IPC::Run would otherwise append to existing contents:
	$$stdout = "" if ref($stdout);

	my $harness = IPC::Run::start \@psql_params,
	  '<', $stdin, '>', $stdout, $timer;

	# Request some output, and pump until we see it.  This means that psql
	# connection failures are caught here, relieving callers of the need to
	# handle those.  (Right now, we have no particularly good handling for
	# errors anyway, but that might be added later.)
	my $banner = "background_psql: ready";
	$$stdin = "\\echo $banner\n";
	pump $harness until $$stdout =~ /$banner/ || $timer->is_expired;

	die "psql startup timed out" if $timer->is_expired;

	return $harness;
}

=pod

=item $node->interactive_psql($dbname, \$stdin, \$stdout, $timer, %params) => harness

Invoke B<psql> on B<$dbname> and return an IPC::Run harness object,
which the caller may use to send interactive input to B<psql>.
The process's stdin is sourced from the $stdin scalar reference,
and its stdout and stderr go to the $stdout scalar reference.
ptys are used so that psql thinks it's being called interactively.

The specified timer object is attached to the harness, as well.  It's caller's
responsibility to set the timeout length (usually
$PostgreSQL::Test::Utils::timeout_default), and to restart the timer after
each command if the timeout is per-command.

psql is invoked in tuples-only unaligned mode with reading of B<.psqlrc>
disabled.  That may be overridden by passing extra psql parameters.

Dies on failure to invoke psql, or if psql fails to connect.
Errors occurring later are the caller's problem.

Be sure to "finish" the harness when done with it.

The only extra parameter currently accepted is

=over

=item extra_params => ['--single-transaction']

If given, it must be an array reference containing additional parameters to B<psql>.

=back

This requires IO::Pty in addition to IPC::Run.

=cut

sub interactive_psql
{
	my ($self, $dbname, $stdin, $stdout, $timer, %params) = @_;

	local %ENV = $self->_get_env();

	my @psql_params = (
		$self->installed_command('psql'),
		'-XAt', '-d', $self->connstr($dbname));

	push @psql_params, @{ $params{extra_params} }
	  if defined $params{extra_params};

	# Ensure there is no data waiting to be sent:
	$$stdin = "" if ref($stdin);
	# IPC::Run would otherwise append to existing contents:
	$$stdout = "" if ref($stdout);

	my $harness = IPC::Run::start \@psql_params,
	  '<pty<', $stdin, '>pty>', $stdout, $timer;

	# Pump until we see psql's help banner.  This ensures that callers
	# won't write anything to the pty before it's ready, avoiding an
	# implementation issue in IPC::Run.  Also, it means that psql
	# connection failures are caught here, relieving callers of
	# the need to handle those.  (Right now, we have no particularly
	# good handling for errors anyway, but that might be added later.)
	pump $harness
	  until $$stdout =~ /Type "help" for help/ || $timer->is_expired;

	die "psql startup timed out" if $timer->is_expired;

	return $harness;
}

# Common sub of pgbench-invoking interfaces.  Makes any requested script files
# and returns pgbench command-line options causing use of those files.
sub _pgbench_make_files
{
	my ($self, $files) = @_;
	my @file_opts;

	if (defined $files)
	{

		# note: files are ordered for determinism
		for my $fn (sort keys %$files)
		{
			my $filename = $self->basedir . '/' . $fn;
			push @file_opts, '-f', $filename;

			# cleanup file weight
			$filename =~ s/\@\d+$//;

			#push @filenames, $filename;
			# filenames are expected to be unique on a test
			if (-e $filename)
			{
				ok(0, "$filename must not already exist");
				unlink $filename or die "cannot unlink $filename: $!";
			}
			PostgreSQL::Test::Utils::append_to_file($filename, $$files{$fn});
		}
	}

	return @file_opts;
}

=pod

=item $node->pgbench($opts, $stat, $out, $err, $name, $files, @args)

Invoke B<pgbench>, with parameters and files.

=over

=item $opts

Options as a string to be split on spaces.

=item $stat

Expected exit status.

=item $out

Reference to a regexp list that must match stdout.

=item $err

Reference to a regexp list that must match stderr.

=item $name

Name of test for error messages.

=item $files

Reference to filename/contents dictionary.

=item @args

Further raw options or arguments.

=back

=cut

sub pgbench
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($self, $opts, $stat, $out, $err, $name, $files, @args) = @_;
	my @cmd = (
		'pgbench',
		split(/\s+/, $opts),
		$self->_pgbench_make_files($files), @args);

	$self->command_checks_all(\@cmd, $stat, $out, $err, $name);
}

=pod

=item $node->connect_ok($connstr, $test_name, %params)

Attempt a connection with a custom connection string.  This is expected
to succeed.

=over

=item sql => B<value>

If this parameter is set, this query is used for the connection attempt
instead of the default.

=item expected_stdout => B<value>

If this regular expression is set, matches it with the output generated.

=item log_like => [ qr/required message/ ]

If given, it must be an array reference containing a list of regular
expressions that must match against the server log, using
C<Test::More::like()>.

=item log_unlike => [ qr/prohibited message/ ]

If given, it must be an array reference containing a list of regular
expressions that must NOT match against the server log.  They will be
passed to C<Test::More::unlike()>.

=back

=cut

sub connect_ok
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;
	my ($self, $connstr, $test_name, %params) = @_;

	my $sql;
	if (defined($params{sql}))
	{
		$sql = $params{sql};
	}
	else
	{
		$sql = "SELECT \$\$connected with $connstr\$\$";
	}

	my (@log_like, @log_unlike);
	if (defined($params{log_like}))
	{
		@log_like = @{ $params{log_like} };
	}
	if (defined($params{log_unlike}))
	{
		@log_unlike = @{ $params{log_unlike} };
	}

	my $log_location = -s $self->logfile;

	# Never prompt for a password, any callers of this routine should
	# have set up things properly, and this should not block.
	my ($ret, $stdout, $stderr) = $self->psql(
		'postgres',
		$sql,
		extra_params  => ['-w'],
		connstr       => "$connstr",
		on_error_stop => 0);

	is($ret, 0, $test_name);

	if (defined($params{expected_stdout}))
	{
		like($stdout, $params{expected_stdout}, "$test_name: stdout matches");
	}

	is($stderr, "", "$test_name: no stderr");

	if (@log_like or @log_unlike)
	{
		my $log_contents =
		  PostgreSQL::Test::Utils::slurp_file($self->logfile, $log_location);

		while (my $regex = shift @log_like)
		{
			like($log_contents, $regex, "$test_name: log matches");
		}
		while (my $regex = shift @log_unlike)
		{
			unlike($log_contents, $regex, "$test_name: log does not match");
		}
	}
}

=pod

=item $node->connect_fails($connstr, $test_name, %params)

Attempt a connection with a custom connection string.  This is expected
to fail.

=over

=item expected_stderr => B<value>

If this regular expression is set, matches it with the output generated.

=item log_like => [ qr/required message/ ]

=item log_unlike => [ qr/prohibited message/ ]

See C<connect_ok(...)>, above.

=back

=cut

sub connect_fails
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;
	my ($self, $connstr, $test_name, %params) = @_;

	my (@log_like, @log_unlike);
	if (defined($params{log_like}))
	{
		@log_like = @{ $params{log_like} };
	}
	if (defined($params{log_unlike}))
	{
		@log_unlike = @{ $params{log_unlike} };
	}

	my $log_location = -s $self->logfile;

	# Never prompt for a password, any callers of this routine should
	# have set up things properly, and this should not block.
	my ($ret, $stdout, $stderr) = $self->psql(
		'postgres',
		undef,
		extra_params => ['-w'],
		connstr      => "$connstr");

	isnt($ret, 0, $test_name);

	if (defined($params{expected_stderr}))
	{
		like($stderr, $params{expected_stderr}, "$test_name: matches");
	}

	if (@log_like or @log_unlike)
	{
		my $log_contents =
		  PostgreSQL::Test::Utils::slurp_file($self->logfile, $log_location);

		while (my $regex = shift @log_like)
		{
			like($log_contents, $regex, "$test_name: log matches");
		}
		while (my $regex = shift @log_unlike)
		{
			unlike($log_contents, $regex, "$test_name: log does not match");
		}
	}
}

=pod

=item $node->poll_query_until($dbname, $query [, $expected ])

Run B<$query> repeatedly, until it returns the B<$expected> result
('t', or SQL boolean true, by default).
Continues polling if B<psql> returns an error result.
Times out after $PostgreSQL::Test::Utils::timeout_default seconds.
Returns 1 if successful, 0 if timed out.

=cut

sub poll_query_until
{
	my ($self, $dbname, $query, $expected) = @_;

	local %ENV = $self->_get_env();

	$expected = 't' unless defined($expected);    # default value

	my $cmd = [
		$self->installed_command('psql'), '-XAt',
		'-d',                             $self->connstr($dbname)
	];
	my ($stdout, $stderr);
	my $max_attempts = 10 * $PostgreSQL::Test::Utils::timeout_default;
	my $attempts     = 0;

	while ($attempts < $max_attempts)
	{
		my $result = IPC::Run::run $cmd, '<', \$query,
		  '>', \$stdout, '2>', \$stderr;

		chomp($stdout);
		chomp($stderr);

		if ($stdout eq $expected && $stderr eq '')
		{
			return 1;
		}

		# Wait 0.1 second before retrying.
		usleep(100_000);

		$attempts++;
	}

	# Give up. Print the output from the last attempt, hopefully that's useful
	# for debugging.
	diag qq(poll_query_until timed out executing this query:
$query
expecting this output:
$expected
last actual query output:
$stdout
with stderr:
$stderr);
	return 0;
}

=pod

=item $node->command_ok(...)

Runs a shell command like PostgreSQL::Test::Utils::command_ok, but with PGHOST and PGPORT set
so that the command will default to connecting to this PostgreSQL::Test::Cluster.

=cut

sub command_ok
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my $self = shift;

	local %ENV = $self->_get_env();

	PostgreSQL::Test::Utils::command_ok(@_);
	return;
}

=pod

=item $node->command_fails(...)

PostgreSQL::Test::Utils::command_fails with our connection parameters. See command_ok(...)

=cut

sub command_fails
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my $self = shift;

	local %ENV = $self->_get_env();

	PostgreSQL::Test::Utils::command_fails(@_);
	return;
}

=pod

=item $node->command_like(...)

PostgreSQL::Test::Utils::command_like with our connection parameters. See command_ok(...)

=cut

sub command_like
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my $self = shift;

	local %ENV = $self->_get_env();

	PostgreSQL::Test::Utils::command_like(@_);
	return;
}

=pod

=item $node->command_fails_like(...)

PostgreSQL::Test::Utils::command_fails_like with our connection parameters. See command_ok(...)

=cut

sub command_fails_like
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my $self = shift;

	local %ENV = $self->_get_env();

	PostgreSQL::Test::Utils::command_fails_like(@_);
	return;
}

=pod

=item $node->command_checks_all(...)

PostgreSQL::Test::Utils::command_checks_all with our connection parameters. See
command_ok(...)

=cut

sub command_checks_all
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my $self = shift;

	local %ENV = $self->_get_env();

	PostgreSQL::Test::Utils::command_checks_all(@_);
	return;
}

=pod

=item $node->issues_sql_like(cmd, expected_sql, test_name)

Run a command on the node, then verify that $expected_sql appears in the
server log file.

=cut

sub issues_sql_like
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($self, $cmd, $expected_sql, $test_name) = @_;

	local %ENV = $self->_get_env();

	my $log_location = -s $self->logfile;

	my $result = PostgreSQL::Test::Utils::run_log($cmd);
	ok($result, "@$cmd exit code 0");
	my $log =
	  PostgreSQL::Test::Utils::slurp_file($self->logfile, $log_location);
	like($log, $expected_sql, "$test_name: SQL found in server log");
	return;
}

=pod

=item $node->run_log(...)

Runs a shell command like PostgreSQL::Test::Utils::run_log, but with connection parameters set
so that the command will default to connecting to this PostgreSQL::Test::Cluster.

=cut

sub run_log
{
	my $self = shift;

	local %ENV = $self->_get_env();

	return PostgreSQL::Test::Utils::run_log(@_);
}

=pod

=item $node->lsn(mode)

Look up WAL locations on the server:

 * insert location (primary only, error on replica)
 * write location (primary only, error on replica)
 * flush location (primary only, error on replica)
 * receive location (always undef on primary)
 * replay location (always undef on primary)

mode must be specified.

=cut

sub lsn
{
	my ($self, $mode) = @_;
	my %modes = (
		'insert'  => 'pg_current_wal_insert_lsn()',
		'flush'   => 'pg_current_wal_flush_lsn()',
		'write'   => 'pg_current_wal_lsn()',
		'receive' => 'pg_last_wal_receive_lsn()',
		'replay'  => 'pg_last_wal_replay_lsn()');

	$mode = '<undef>' if !defined($mode);
	croak "unknown mode for 'lsn': '$mode', valid modes are "
	  . join(', ', keys %modes)
	  if !defined($modes{$mode});

	my $result = $self->safe_psql('postgres', "SELECT $modes{$mode}");
	chomp($result);
	if ($result eq '')
	{
		return;
	}
	else
	{
		return $result;
	}
}

=pod

=item $node->wait_for_catchup(standby_name, mode, target_lsn)

Wait for the replication connection with application_name standby_name until
its 'mode' replication column in pg_stat_replication equals or passes the
specified or default target_lsn.  By default the replay_lsn is waited for,
but 'mode' may be specified to wait for any of sent|write|flush|replay.
The replication connection must be in a streaming state.

When doing physical replication, the standby is usually identified by
passing its PostgreSQL::Test::Cluster instance.  When doing logical
replication, standby_name identifies a subscription.

The default value of target_lsn is $node->lsn('write'), which ensures
that the standby has caught up to what has been committed on the primary.
If you pass an explicit value of target_lsn, it should almost always be
the primary's write LSN; so this parameter is seldom needed except when
querying some intermediate replication node rather than the primary.

If there is no active replication connection from this peer, waits until
poll_query_until timeout.

Requires that the 'postgres' db exists and is accessible.

This is not a test. It die()s on failure.

=cut

sub wait_for_catchup
{
	my ($self, $standby_name, $mode, $target_lsn) = @_;
	$mode = defined($mode) ? $mode : 'replay';
	my %valid_modes =
	  ('sent' => 1, 'write' => 1, 'flush' => 1, 'replay' => 1);
	croak "unknown mode $mode for 'wait_for_catchup', valid modes are "
	  . join(', ', keys(%valid_modes))
	  unless exists($valid_modes{$mode});

	# Allow passing of a PostgreSQL::Test::Cluster instance as shorthand
	if (blessed($standby_name)
		&& $standby_name->isa("PostgreSQL::Test::Cluster"))
	{
		$standby_name = $standby_name->name;
	}
	if (!defined($target_lsn))
	{
		$target_lsn = $self->lsn('write');
	}
	print "Waiting for replication conn "
	  . $standby_name . "'s "
	  . $mode
	  . "_lsn to pass "
	  . $target_lsn . " on "
	  . $self->name . "\n";
	# Before release 12 walreceiver just set the application name to
	# "walreceiver"
	my $query = qq[SELECT '$target_lsn' <= ${mode}_lsn AND state = 'streaming'
         FROM pg_catalog.pg_stat_replication
         WHERE application_name IN ('$standby_name', 'walreceiver')];
	if (!$self->poll_query_until('postgres', $query))
	{
		if (PostgreSQL::Test::Utils::has_wal_read_bug)
		{
			# Mimic having skipped the test file.  If >0 tests have run, the
			# harness won't accept a skip; otherwise, it won't accept
			# done_testing().  Force a nonzero count by running one test.
			ok(1, 'dummy test before skip for filesystem bug');
			carp "skip rest: timed out waiting for catchup & filesystem bug";
			done_testing();
			exit 0;
		}
		else
		{
			croak "timed out waiting for catchup";
		}
	}
	print "done\n";
	return;
}

=pod

=item $node->wait_for_slot_catchup(slot_name, mode, target_lsn)

Wait for the named replication slot to equal or pass the supplied target_lsn.
The location used is the restart_lsn unless mode is given, in which case it may
be 'restart' or 'confirmed_flush'.

Requires that the 'postgres' db exists and is accessible.

This is not a test. It die()s on failure.

If the slot is not active, will time out after poll_query_until's timeout.

target_lsn may be any arbitrary lsn, but is typically $primary_node->lsn('insert').

Note that for logical slots, restart_lsn is held down by the oldest in-progress tx.

=cut

sub wait_for_slot_catchup
{
	my ($self, $slot_name, $mode, $target_lsn) = @_;
	$mode = defined($mode) ? $mode : 'restart';
	if (!($mode eq 'restart' || $mode eq 'confirmed_flush'))
	{
		croak "valid modes are restart, confirmed_flush";
	}
	croak 'target lsn must be specified' unless defined($target_lsn);
	print "Waiting for replication slot "
	  . $slot_name . "'s "
	  . $mode
	  . "_lsn to pass "
	  . $target_lsn . " on "
	  . $self->name . "\n";
	my $query =
	  qq[SELECT '$target_lsn' <= ${mode}_lsn FROM pg_catalog.pg_replication_slots WHERE slot_name = '$slot_name';];
	$self->poll_query_until('postgres', $query)
	  or croak "timed out waiting for catchup";
	print "done\n";
	return;
}

=pod

=item $node->wait_for_subscription_sync(publisher, subname, dbname)

Wait for all tables in pg_subscription_rel to complete the initial
synchronization (i.e to be either in 'syncdone' or 'ready' state).

If the publisher node is given, additionally, check if the subscriber has
caught up to what has been committed on the primary. This is useful to
ensure that the initial data synchronization has been completed after
creating a new subscription.

If there is no active replication connection from this peer, wait until
poll_query_until timeout.

This is not a test. It die()s on failure.

=cut

sub wait_for_subscription_sync
{
	my ($self, $publisher, $subname, $dbname) = @_;
	my $name = $self->name;

	$dbname = defined($dbname) ? $dbname : 'postgres';

	# Wait for all tables to finish initial sync.
	print "Waiting for all subscriptions in \"$name\" to synchronize data\n";
	my $query =
	    qq[SELECT count(1) = 0 FROM pg_subscription_rel WHERE srsubstate NOT IN ('r', 's');];
	$self->poll_query_until($dbname, $query)
	  or croak "timed out waiting for subscriber to synchronize data";

	# Then, wait for the replication to catchup if required.
	if (defined($publisher))
	{
		croak 'subscription name must be specified' unless defined($subname);
		$publisher->wait_for_catchup($subname);
	}

	print "done\n";
	return;
}

=pod

=item $node->wait_for_log(regexp, offset)

Waits for the contents of the server log file, starting at the given offset, to
match the supplied regular expression.  Checks the entire log if no offset is
given.  Times out after $PostgreSQL::Test::Utils::timeout_default seconds.

If successful, returns the length of the entire log file, in bytes.

=cut

sub wait_for_log
{
	my ($self, $regexp, $offset) = @_;
	$offset = 0 unless defined $offset;

	my $max_attempts = 10 * $PostgreSQL::Test::Utils::timeout_default;
	my $attempts     = 0;

	while ($attempts < $max_attempts)
	{
		my $log =
		  PostgreSQL::Test::Utils::slurp_file($self->logfile, $offset);

		return $offset + length($log) if ($log =~ m/$regexp/);

		# Wait 0.1 second before retrying.
		usleep(100_000);

		$attempts++;
	}

	croak "timed out waiting for match: $regexp";
}

=pod

=item $node->query_hash($dbname, $query, @columns)

Execute $query on $dbname, replacing any appearance of the string __COLUMNS__
within the query with a comma-separated list of @columns.

If __COLUMNS__ does not appear in the query, its result columns must EXACTLY
match the order and number (but not necessarily alias) of supplied @columns.

The query must return zero or one rows.

Return a hash-ref representation of the results of the query, with any empty
or null results as defined keys with an empty-string value. There is no way
to differentiate between null and empty-string result fields.

If the query returns zero rows, return a hash with all columns empty. There
is no way to differentiate between zero rows returned and a row with only
null columns.

=cut

sub query_hash
{
	my ($self, $dbname, $query, @columns) = @_;
	croak 'calls in array context for multi-row results not supported yet'
	  if (wantarray);

	# Replace __COLUMNS__ if found
	substr($query, index($query, '__COLUMNS__'), length('__COLUMNS__')) =
	  join(', ', @columns)
	  if index($query, '__COLUMNS__') >= 0;
	my $result = $self->safe_psql($dbname, $query);

	# hash slice, see http://stackoverflow.com/a/16755894/398670 .
	#
	# Fills the hash with empty strings produced by x-operator element
	# duplication if result is an empty row
	#
	my %val;
	@val{@columns} =
	  $result ne '' ? split(qr/\|/, $result, -1) : ('',) x scalar(@columns);
	return \%val;
}

=pod

=item $node->slot(slot_name)

Return hash-ref of replication slot data for the named slot, or a hash-ref with
all values '' if not found. Does not differentiate between null and empty string
for fields, no field is ever undef.

The restart_lsn and confirmed_flush_lsn fields are returned verbatim, and also
as a 2-list of [highword, lowword] integer. Since we rely on Perl 5.8.8 we can't
"use bigint", it's from 5.20, and we can't assume we have Math::Bigint from CPAN
either.

=cut

sub slot
{
	my ($self, $slot_name) = @_;
	my @columns = (
		'plugin', 'slot_type',  'datoid', 'database',
		'active', 'active_pid', 'xmin',   'catalog_xmin',
		'restart_lsn');
	return $self->query_hash(
		'postgres',
		"SELECT __COLUMNS__ FROM pg_catalog.pg_replication_slots WHERE slot_name = '$slot_name'",
		@columns);
}

=pod

=item $node->pg_recvlogical_upto(self, dbname, slot_name, endpos, timeout_secs, ...)

Invoke pg_recvlogical to read from slot_name on dbname until LSN endpos, which
corresponds to pg_recvlogical --endpos.  Gives up after timeout (if nonzero).

Disallows pg_recvlogical from internally retrying on error by passing --no-loop.

Plugin options are passed as additional keyword arguments.

If called in scalar context, returns stdout, and die()s on timeout or nonzero return.

If called in array context, returns a tuple of (retval, stdout, stderr, timeout).
timeout is the IPC::Run::Timeout object whose is_expired method can be tested
to check for timeout. retval is undef on timeout.

=cut

sub pg_recvlogical_upto
{
	my ($self, $dbname, $slot_name, $endpos, $timeout_secs, %plugin_options)
	  = @_;

	local %ENV = $self->_get_env();

	my ($stdout, $stderr);

	my $timeout_exception = 'pg_recvlogical timed out';

	croak 'slot name must be specified' unless defined($slot_name);
	croak 'endpos must be specified'    unless defined($endpos);

	my @cmd = (
		$self->installed_command('pg_recvlogical'),
		'-S', $slot_name, '--dbname', $self->connstr($dbname));
	push @cmd, '--endpos', $endpos;
	push @cmd, '-f', '-', '--no-loop', '--start';

	while (my ($k, $v) = each %plugin_options)
	{
		croak "= is not permitted to appear in replication option name"
		  if ($k =~ qr/=/);
		push @cmd, "-o", "$k=$v";
	}

	my $timeout;
	$timeout =
	  IPC::Run::timeout($timeout_secs, exception => $timeout_exception)
	  if $timeout_secs;
	my $ret = 0;

	do
	{
		local $@;
		eval {
			IPC::Run::run(\@cmd, ">", \$stdout, "2>", \$stderr, $timeout);
			$ret = $?;
		};
		my $exc_save = $@;
		if ($exc_save)
		{

			# IPC::Run::run threw an exception. re-throw unless it's a
			# timeout, which we'll handle by testing is_expired
			die $exc_save
			  if (blessed($exc_save) || $exc_save !~ qr/$timeout_exception/);

			$ret = undef;

			die "Got timeout exception '$exc_save' but timer not expired?!"
			  unless $timeout->is_expired;

			die
			  "$exc_save waiting for endpos $endpos with stdout '$stdout', stderr '$stderr'"
			  unless wantarray;
		}
	};

	if (wantarray)
	{
		return ($ret, $stdout, $stderr, $timeout);
	}
	else
	{
		die
		  "pg_recvlogical exited with code '$ret', stdout '$stdout' and stderr '$stderr'"
		  if $ret;
		return $stdout;
	}
}

=pod

=item $node->corrupt_page_checksum(self, file, page_offset)

Intentionally corrupt the checksum field of one page in a file.
The server must be stopped for this to work reliably.

The file name should be specified relative to the cluster datadir.
page_offset had better be a multiple of the cluster's block size.

=cut

sub corrupt_page_checksum
{
	my ($self, $file, $page_offset) = @_;
	my $pgdata = $self->data_dir;
	my $pageheader;

	open my $fh, '+<', "$pgdata/$file" or die "open($file) failed: $!";
	binmode $fh;
	sysseek($fh, $page_offset, 0) or die "sysseek failed: $!";
	sysread($fh, $pageheader, 24) or die "sysread failed: $!";
	# This inverts the pd_checksum field (only); see struct PageHeaderData
	$pageheader ^= "\0\0\0\0\0\0\0\0\xff\xff";
	sysseek($fh, $page_offset, 0) or die "sysseek failed: $!";
	syswrite($fh, $pageheader) or die "syswrite failed: $!";
	close $fh;

	return;
}

=pod

=back

=cut

##########################################################################

package PostgreSQL::Test::Cluster::V_11
  ;    ## no critic (ProhibitMultiplePackages)

# parent.pm is not present in all perl versions before 5.10.1, so instead
# do directly what it would do for this:
# use parent -norequire, qw(PostgreSQL::Test::Cluster);
push @PostgreSQL::Test::Cluster::V_11::ISA, 'PostgreSQL::Test::Cluster';

# https://www.postgresql.org/docs/11/release-11.html

# max_wal_senders + superuser_reserved_connections must be < max_connections
# uses recovery.conf

sub _recovery_file { return "recovery.conf"; }

sub set_standby_mode
{
	my $self = shift;
	$self->append_conf("recovery.conf", "standby_mode = on\n");
}

sub init
{
	my ($self, %params) = @_;
	$self->SUPER::init(%params);
	$self->adjust_conf('postgresql.conf', 'max_wal_senders',
		$params{allows_streaming} ? 5 : 0);
}

##########################################################################

package PostgreSQL::Test::Cluster::V_10
  ;    ## no critic (ProhibitMultiplePackages)

# use parent -norequire, qw(PostgreSQL::Test::Cluster::V_11);
push @PostgreSQL::Test::Cluster::V_10::ISA, 'PostgreSQL::Test::Cluster::V_11';

# https://www.postgresql.org/docs/10/release-10.html

########################################################################

1;

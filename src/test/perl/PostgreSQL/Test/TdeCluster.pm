package PostgreSQL::Test::TdeCluster;

use parent 'PostgreSQL::Test::Cluster';

use strict;
use warnings FATAL => 'all';

use List::Util                      ();
use PostgreSQL::Test::RecursiveCopy ();
use PostgreSQL::Test::Utils         ();
use Test::More;

our $tde_mode = defined($ENV{TDE_MODE}) ? $ENV{TDE_MODE} + 0 : 0;
my $tde_mode_noskip = defined($ENV{TDE_MODE_NOSKIP}) ? $ENV{TDE_MODE_NOSKIP} + 0 : 0;
my $tde_mode_smgr = defined($ENV{TDE_MODE_SMGR}) ? $ENV{TDE_MODE_SMGR} + 0 : $tde_mode;
my $tde_mode_wal = defined($ENV{TDE_MODE_WAL}) ? $ENV{TDE_MODE_WAL} + 0 : $tde_mode;

sub init
{
	my ($self, %params) = @_;

	$self->SUPER::init(%params);

	$self->SUPER::append_conf('postgresql.conf',
		'shared_preload_libraries = pg_tde');

	$self->_tde_init_pg_tde_dir($params{extra});

	if ($tde_mode_smgr)
	{
		# Enable the TDE extension in all databases created by initdb, this is
		# necessary for the tde_heap access method to be available everywhere.
		foreach ('postgres', 'template0', 'template1')
		{
			_tde_init_sql_command(
				$self->data_dir, $_, q(
				CREATE SCHEMA _pg_tde;
				CREATE EXTENSION pg_tde WITH SCHEMA _pg_tde;
			));
		}
		$self->SUPER::append_conf('postgresql.conf',
			'default_table_access_method = tde_heap');
	}

	if ($tde_mode_wal)
	{
		$self->SUPER::append_conf('postgresql.conf',
			'pg_tde.wal_encrypt = on');
	}

	return;
}

sub append_conf
{
	my ($self, $filename, $str) = @_;

	if ($filename eq 'postgresql.conf' or $filename eq 'postgresql.auto.conf')
	{
		# TODO: Will not work with shared_preload_libraries= without any
		#       libraries, but no TAP test currently do that.
		$str =~
		  s/shared_preload_libraries *= *'?([^'\n]+)'?/shared_preload_libraries = 'pg_tde,$1'/;
	}

	$self->SUPER::append_conf($filename, $str);
}

sub backup
{
	my ($self, $backup_name, %params) = @_;
	my $backup_dir = $self->backup_dir . '/' . $backup_name;

	mkdir $backup_dir or die "mkdir($backup_dir) failed: $!";

	if ($tde_mode_wal)
	{
		PostgreSQL::Test::Utils::system_log('cp', '-R', '-P', '-p',
			$self->pg_tde_dir, $backup_dir . '/pg_tde',);

		# TODO: More thorough checking for options incompatible with --encrypt-wal
		$params{backup_options} = [] unless defined $params{backup_options};
		unless (
			List::Util::any { $_ eq '-Ft' or $_ eq '-Xnone' }
			@{ $params{backup_options} })
		{
			push @{ $params{backup_options} }, '--encrypt-wal';
		}
	}

	$self->SUPER::backup($backup_name, %params);
}

sub enable_archiving
{
	my ($self) = @_;
	my $path = $self->archive_dir;

	$self->SUPER::enable_archiving;
	if ($tde_mode_wal)
	{
		$self->adjust_conf('postgresql.conf', 'archive_command',
			qq('pg_tde_archive_decrypt %f %p "cp \\"%%p\\" \\"$path/%%f\\""')
		);
	}

	return;
}

sub enable_restoring
{
	my ($self, $root_node, $standby) = @_;
	my $path = $root_node->archive_dir;

	$self->SUPER::enable_restoring($root_node, $standby);
	if ($tde_mode_wal)
	{
		$self->adjust_conf('postgresql.conf', 'restore_command',
			qq('pg_tde_restore_encrypt %f %p "cp \\"$path/%%f\\" \\"%%p\\""')
		);
	}

	return;
}

sub pg_tde_dir
{
	my ($self) = @_;
	return $self->data_dir . '/pg_tde';
}

sub _tde_init_pg_tde_dir
{
	my ($self, $extra) = @_;
	my $tde_source_dir;

	if (defined($extra))
	{
		$tde_source_dir = $self->_tde_generate_pg_tde_dir($extra);
	}
	else
	{
		$tde_source_dir = $self->_tde_init_pg_tde_dir_template;
	}

	PostgreSQL::Test::Utils::system_log('cp', '-R', '-P', '-p',
		$tde_source_dir . '/pg_tde',
		$self->pg_tde_dir);

	# We don't want clusters sharing the KMS file as any concurrent writes will
	# mess it up.
	PostgreSQL::Test::Utils::system_log(
		'cp', '-R', '-P', '-p',
		$tde_source_dir . '/pg_tde_test_keys',
		$self->basedir . '/pg_tde_test_keys');

	PostgreSQL::Test::Utils::system_log(
		'pg_tde_change_key_provider',
		'-D' => $self->data_dir,
		'1664',
		'global_test_provider',
		'file',
		$self->basedir . '/pg_tde_test_keys');
}

sub _tde_init_pg_tde_dir_template
{
	my ($self) = @_;
	my $tde_template_dir;

	if (defined($ENV{TDE_TEMPLATE_DIR}))
	{
		$tde_template_dir = $ENV{TDE_TEMPLATE_DIR};
	}
	else
	{
		$tde_template_dir =
		  $PostgreSQL::Test::Utils::tmp_check . '/pg_tde_template';
	}

	unless (-e $tde_template_dir)
	{
		my $temp_dir = $self->_tde_generate_pg_tde_dir;
		mkdir $tde_template_dir;

		PostgreSQL::Test::Utils::system_log('cp', '-R', '-P', '-p',
			$temp_dir . '/pg_tde',
			$tde_template_dir);

		PostgreSQL::Test::Utils::system_log(
			'cp', '-R', '-P', '-p',
			$temp_dir . '/pg_tde_test_keys',
			$tde_template_dir . '/pg_tde_test_keys');
	}

	return $tde_template_dir;
}

sub _tde_generate_pg_tde_dir
{
	my ($self, $extra) = @_;
	my $temp_dir = PostgreSQL::Test::Utils::tempdir();

	PostgreSQL::Test::Utils::system_log(
		'initdb',
		'-D' => $temp_dir,
		'--set' => 'shared_preload_libraries=pg_tde',
		@{ $extra });

	_tde_init_sql_command(
		$temp_dir, 'postgres', qq(
		CREATE EXTENSION pg_tde;
		SELECT pg_tde_add_global_key_provider_file('global_test_provider', '$temp_dir/pg_tde_test_keys');
		SELECT pg_tde_create_key_using_global_key_provider('default_test_key', 'global_test_provider');
		SELECT pg_tde_set_default_key_using_global_key_provider('default_test_key', 'global_test_provider');
	));

	return $temp_dir;
}

sub _tde_init_sql_command
{
	my ($datadir, $database, $sql) = @_;
	PostgreSQL::Test::Utils::run_log(
		[
			'postgres',
			'--single', '-j', '-F',
			'-D' => $datadir,
			'-c' => 'exit_on_error=true',
			'-c' => 'log_checkpoints=false',
			'-c' => 'archive_mode=off',
			$database,
		],
		'<',
		\$sql);
}

sub skip_if_tde_mode_wal
{
	my ($msg) = @_;
	plan(skip_all => $msg) if ($tde_mode_wal && !$tde_mode_noskip);
}

sub skip_if_tde_mode_smgr
{
	my ($msg) = @_;
	plan(skip_all => $msg) if ($tde_mode_smgr && !$tde_mode_noskip);
}

1;

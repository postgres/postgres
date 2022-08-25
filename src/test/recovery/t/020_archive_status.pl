
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

#
# Tests related to WAL archiving and recovery.
#
use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(
	has_archiving    => 1,
	allows_streaming => 1);
$primary->append_conf('postgresql.conf', 'autovacuum = off');
$primary->start;
my $primary_data = $primary->data_dir;

# Temporarily use an archive_command value to make the archiver fail,
# knowing that archiving is enabled.  Note that we cannot use a command
# that does not exist as in this case the archiver process would just exit
# without reporting the failure to pg_stat_archiver.  This also cannot
# use a plain "false" as that's unportable on Windows.  So, instead, as
# a portable solution, use an archive command based on a command known to
# work but will fail: copy with an incorrect original path.
my $incorrect_command =
  $PostgreSQL::Test::Utils::windows_os
  ? qq{copy "%p_does_not_exist" "%f_does_not_exist"}
  : qq{cp "%p_does_not_exist" "%f_does_not_exist"};
$primary->safe_psql(
	'postgres', qq{
    ALTER SYSTEM SET archive_command TO '$incorrect_command';
    SELECT pg_reload_conf();
});

# Save the WAL segment currently in use and switch to a new segment.
# This will be used to track the activity of the archiver.
my $segment_name_1 = $primary->safe_psql('postgres',
	q{SELECT pg_walfile_name(pg_current_wal_lsn())});
my $segment_path_1       = "pg_wal/archive_status/$segment_name_1";
my $segment_path_1_ready = "$segment_path_1.ready";
my $segment_path_1_done  = "$segment_path_1.done";
$primary->safe_psql(
	'postgres', q{
	CREATE TABLE mine AS SELECT generate_series(1,10) AS x;
	SELECT pg_switch_wal();
	CHECKPOINT;
});

# Wait for an archive failure.
$primary->poll_query_until('postgres',
	q{SELECT failed_count > 0 FROM pg_stat_archiver}, 't')
  or die "Timed out while waiting for archiving to fail";
ok( -f "$primary_data/$segment_path_1_ready",
	".ready file exists for WAL segment $segment_name_1 waiting to be archived"
);
ok( !-f "$primary_data/$segment_path_1_done",
	".done file does not exist for WAL segment $segment_name_1 waiting to be archived"
);

is( $primary->safe_psql(
		'postgres', q{
		SELECT archived_count, last_failed_wal
		FROM pg_stat_archiver
	}),
	"0|$segment_name_1",
	"pg_stat_archiver failed to archive $segment_name_1");

# Crash the cluster for the next test in charge of checking that non-archived
# WAL segments are not removed.
$primary->stop('immediate');

# Recovery tests for the archiving with a standby partially check
# the recovery behavior when restoring a backup taken using a
# snapshot with no pg_backup_start/stop.  In this situation,
# the recovered standby should enter first crash recovery then
# switch to regular archive recovery.  Note that the base backup
# is taken here so as archive_command will fail.  This is necessary
# for the assumptions of the tests done with the standbys below.
$primary->backup_fs_cold('backup');

$primary->start;
ok( -f "$primary_data/$segment_path_1_ready",
	".ready file for WAL segment $segment_name_1 still exists after crash recovery on primary"
);

# Allow WAL archiving again and wait for a success.
$primary->safe_psql(
	'postgres', q{
	ALTER SYSTEM RESET archive_command;
	SELECT pg_reload_conf();
});

$primary->poll_query_until('postgres',
	q{SELECT archived_count FROM pg_stat_archiver}, '1')
  or die "Timed out while waiting for archiving to finish";

ok(!-f "$primary_data/$segment_path_1_ready",
	".ready file for archived WAL segment $segment_name_1 removed");

ok(-f "$primary_data/$segment_path_1_done",
	".done file for archived WAL segment $segment_name_1 exists");

is( $primary->safe_psql(
		'postgres', q{ SELECT last_archived_wal FROM pg_stat_archiver }),
	$segment_name_1,
	"archive success reported in pg_stat_archiver for WAL segment $segment_name_1"
);

# Create some WAL activity and a new checkpoint so as the next standby can
# create a restartpoint.  As this standby starts in crash recovery because
# of the cold backup taken previously, it needs a clean restartpoint to deal
# with existing status files.
my $segment_name_2 = $primary->safe_psql('postgres',
	q{SELECT pg_walfile_name(pg_current_wal_lsn())});
my $segment_path_2       = "pg_wal/archive_status/$segment_name_2";
my $segment_path_2_ready = "$segment_path_2.ready";
my $segment_path_2_done  = "$segment_path_2.done";
$primary->safe_psql(
	'postgres', q{
	INSERT INTO mine SELECT generate_series(10,20) AS x;
	CHECKPOINT;
});

# Switch to a new segment and use the returned LSN to make sure that
# standbys have caught up to this point.
my $primary_lsn = $primary->safe_psql(
	'postgres', q{
	SELECT pg_switch_wal();
});

$primary->poll_query_until('postgres',
	q{ SELECT last_archived_wal FROM pg_stat_archiver },
	$segment_name_2)
  or die "Timed out while waiting for archiving to finish";

# Test standby with archive_mode = on.
my $standby1 = PostgreSQL::Test::Cluster->new('standby');
$standby1->init_from_backup($primary, 'backup', has_restoring => 1);
$standby1->append_conf('postgresql.conf', "archive_mode = on");
my $standby1_data = $standby1->data_dir;
$standby1->start;

# Wait for the replay of the segment switch done previously, ensuring
# that all segments needed are restored from the archives.
$standby1->poll_query_until('postgres',
	qq{ SELECT pg_wal_lsn_diff(pg_last_wal_replay_lsn(), '$primary_lsn') >= 0 }
) or die "Timed out while waiting for xlog replay on standby1";

$standby1->safe_psql('postgres', q{CHECKPOINT});

# Recovery with archive_mode=on does not keep .ready signal files inherited
# from backup.  Note that this WAL segment existed in the backup.
ok( !-f "$standby1_data/$segment_path_1_ready",
	".ready file for WAL segment $segment_name_1 present in backup got removed with archive_mode=on on standby"
);

# Recovery with archive_mode=on should not create .ready files.
# Note that this segment did not exist in the backup.
ok( !-f "$standby1_data/$segment_path_2_ready",
	".ready file for WAL segment $segment_name_2 not created on standby when archive_mode=on on standby"
);

# Recovery with archive_mode = on creates .done files.
ok( -f "$standby1_data/$segment_path_2_done",
	".done file for WAL segment $segment_name_2 created when archive_mode=on on standby"
);

# Test recovery with archive_mode = always, which should always keep
# .ready files if archiving is enabled, though here we want the archive
# command to fail to persist the .ready files.  Note that this node
# has inherited the archive command of the previous cold backup that
# will cause archiving failures.
my $standby2 = PostgreSQL::Test::Cluster->new('standby2');
$standby2->init_from_backup($primary, 'backup', has_restoring => 1);
$standby2->append_conf('postgresql.conf', 'archive_mode = always');
my $standby2_data = $standby2->data_dir;
$standby2->start;

# Wait for the replay of the segment switch done previously, ensuring
# that all segments needed are restored from the archives.
$standby2->poll_query_until('postgres',
	qq{ SELECT pg_wal_lsn_diff(pg_last_wal_replay_lsn(), '$primary_lsn') >= 0 }
) or die "Timed out while waiting for xlog replay on standby2";

$standby2->safe_psql('postgres', q{CHECKPOINT});

ok( -f "$standby2_data/$segment_path_1_ready",
	".ready file for WAL segment $segment_name_1 existing in backup is kept with archive_mode=always on standby"
);

ok( -f "$standby2_data/$segment_path_2_ready",
	".ready file for WAL segment $segment_name_2 created with archive_mode=always on standby"
);

# Reset statistics of the archiver for the next checks.
$standby2->safe_psql('postgres', q{SELECT pg_stat_reset_shared('archiver')});

# Now crash the cluster to check that recovery step does not
# remove non-archived WAL segments on a standby where archiving
# is enabled.
$standby2->stop('immediate');
$standby2->start;

ok( -f "$standby2_data/$segment_path_1_ready",
	"WAL segment still ready to archive after crash recovery on standby with archive_mode=always"
);

# Allow WAL archiving again, and wait for the segments to be archived.
$standby2->safe_psql(
	'postgres', q{
	ALTER SYSTEM RESET archive_command;
	SELECT pg_reload_conf();
});
$standby2->poll_query_until('postgres',
	q{SELECT last_archived_wal FROM pg_stat_archiver},
	$segment_name_2)
  or die "Timed out while waiting for archiving to finish";

is( $standby2->safe_psql(
		'postgres', q{SELECT archived_count FROM pg_stat_archiver}),
	'2',
	"correct number of WAL segments archived from standby");

ok( !-f "$standby2_data/$segment_path_1_ready"
	  && !-f "$standby2_data/$segment_path_2_ready",
	".ready files removed after archive success with archive_mode=always on standby"
);

ok( -f "$standby2_data/$segment_path_1_done"
	  && -f "$standby2_data/$segment_path_2_done",
	".done files created after archive success with archive_mode=always on standby"
);

done_testing();

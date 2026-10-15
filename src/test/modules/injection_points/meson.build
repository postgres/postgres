# Copyright (c) 2022-2026, PostgreSQL Global Development Group

if not get_option('injection_points')
  subdir_done()
endif

injection_points_sources = files(
  'injection_points.c',
  'regress_injection.c',
)

if host_system == 'windows'
  injection_points_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'injection_points',
    '--FILEDESC', 'injection_points - facility for injection points',])
endif

injection_points = shared_module('injection_points',
  injection_points_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += injection_points

test_install_data += files(
  'injection_points.control',
  'injection_points--1.0.sql',
)

injection_points_isolation = [
  'basic',
  'inplace',
  'reindex_concurrently_deferred',
  'repack',
  'repack_commit_race',
  'repack_decode',
  'repack_dropped',
  'repack_missingval',
  'repack_temporal',
  'repack_temporal_multirange',
  'repack_toast',
  'ri_fastpath_reindex',
  'ri_fastpath_snapshot',
  'syscache-update-pruned',
  'heap_lock_update',
  'on_conflict_probe_window',
  'multixact-aborted-updater',
]

# wait_cleanup terminates another backend, whose FATAL message can be lost
# on Windows, making the test unstable there.
# See https://postgr.es/m/088AB35D-0860-494A-A9CF-8B301543AA51@yandex-team.ru
if host_system != 'windows'
  injection_points_isolation += 'wait_cleanup'
endif

tests += {
  'name': 'injection_points',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'injection_points',
      'hashagg',
      'reindex_conc',
      'vacuum',
    ],
    'regress_args': ['--dlpath', meson.project_build_root() / 'src/test/regress'],
    # The injection points are cluster-wide, so disable installcheck
    'runningcheck': false,
  },
  'isolation': {
    'specs': injection_points_isolation,
    'runningcheck': false, # see syscache-update-pruned
    # Some tests wait for all snapshots, so avoid parallel execution
    'runningcheck-parallel': false,
    # some tests require wal_level=replica
    'regress_args': [
      '--temp-config', files('extra.conf'),
    ],
  },
}

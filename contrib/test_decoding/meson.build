# Copyright (c) 2022-2024, PostgreSQL Global Development Group

test_decoding_sources = files(
  'test_decoding.c',
)

if host_system == 'windows'
  test_decoding_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_decoding',
    '--FILEDESC', 'test_decoding - example of a logical decoding output plugin',])
endif

test_decoding = shared_module('test_decoding',
  test_decoding_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += test_decoding

tests += {
  'name': 'test_decoding',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'ddl',
      'xact',
      'rewrite',
      'toast',
      'permissions',
      'decoding_in_xact',
      'decoding_into_rel',
      'binary',
      'prepared',
      'replorigin',
      'time',
      'messages',
      'spill',
      'slot',
      'truncate',
      'stream',
      'stats',
      'twophase',
      'twophase_stream',
    ],
    'regress_args': [
      '--temp-config', files('logical.conf'),
    ],
    # Disabled because these tests require "wal_level=logical", which
    # typical runningcheck users do not have (e.g. buildfarm clients).
    'runningcheck': false,
  },
  'isolation': {
    'specs': [
      'mxact',
      'delayed_startup',
      'ondisk_startup',
      'catalog_change_snapshot',
      'concurrent_ddl_dml',
      'oldest_xmin',
      'snapshot_transfer',
      'subxact_without_top',
      'concurrent_stream',
      'twophase_snapshot',
      'slot_creation_error',
      'skip_snapshot_restore',
    ],
    'regress_args': [
      '--temp-config', files('logical.conf'),
    ],
    # see above
    'runningcheck': false,
  },
  'tap': {
    'tests': [
      't/001_repl_stats.pl',
    ],
  },
}

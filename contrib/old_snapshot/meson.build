# Copyright (c) 2022-2023, PostgreSQL Global Development Group

old_snapshot_sources = files(
  'time_mapping.c',
)

if host_system == 'windows'
  old_snapshot_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'old_snapshot',
    '--FILEDESC', 'old_snapshot - utilities in support of old_snapshot_threshold',])
endif

old_snapshot = shared_module('old_snapshot',
  old_snapshot_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += old_snapshot

install_data(
  'old_snapshot.control',
  'old_snapshot--1.0.sql',
  kwargs: contrib_data_args,
)

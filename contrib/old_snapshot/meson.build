old_snapshot_sources = files(
  'time_mapping.c',
)

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

# Copyright (c) 2022-2025, PostgreSQL Global Development Group

backend_sources += files(
  'backend_progress.c',
  'backend_status.c',
  'pgstat.c',
  'pgstat_archiver.c',
  'pgstat_backend.c',
  'pgstat_bgwriter.c',
  'pgstat_checkpointer.c',
  'pgstat_database.c',
  'pgstat_function.c',
  'pgstat_io.c',
  'pgstat_relation.c',
  'pgstat_replslot.c',
  'pgstat_shmem.c',
  'pgstat_slru.c',
  'pgstat_subscription.c',
  'pgstat_wal.c',
  'pgstat_xact.c',
)

# this includes a .c file with contents generated in ../../../include/activity,
# seems nicer to not add that as an include path for the whole backend.
waitevent_sources = files(
  'wait_event.c',
  'wait_event_funcs.c',
)

wait_event = static_library('wait_event_names',
  waitevent_sources,
  dependencies: [backend_code],
  include_directories: include_directories('../../../include/utils'),
  kwargs: internal_lib_args,
)

backend_link_with += wait_event

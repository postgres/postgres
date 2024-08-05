# Copyright (c) 2022-2024, PostgreSQL Global Development Group

fe_utils_sources = files(
  'archive.c',
  'astreamer_file.c',
  'astreamer_gzip.c',
  'astreamer_lz4.c',
  'astreamer_tar.c',
  'astreamer_zstd.c',
  'cancel.c',
  'conditional.c',
  'connect_utils.c',
  'mbprint.c',
  'option_utils.c',
  'parallel_slot.c',
  'print.c',
  'query_utils.c',
  'recovery_gen.c',
  'simple_list.c',
  'string_utils.c',
)

psqlscan = custom_target('psqlscan',
  input: 'psqlscan.l',
  output: 'psqlscan.c',
  command: [flex_cmd, '--no-backup', '--fix-warnings', '--', '-Cfe', '-p', '-p'],
)
generated_sources += psqlscan
fe_utils_sources += psqlscan

fe_utils = static_library('libpgfeutils',
  fe_utils_sources + generated_headers,
  c_pch: pch_postgres_fe_h,
  include_directories: [postgres_inc, libpq_inc],
  c_args: host_system == 'windows' ? ['-DFD_SETSIZE=1024'] : [],
  dependencies: frontend_common_code,
  kwargs: default_lib_args,
)

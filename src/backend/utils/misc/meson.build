# Copyright (c) 2022-2025, PostgreSQL Global Development Group

backend_sources += files(
  'conffiles.c',
  'guc.c',
  'guc_funcs.c',
  'guc_tables.c',
  'help_config.c',
  'injection_point.c',
  'pg_config.c',
  'pg_controldata.c',
  'pg_rusage.c',
  'ps_status.c',
  'queryenvironment.c',
  'rls.c',
  'sampling.c',
  'stack_depth.c',
  'superuser.c',
  'timeout.c',
  'tzparser.c',
)

guc_scan = custom_target('guc_scan',
  input: 'guc-file.l',
  output: 'guc-file.c',
  command: flex_cmd)
generated_sources += guc_scan

# so we don't need to add . as an include dir for the whole backend
backend_link_with += static_library('guc-file',
  guc_scan,
  dependencies: [backend_code],
  include_directories: include_directories('.'),
  kwargs: internal_lib_args,
)

if llvm.found()
  llvm_guc_file_cflags = [
    '-I@SOURCE_ROOT@/src/backend/utils/misc',
  ]

  llvm_guc_file = generator(llvm_irgen_command,
    arguments: llvm_irgen_args + bitcode_cflags + llvm_guc_file_cflags,
    depends: guc_scan,
    depfile: '@BASENAME@.c.bc.d',
    output: '@PLAINNAME@.bc',
  )

  bc_gen_custom_backend += llvm_guc_file.process(guc_scan)
endif

install_data('postgresql.conf.sample',
  install_dir: dir_data,
)

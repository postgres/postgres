# Copyright (c) 2022-2025, PostgreSQL Global Development Group

# can't run ecpg to build test dependencies, at least not without an emulator
if meson.is_cross_build()
  subdir_done()
endif

pg_regress_ecpg_sources = pg_regress_c + files(
  'pg_regress_ecpg.c',
)

if host_system == 'windows'
  pg_regress_ecpg_sources += rc_bin_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_regress_ecpg',])
endif

pg_regress_ecpg = executable('pg_regress_ecpg',
  pg_regress_ecpg_sources,
  c_args: pg_regress_cflags,
  include_directories: [pg_regress_inc, include_directories('.')],
  dependencies: [frontend_code, libpq],
  kwargs: default_bin_args + {
    'install': false
  },
)
testprep_targets += pg_regress_ecpg

# create .c files and executables from .pgc files
ecpg_test_exec_kw = {
  'c_args': cflags_no_missing_var_decls,
  'dependencies': [frontend_code, libpq],
  'include_directories': [ecpg_inc],
  'link_with': [ecpglib_so, ecpg_compat_so, ecpg_pgtypes_so],
  'build_by_default': false,
  'install': false,
}

ecpg_preproc_kw = {
  'output': '@BASENAME@.c',
  'install': false,
  'build_by_default': false,
}

ecpg_preproc_test_command_start = [
  ecpg_exe,
  '--regression',
  '-I@CURRENT_SOURCE_DIR@',
  '-I@SOURCE_ROOT@' + '/src/interfaces/ecpg/include/',
]
ecpg_preproc_test_command_end = [
  '-o', '@OUTPUT@', '@INPUT@'
]

ecpg_test_dependencies = []

subdir('compat_informix')
subdir('compat_oracle')
subdir('connect')
subdir('pgtypeslib')
subdir('preproc')
subdir('sql')
subdir('thread')

testprep_targets += ecpg_test_dependencies

ecpg_test_files = files(
  'ecpg_schedule',
)

ecpg_regress_args = [
  '--dbname=ecpg1_regression,ecpg2_regression',
  '--create-role=regress_ecpg_user1,regress_ecpg_user2',
]

tests += {
  'name': 'ecpg',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'ecpg': {
    'expecteddir': meson.current_source_dir(),
    'inputdir': meson.current_build_dir(),
    'schedule': ecpg_test_files,
    'sql': [
      'sql/twophase',
    ],
    'test_kwargs': {
      'depends': ecpg_test_dependencies,
    },
    'dbname': 'ecpg1_regression,ecpg2_regression',
    'regress_args': ecpg_regress_args,
  },
}

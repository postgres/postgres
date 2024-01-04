# Copyright (c) 2022-2024, PostgreSQL Global Development Group

if not tcl_dep.found()
  subdir_done()
endif

pltcl_sources = files(
  'pltcl.c',
)

gen_pltclerrcodes = files('generate-pltclerrcodes.pl')
pltcl_sources += custom_target('pltclerrcodes.h',
  input: files('../../backend/utils/errcodes.txt'),
  output: 'pltclerrcodes.h',
  capture: true,
  command: [perl, gen_pltclerrcodes, '@INPUT@']
)

if host_system == 'windows'
  pltcl_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pltcl',
    '--FILEDESC', 'PL/Tcl - procedural language',])
endif

pltcl = shared_module('pltcl',
  pltcl_sources,
  c_pch: pch_postgres_h,
  include_directories: [include_directories('.'), postgres_inc],
  kwargs: pg_mod_args + {
    'dependencies': [tcl_dep, pg_mod_args['dependencies']],
  },
)
pl_targets += pltcl

install_data(
  'pltcl.control',
  'pltcl--1.0.sql',
  'pltclu.control',
  'pltclu--1.0.sql',
  install_dir: dir_data_extension,
)

pltcl_regress = [
  'pltcl_setup',
  'pltcl_queries',
  'pltcl_trigger',
  'pltcl_call',
  'pltcl_start_proc',
  'pltcl_subxact',
  'pltcl_unicode',
  'pltcl_transaction',
]

tests += {
  'name': 'pltcl',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': pltcl_regress,
    'regress_args': ['--load-extension=pltcl'],
  },
}

subdir('po', if_found: libintl)

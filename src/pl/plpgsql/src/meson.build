# Copyright (c) 2022-2025, PostgreSQL Global Development Group

plpgsql_sources = files(
  'pl_comp.c',
  'pl_exec.c',
  'pl_funcs.c',
  'pl_handler.c',
  'pl_scanner.c',
)

pl_gram = custom_target('gram',
  input: 'pl_gram.y',
  kwargs: bison_kw,
)
generated_sources += pl_gram.to_list()
plpgsql_sources += pl_gram

gen_plerrcodes = files('generate-plerrcodes.pl')
pl_errcodes = custom_target('plerrcodes',
  input: ['../../../../src/backend/utils/errcodes.txt'],
  output: ['plerrcodes.h'],
  command: [perl, gen_plerrcodes, '@INPUT0@'],
  capture: true,
)
generated_sources += pl_errcodes
plpgsql_sources += pl_errcodes

pl_reserved = custom_target('pl_reserved_kwlist',
  input: ['pl_reserved_kwlist.h'],
  output: ['pl_reserved_kwlist_d.h'],
  depend_files: gen_kwlist_deps,
  command: [gen_kwlist_cmd, '--varname', 'ReservedPLKeywords'],
)
generated_sources += pl_reserved
plpgsql_sources += pl_reserved

pl_unreserved = custom_target('pl_unreserved_kwlist',
  input: ['pl_unreserved_kwlist.h'],
  output: ['pl_unreserved_kwlist_d.h'],
  depend_files: gen_kwlist_deps,
  command: [gen_kwlist_cmd, '--varname', 'UnreservedPLKeywords'],
)
generated_sources += pl_unreserved
plpgsql_sources += pl_unreserved

if host_system == 'windows'
  plpgsql_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'plpgsql',
    '--FILEDESC', 'PL/pgSQL - procedural language',])
endif

plpgsql = shared_module('plpgsql',
  plpgsql_sources,
  c_pch: pch_postgres_h,
  include_directories: include_directories('.'),
  kwargs: pg_mod_args,
)
pl_targets += plpgsql

install_data(
  'plpgsql.control',
  'plpgsql--1.0.sql',
  install_dir: dir_data_extension,
)

install_headers(
  'plpgsql.h',
  install_dir: dir_include_server
)


tests += {
  'name': 'plpgsql',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'plpgsql_array',
      'plpgsql_cache',
      'plpgsql_call',
      'plpgsql_control',
      'plpgsql_copy',
      'plpgsql_domain',
      'plpgsql_misc',
      'plpgsql_record',
      'plpgsql_simple',
      'plpgsql_transaction',
      'plpgsql_trap',
      'plpgsql_trigger',
      'plpgsql_varprops',
    ],
  },
}

subdir('po', if_found: libintl)

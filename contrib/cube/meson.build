# Copyright (c) 2022-2025, PostgreSQL Global Development Group

cube_sources = files(
  'cube.c',
)

cube_scan = custom_target('cubescan',
  input: 'cubescan.l',
  output: 'cubescan.c',
  command: flex_cmd,
)
generated_sources += cube_scan
cube_sources += cube_scan

cube_parse = custom_target('cubeparse',
  input: 'cubeparse.y',
  kwargs: bison_kw,
)
generated_sources += cube_parse.to_list()
cube_sources += cube_parse

if host_system == 'windows'
  cube_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'cube',
    '--FILEDESC', 'cube - multidimensional cube data type',])
endif

cube = shared_module('cube',
  cube_sources,
  include_directories: include_directories('.'),
  kwargs: contrib_mod_args,
)
contrib_targets += cube

install_data(
  'cube.control',
  'cube--1.0--1.1.sql',
  'cube--1.1--1.2.sql',
  'cube--1.2.sql',
  'cube--1.2--1.3.sql',
  'cube--1.3--1.4.sql',
  'cube--1.4--1.5.sql',
  kwargs: contrib_data_args,
)

install_headers(
  'cubedata.h',
  install_dir: dir_include_extension / 'cube',
)

tests += {
  'name': 'cube',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'cube',
      'cube_sci',
    ],
  },
}

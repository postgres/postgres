# Copyright (c) 2022-2025, PostgreSQL Global Development Group

ecpg_sources = files(
  '../ecpglib/typename.c',
  'c_keywords.c',
  'descriptor.c',
  'ecpg.c',
  'ecpg_keywords.c',
  'keywords.c',
  'output.c',
  'parser.c',
  'type.c',
  'util.c',
  'variable.c',
)

pgc = custom_target('pgc.c',
  input: 'pgc.l',
  output: 'pgc.c',
  command: flex_cmd,
)
generated_sources += pgc
ecpg_sources += pgc

ecpg_files = [
  'ecpg.addons',
  'ecpg.header',
  'ecpg.tokens',
  'ecpg.trailer',
  'ecpg.type',
]

preproc_y = custom_target('preproc.y',
  input: [
    '../../../backend/parser/gram.y',
    ecpg_files,
  ],
  output: ['preproc.y'],
  command: [
    perl, files('parse.pl'),
    '--srcdir', '@CURRENT_SOURCE_DIR@',
    '--parser', '@INPUT0@',
    '--output', '@OUTPUT0@',
  ],
)
generated_sources += preproc_y

preproc = custom_target('preproc.c',
  input: preproc_y,
  kwargs: bison_kw,
)
generated_sources += preproc.to_list()
ecpg_sources += preproc

c_kwlist = custom_target('c_kwlist_d.h',
  input: ['c_kwlist.h'],
  output: ['c_kwlist_d.h'],
  depend_files: gen_kwlist_deps,
  command: [gen_kwlist_cmd, '--varname', 'ScanCKeywords', '--no-case-fold'],
)
generated_sources += c_kwlist
ecpg_sources += c_kwlist

ecpg_kwlist = custom_target('ecpg_kwlist_d.h',
  input: ['ecpg_kwlist.h'],
  output: ['ecpg_kwlist_d.h'],
  depend_files: gen_kwlist_deps,
  command: [gen_kwlist_cmd, '--varname', 'ScanECPGKeywords'],
)
generated_sources += ecpg_kwlist
ecpg_sources += ecpg_kwlist

if host_system == 'windows'
  ecpg_sources += rc_bin_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'ecpg',
    '--FILEDESC', 'ecpg - embedded SQL precompiler for C',])
endif

ecpg_exe = executable('ecpg',
  ecpg_sources,
  include_directories: ['.', ecpg_inc, postgres_inc, libpq_inc],
  c_pch: pch_postgres_fe_h,
  dependencies: [frontend_code],
  kwargs: default_bin_args,
)
ecpg_targets += ecpg_exe

subdir('po', if_found: libintl)

tests += {
  'name': 'ecpg',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_ecpg_err_warn_msg.pl',
      't/002_ecpg_err_warn_msg_informix.pl',
    ],
    'deps': [ecpg_exe],
  },
}

# Copyright (c) 2022-2024, PostgreSQL Global Development Group

if not libxml.found()
  subdir_done()
endif

xml2_sources = files(
  'xpath.c',
  'xslt_proc.c',
)

if host_system == 'windows'
  xml2_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pgxml',
    '--FILEDESC', 'xml2 - XPath querying and XSLT',])
endif

xml2 = shared_module('pgxml',
  xml2_sources,
  c_pch: pch_postgres_h,
  kwargs: contrib_mod_args + {
    'dependencies': [libxml, libxslt, contrib_mod_args['dependencies']],
  },
)
contrib_targets += xml2

install_data(
  'xml2--1.0--1.1.sql',
  'xml2--1.1.sql',
  'xml2.control',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'xml2',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'xml2',
    ],
  },
}

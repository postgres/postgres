if not libxml.found()
  subdir_done()
endif

xml2 = shared_module('pgxml',
  files(
    'xpath.c',
    'xslt_proc.c',
  ),
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

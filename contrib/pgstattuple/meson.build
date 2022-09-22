pgstattuple = shared_module('pgstattuple',
  files(
    'pgstatapprox.c',
    'pgstatindex.c',
    'pgstattuple.c',
  ),
  kwargs: contrib_mod_args,
)
contrib_targets += pgstattuple

install_data(
  'pgstattuple--1.0--1.1.sql',
  'pgstattuple--1.1--1.2.sql',
  'pgstattuple--1.2--1.3.sql',
  'pgstattuple--1.3--1.4.sql',
  'pgstattuple--1.4--1.5.sql',
  'pgstattuple--1.4.sql',
  'pgstattuple.control',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'pgstattuple',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'pgstattuple',
    ],
  },
}

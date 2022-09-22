scripts_common = static_library('libscripts_common',
  files('common.c'),
  dependencies: [frontend_code, libpq],
  kwargs: internal_lib_args,
)

binaries = [
  'createdb',
  'dropdb',
  'createuser',
  'dropuser',
  'clusterdb',
  'vacuumdb',
  'reindexdb',
  'pg_isready',
]

foreach binary : binaries
  binary = executable(binary,
    files(binary + '.c'),
    link_with: [scripts_common],
    dependencies: [frontend_code, libpq],
    kwargs: default_bin_args,
  )
  bin_targets += binary
endforeach

tests += {
  'name': 'scripts',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/010_clusterdb.pl',
      't/011_clusterdb_all.pl',
      't/020_createdb.pl',
      't/040_createuser.pl',
      't/050_dropdb.pl',
      't/070_dropuser.pl',
      't/080_pg_isready.pl',
      't/090_reindexdb.pl',
      't/091_reindexdb_all.pl',
      't/100_vacuumdb.pl',
      't/101_vacuumdb_all.pl',
      't/102_vacuumdb_stages.pl',
      't/200_connstr.pl',
    ],
  },
}

subdir('po', if_found: libintl)

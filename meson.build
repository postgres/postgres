
# libjson-c-dev on ubuntu
jsondep = dependency('json-c')

pg_tde_sources = files(
        'src/pg_tde.c',
        'src/transam/pg_tde_xact_handler.c',
        'src/access/pg_tde_tdemap.c',
        'src/access/pg_tdeam.c',
        'src/access/pg_tdeam_handler.c',
        'src/access/pg_tdeam_visibility.c',
        'src/access/pg_tdetoast.c',
        'src/access/pg_tde_io.c',
        'src/access/pg_tde_prune.c',
        'src/access/pg_tde_rewrite.c',
        'src/access/pg_tde_vacuumlazy.c',
        'src/access/pg_tde_visibilitymap.c',

        'src/encryption/enc_tuple.c',
        'src/encryption/enc_aes.c',

        'src/keyring/keyring_config.c',
        'src/keyring/keyring_file.c',
        'src/keyring/keyring_api.c',

        'src/pg_tde.c',
)

incdir = include_directories('src/include')

deps_update = {'dependencies': contrib_mod_args.get('dependencies') + [jsondep]}

mod_args = contrib_mod_args + deps_update

pg_tde = shared_module('pg_tde',
  pg_tde_sources,
  c_pch: pch_postgres_h,
  kwargs: mod_args,
  include_directories: incdir,
)
contrib_targets += pg_tde

enc_test = executable('enc_test',
  files('src/encryption/enc_aes.c', 'src/encryption/test.c'),
  kwargs: mod_args,
  include_directories: incdir,
)

install_data(
  'pg_tde.control',
  'pg_tde--1.0.sql',
  kwargs: contrib_data_args,
)

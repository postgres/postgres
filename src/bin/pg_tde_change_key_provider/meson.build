pg_tde_change_key_provider_sources = files(
  'pg_tde_change_key_provider.c',
)

link_w = [pg_tde_frontend]
include_dirs = [postgres_inc, pg_tde_inc]

pg_tde_change_key_provider = executable('pg_tde_change_key_provider',
  pg_tde_change_key_provider_sources,
  dependencies: [frontend_code, lz4, zstd],
  c_args: ['-DFRONTEND'], # needed for xlogreader et al
  kwargs: default_bin_args,
  include_directories: include_dirs,
  link_with: link_w
)
bin_targets += pg_tde_change_key_provider

tests += {
  'name': 'pg_tde_change_key_provider',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
    ],
  },
}
tests += {
  'name': 'snapshot_too_old',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'isolation': {
    'test_kwargs': {'priority': 40}, # sto tests are slow, start early
    'specs': [
      'sto_using_cursor',
      'sto_using_select',
      'sto_using_hash_index',
    ],
    'regress_args': ['--temp-config', files('sto.conf')],
    # Disabled because these tests require "old_snapshot_threshold" >= 0, which
    # typical runningcheck users do not have (e.g. buildfarm clients).
    'runningcheck': false,
  },
}

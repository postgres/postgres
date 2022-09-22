pgoutput_sources = files(
  'pgoutput.c',
)

pgoutput = shared_module('pgoutput',
  pgoutput_sources,
  kwargs: pg_mod_args,
)

backend_targets += pgoutput

# Copyright (c) 2022-2025, PostgreSQL Global Development Group

node_support_input_i = [
  'nodes/nodes.h',
  'nodes/primnodes.h',
  'nodes/parsenodes.h',
  'nodes/pathnodes.h',
  'nodes/plannodes.h',
  'nodes/execnodes.h',
  'access/amapi.h',
  'access/cmptype.h',
  'access/sdir.h',
  'access/tableam.h',
  'access/tsmapi.h',
  'commands/event_trigger.h',
  'commands/trigger.h',
  'executor/tuptable.h',
  'foreign/fdwapi.h',
  'nodes/bitmapset.h',
  'nodes/extensible.h',
  'nodes/lockoptions.h',
  'nodes/miscnodes.h',
  'nodes/replnodes.h',
  'nodes/supportnodes.h',
  'nodes/value.h',
  'utils/rel.h',
]

node_support_input = []
foreach i : node_support_input_i
  node_support_input += meson.source_root() / 'src' / 'include' / i
endforeach

node_support_output = [
  'nodetags.h',
  'outfuncs.funcs.c', 'outfuncs.switch.c',
  'readfuncs.funcs.c', 'readfuncs.switch.c',
  'copyfuncs.funcs.c', 'copyfuncs.switch.c',
  'equalfuncs.funcs.c', 'equalfuncs.switch.c',
  'queryjumblefuncs.funcs.c', 'queryjumblefuncs.switch.c',
]
node_support_install = [
  dir_include_server / 'nodes',
  false, false,
  false, false,
  false, false,
  false, false,
  false, false,
]

generated_nodes = custom_target('nodetags.h',
  input: node_support_input,
  output: node_support_output,
  depend_files: catalog_pm,
  command: [
    perl, files('../../backend/nodes/gen_node_support.pl'),
    '-o', '@OUTDIR@',
    '@INPUT@'],
  install: true,
  install_dir: node_support_install,
)
generated_headers += generated_nodes[0]

# autoconf generates the file there, ensure we get a conflict
generated_sources_ac += {'src/backend/nodes': node_support_output + ['node-support-stamp']}
generated_sources_ac += {'src/include/nodes': ['header-stamp']}

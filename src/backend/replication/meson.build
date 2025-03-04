# Copyright (c) 2022-2025, PostgreSQL Global Development Group

backend_sources += files(
  'slot.c',
  'slotfuncs.c',
  'syncrep.c',
  'walreceiver.c',
  'walreceiverfuncs.c',
  'walsender.c',
)

# see ../parser/meson.build
repl_parser_sources = []

repl_scanner = custom_target('repl_scanner',
  input: 'repl_scanner.l',
  output: 'repl_scanner.c',
  command: flex_cmd,
)
generated_sources += repl_scanner
repl_parser_sources += repl_scanner
bc_repl_sources = repl_parser_sources

repl_gram = custom_target('repl_gram',
  input: 'repl_gram.y',
  kwargs: bison_kw,
)
generated_sources += repl_gram.to_list()
repl_parser_sources += repl_gram
bc_repl_sources += repl_gram[0]

syncrep_scanner = custom_target('syncrep_scanner',
  input: 'syncrep_scanner.l',
  output: 'syncrep_scanner.c',
  command: flex_cmd,
)
generated_sources += syncrep_scanner
repl_parser_sources += syncrep_scanner
bc_repl_sources += syncrep_scanner

syncrep_gram = custom_target('syncrep_gram',
  input: 'syncrep_gram.y',
  kwargs: bison_kw,
)
generated_sources += syncrep_gram.to_list()
repl_parser_sources += syncrep_gram
bc_repl_sources += syncrep_gram[0]

repl_parser = static_library('repl_parser',
  repl_parser_sources,
  dependencies: [backend_code],
  include_directories: include_directories('.'),
  kwargs: internal_lib_args,
)
backend_link_with += repl_parser

if llvm.found()
  llvm_gen_repl_cflags = [
  '-I@BUILD_ROOT@/src/backend/replication',
  '-I@SOURCE_ROOT@/src/backend/replication',
  ]

  llvm_gen_repl = generator(llvm_irgen_command,
    arguments: llvm_irgen_args + bitcode_cflags + llvm_gen_repl_cflags,
    depends: repl_parser,
    depfile: '@BASENAME@.c.bc.d',
    output: '@PLAINNAME@.bc',
  )

  bc_gen_custom_backend += llvm_gen_repl.process(bc_repl_sources)
endif

subdir('logical')

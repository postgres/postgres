# Copyright (c) 2022-2025, PostgreSQL Global Development Group

backend_sources += files(
  'analyze.c',
  'parse_agg.c',
  'parse_clause.c',
  'parse_coerce.c',
  'parse_collate.c',
  'parse_cte.c',
  'parse_enr.c',
  'parse_expr.c',
  'parse_func.c',
  'parse_jsontable.c',
  'parse_merge.c',
  'parse_node.c',
  'parse_oper.c',
  'parse_param.c',
  'parse_relation.c',
  'parse_target.c',
  'parse_type.c',
  'parse_utilcmd.c',
  'scansup.c',
)

# Build a small utility static lib for the parser. The generation of the
# parser is slow, and building this separately avoids other parts of the
# backend having to wait till gram.h is generated.
parser_sources = files('parser.c')

backend_scanner = custom_target('scan',
  input: 'scan.l',
  output: 'scan.c',
  command: [flex_cmd, '--no-backup', '--', '-CF', '-p', '-p'],
)
generated_sources += backend_scanner
parser_sources += backend_scanner
bc_parser_sources = parser_sources

backend_parser = custom_target('gram',
  input: 'gram.y',
  kwargs: bison_kw,
)
generated_sources += backend_parser.to_list()
parser_sources += backend_parser
bc_parser_sources += backend_parser[0]

parser = static_library('parser',
  parser_sources,
  dependencies: [backend_code],
  include_directories: include_directories('.'),
  kwargs: internal_lib_args,
)
backend_link_with += parser

if llvm.found()
  llvm_parser_cflags = [
    '-I@BUILD_ROOT@/src/backend/parser',
    '-I@SOURCE_ROOT@/src/backend/parser',
  ]

  llvm_gen_parser = generator(llvm_irgen_command,
    arguments: llvm_irgen_args + bitcode_cflags + llvm_parser_cflags,
    depends: parser,
    depfile: '@BASENAME@.c.bc.d',
    output: '@PLAINNAME@.bc',
  )

  bc_gen_custom_backend += llvm_gen_parser.process(bc_parser_sources)
endif
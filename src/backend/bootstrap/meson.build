# Copyright (c) 2022-2025, PostgreSQL Global Development Group

backend_sources += files(
 'bootstrap.c')

# see ../parser/meson.build
boot_parser_sources = []
bc_bootparse_sources = []

bootscanner = custom_target('bootscanner',
  input: 'bootscanner.l',
  output: 'bootscanner.c',
  command: flex_cmd,
)
generated_sources += bootscanner
boot_parser_sources += bootscanner
bc_bootparse_sources += bootscanner

bootparse = custom_target('bootparse',
  input: 'bootparse.y',
  kwargs: bison_kw,
)
generated_sources += bootparse.to_list()
boot_parser_sources += bootparse
bc_bootparse_sources += bootparse[0]

boot_parser = static_library('boot_parser',
  boot_parser_sources,
  dependencies: [backend_code],
  include_directories: include_directories('.'),
  kwargs: internal_lib_args,
)
backend_link_with += boot_parser

if llvm.found()
  llvm_bootparse_cflags = [
    '-I@BUILD_ROOT@/src/backend/bootstrap',
    '-I@SOURCE_ROOT@/src/backend/bootstrap',
  ]

  llvm_gen_bootparser = generator(llvm_irgen_command,
    arguments: llvm_irgen_args + bitcode_cflags + llvm_bootparse_cflags,
    depends: boot_parser,
    depfile: '@BASENAME@.c.bc.d',
    output: '@PLAINNAME@.bc',
  )

  bc_gen_custom_backend += llvm_gen_bootparser.process(bc_bootparse_sources)
endif

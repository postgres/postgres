# Copyright (c) 2022-2025, PostgreSQL Global Development Group

backend_sources += files(
  'bitmapset.c',
  'extensible.c',
  'list.c',
  'makefuncs.c',
  'multibitmapset.c',
  'nodeFuncs.c',
  'params.c',
  'print.c',
  'read.c',
  'tidbitmap.c',
  'value.c',
)

# these include .c files generated in ../../include/nodes, seems nicer to not
# add that as an include path for the whole backend
nodefunc_sources = files(
  'copyfuncs.c',
  'equalfuncs.c',
  'queryjumblefuncs.c',
  'outfuncs.c',
  'readfuncs.c',
)
nodefuncs = static_library('nodefuncs',
  nodefunc_sources,
  dependencies: [backend_code],
  include_directories: include_directories('../../include/nodes'),
  kwargs: internal_lib_args,
)
backend_link_with += nodefuncs

if llvm.found()
  llvm_nodefuncs_cflags = [
    '-I@SOURCE_ROOT@/src/include/nodes',
    '-I@BUILD_ROOT@/src/include/nodes',
  ]

  llvm_gen_nodefunc = generator(llvm_irgen_command,
    arguments: llvm_irgen_args + bitcode_cflags + llvm_nodefuncs_cflags,
    depends: nodefuncs,
    depfile: '@BASENAME@.c.bc.d',
    output: '@PLAINNAME@.bc',
  )

  bc_gen_custom_backend += llvm_gen_nodefunc.process(nodefunc_sources,
    preserve_path_from: meson.current_source_dir())
endif
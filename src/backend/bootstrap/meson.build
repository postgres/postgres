# Copyright (c) 2022-2024, PostgreSQL Global Development Group

backend_sources += files(
 'bootstrap.c')

# see ../parser/meson.build
boot_parser_sources = []

bootscanner = custom_target('bootscanner',
  input: 'bootscanner.l',
  output: 'bootscanner.c',
  command: flex_cmd,
)
generated_sources += bootscanner
boot_parser_sources += bootscanner

bootparse = custom_target('bootparse',
  input: 'bootparse.y',
  kwargs: bison_kw,
)
generated_sources += bootparse.to_list()
boot_parser_sources += bootparse

boot_parser = static_library('boot_parser',
  boot_parser_sources,
  dependencies: [backend_code],
  include_directories: include_directories('.'),
  kwargs: internal_lib_args,
)
backend_link_with += boot_parser

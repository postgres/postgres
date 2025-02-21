# Copyright (c) 2022-2025, PostgreSQL Global Development Group

if not llvm.found()
  subdir_done()
endif

# Build LLVM JIT backend module

llvmjit_sources = []

# Infrastructure
llvmjit_sources += files(
  'llvmjit.c',
  'llvmjit_error.cpp',
  'llvmjit_inline.cpp',
  'llvmjit_wrap.cpp',
  'SectionMemoryManager.cpp',
)

# Code generation
llvmjit_sources += files(
  'llvmjit_deform.c',
  'llvmjit_expr.c',
)

if host_system == 'windows'
  llvmjit_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'llvmjit',
    '--FILEDESC', 'llvmjit - JIT using LLVM',])
endif

llvmjit = shared_module('llvmjit',
  llvmjit_sources,
  kwargs: pg_mod_args + {
    'dependencies': pg_mod_args['dependencies'] + [llvm],
    'cpp_args': pg_mod_args['cpp_args'] + llvm.get_variable(configtool: 'cxxflags').split(),
  }
)

backend_targets += llvmjit

# Note this is intentionally not installed to bitcodedir, as it's not for
# inlining
llvmjit_types = custom_target('llvmjit_types.bc',
  command: [llvm_irgen_command] + llvm_irgen_args + bitcode_cflags,
  input: 'llvmjit_types.c',
  output: 'llvmjit_types.bc',
  depends: [postgres],
  install: true,
  install_dir: dir_lib_pkg,
  depfile: '@BASENAME@.c.bc.d',
)
backend_targets += llvmjit_types

# Figure out -I's needed to build all postgres code, including all its
# dependencies
pkg_config = find_program(['pkg-config', 'pkgconf'], required: true)
r = run_command(pkg_config,
  ['--cflags-only-I', meson.build_root() / 'meson-uninstalled/postgresql-extension-uninstalled.pc'],
  check: true)
bitcode_cflags += r.stdout().split()
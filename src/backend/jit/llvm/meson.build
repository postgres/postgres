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
)

# Code generation
llvmjit_sources += files(
  'llvmjit_deform.c',
  'llvmjit_expr.c',
)

llvmjit = shared_module('llvmjit',
  llvmjit_sources,
  kwargs: pg_mod_args + {
    'dependencies': pg_mod_args['dependencies'] + [llvm],
  }
)

backend_targets += llvmjit


# Define a few bits and pieces used here and elsewhere to generate bitcode

llvm_irgen_args = [
  '-c', '-o', '@OUTPUT@', '@INPUT@',
  '-flto=thin', '-emit-llvm',
  '-MD', '-MQ', '@OUTPUT@', '-MF', '@DEPFILE@',
  '-O2',
  '-Wno-ignored-attributes',
  '-Wno-empty-body',
]

if ccache.found()
  llvm_irgen_command = ccache
  llvm_irgen_args = [clang.path()] + llvm_irgen_args
else
  llvm_irgen_command = clang
endif


# XXX: Need to determine proper version of the function cflags for clang
bitcode_cflags = ['-fno-strict-aliasing', '-fwrapv']
bitcode_cflags += cppflags

# XXX: Worth improving on the logic to find directories here
bitcode_cflags += '-I@BUILD_ROOT@/src/include'
bitcode_cflags += '-I@BUILD_ROOT@/src/backend/utils/misc'
bitcode_cflags += '-I@SOURCE_ROOT@/src/include'


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

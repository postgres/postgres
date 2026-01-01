# Copyright (c) 2025-2026, PostgreSQL Global Development Group
use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Utils;
use Test::More;

# Defer entirely to the oauth_tests executable. stdout/err is routed through
# Test::More so that our logging infrastructure can handle it correctly. Using
# IPC::Run::new_chunker seems to help interleave the two streams a little better
# than without.
#
# TODO: prove can also deal with native executables itself, which we could
# probably make use of via PROVE_TESTS on the Makefile side. But the Meson setup
# calls Perl directly, which would require more code to work around... and
# there's still the matter of logging.
my $builder = Test::More->builder;
my $out = $builder->output;
my $err = $builder->failure_output;

IPC::Run::run ['oauth_tests'],
  '>' => (IPC::Run::new_chunker, sub { $out->print($_[0]) }),
  '2>' => (IPC::Run::new_chunker, sub { $err->print($_[0]) })
  or die "oauth_tests returned $?";

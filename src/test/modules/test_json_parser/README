Module `test_json_parser`
=========================

This module contains two programs for testing the json parsers.

- `test_json_parser_incremental` is for testing the incremental parser, It
  reads in a file and pases it in very small chunks (60 bytes at a time) to
  the incremental parser. It's not meant to be a speed test but to test the
  accuracy of the incremental parser. It takes one argument: the name of the
  input file.
- `test_json_parser_perf` is for speed testing both the standard
  recursive descent parser and the non-recursive incremental
  parser. If given the `-i` flag it uses the non-recursive parser,
  otherwise the stardard parser. The remaining flags are the number of
  parsing iterations and the file containing the input. Even when
  using the non-recursive parser, the input is passed to the parser in a
  single chunk. The results are thus comparable to those of the
  standard parser.

The easiest way to use these is to run `make check` and `make speed-check`

The sample input file is a small extract from a list of `delicious`
bookmarks taken some years ago, all wrapped in a single json
array. 10,000 iterations of parsing this file gives a reasonable
benchmark, and that is what the `speed-check` target does.

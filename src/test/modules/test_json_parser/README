Module `test_json_parser`
=========================

This module contains two programs for testing the json parsers.

- `test_json_parser_incremental` is for testing the incremental parser, It
  reads in a file and passes it in very small chunks (default is 60 bytes at a
  time) to the incremental parser. It's not meant to be a speed test but to
  test the accuracy of the incremental parser.  There are two option arguments,
  "-c nn" specifies an alternative chunk size, and "-s" specifies using
  semantic routines. The semantic routines re-output the json, although not in
  a very pretty form. The required non-option argument is the input file name.
- `test_json_parser_perf` is for speed testing both the standard
  recursive descent parser and the non-recursive incremental
  parser. If given the `-i` flag it uses the non-recursive parser,
  otherwise the standard parser. The remaining flags are the number of
  parsing iterations and the file containing the input. Even when
  using the non-recursive parser, the input is passed to the parser in a
  single chunk. The results are thus comparable to those of the
  standard parser.

The sample input file is a small, sanitized extract from a list of `delicious`
bookmarks taken some years ago, all wrapped in a single json
array.

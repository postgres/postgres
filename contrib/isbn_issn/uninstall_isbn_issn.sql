SET search_path = public;

DROP OPERATOR CLASS isbn_ops;

DROP FUNCTION isbn_cmp(isbn, isbn);

DROP OPERATOR <> (isbn, isbn);

DROP OPERATOR > (isbn, isbn);

DROP OPERATOR >= (isbn, isbn);

DROP OPERATOR = (isbn, isbn);

DROP OPERATOR <= (isbn, isbn);

DROP OPERATOR < (isbn, isbn);

DROP FUNCTION isbn_ne(isbn, isbn);

DROP FUNCTION isbn_gt(isbn, isbn);

DROP FUNCTION isbn_ge(isbn, isbn);

DROP FUNCTION isbn_eq(isbn, isbn);

DROP FUNCTION isbn_le(isbn, isbn);

DROP FUNCTION isbn_lt(isbn, isbn);

DROP TYPE isbn;

DROP FUNCTION isbn_out(isbn);

DROP FUNCTION isbn_in(cstring);

DROP OPERATOR CLASS issn_ops;

DROP FUNCTION issn_cmp(issn, issn);

DROP OPERATOR <> (issn, issn);

DROP OPERATOR > (issn, issn);

DROP OPERATOR >= (issn, issn);

DROP OPERATOR = (issn, issn);

DROP OPERATOR <= (issn, issn);

DROP OPERATOR < (issn, issn);

DROP FUNCTION issn_ne(issn, issn);

DROP FUNCTION issn_gt(issn, issn);

DROP FUNCTION issn_ge(issn, issn);

DROP FUNCTION issn_eq(issn, issn);

DROP FUNCTION issn_le(issn, issn);

DROP FUNCTION issn_lt(issn, issn);

DROP TYPE issn;

DROP FUNCTION issn_out(issn);

DROP FUNCTION issn_in(cstring);

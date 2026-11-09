# Test that a type cannot be dropped or altered in an incompatible way
# while it's being used as a column type in a concurrent CREATE TABLE
# transaction.

session "s1"

step s1_create_domain { CREATE DOMAIN testdom AS integer; }
step s1_create_rowtype_tbl { CREATE TABLE rowtype(a int8, b text); }
step s1_create_rowtype_comp { CREATE TYPE rowtype AS(a int8, b text); }

step s1_begin { BEGIN; }
step s1_create_testtab { CREATE TABLE testtab (a int); }
step s1_add_domain_col { ALTER TABLE testtab ADD COLUMN b testdom; }
step s1_create_testtab_with_domain { CREATE TABLE testtab (a int, b testdom); }
step s1_create_testview_with_domain { CREATE VIEW testview AS SELECT 123::testdom IS NULL; }
step s1_create_testtab_with_rowtype { CREATE TABLE testtab (a int, b rowtype); }
step s1_create_testview_with_rowtype_top { CREATE VIEW testview AS SELECT '(1,b)'::rowtype; }
step s1_create_testview_with_rowtype_sub { CREATE VIEW testview AS SELECT '(1,b)'::rowtype IS NULL; }
step s1_commit { COMMIT; }

step s1_drop_domain { DROP DOMAIN testdom; }
step s1_drop_testtab { DROP TABLE testtab; }
step s1_drop_testview { DROP VIEW testview; }
step s1_drop_rowtype_tbl { DROP TABLE rowtype; }
step s1_drop_rowtype_comp { DROP TYPE rowtype; }

session "s2"

#step s2_begin { BEGIN; }
#step s2_commit { COMMIT; }
step s2_drop_domain { DROP DOMAIN testdom; }
step s2_alter_rowtype_comp_to_text { ALTER TYPE rowtype ALTER ATTRIBUTE a TYPE text; }
step s2_alter_rowtype_tbl_to_text { ALTER TABLE rowtype ALTER COLUMN a TYPE text; }
step s2_alter_rowtype_tbl_add_col { ALTER TABLE rowtype ADD COLUMN c bigint; }
step s2_drop_rowtype_tbl { DROP TABLE rowtype; }

# Test that when a domain is used in a new, not-yet-committed, table,
# it cannot be concurrently dropped.
permutation
    s1_create_domain
    s1_begin
    s1_create_testtab_with_domain
    s2_drop_domain
    s1_commit
    s1_drop_testtab
    s1_drop_domain

# Test the same race condition with ADD COLUMN to an existing table
permutation
    s1_create_domain
    s1_create_testtab
    s1_begin
    s1_add_domain_col
    s2_drop_domain
    s1_commit
    s1_drop_testtab
    s1_drop_domain

# Same, but it's not used as a column type, but in an expression in a view
permutation
    s1_create_domain
    s1_begin
    s1_create_testview_with_domain
    s2_drop_domain
    s1_commit
    s1_drop_testview
    s1_drop_domain

# Test that a new reference to a rowtype that is concurrently being
# dropped is handled correctly. Here the drop happens first.
#
# XXX: Disabled for now, because the error message "could not open
# relation with OID" contains the relation's OID which is not stable.
#permutation
#    s1_create_rowtype_tbl
#    s2_begin
#    s2_drop_rowtype_tbl
#    s1_create_testtab_with_rowtype # blocks
#    s2_commit
#    # s1 errors out slightly weirdly

# Here the new reference happens first
permutation
    s1_create_rowtype_tbl
    s1_begin
    s1_create_testtab_with_rowtype
    s2_drop_rowtype_tbl  # blocks
    s1_commit
    # s2 errors out with dependency error
    s1_drop_testtab
    s1_drop_rowtype_tbl

# Like above, but with a view that uses the rowtype in the view body
permutation
    s1_create_rowtype_tbl
    s1_begin
    s1_create_testview_with_rowtype_top
    s2_drop_rowtype_tbl
    s1_commit
    s1_drop_testview
    s1_drop_rowtype_tbl

permutation
    s1_create_rowtype_tbl
    s1_begin
    s1_create_testview_with_rowtype_sub
    s2_drop_rowtype_tbl
    s1_commit
    s1_drop_testview
    s1_drop_rowtype_tbl

# Test that when a composite type is used in a new, not-yet-committed,
# table, it cannot be ALTERed concurrently in a way that would require
# rewriting the table that uses it.
permutation
    s1_create_rowtype_comp
    s1_begin
    s1_create_testtab_with_rowtype
    s2_alter_rowtype_comp_to_text
    s1_commit
    s1_drop_testtab
    s1_drop_rowtype_comp

# Same with a rowtype of another table rather than an explicit
# composite type.
permutation
    s1_create_rowtype_tbl
    s1_begin
    s1_create_testtab_with_rowtype
    s2_alter_rowtype_tbl_to_text
    s1_commit
    s1_drop_testtab
    s1_drop_rowtype_tbl

# Same, but the rowtype isn't used directly as a column in another table,
# but in an expression in a view.
permutation
    s1_create_rowtype_comp
    s1_begin
    s1_create_testview_with_rowtype_sub
    s2_alter_rowtype_comp_to_text
    s1_commit
    s1_drop_testview
    s1_drop_rowtype_comp

# Adding a column doesn't require rewriting existing data, so this
# could be allowed, but is blocked currently by the AccessShareLock
# that CREATE TABLE holds on the rowtype.
permutation
    s1_create_rowtype_tbl
    s1_begin
    s1_create_testtab_with_rowtype
    s2_alter_rowtype_tbl_add_col # blocks
    s1_commit
    s1_drop_testtab
    s1_drop_rowtype_tbl

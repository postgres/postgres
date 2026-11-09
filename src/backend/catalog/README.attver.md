# Attribute versioning

## Background

Datums of both named composite types (typcategory `C`) and anonymous record
types (the `record` type) must be deformed using a tuple descriptor that is
compatible with the one used to form them, but there is an important
difference: subsequent DDL can change the definition of a composite type, but
an anonymous record type's column definitions are frozen at the moment of its
creation. Anonymous record types can be created by `ROW()`, where the SQL gives
rise to the tuple descriptor; or by whole-row references to a subquery, which
work the same way; or by functions whose return type is `record`, where the
record type may be decided by the function itself at runtime, provided by the
caller, or encoded in the function's `pg_proc` row. In none of these cases can
the column list be changed later.

On the other hand, stand-alone composite types can be modified using `ALTER
TYPE ... { ADD | DROP | ALTER } ATTRIBUTE`, and the automatically generated
composite types for tables, views, etc. can be modified using `ALTER TABLE
... { ADD | DROP | ALTER } COLUMN`. This creates a risk: the type definition
as stored in the catalogs might change between the time a datum is created and
the time the datum is used. That can lead to using the wrong tuple descriptor
when deforming.

The fact that DDL can be rolled back adds to the danger: Attributes can be
added or changed inside of a transaction, and tuples using the modified tuple
descriptor can be retained beyond the rollback, creating additional potential
for mismatches.

Using a tuple descriptor that does not match the "shape" of a composite datum
can lead to crashes, spurious errors and in some circumstances can be used to
achieve arbitrary code execution.


## Higher-level protections

There are protections against changing row types in a way that causes
problems:

- A table may not be altered while it is in use in the same session

  This is important because relation locking alone would not protect against
  the same session altering the relation.

  See `CheckTableNotInUse()`

- Relations are locked when they are accessed and the relation's tupledesc is
  [re-]built with that lock held

  Together with the prior point, this ensures the relation's tuple descriptor
  is always compatible when scanning a relation.

- Columns of composite types are not allowed to be altered if they are used in
  other objects (e.g. as a table column).

  See `find_composite_type_dependencies()`

None of these mechanisms, however, prevents type alteration while transient
"objects" like prepared statements or cursors hold composite datums.

Making these protections complete is nontrivial.


## Attribute versioning as a safety net

Attribute versioning adds a safety layer below those other protections. It
detects cases in which composite datums are about to be deformed with tuple
descriptors that differ, in incompatible ways, from the one used to form the
datum.  If such a mismatch is detected, an error is raised.

However, most, if not all, such mismatches should really be prevented well
before this safety layer kicks in:

- By nature of being a lower-level safety net, the errors it raises have to be
  fairly generic and will often be difficult for users to understand.

- Preventing unsafe deforming of composites does not guarantee that the system
  does not contain any composite datums that cannot be deformed anymore. Data
  that is stored, but cannot be accessed anymore, is obviously bad.

Despite these disadvantages, this safety net was deemed worthwhile, given that
we have had a large number of cases in which the existing, more targeted,
protections could be circumvented.

It also turns out to be surprisingly easy to introduce new paths that violate
type safety, making the safety layer worthwhile even if we were sure there
were no extant issues.


## High-level design

To prevent unsafe deforming, named composite datums are tagged with the
version of the tuple descriptor that formed the composite.  During deforming
the tuple descriptor's and tuple's versions are checked for compatibility.
Anonymous record datums do not need this, as their descriptor cannot change.

As composite datums can be stored in tables etc., the version for the tuple
descriptor needs to be durable too.

At first blush it seems attractive to store the version at the rowtype level
(i.e. introduce a version associated with each `pg_class`/`pg_type` entry that
has a rowtype). That however turns out to be insufficient, due to not being
able to protect against deforming tuples that were formed in an aborted
[sub-]transaction.

Aborted [sub-]transactions are a problem because a new column / attribute
added in the subtransaction may be incompatible with a new column, with the
same attribute number, that has been added since.  Outside of composites from
aborted transactions that is not a problem because otherwise attribute numbers
are never reused.

The most obvious answer to that would be to increment a rowtype-wide version
at every `ADD COLUMN`, but that would not allow deforming of composite datums
formed before the `ADD COLUMN`. That clearly needs to continue to be
supported.

Therefore the versioning scheme has to be at the column/attribute level and be
resistant to rollbacks.

One major driver of complexity around making it safe to deform composite
datums is that the places that actually do the deforming don't statically know
whether they are dealing with a row in a table (for which we do not need to
check versions, locking protects them against changes) or with a composite
tuple.  This is addressed by the `HEAP_IS_DATUM` `t_infomask` flag
combination.


## Versioning scheme

This versioning scheme is implemented as follows:

- A WAL LSN of an upcoming WAL insertion is used as the version

  This protects against rollback related issues, as the WAL insertion point
  continues to increase after rollbacks.

  See `GetNextAttributeVersion()`

- In the catalog, versions are associated with the `pg_attribute` rows of
  relations that have a rowtype

- Whenever a new relation is created, all its attributes start out with the
  same new version

  See `heap_create_with_catalog()`

- Whenever a new attribute is added, it gets a new version

  See `ATExecAddColumn()`

- Whenever an attribute's type is changed, the versions of that attribute and
  of all following attributes are increased, as the change can shift the
  offsets of all following attributes

  See `ATExecAlterColumnType()`

- Dropping an attribute does not change any version, as the dropped
  attribute's `attlen`/`attalign` are retained and existing datums therefore
  remain deformable

  See `RemoveAttributeById()`

- Composite datums are stamped with the current version of their last
  physically present attribute

  For a full-width datum this equals the version of the descriptor's last
  attribute; for a short datum it preserves the version of the prefix actually
  stored.

  See `heap_tuple_set_datum()`

- Deforming of composite datums compares the tuple's version with the version
  associated with `HeapTupleHeaderGetNatts(tup)`

  This is important to allow deforming of a tuple that was formed when the
  type had fewer attributes.  It is sufficient because, due to the rules
  above, the version of an attribute changes whenever the layout of any
  attribute up to and including it changes.

  Every code path that deforms a tuple that might be a versioned datum has to
  perform this check, by calling `heap_datum_check()` if
  `HeapTupleHeaderIsVersionedDatum()`.

The above describes a scheme in which attribute versions for one rowtype are
increasing monotonically with the attribute number. If that property were
violated, the whole scheme would break down.

Version storage has three aspects, described in the following sections:

1) Persistently, in the catalog, for each attribute

2) In tuple descriptors, one version per attribute

3) In each composite datum, the version used to form it


## Version storage in the catalog

Starting in PG 20, each attribute's version is stored in
`pg_attribute.attversion`.

Before that the version was stored in `pg_attribute.attoptions`, using a
reserved name (`pg_attver`). That key was only used internally and could not be
set by the user. Even though not used anymore, we still ignore that key when
set via ALTER: Dumps produced by older pg_dump versions or other external
tooling, which doesn't yet know to strip the key, would otherwise fail to
restore.


## Version storage in tuple descriptors

Starting in PG 20, each attribute's version is stored in its `attversion`
field; `tdversion` caches the last attribute's version for fast access.  Before
that we had to resort to uglier measures to retain ABI compatibility.


## Version storage in composite datums

Composite datums store the version (an 8-byte LSN) split evenly across the
following two locations:

- `HeapTupleHeaderData.t_choice.t_datum.datum_typmod`

  Previously it was always set to -1 for named composite datums.

- `HeapTupleHeaderData.t_ctid.ip_blkid`

  Previously we did not use the ctid for composites.

Datum tuples are marked as such using `HEAP_IS_DATUM`, using a previously
unused `t_infomask` bit combination
(`HEAP_XMAX_COMMITTED | HEAP_XMAX_INVALID`). Both named composites and
anonymous records are marked as datums. This allows us to distinguish
composite tuples from tuples coming from a table etc.

Named composite datums are marked as versioned by storing the special offset
number `VersionedDatumOffsetNumber` in `t_ctid.ip_posid`, storing the version
as explained above.

A happy side-effect of using `VersionedDatumOffsetNumber` is that it avoids
spurious errors if extensions overwrite ctid. That is important as
`postgres_fdw` has established the pattern of resetting various fields in the
tuples it returns, which has since been copied into many extensions.


## Upgrading from pre-versioning composite datums

To deal with pre-upgrade (minor or pg_upgrade) tuples that are not versioned,
`DatumGetHeapTupleHeader()` copies such datums, marks them as datums, and, for
named composites, sets the copy's version to 0. As every datum tuple at some
point goes through `DatumGetHeapTupleHeader()`, this allows us to

a) Differentiate between datum and non-datum tuples in places that otherwise
   would not know what they're dealing with.

b) Prevent deforming of composite datums created before an upgrade that have
   become unsafe due to post-upgrade DDL, as the inferred version 0 will not
   match the version after DDL.

Similarly, the version of attributes predating versioning is assumed to be 0.

It is important to treat a tuple with no version as if it had version 0. If
we did not do so, composite tuples would not be protected against type
alterations that occur after upgrading to a server version that includes
attribute versioning. For example, imagine storing a composite-type value
loaded from disk into a plpgsql variable, dropping the containing relation,
and then altering the type (dropping the containing relation is necessary to
avoid tripping over `find_composite_type_dependencies()`).


## `pg_upgrade` handling

`pg_upgrade` must preserve attribute versions. `pg_dump --binary-upgrade` does
so by emitting calls to `binary_upgrade_set_next_attribute_versions()`.
Without this, composite datums stored in a table wouldn't be readable after
`pg_upgrade`.

`pg_upgrade` has always adjusted the target cluster's WAL to start after the
source cluster's, so versions are still monotonically increasing after
`pg_upgrade`.


## The `composite_type_mismatch` setting

The developer option `composite_type_mismatch` can downgrade the mismatch
error to a warning or disable the checks entirely.  Both settings are unsafe.

This exists as an escape hatch in case of problems with this safety net
(e.g. due to extensions) and to allow for data recovery in some cases.

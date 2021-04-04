spgist_name_ops implements an SP-GiST operator class that indexes
columns of type "name", but with storage identical to that used
by SP-GiST text_ops.

This is not terribly useful in itself, perhaps, but it allows
testing cases where the indexed data type is different from the leaf
data type and yet we can reconstruct the original indexed value.
That situation is not tested by any built-in SP-GiST opclass.

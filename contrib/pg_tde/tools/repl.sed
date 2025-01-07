# These first few lines are only for the initial run, but should be harmless in later runs
s/\theap_/\ttdeheap_/g
s/\t\*heap_/\t*tdeheap_/g
s/ heap_/ tdeheap_/g
s/ \*heap_/ *tdeheap_/g
s/(heap_/ (tdeheap_/g
s/^heap_/tdeheap_/g
s/_heap_/_tdeheap_/g
s/-heap_/-tdeheap_/g
s/+heap_/+tdeheap_/g
s/!heap_/!tdeheap_/g
s/heapam_/pg_tdeam_/g
s/heap2_/tdeheap2_/g
s/heapgettup/tdeheapgettup/g
s/heapgetpage/tdeheapgetpage/g
s/visibilitymap_/tdeheap_visibilitymap_/g
s/RelationPutHeapTuple/tdeheap_RelationPutHeapTuple/g
s/RelationGetBufferForTuple/tdeheap_RelationGetBufferForTuple/g
s/TTSOpsBufferHeapTuple/TTSOpsTDEBufferHeapTuple/g
s/TTS_IS_BUFFERTUPLE/TTS_IS_TDE_BUFFERTUPLE/g
s/toast_tuple_externalize/tdeheap_toast_tuple_externalize/g
# Repairing error by earlier rule
s/num_tdeheap_tuples/num_heap_tuples/g
s/pgstat_update_tdeheap_dead_tuples/pgstat_update_heap_dead_tuples/g
s/tdeheap_xlog_deserialize_prune_and_freeze/heap_xlog_deserialize_prune_and_freeze/g
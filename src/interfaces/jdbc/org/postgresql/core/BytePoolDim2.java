package org.postgresql.core;

public class BytePoolDim2 {
    int maxsize = 32;
    ObjectPool notusemap[] = new ObjectPool[maxsize+1];
    ObjectPool inusemap[] = new ObjectPool[maxsize+1];

    public BytePoolDim2(){
	for(int i = 0; i <= maxsize; i++){
	    inusemap[i] = new SimpleObjectPool();
	    notusemap[i] = new SimpleObjectPool();
	}
    }

    public byte[][] allocByte(int size){
      // For now until the bug can be removed
      return new byte[size][0];
      /*
	if(size > maxsize){
	    return new byte[size][0];
	}
	ObjectPool not_usel = notusemap[size];
	ObjectPool in_usel =  inusemap[size];

	byte b[][] = null;

	if(!not_usel.isEmpty()) {
	    Object o = not_usel.remove();
	    b = (byte[][]) o;
	} else
	    b = new byte[size][0];
	in_usel.add(b);
	return b;
        */
    }

    public void release(byte[][] b){
	if(b.length > maxsize){
	    return;
	}
	ObjectPool not_usel = notusemap[b.length];
	ObjectPool in_usel =  inusemap[b.length];

	in_usel.remove(b);
        not_usel.add(b);
    }

    /**
     * Deallocate the object cache.
     * PM 17/01/01: Commented out this code as it blows away any hope of
     * multiple queries on the same connection. I'll redesign the allocation
     * code to use some form of Statement context, so the buffers are per
     * Statement and not per Connection/PG_Stream as it is now.
     */
    public void deallocate(){
	//for(int i = 0; i <= maxsize; i++){
	//    notusemap[i].addAll(inusemap[i]);
	//    inusemap[i].clear();
	//}
    }
}


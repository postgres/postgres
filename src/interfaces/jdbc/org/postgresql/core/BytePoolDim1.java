package org.postgresql.core;

/**
 * A simple and efficient class to pool one dimensional byte arrays
 * of different sizes.
 */
public class BytePoolDim1 {

    /**
     * The maximum size of the array we manage.
     */
    int maxsize = 256;
    /**
     * The pools not currently in use
     */
    ObjectPool notusemap[] = new ObjectPool[maxsize+1];
    /**
     * The pools currently in use
     */
    ObjectPool inusemap[] = new ObjectPool[maxsize+1];
    /**
     *
     */
    byte binit[][] = new byte[maxsize][0];

    /**
     * Construct a new pool
     */
    public BytePoolDim1(){
	for(int i = 0; i <= maxsize; i++){
	    binit[i] = new byte[i];
	    inusemap[i] = new SimpleObjectPool();
	    notusemap[i] = new SimpleObjectPool();
	}
    }

    /**
     * Allocate a byte[] of a specified size and put it in the pool. If it's
     * larger than maxsize then it is not pooled.
     * @return the byte[] allocated
     */
    public byte[] allocByte(int size) {
      // for now until the bug can be removed
      return new byte[size];
      /*
        // Don't pool if >maxsize
	if(size > maxsize){
	    return new byte[size];
	}

	ObjectPool not_usel = notusemap[size];
	ObjectPool in_usel = inusemap[size];
	byte b[] = null;

        // Fetch from the unused pool if available otherwise allocate a new
        // now array
	if(!not_usel.isEmpty()) {
	    Object o = not_usel.remove();
	    b = (byte[]) o;
	} else
	    b = new byte[size];
	in_usel.add(b);

	return b;
        */
    }

    /**
     * Release an array
     * @param b byte[] to release
     */
    public void release(byte[] b) {
      // If it's larger than maxsize then we don't touch it
      if(b.length>maxsize)
        return;

      ObjectPool not_usel = notusemap[b.length];
      ObjectPool in_usel = inusemap[b.length];

      in_usel.remove(b);
      not_usel.add(b);
    }

    /**
     * Deallocate all
     * @deprecated Real bad things happen if this is called!
     */
    public void deallocate() {
	//for(int i = 0; i <= maxsize; i++){
	//    notusemap[i].addAll(inusemap[i]);
	//    inusemap[i].clear();
	//}
    }
}


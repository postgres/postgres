package org.postgresql.core;

/**
 * A simple and fast object pool implementation that can pool objects
 * of any type. This implementation is not thread safe, it is up to the users
 * of this class to assure thread safety.
 */

public class SimpleObjectPool implements ObjectPool
{
    // This was originally in PG_Stream but moved out to fix the major problem
    // where more than one query (usually all the time) overwrote the results
    // of another query.
    int cursize = 0;
    int maxsize = 16;
    Object arr[] = new Object[maxsize];

    /**
     * Adds an object to the pool
     * @param o Object to add
     */
    public void add(Object o)
    {
	if(cursize >= maxsize){
	    Object newarr[] = new Object[maxsize*2];
	    System.arraycopy(arr, 0, newarr, 0, maxsize);
	    maxsize = maxsize * 2;
	    arr = newarr;
	}
	arr[cursize++] = o;
    }

    /**
     * Removes the top object from the pool
     * @return Object from the top.
     */
    public Object remove(){
	return arr[--cursize];
    }

    /**
     * Removes the given object from the pool
     * @param o Object to remove
     */
    public void remove(Object o) {
      int p=0;
      while(p<cursize && !arr[p].equals(o))
        p++;
      if(arr[p].equals(o)) {
        // This should be ok as there should be no overlap conflict
        System.arraycopy(arr,p+1,arr,p,cursize-p);
        cursize--;
      }
    }

    /**
     * @return true if the pool is empty
     */
    public boolean isEmpty(){
	return cursize == 0;
    }

    /**
     * @return the number of objects in the pool
     */
    public int size(){
	return cursize;
    }

    /**
     * Adds all objects in one pool to this one
     * @param pool The pool to take the objects from
     */
    public void addAll(ObjectPool p){
      SimpleObjectPool pool = (SimpleObjectPool)p;

	int srcsize = pool.size();
	if(srcsize == 0)
	    return;
	int totalsize = srcsize + cursize;
	if(totalsize > maxsize){
	    Object newarr[] = new Object[totalsize*2];
	    System.arraycopy(arr, 0, newarr, 0, cursize);
	    maxsize = maxsize = totalsize * 2;
	    arr = newarr;
	}
	System.arraycopy(pool.arr, 0, arr, cursize, srcsize);
	cursize = totalsize;
    }

    /**
     * Clears the pool of all objects
     */
    public void clear(){
	    cursize = 0;
    }
}

package org.postgresql;

/**
 * A simple and fast object pool implementation that can pool objects 
 * of any type. This implementation is not thread safe, it is up to the users
 * of this class to assure thread safety. 
 */
public class ObjectPool {
    int cursize = 0;
    int maxsize = 8;
    Object arr[] = new Object[8];
    
    /**
     * Add object to the pool.
     * @param o The object to add.
     */
    public void add(Object o){
	if(cursize >= maxsize){
	    Object newarr[] = new Object[maxsize*2];
	    System.arraycopy(arr, 0, newarr, 0, maxsize);
	    maxsize = maxsize * 2;
	    arr = newarr;
	}
	arr[cursize++] = o;
    }
    
    /**
     * Remove an object from the pool. If the pool is empty
     * ArrayIndexOutOfBoundsException will be thrown.
     * @return Returns the removed object.
     * @exception If the pool is empty 
     *            ArrayIndexOutOfBoundsException will be thrown.
     */
    public Object remove(){
	Object o = arr[cursize-1];
	// This have to be here, so we don't decrease the counter when
	// cursize == 0;
	cursize--;
	return o;
    }

    /**
     * Check if pool is empty.
     * @return true if pool is empty, false otherwise.
     */
    public boolean isEmpty(){
	return cursize == 0;
    }
    
    /**
     * Get the size of the pool.
     * @return Returns the number of objects in the pool.
     */
    public int size(){
	return cursize;
    }
    /**
     * Add all the objects from another pool to this pool.
     * @pool The pool to add the objects from.
     */
    public void addAll(ObjectPool pool){
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
     * Clear the elements from this pool.
     * The method is lazy, so it just resets the index counter without
     * removing references to pooled objects. This could possibly 
     * be an issue with garbage collection, depending on how the 
     * pool is used.
     */
    public void clear(){
	cursize = 0;
    }
}

package org.postgresql.core;

/**
 * This interface defines methods needed to implement a simple object pool.
 * There are two known classes that implement this, one for jdk1.1 and the
 * other for jdk1.2+
 */

public interface ObjectPool {
    /**
     * Adds an object to the pool
     * @param o Object to add
     */
    public void add(Object o);

    /**
     * Removes an object from the pool
     * @param o Object to remove
     */
    public void remove(Object o);

    /**
     * Removes the top object from the pool
     * @return Object from the top.
     */
    public Object remove();

    /**
     * @return true if the pool is empty
     */
    public boolean isEmpty();

    /**
     * @return the number of objects in the pool
     */
    public int size();

    /**
     * Adds all objects in one pool to this one
     * @param pool The pool to take the objects from
     */
    public void addAll(ObjectPool pool);

    /**
     * Clears the pool of all objects
     */
    public void clear();
}

package org.postgresql.core;

/**
 * This interface defines the methods to access the memory pool classes.
 */
public interface MemoryPool {
  /**
   * Allocate an array from the pool
   * @return byte[] allocated
   */
  public byte[] allocByte(int size);

  /**
   * Frees an object back to the pool
   * @param o Object to release
   */
  public void release(Object o);
}
package org.postgresql.largeobject;

import java.io.IOException;
import java.io.OutputStream;
import java.sql.SQLException;

/**
 * This implements a basic output stream that writes to a LargeObject
 */
public class BlobOutputStream extends OutputStream {
  /**
   * The parent LargeObject
   */
  private LargeObject lo;

  /**
   * Buffer
   */
  private byte buf[];

  /**
   * Size of the buffer (default 1K)
   */
  private int bsize;

  /**
   * Position within the buffer
   */
  private int bpos;

  /**
   * Create an OutputStream to a large object
   * @param lo LargeObject
   */
  public BlobOutputStream(LargeObject lo) {
    this(lo,1024);
  }

  /**
   * Create an OutputStream to a large object
   * @param lo LargeObject
   * @param bsize The size of the buffer used to improve performance
   */
  public BlobOutputStream(LargeObject lo,int bsize) {
    this.lo=lo;
    this.bsize=bsize;
    buf=new byte[bsize];
    bpos=0;
  }

  public void write(int b) throws java.io.IOException {
      try {
        if(bpos>=bsize) {
          lo.write(buf);
          bpos=0;
        }
        buf[bpos++]=(byte)b;
      } catch(SQLException se) {
        throw new IOException(se.toString());
      }
  }

    /**
     * Flushes this output stream and forces any buffered output bytes
     * to be written out. The general contract of <code>flush</code> is
     * that calling it is an indication that, if any bytes previously
     * written have been buffered by the implementation of the output
     * stream, such bytes should immediately be written to their
     * intended destination.
     *
     * @exception  IOException  if an I/O error occurs.
     */
    public void flush() throws IOException {
      try {
        if(bpos>0)
          lo.write(buf,0,bpos);
        bpos=0;
      } catch(SQLException se) {
        throw new IOException(se.toString());
      }
    }

    /**
     * Closes this output stream and releases any system resources
     * associated with this stream. The general contract of <code>close</code>
     * is that it closes the output stream. A closed stream cannot perform
     * output operations and cannot be reopened.
     * <p>
     * The <code>close</code> method of <code>OutputStream</code> does nothing.
     *
     * @exception  IOException  if an I/O error occurs.
     */
    public void close() throws IOException {
      try {
        flush();
        lo.close();
        lo=null;
      } catch(SQLException se) {
        throw new IOException(se.toString());
      }
    }

}
package org.postgresql.largeobject;

import java.io.InputStream;
import java.io.IOException;
import java.sql.SQLException;

/**
 * This is an initial implementation of an InputStream from a large object.
 * For now, the bare minimum is implemented. Later (after 7.1) we will overide
 * the other read methods to optimise them.
 */
public class BlobInputStream extends InputStream {
  /**
   * The parent LargeObject
   */
  private LargeObject lo;

  /**
   * Buffer used to improve performance
   */
  private byte[] buffer;

  /**
   * Position within buffer
   */
  private int bpos;

  /**
   * The buffer size
   */
  private int bsize;

  /**
   * The mark position
   */
  private int mpos=0;

  /**
   * @param lo LargeObject to read from
   */
  public BlobInputStream(LargeObject lo) {
    this(lo,1024);
  }

  /**
   * @param lo LargeObject to read from
   * @param bsize buffer size
   */
  public BlobInputStream(LargeObject lo,int bsize) {
    this.lo=lo;
    buffer=null;
    bpos=0;
    this.bsize=bsize;
  }

  /**
   * The minimum required to implement input stream
   */
  public int read() throws java.io.IOException {
    try {
      if(buffer==null || bpos>=buffer.length) {
        buffer=lo.read(bsize);
        bpos=0;
      }

      // Handle EOF
      if(bpos>=buffer.length)
        return -1;

      return (int) buffer[bpos++];
    } catch(SQLException se) {
      throw new IOException(se.toString());
    }
  }


    /**
     * Closes this input stream and releases any system resources associated
     * with the stream.
     *
     * <p> The <code>close</code> method of <code>InputStream</code> does
     * nothing.
     *
     * @exception  IOException  if an I/O error occurs.
     */
    public void close() throws IOException {
      try {
        lo.close();
        lo=null;
      } catch(SQLException se) {
        throw new IOException(se.toString());
      }
    }

    /**
     * Marks the current position in this input stream. A subsequent call to
     * the <code>reset</code> method repositions this stream at the last marked
     * position so that subsequent reads re-read the same bytes.
     *
     * <p> The <code>readlimit</code> arguments tells this input stream to
     * allow that many bytes to be read before the mark position gets
     * invalidated.
     *
     * <p> The general contract of <code>mark</code> is that, if the method
     * <code>markSupported</code> returns <code>true</code>, the stream somehow
     * remembers all the bytes read after the call to <code>mark</code> and
     * stands ready to supply those same bytes again if and whenever the method
     * <code>reset</code> is called.  However, the stream is not required to
     * remember any data at all if more than <code>readlimit</code> bytes are
     * read from the stream before <code>reset</code> is called.
     *
     * <p> The <code>mark</code> method of <code>InputStream</code> does
     * nothing.
     *
     * @param   readlimit   the maximum limit of bytes that can be read before
     *                      the mark position becomes invalid.
     * @see     java.io.InputStream#reset()
     */
    public synchronized void mark(int readlimit) {
      try {
        mpos=lo.tell();
      } catch(SQLException se) {
        //throw new IOException(se.toString());
      }
    }

    /**
     * Repositions this stream to the position at the time the
     * <code>mark</code> method was last called on this input stream.
     * NB: If mark is not called we move to the begining.
     * @see     java.io.InputStream#mark(int)
     * @see     java.io.IOException
     */
    public synchronized void reset() throws IOException {
      try {
        lo.seek(mpos);
      } catch(SQLException se) {
        throw new IOException(se.toString());
      }
    }

    /**
     * Tests if this input stream supports the <code>mark</code> and
     * <code>reset</code> methods. The <code>markSupported</code> method of
     * <code>InputStream</code> returns <code>false</code>.
     *
     * @return  <code>true</code> if this true type supports the mark and reset
     *          method; <code>false</code> otherwise.
     * @see     java.io.InputStream#mark(int)
     * @see     java.io.InputStream#reset()
     */
    public boolean markSupported() {
	return true;
    }

}
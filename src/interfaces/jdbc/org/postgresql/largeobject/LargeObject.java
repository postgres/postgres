/*-------------------------------------------------------------------------
 *
 * LargeObject.java
 *     This class implements the large object interface to org.postgresql.
 *
 * Copyright (c) 2003, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/interfaces/jdbc/org/postgresql/largeobject/Attic/LargeObject.java,v 1.10 2003/03/07 18:39:45 barry Exp $
 *
 *-------------------------------------------------------------------------
 */
package org.postgresql.largeobject;

import java.io.InputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.sql.SQLException;
import org.postgresql.fastpath.Fastpath;
import org.postgresql.fastpath.FastpathArg;

/*
 * This class provides the basic methods required to run the interface, plus
 * a pair of methods that provide InputStream and OutputStream classes
 * for this object.
 *
 * <p>Normally, client code would use the getAsciiStream, getBinaryStream,
 * or getUnicodeStream methods in ResultSet, or setAsciiStream,
 * setBinaryStream, or setUnicodeStream methods in PreparedStatement to
 * access Large Objects.
 *
 * <p>However, sometimes lower level access to Large Objects are required,
 * that are not supported by the JDBC specification.
 *
 * <p>Refer to org.postgresql.largeobject.LargeObjectManager on how to gain access
 * to a Large Object, or how to create one.
 *
 * @see org.postgresql.largeobject.LargeObjectManager
 * @see java.sql.ResultSet#getAsciiStream
 * @see java.sql.ResultSet#getBinaryStream
 * @see java.sql.ResultSet#getUnicodeStream
 * @see java.sql.PreparedStatement#setAsciiStream
 * @see java.sql.PreparedStatement#setBinaryStream
 * @see java.sql.PreparedStatement#setUnicodeStream
 *
 */
public class LargeObject
{
	/*
	 * Indicates a seek from the begining of a file
	 */
	public static final int SEEK_SET = 0;

	/*
	 * Indicates a seek from the current position
	 */
	public static final int SEEK_CUR = 1;

	/*
	 * Indicates a seek from the end of a file
	 */
	public static final int SEEK_END = 2;

	private Fastpath	fp; // Fastpath API to use
	private int oid;	// OID of this object
	private int fd; // the descriptor of the open large object

	private BlobOutputStream os;  // The current output stream

	private boolean closed = false; // true when we are closed

	/*
	 * This opens a large object.
	 *
	 * <p>If the object does not exist, then an SQLException is thrown.
	 *
	 * @param fp FastPath API for the connection to use
	 * @param oid of the Large Object to open
	 * @param mode Mode of opening the large object
	 * (defined in LargeObjectManager)
	 * @exception SQLException if a database-access error occurs.
	 * @see org.postgresql.largeobject.LargeObjectManager
	 */
	protected LargeObject(Fastpath fp, int oid, int mode) throws SQLException
	{
		this.fp = fp;
		this.oid = oid;

		FastpathArg args[] = new FastpathArg[2];
		args[0] = new FastpathArg(oid);
		args[1] = new FastpathArg(mode);
		this.fd = fp.getInteger("lo_open", args);
	}

	/* Release large object resources during garbage cleanup */
	protected void finalize() throws SQLException
	{
		//This code used to call close() however that was problematic
		//because the scope of the fd is a transaction, thus if commit
		//or rollback was called before garbage collection ran then
		//the call to close would error out with an invalid large object
		//handle.  So this method now does nothing and lets the server
		//handle cleanup when it ends the transaction.
	}

	/*
	 * @return the OID of this LargeObject
	 */
	public int getOID()
	{
		return oid;
	}

	/*
	 * This method closes the object. You must not call methods in this
	 * object after this is called.
	 * @exception SQLException if a database-access error occurs.
	 */
	public void close() throws SQLException
	{
		if (!closed)
		{
			// flush any open output streams
			if (os != null)
			{
				try
				{
					// we can't call os.close() otherwise we go into an infinite loop!
					os.flush();
				}
				catch (IOException ioe)
				{
					throw new SQLException(ioe.getMessage());
				}
				finally
				{
					os = null;
				}
			}

			// finally close
			FastpathArg args[] = new FastpathArg[1];
			args[0] = new FastpathArg(fd);
			fp.fastpath("lo_close", false, args); // true here as we dont care!!
			closed = true;
		}
	}

	/*
	 * Reads some data from the object, and return as a byte[] array
	 *
	 * @param len number of bytes to read
	 * @return byte[] array containing data read
	 * @exception SQLException if a database-access error occurs.
	 */
	public byte[] read(int len) throws SQLException
	{
		// This is the original method, where the entire block (len bytes)
		// is retrieved in one go.
		FastpathArg args[] = new FastpathArg[2];
		args[0] = new FastpathArg(fd);
		args[1] = new FastpathArg(len);
		return fp.getData("loread", args);

		// This version allows us to break this down into 4k blocks
		//if (len<=4048) {
		//// handle as before, return the whole block in one go
		//FastpathArg args[] = new FastpathArg[2];
		//args[0] = new FastpathArg(fd);
		//args[1] = new FastpathArg(len);
		//return fp.getData("loread",args);
		//} else {
		//// return in 4k blocks
		//byte[] buf=new byte[len];
		//int off=0;
		//while (len>0) {
		//int bs=4048;
		//len-=bs;
		//if (len<0) {
		//bs+=len;
		//len=0;
		//}
		//read(buf,off,bs);
		//off+=bs;
		//}
		//return buf;
		//}
	}

	/*
	 * Reads some data from the object into an existing array
	 *
	 * @param buf destination array
	 * @param off offset within array
	 * @param len number of bytes to read
	 * @return the number of bytes actually read
	 * @exception SQLException if a database-access error occurs.
	 */
	public int read(byte buf[], int off, int len) throws SQLException
	{
		byte b[] = read(len);
		if (b.length < len)
			len = b.length;
		System.arraycopy(b, 0, buf, off, len);
		return len;
	}

	/*
	 * Writes an array to the object
	 *
	 * @param buf array to write
	 * @exception SQLException if a database-access error occurs.
	 */
	public void write(byte buf[]) throws SQLException
	{
		FastpathArg args[] = new FastpathArg[2];
		args[0] = new FastpathArg(fd);
		args[1] = new FastpathArg(buf);
		fp.fastpath("lowrite", false, args);
	}

	/*
	 * Writes some data from an array to the object
	 *
	 * @param buf destination array
	 * @param off offset within array
	 * @param len number of bytes to write
	 * @exception SQLException if a database-access error occurs.
	 */
	public void write(byte buf[], int off, int len) throws SQLException
	{
		byte data[] = new byte[len];
		System.arraycopy(buf, off, data, 0, len);
		write(data);
	}

	/*
	 * Sets the current position within the object.
	 *
	 * <p>This is similar to the fseek() call in the standard C library. It
	 * allows you to have random access to the large object.
	 *
	 * @param pos position within object
	 * @param ref Either SEEK_SET, SEEK_CUR or SEEK_END
	 * @exception SQLException if a database-access error occurs.
	 */
	public void seek(int pos, int ref) throws SQLException
	{
		FastpathArg args[] = new FastpathArg[3];
		args[0] = new FastpathArg(fd);
		args[1] = new FastpathArg(pos);
		args[2] = new FastpathArg(ref);
		fp.fastpath("lo_lseek", false, args);
	}

	/*
	 * Sets the current position within the object.
	 *
	 * <p>This is similar to the fseek() call in the standard C library. It
	 * allows you to have random access to the large object.
	 *
	 * @param pos position within object from begining
	 * @exception SQLException if a database-access error occurs.
	 */
	public void seek(int pos) throws SQLException
	{
		seek(pos, SEEK_SET);
	}

	/*
	 * @return the current position within the object
	 * @exception SQLException if a database-access error occurs.
	 */
	public int tell() throws SQLException
	{
		FastpathArg args[] = new FastpathArg[1];
		args[0] = new FastpathArg(fd);
		return fp.getInteger("lo_tell", args);
	}

	/*
	 * This method is inefficient, as the only way to find out the size of
	 * the object is to seek to the end, record the current position, then
	 * return to the original position.
	 *
	 * <p>A better method will be found in the future.
	 *
	 * @return the size of the large object
	 * @exception SQLException if a database-access error occurs.
	 */
	public int size() throws SQLException
	{
		int cp = tell();
		seek(0, SEEK_END);
		int sz = tell();
		seek(cp, SEEK_SET);
		return sz;
	}

	/*
	 * Returns an InputStream from this object.
	 *
	 * <p>This InputStream can then be used in any method that requires an
	 * InputStream.
	 *
	 * @exception SQLException if a database-access error occurs.
	 */
	public InputStream getInputStream() throws SQLException
	{
		return new BlobInputStream(this, 4096);
	}

	/*
	 * Returns an OutputStream to this object
	 *
	 * <p>This OutputStream can then be used in any method that requires an
	 * OutputStream.
	 *
	 * @exception SQLException if a database-access error occurs.
	 */
	public OutputStream getOutputStream() throws SQLException
	{
		if (os == null)
			os = new BlobOutputStream(this, 4096);
		return os;
	}

}

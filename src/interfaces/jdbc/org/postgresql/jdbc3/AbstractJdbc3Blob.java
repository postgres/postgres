package org.postgresql.jdbc3;


import java.sql.*;

public abstract class AbstractJdbc3Blob extends org.postgresql.jdbc2.AbstractJdbc2Blob
{

	public AbstractJdbc3Blob(org.postgresql.PGConnection conn, int oid) throws SQLException
	{
		super(conn, oid);
	}

	/**
	 * Writes the given array of bytes to the <code>BLOB</code> value that
	 * this <code>Blob</code> object represents, starting at position
	 * <code>pos</code>, and returns the number of bytes written.
	 *
	 * @param pos the position in the <code>BLOB</code> object at which
	 *		  to start writing
	 * @param bytes the array of bytes to be written to the <code>BLOB</code>
	 *		  value that this <code>Blob</code> object represents
	 * @return the number of bytes written
	 * @exception SQLException if there is an error accessing the
	 *			  <code>BLOB</code> value
	 * @see #getBytes
	 * @since 1.4
	 */
	public int setBytes(long pos, byte[] bytes) throws SQLException
	{
		throw org.postgresql.Driver.notImplemented();
	}

	/**
	 * Writes all or part of the given <code>byte</code> array to the
	 * <code>BLOB</code> value that this <code>Blob</code> object represents
	 * and returns the number of bytes written.
	 * Writing starts at position <code>pos</code> in the <code>BLOB</code>
	 * value; <code>len</code> bytes from the given byte array are written.
	 *
	 * @param pos the position in the <code>BLOB</code> object at which
	 *		  to start writing
	 * @param bytes the array of bytes to be written to this <code>BLOB</code>
	 *		  object
	 * @param offset the offset into the array <code>bytes</code> at which
	 *		  to start reading the bytes to be set
	 * @param len the number of bytes to be written to the <code>BLOB</code>
	 *		  value from the array of bytes <code>bytes</code>
	 * @return the number of bytes written
	 * @exception SQLException if there is an error accessing the
	 *			  <code>BLOB</code> value
	 * @see #getBytes
	 * @since 1.4
	 */
	public int setBytes(long pos, byte[] bytes, int offset, int len) throws SQLException
	{
		throw org.postgresql.Driver.notImplemented();
	}

	/**
	 * Retrieves a stream that can be used to write to the <code>BLOB</code>
	 * value that this <code>Blob</code> object represents.  The stream begins
	 * at position <code>pos</code>.
	 *
	 * @param pos the position in the <code>BLOB</code> value at which
	 *		  to start writing
	 * @return a <code>java.io.OutputStream</code> object to which data can
	 *		   be written
	 * @exception SQLException if there is an error accessing the
	 *			  <code>BLOB</code> value
	 * @see #getBinaryStream
	 * @since 1.4
	 */
	public java.io.OutputStream setBinaryStream(long pos) throws SQLException
	{
		throw org.postgresql.Driver.notImplemented();
	}

	/**
	 * Truncates the <code>BLOB</code> value that this <code>Blob</code>
	 * object represents to be <code>len</code> bytes in length.
	 *
	 * @param len the length, in bytes, to which the <code>BLOB</code> value
	 *		  that this <code>Blob</code> object represents should be truncated
	 * @exception SQLException if there is an error accessing the
	 *			  <code>BLOB</code> value
	 * @since 1.4
	 */
	public void truncate(long len) throws SQLException
	{
		throw org.postgresql.Driver.notImplemented();
	}

}

package org.postgresql.jdbc3;


import java.sql.*;

public abstract class AbstractJdbc3Clob extends org.postgresql.jdbc2.AbstractJdbc2Clob
{

	public AbstractJdbc3Clob(org.postgresql.PGConnection conn, int oid) throws SQLException
	{
		super(conn, oid);
	}

	/**
	 * Writes the given Java <code>String</code> to the <code>CLOB</code>
	 * value that this <code>Clob</code> object designates at the position
	 * <code>pos</code>.
	 *
	 * @param pos the position at which to start writing to the <code>CLOB</code>
	 *		   value that this <code>Clob</code> object represents
	 * @param str the string to be written to the <code>CLOB</code>
	 *		  value that this <code>Clob</code> designates
	 * @return the number of characters written
	 * @exception SQLException if there is an error accessing the
	 *			  <code>CLOB</code> value
	 *
	 * @since 1.4
	 */
	public int setString(long pos, String str) throws SQLException
	{
		throw org.postgresql.Driver.notImplemented();
	}

	/**
	 * Writes <code>len</code> characters of <code>str</code>, starting
	 * at character <code>offset</code>, to the <code>CLOB</code> value
	 * that this <code>Clob</code> represents.
	 *
	 * @param pos the position at which to start writing to this
	 *		  <code>CLOB</code> object
	 * @param str the string to be written to the <code>CLOB</code>
	 *		  value that this <code>Clob</code> object represents
	 * @param offset the offset into <code>str</code> to start reading
	 *		  the characters to be written
	 * @param len the number of characters to be written
	 * @return the number of characters written
	 * @exception SQLException if there is an error accessing the
	 *			  <code>CLOB</code> value
	 *
	 * @since 1.4
	 */
	public int setString(long pos, String str, int offset, int len) throws SQLException
	{
		throw org.postgresql.Driver.notImplemented();
	}

	/**
	 * Retrieves a stream to be used to write Ascii characters to the
	 * <code>CLOB</code> value that this <code>Clob</code> object represents,
	 * starting at position <code>pos</code>.
	 *
	 * @param pos the position at which to start writing to this
	 *		  <code>CLOB</code> object
	 * @return the stream to which ASCII encoded characters can be written
	 * @exception SQLException if there is an error accessing the
	 *			  <code>CLOB</code> value
	 * @see #getAsciiStream
	 *
	 * @since 1.4
	 */
	public java.io.OutputStream setAsciiStream(long pos) throws SQLException
	{
		throw org.postgresql.Driver.notImplemented();
	}

	/**
	 * Retrieves a stream to be used to write a stream of Unicode characters
	 * to the <code>CLOB</code> value that this <code>Clob</code> object
	 * represents, at position <code>pos</code>.
	 *
	 * @param  pos the position at which to start writing to the
	 *		  <code>CLOB</code> value
	 *
	 * @return a stream to which Unicode encoded characters can be written
	 * @exception SQLException if there is an error accessing the
	 *			  <code>CLOB</code> value
	 * @see #getCharacterStream
	 *
	 * @since 1.4
	 */
	public java.io.Writer setCharacterStream(long pos) throws SQLException
	{
		throw org.postgresql.Driver.notImplemented();
	}

	/**
	 * Truncates the <code>CLOB</code> value that this <code>Clob</code>
	 * designates to have a length of <code>len</code>
	 * characters.
	 * @param len the length, in bytes, to which the <code>CLOB</code> value
	 *		  should be truncated
	 * @exception SQLException if there is an error accessing the
	 *			  <code>CLOB</code> value
	 *
	 * @since 1.4
	 */
	public void truncate(long len) throws SQLException
	{
		throw org.postgresql.Driver.notImplemented();
	}

}

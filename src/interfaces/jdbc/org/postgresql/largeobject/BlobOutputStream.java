/*-------------------------------------------------------------------------
 *
 * BlobOutputStream.java
 *     This implements a basic output stream that writes to a LargeObject
 *
 * Copyright (c) 2003, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/interfaces/jdbc/org/postgresql/largeobject/Attic/BlobOutputStream.java,v 1.6 2003/03/07 18:39:45 barry Exp $
 *
 *-------------------------------------------------------------------------
 */
package org.postgresql.largeobject;

import java.io.IOException;
import java.io.OutputStream;
import java.sql.SQLException;

public class BlobOutputStream extends OutputStream
{
	/*
	 * The parent LargeObject
	 */
	private LargeObject lo;

	/*
	 * Buffer
	 */
	private byte buf[];

	/*
	 * Size of the buffer (default 1K)
	 */
	private int bsize;

	/*
	 * Position within the buffer
	 */
	private int bpos;

	/*
	 * Create an OutputStream to a large object
	 * @param lo LargeObject
	 */
	public BlobOutputStream(LargeObject lo)
	{
		this(lo, 1024);
	}

	/*
	 * Create an OutputStream to a large object
	 * @param lo LargeObject
	 * @param bsize The size of the buffer used to improve performance
	 */
	public BlobOutputStream(LargeObject lo, int bsize)
	{
		this.lo = lo;
		this.bsize = bsize;
		buf = new byte[bsize];
		bpos = 0;
	}

	public void write(int b) throws java.io.IOException
	{
		try
		{
			if (bpos >= bsize)
			{
				lo.write(buf);
				bpos = 0;
			}
			buf[bpos++] = (byte)b;
		}
		catch (SQLException se)
		{
			throw new IOException(se.toString());
		}
	}

	public void write(byte[] buf, int off, int len) throws java.io.IOException
	{
		try
			{
				// If we have any internally buffered data, send it first
				if ( bpos > 0 )
					flush();

				if ( off == 0 && len == buf.length )
					lo.write(buf); // save a buffer creation and copy since full buffer written
				else
					lo.write(buf,off,len);
			}
		catch (SQLException se)
			{
				throw new IOException(se.toString());
			}
	}


	/*
	 * Flushes this output stream and forces any buffered output bytes
	 * to be written out. The general contract of <code>flush</code> is
	 * that calling it is an indication that, if any bytes previously
	 * written have been buffered by the implementation of the output
	 * stream, such bytes should immediately be written to their
	 * intended destination.
	 *
	 * @exception  IOException	if an I/O error occurs.
	 */
	public void flush() throws IOException
	{
		try
		{
			if (bpos > 0)
				lo.write(buf, 0, bpos);
			bpos = 0;
		}
		catch (SQLException se)
		{
			throw new IOException(se.toString());
		}
	}

	/*
	 * Closes this output stream and releases any system resources
	 * associated with this stream. The general contract of <code>close</code>
	 * is that it closes the output stream. A closed stream cannot perform
	 * output operations and cannot be reopened.
	 * <p>
	 * The <code>close</code> method of <code>OutputStream</code> does nothing.
	 *
	 * @exception  IOException	if an I/O error occurs.
	 */
	public void close() throws IOException
	{
		try
		{
			flush();
			lo.close();
			lo = null;
		}
		catch (SQLException se)
		{
			throw new IOException(se.toString());
		}
	}

}

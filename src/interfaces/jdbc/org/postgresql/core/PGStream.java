/*-------------------------------------------------------------------------
 *
 * PGStream.java
 *      This class is used by Connection for communicating with the
 *      backend.
 *
 * Copyright (c) 2003, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/interfaces/jdbc/org/postgresql/core/Attic/PGStream.java,v 1.3.2.1 2004/03/29 17:47:47 barry Exp $
 *
 *-------------------------------------------------------------------------
 */
package org.postgresql.core;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.InputStream;
import java.io.IOException;
import java.net.Socket;
import java.sql.*;
import org.postgresql.util.PSQLException;
import org.postgresql.util.PSQLState;


public class PGStream
{
	public String host;
	public int port;
	public Socket connection;
	public InputStream pg_input;
	public BufferedOutputStream pg_output;
	private byte[] byte_buf = new byte[8*1024];

	/*
	 * Constructor:  Connect to the PostgreSQL back end and return
	 * a stream connection.
	 *
	 * @param host the hostname to connect to
	 * @param port the port number that the postmaster is sitting on
	 * @exception IOException if an IOException occurs below it.
	 */
	public PGStream(String p_host, int p_port) throws IOException
	{
		host = p_host;
		port = p_port;
		connection = new Socket(host, port);

		// Submitted by Jason Venner <jason@idiom.com> adds a 10x speed
		// improvement on FreeBSD machines (caused by a bug in their TCP Stack)
		connection.setTcpNoDelay(true);

		// Buffer sizes submitted by Sverre H Huseby <sverrehu@online.no>
		pg_input = new BufferedInputStream(connection.getInputStream(), 8192);
		pg_output = new BufferedOutputStream(connection.getOutputStream(), 8192);
	}

	/*
	 * Sends a single character to the back end
	 *
	 * @param val the character to be sent
	 * @exception IOException if an I/O error occurs
	 */
	public void SendChar(int val) throws IOException
	{
		pg_output.write((byte)val);
	}

	/*
	 * Sends an integer to the back end
	 *
	 * @param val the integer to be sent
	 * @param siz the length of the integer in bytes (size of structure)
	 * @exception IOException if an I/O error occurs
	 */
	public void SendInteger(int val, int siz) throws IOException
	{
		byte[] buf = new byte[siz];

		while (siz-- > 0)
		{
			buf[siz] = (byte)(val & 0xff);
			val >>= 8;
		}
		Send(buf);
	}

	/*
	 * Sends an integer to the back end
	 *
	 * @param val the integer to be sent
	 * @param siz the length of the integer in bytes (size of structure)
	 * @exception IOException if an I/O error occurs
	 */
	public void SendIntegerR(int val, int siz) throws IOException
	{
		byte[] buf = new byte[siz];

		for (int i = 0; i < siz; i++)
		{
			buf[i] = (byte)(val & 0xff);
			val >>= 8;
		}
		Send(buf);
	}

	/*
	 * Send an array of bytes to the backend
	 *
	 * @param buf The array of bytes to be sent
	 * @exception IOException if an I/O error occurs
	 */
	public void Send(byte buf[]) throws IOException
	{
		pg_output.write(buf);
	}

	/*
	 * Send an exact array of bytes to the backend - if the length
	 * has not been reached, send nulls until it has.
	 *
	 * @param buf the array of bytes to be sent
	 * @param siz the number of bytes to be sent
	 * @exception IOException if an I/O error occurs
	 */
	public void Send(byte buf[], int siz) throws IOException
	{
		Send(buf, 0, siz);
	}

	/*
	 * Send an exact array of bytes to the backend - if the length
	 * has not been reached, send nulls until it has.
	 *
	 * @param buf the array of bytes to be sent
	 * @param off offset in the array to start sending from
	 * @param siz the number of bytes to be sent
	 * @exception IOException if an I/O error occurs
	 */
	public void Send(byte buf[], int off, int siz) throws IOException
	{
		int i;

		pg_output.write(buf, off, ((buf.length - off) < siz ? (buf.length - off) : siz));
		if ((buf.length - off) < siz)
		{
			for (i = buf.length - off ; i < siz ; ++i)
			{
				pg_output.write(0);
			}
		}
	}

	/*
	 * Receives a single character from the backend
	 *
	 * @return the character received
	 * @exception SQLException if an I/O Error returns
	 */
	public int ReceiveChar() throws SQLException
	{
		int c = 0;

		try
		{
			c = pg_input.read();
			if (c < 0)
				throw new PSQLException("postgresql.stream.eof", PSQLState.COMMUNICATION_ERROR);
		}
		catch (IOException e)
		{
			throw new PSQLException("postgresql.stream.ioerror", PSQLState.COMMUNICATION_ERROR, e);
		}
		return c;
	}

	/*
	 * Receives an integer from the backend
	 *
	 * @param siz length of the integer in bytes
	 * @return the integer received from the backend
	 * @exception SQLException if an I/O error occurs
	 */
	public int ReceiveInteger(int siz) throws SQLException
	{
		int n = 0;

		try
		{
			for (int i = 0 ; i < siz ; i++)
			{
				int b = pg_input.read();

				if (b < 0)
					throw new PSQLException("postgresql.stream.eof", PSQLState.COMMUNICATION_ERROR);
				n = n | (b << (8 * i)) ;
			}
		}
		catch (IOException e)
		{
			throw new PSQLException("postgresql.stream.ioerror", PSQLState.COMMUNICATION_ERROR, e);
		}
		return n;
	}

	/*
	 * Receives an integer from the backend
	 *
	 * @param siz length of the integer in bytes
	 * @return the integer received from the backend
	 * @exception SQLException if an I/O error occurs
	 */
	public int ReceiveIntegerR(int siz) throws SQLException
	{
		int n = 0;

		try
		{
			for (int i = 0 ; i < siz ; i++)
			{
				int b = pg_input.read();

				if (b < 0)
					throw new PSQLException("postgresql.stream.eof", PSQLState.COMMUNICATION_ERROR);
				n = b | (n << 8);
			}
		}
		catch (IOException e)
		{
			throw new PSQLException("postgresql.stream.ioerror", PSQLState.COMMUNICATION_ERROR, e);
		}
		return n;
	}

	/*
	 * Receives a null-terminated string from the backend.	If we don't see a
	 * null, then we assume something has gone wrong.
	 *
	 * @param encoding the charset encoding to use.
	 * @return string from back end
	 * @exception SQLException if an I/O error occurs, or end of file
	 */
	public String ReceiveString(Encoding encoding)
	throws SQLException
	{
		int s = 0;
		byte[] rst = byte_buf;
		try
		{
			int buflen = rst.length;
			boolean done = false;
			while (!done)
			{
				while (s < buflen)
				{
					int c = pg_input.read();
					if (c < 0)
						throw new PSQLException("postgresql.stream.eof", PSQLState.COMMUNICATION_ERROR);
					else if (c == 0)
					{
						rst[s] = 0;
						done = true;
						break;
					}
					else
					{
						rst[s++] = (byte)c;
					}
					if (s >= buflen)
					{ // Grow the buffer
						buflen = (int)(buflen * 2); // 100% bigger
						byte[] newrst = new byte[buflen];
						System.arraycopy(rst, 0, newrst, 0, s);
						rst = newrst;
					}
				}
			}
		}
		catch (IOException e)
		{
			throw new PSQLException("postgresql.stream.ioerror", PSQLState.COMMUNICATION_ERROR, e);
		}
		return encoding.decode(rst, 0, s);
	}

	/*
	 * Read a tuple from the back end.	A tuple is a two dimensional
	 * array of bytes
	 *
	 * @param nf the number of fields expected
	 * @return null if the current response has no more tuples, otherwise
	 *	an array of strings
	 * @exception SQLException if a data I/O error occurs
	 */
	public byte[][] ReceiveTupleV3(int nf) throws SQLException
	{
		//TODO: use l_msgSize
		int l_msgSize = ReceiveIntegerR(4);
		int i;
		int l_nf = ReceiveIntegerR(2);
		byte[][] answer = new byte[l_nf][0];
		
		for (i = 0 ; i < l_nf ; ++i)
		{
			int l_size = ReceiveIntegerR(4);
			boolean isNull = l_size == -1;
			if (isNull)
				answer[i] = null;
			else
			{
				answer[i] = Receive(l_size);
			}
		}
		return answer;
	}

	/*
	 * Read a tuple from the back end.	A tuple is a two dimensional
	 * array of bytes
	 *
	 * @param nf the number of fields expected
	 * @param bin true if the tuple is a binary tuple
	 * @return null if the current response has no more tuples, otherwise
	 *	an array of strings
	 * @exception SQLException if a data I/O error occurs
	 */
	public byte[][] ReceiveTupleV2(int nf, boolean bin) throws SQLException
	{
		int i, bim = (nf + 7) / 8;
		byte[] bitmask = Receive(bim);
		byte[][] answer = new byte[nf][0];

		int whichbit = 0x80;
		int whichbyte = 0;

		for (i = 0 ; i < nf ; ++i)
		{
			boolean isNull = ((bitmask[whichbyte] & whichbit) == 0);
			whichbit >>= 1;
			if (whichbit == 0)
			{
				++whichbyte;
				whichbit = 0x80;
			}
			if (isNull)
				answer[i] = null;
			else
			{
				int len = ReceiveIntegerR(4);
				if (!bin)
					len -= 4;
				if (len < 0)
					len = 0;
				answer[i] = Receive(len);
			}
		}
		return answer;
	}

	/*
	 * Reads in a given number of bytes from the backend
	 *
	 * @param siz number of bytes to read
	 * @return array of bytes received
	 * @exception SQLException if a data I/O error occurs
	 */
	public byte[] Receive(int siz) throws SQLException
	{
		byte[] answer = new byte[siz];
		Receive(answer, 0, siz);
		return answer;
	}

	/*
	 * Reads in a given number of bytes from the backend
	 *
	 * @param buf buffer to store result
	 * @param off offset in buffer
	 * @param siz number of bytes to read
	 * @exception SQLException if a data I/O error occurs
	 */
	public void Receive(byte[] b, int off, int siz) throws SQLException
	{
		int s = 0;

		try
		{
			while (s < siz)
			{
				int w = pg_input.read(b, off + s, siz - s);
				if (w < 0)
					throw new PSQLException("postgresql.stream.eof", PSQLState.COMMUNICATION_ERROR);
				s += w;
			}
		}
		catch (IOException e)
		{
			throw new PSQLException("postgresql.stream.ioerror", PSQLState.COMMUNICATION_ERROR, e);
		}
	}

	/*
	 * This flushes any pending output to the backend. It is used primarily
	 * by the Fastpath code.
	 * @exception SQLException if an I/O error occurs
	 */
	public void flush() throws SQLException
	{
		try
		{
			pg_output.flush();
		}
		catch (IOException e)
		{
			throw new PSQLException("postgresql.stream.flush", PSQLState.COMMUNICATION_ERROR, e);
		}
	}

	/*
	 * Closes the connection
	 *
	 * @exception IOException if a IO Error occurs
	 */
	public void close() throws IOException
	{
		pg_output.close();
		pg_input.close();
		connection.close();
	}
}

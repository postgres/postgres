/*-------------------------------------------------------------------------
 *
 * PGbytea.java
 *     Converts to and from the postgresql bytea datatype used by the backend.
 *
 * Copyright (c) 2003, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/interfaces/jdbc/org/postgresql/util/Attic/PGbytea.java,v 1.8 2003/05/29 04:48:33 barry Exp $
 *
 *-------------------------------------------------------------------------
 */
package org.postgresql.util;

import java.sql.*;

public class PGbytea
{

	/*
	 * Converts a PG bytea raw value (i.e. the raw binary representation
	 * of the bytea data type) into a java byte[]
	 */
	public static byte[] toBytes(byte[] s) throws SQLException
	{
		if (s == null)
			return null;
		int slength = s.length;
		byte[] buf = new byte[slength];
		int bufpos = 0;
		int thebyte;
		byte nextbyte;
		byte secondbyte;
		for (int i = 0; i < slength; i++)
		{
			nextbyte = s[i];
			if (nextbyte == (byte)'\\')
			{
				secondbyte = s[++i];
				if (secondbyte == (byte)'\\')
				{
					//escaped \
					buf[bufpos++] = (byte)'\\';
				}
				else
				{
					thebyte = (secondbyte - 48) * 64 + (s[++i] - 48) * 8 + (s[++i] - 48);
					if (thebyte > 127)
						thebyte -= 256;
					buf[bufpos++] = (byte)thebyte;
				}
			}
			else
			{
				buf[bufpos++] = nextbyte;
			}
		}
		byte[] l_return = new byte[bufpos];
		System.arraycopy(buf, 0, l_return, 0, bufpos);
		return l_return;
	}

	/*
	 * Converts a java byte[] into a PG bytea string (i.e. the text
	 * representation of the bytea data type)
	 */
	public static String toPGString(byte[] p_buf) throws SQLException
	{
		if (p_buf == null)
			return null;
		StringBuffer l_strbuf = new StringBuffer(2 * p_buf.length);
		for (int i = 0; i < p_buf.length; i++)
		{
			int l_int = (int)p_buf[i];
			if (l_int < 0)
			{
				l_int = 256 + l_int;
			}
			//we escape the same non-printable characters as the backend
			//we must escape all 8bit characters otherwise when convering
			//from java unicode to the db character set we may end up with
			//question marks if the character set is SQL_ASCII
			if (l_int < 040 || l_int > 0176)
			{
				//escape charcter with the form \000, but need two \\ because of
				//the parser
				l_strbuf.append("\\");
				l_strbuf.append((char)(((l_int >> 6) & 0x3) + 48));
				l_strbuf.append((char)(((l_int >> 3) & 0x7) + 48));
				l_strbuf.append((char)((l_int & 0x07) + 48));
			}
			else if (p_buf[i] == (byte)'\\')
			{
				//escape the backslash character as \\, but need four \\\\ because
				//of the parser
				l_strbuf.append("\\\\");
			}
			else
			{
				//other characters are left alone
				l_strbuf.append((char)p_buf[i]);
			}
		}
		return l_strbuf.toString();
	}


}

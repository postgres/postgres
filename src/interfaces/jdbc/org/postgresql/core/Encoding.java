/*-------------------------------------------------------------------------
 *
 * Encoding.java
 *     Converts to and from the character encoding used by the backend.
 *
 * Copyright (c) 2003, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/interfaces/jdbc/org/postgresql/core/Attic/Encoding.java,v 1.12 2003/09/08 17:30:22 barry Exp $
 *
 *-------------------------------------------------------------------------
 */
package org.postgresql.core;

import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.Reader;
import java.io.UnsupportedEncodingException;
import java.sql.SQLException;
import java.util.Hashtable;
import org.postgresql.util.PSQLException;
import org.postgresql.util.PSQLState;

public class Encoding
{

	private static final Encoding DEFAULT_ENCODING = new Encoding(null);

	/*
	 * Preferred JVM encodings for backend encodings.
	 */
	private static final Hashtable encodings = new Hashtable();

	static {
		//Note: this list should match the set of supported server
		// encodings found in backend/util/mb/encnames.c
		encodings.put("SQL_ASCII", new String[] { "ASCII", "us-ascii" });
		encodings.put("UNICODE", new String[] { "UTF-8", "UTF8" });
		encodings.put("LATIN1", new String[] { "ISO8859_1" });
		encodings.put("LATIN2", new String[] { "ISO8859_2" });
		encodings.put("LATIN3", new String[] { "ISO8859_3" });
		encodings.put("LATIN4", new String[] { "ISO8859_4" });
		encodings.put("ISO_8859_5", new String[] { "ISO8859_5" });
		encodings.put("ISO_8859_6", new String[] { "ISO8859_6" });
		encodings.put("ISO_8859_7", new String[] { "ISO8859_7" });
		encodings.put("ISO_8859_8", new String[] { "ISO8859_8" });
		encodings.put("LATIN5", new String[] { "ISO8859_9" });
		encodings.put("LATIN7", new String[] { "ISO8859_13" });
		encodings.put("LATIN9", new String[] { "ISO8859_15_FDIS" });
		encodings.put("EUC_JP", new String[] { "EUC_JP" });
		encodings.put("EUC_CN", new String[] { "EUC_CN" });
		encodings.put("EUC_KR", new String[] { "EUC_KR" });
		encodings.put("JOHAB", new String[] { "Johab" });
		encodings.put("EUC_TW", new String[] { "EUC_TW" });
		encodings.put("SJIS", new String[] { "MS932", "SJIS" });
		encodings.put("BIG5", new String[] { "Big5", "MS950", "Cp950" });
		encodings.put("GBK", new String[] { "GBK", "MS936" });
		encodings.put("UHC", new String[] { "MS949", "Cp949", "Cp949C" });
		encodings.put("TCVN", new String[] { "Cp1258" });
		encodings.put("WIN1256", new String[] { "Cp1256" });
		encodings.put("WIN1250", new String[] { "Cp1250" });
		encodings.put("WIN874", new String[] { "MS874", "Cp874" });
		encodings.put("WIN", new String[] { "Cp1251" });
		encodings.put("ALT", new String[] { "Cp866" });
		// We prefer KOI8-U, since it is a superset of KOI8-R.
		encodings.put("KOI8", new String[] { "KOI8_U", "KOI8_R" });
		// If the database isn't encoding-aware then we can't have
		// any preferred encodings.
		encodings.put("UNKNOWN", new String[0]);
		// The following encodings do not have a java equivalent
		encodings.put("MULE_INTERNAL", new String[0]);
		encodings.put("LATIN6", new String[0]);
		encodings.put("LATIN8", new String[0]);
		encodings.put("LATIN10", new String[0]);
	}

	private final String encoding;

	private Encoding(String encoding)
	{
		this.encoding = encoding;
	}

	/*
	 * Get an Encoding for from the given database encoding and
	 * the encoding passed in by the user.
	 */
	public static Encoding getEncoding(String databaseEncoding,
									   String passedEncoding)
	{
		if (passedEncoding != null)
		{
			if (isAvailable(passedEncoding))
			{
				return new Encoding(passedEncoding);
			}
			else
			{
				return defaultEncoding();
			}
		}
		else
		{
			return encodingForDatabaseEncoding(databaseEncoding);
		}
	}

	/*
	 * Get an Encoding matching the given database encoding.
	 */
	private static Encoding encodingForDatabaseEncoding(String databaseEncoding)
	{
		// If the backend encoding is known and there is a suitable
		// encoding in the JVM we use that. Otherwise we fall back
		// to the default encoding of the JVM.

		if (encodings.containsKey(databaseEncoding))
		{
			String[] candidates = (String[]) encodings.get(databaseEncoding);
			for (int i = 0; i < candidates.length; i++)
			{
				if (isAvailable(candidates[i]))
				{
					return new Encoding(candidates[i]);
				}
			}
		}
		return defaultEncoding();
	}

	/*
	 * Name of the (JVM) encoding used.
	 */
	public String name()
	{
		return encoding;
	}

	/*
	 * Encode a string to an array of bytes.
	 */
	public byte[] encode(String s) throws SQLException
	{
		byte[] l_return;
		try
		{
			if (encoding == null)
			{
				l_return = s.getBytes();
			}
			else
			{
				l_return = s.getBytes(encoding);
			}
			//Don't return null, return an empty byte[] instead
			if (l_return == null) {
				return new byte[0];
			} else {
				return l_return;
			}
		}
		catch (UnsupportedEncodingException e)
		{
			throw new PSQLException("postgresql.stream.encoding", PSQLState.DATA_ERROR, e);
		}
	}

	/*
	 * Decode an array of bytes into a string.
	 */
	public String decode(byte[] encodedString, int offset, int length) throws SQLException
	{
		try
		{
			if (encoding == null)
			{
				return new String(encodedString, offset, length);
			}
			else
			{
				if (encoding.equals("UTF-8")) {
					return decodeUTF8(encodedString, offset, length);
				}
				return new String(encodedString, offset, length, encoding);
			}
		}
		catch (UnsupportedEncodingException e)
		{
			throw new PSQLException("postgresql.stream.encoding", PSQLState.DATA_ERROR, e);
		}
	}

	/*
	 * Decode an array of bytes into a string.
	 */
	public String decode(byte[] encodedString) throws SQLException
	{
		return decode(encodedString, 0, encodedString.length);
	}

	/*
	 * Get a Reader that decodes the given InputStream.
	 */
	public Reader getDecodingReader(InputStream in) throws SQLException
	{
		try
		{
			if (encoding == null)
			{
				return new InputStreamReader(in);
			}
			else
			{
				return new InputStreamReader(in, encoding);
			}
		}
		catch (UnsupportedEncodingException e)
		{
			throw new PSQLException("postgresql.res.encoding", PSQLState.DATA_ERROR, e);
		}
	}

	/*
	 * Get an Encoding using the default encoding for the JVM.
	 */
	public static Encoding defaultEncoding()
	{
		return DEFAULT_ENCODING;
	}

	/*
	 * Test if an encoding is available in the JVM.
	 */
	private static boolean isAvailable(String encodingName)
	{
		try
		{
			"DUMMY".getBytes(encodingName);
			return true;
		}
		catch (UnsupportedEncodingException e)
		{
			return false;
		}
	}

	/**
	 * custom byte[] -> String conversion routine, 3x-10x faster than
	 * standard new String(byte[])
	 */
	private static final int pow2_6 = 64;		// 26
	private static final int pow2_12 = 4096;	// 212
	private char[] cdata = new char[50];

	private synchronized String decodeUTF8(byte data[], int offset, int length) throws SQLException {
		try {
			char[] l_cdata = cdata;
			if (l_cdata.length < (length)) {
				l_cdata = new char[length];
			}
			int i = offset;
			int j = 0;
			int k = length + offset;
			int z, y, x, val;
			while (i < k) {
				z = data[i] & 0xFF;
				if (z < 0x80) {
					l_cdata[j++] = (char)data[i];
					i++;
				} else if (z >= 0xE0) {		// length == 3
					y = data[i+1] & 0xFF;
					x = data[i+2] & 0xFF;
					val = (z-0xE0)*pow2_12 + (y-0x80)*pow2_6 + (x-0x80);
					l_cdata[j++] = (char) val;
					i+= 3;
				} else {		// length == 2 (maybe add checking for length > 3, throw exception if it is
					y = data[i+1] & 0xFF;
					val = (z - 0xC0)* (pow2_6)+(y-0x80);
					l_cdata[j++] = (char) val;
					i+=2;
				} 
			}
	
			String s = new String(l_cdata, 0, j);
			return s;
		} catch (Exception l_e) {
			throw new PSQLException("postgresql.con.invalidchar", l_e);
		}
	}

}

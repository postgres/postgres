package org.postgresql.core;

import java.io.*;
import java.util.*;
import java.sql.SQLException;
import org.postgresql.util.*;

/**
 * Converts to and from the character encoding used by the backend.
 *
 * $Id: Encoding.java,v 1.1 2001/07/21 18:52:11 momjian Exp $
 */

public class Encoding {

    private static final Encoding DEFAULT_ENCODING = new Encoding(null);

    /**
     * Preferred JVM encodings for backend encodings.
     */
    private static final Hashtable encodings = new Hashtable();

    static {
        encodings.put("SQL_ASCII", new String[] { "ASCII", "us-ascii" });
        encodings.put("UNICODE", new String[] { "UTF-8", "UTF8" });
        encodings.put("LATIN1", new String[] { "ISO8859_1" });
        encodings.put("LATIN2", new String[] { "ISO8859_2" });
        encodings.put("LATIN3", new String[] { "ISO8859_3" });
        encodings.put("LATIN4", new String[] { "ISO8859_4" });
	encodings.put("LATIN5", new String[] { "ISO8859_5" });
	encodings.put("LATIN6", new String[] { "ISO8859_6" });
	encodings.put("LATIN7", new String[] { "ISO8859_7" });
	encodings.put("LATIN8", new String[] { "ISO8859_8" });
	encodings.put("LATIN9", new String[] { "ISO8859_9" });
	encodings.put("EUC_JP", new String[] { "EUC_JP" });
	encodings.put("EUC_CN", new String[] { "EUC_CN" });
	encodings.put("EUC_KR", new String[] { "EUC_KR" });
	encodings.put("EUC_TW", new String[] { "EUC_TW" });
        encodings.put("WIN", new String[] { "Cp1252" });
	// We prefer KOI8-U, since it is a superset of KOI8-R.
	encodings.put("KOI8", new String[] { "KOI8_U", "KOI8_R" });
	// If the database isn't encoding-aware then we can't have
	// any preferred encodings.
        encodings.put("UNKNOWN", new String[0]);
    }

    private final String encoding;

    private Encoding(String encoding) {
	this.encoding = encoding;
    }

    /**
     * Get an Encoding for from the given database encoding and
     * the encoding passed in by the user.
     */
    public static Encoding getEncoding(String databaseEncoding,
				       String passedEncoding)
    {
	if (passedEncoding != null) {
	    if (isAvailable(passedEncoding)) {
		return new Encoding(passedEncoding);
	    } else {
		return defaultEncoding();
	    }
	} else {
	    return encodingForDatabaseEncoding(databaseEncoding);
	}
    }

    /**
     * Get an Encoding matching the given database encoding.
     */
    private static Encoding encodingForDatabaseEncoding(String databaseEncoding) {
	// If the backend encoding is known and there is a suitable
	// encoding in the JVM we use that. Otherwise we fall back
	// to the default encoding of the JVM.

	if (encodings.containsKey(databaseEncoding)) {
	    String[] candidates = (String[]) encodings.get(databaseEncoding);
	    for (int i = 0; i < candidates.length; i++) {
		if (isAvailable(candidates[i])) {
		    return new Encoding(candidates[i]);
		}
	    }
	}
	return defaultEncoding();
    }

    /**
     * Name of the (JVM) encoding used.
     */
    public String name() {
	return encoding;
    }

    /**
     * Encode a string to an array of bytes.
     */
    public byte[] encode(String s) throws SQLException {
	try {
	    if (encoding == null) {
		return s.getBytes();
	    } else {
		return s.getBytes(encoding);
	    }
	} catch (UnsupportedEncodingException e) {
	    throw new PSQLException("postgresql.stream.encoding", e);
	}
    }

    /**
     * Decode an array of bytes into a string.
     */
    public String decode(byte[] encodedString, int offset, int length) throws SQLException {
	try {
	    if (encoding == null) {
		return new String(encodedString, offset, length);
	    } else {
		return new String(encodedString, offset, length, encoding);
	    }
	} catch (UnsupportedEncodingException e) {
	    throw new PSQLException("postgresql.stream.encoding", e);
	}
    }

    /**
     * Decode an array of bytes into a string.
     */
    public String decode(byte[] encodedString) throws SQLException {
	return decode(encodedString, 0, encodedString.length);
    }

    /**
     * Get a Reader that decodes the given InputStream.
     */
    public Reader getDecodingReader(InputStream in) throws SQLException {
	try {
	    if (encoding == null) {
		return new InputStreamReader(in);
	    } else {
		return new InputStreamReader(in, encoding);
	    }
	} catch (UnsupportedEncodingException e) {
	    throw new PSQLException("postgresql.res.encoding", e);
	}
    }

    /**
     * Get an Encoding using the default encoding for the JVM.
     */
    public static Encoding defaultEncoding() {
	return DEFAULT_ENCODING;
    }

    /**
     * Test if an encoding is available in the JVM.
     */
    private static boolean isAvailable(String encodingName) {
	try {
	    "DUMMY".getBytes(encodingName);
	    return true;
	} catch (UnsupportedEncodingException e) {
	    return false;
	}
    }
}


package org.postgresql.test.jdbc2;

import junit.framework.*;
import org.postgresql.core.Encoding;
import java.io.*;

/*
 * Tests for the Encoding class.
 *
 * $Id: EncodingTest.java,v 1.4 2001/11/19 22:33:39 momjian Exp $
 */


public class EncodingTest extends TestCase
{

	public EncodingTest(String name)
	{
		super(name);
	}

	public void testCreation() throws Exception
	{
		Encoding encoding;
		encoding = Encoding.getEncoding("UNICODE", null);
		assertEquals("UTF", encoding.name().substring(0, 3).toUpperCase());
		encoding = Encoding.getEncoding("SQL_ASCII", null);
		assertTrue(encoding.name().toUpperCase().indexOf("ASCII") != -1);
		assertEquals("When encoding is unknown the default encoding should be used",
					 Encoding.defaultEncoding(),
					 Encoding.getEncoding("UNKNOWN", null));
		encoding = Encoding.getEncoding("SQL_ASCII", "utf-8");
		assertTrue("Encoding passed in by the user should be preferred",
				   encoding.name().toUpperCase().indexOf("UTF") != -1);
	}

	public void testTransformations() throws Exception
	{
		Encoding encoding = Encoding.getEncoding("UNICODE", null);
		assertEquals("ab", encoding.decode(new byte[] { 97, 98 }));

		assertEquals(2, encoding.encode("ab").length);
		assertEquals(97, encoding.encode("a")[0]);
		assertEquals(98, encoding.encode("b")[0]);

		encoding = Encoding.defaultEncoding();
		assertEquals("a".getBytes()[0], encoding.encode("a")[0]);
		assertEquals(new String(new byte[] { 97 }),
					 encoding.decode(new byte[] { 97 }));
	}

	public void testReader() throws Exception
	{
		Encoding encoding = Encoding.getEncoding("SQL_ASCII", null);
		InputStream stream = new ByteArrayInputStream(new byte[] { 97, 98 });
		Reader reader = encoding.getDecodingReader(stream);
		assertEquals(97, reader.read());
		assertEquals(98, reader.read());
		assertEquals( -1, reader.read());
	}
}

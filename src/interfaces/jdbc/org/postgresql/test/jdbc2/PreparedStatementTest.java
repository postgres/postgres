package org.postgresql.test.jdbc2;

import org.postgresql.test.TestUtil;
import junit.framework.TestCase;
import java.io.*;
import java.sql.*;


public class PreparedStatementTest extends TestCase
{

	private Connection conn;

	public PreparedStatementTest(String name)
	{
		super(name);
	}

	protected void setUp() throws SQLException
	{
		conn = TestUtil.openDB();
		TestUtil.createTable(conn, "streamtable", "bin bytea, str text");
	}

	protected void tearDown() throws SQLException
	{
		TestUtil.dropTable(conn, "streamtable");
		TestUtil.closeDB(conn);
	}

	public void testSetBinaryStream() throws SQLException
	{
		ByteArrayInputStream bais;
		byte buf[] = new byte[10];
		for (int i=0; i<buf.length; i++) {
			buf[i] = (byte)i;
		}

		bais = null;
		doSetBinaryStream(bais,0);

		bais = new ByteArrayInputStream(new byte[0]);
		doSetBinaryStream(bais,100);

		bais = new ByteArrayInputStream(buf);
		doSetBinaryStream(bais,0);

		bais = new ByteArrayInputStream(buf);
		doSetBinaryStream(bais,10);

		bais = new ByteArrayInputStream(buf);
		doSetBinaryStream(bais,100);
	}

	public void testSetAsciiStream() throws Exception
	{
		ByteArrayOutputStream baos = new ByteArrayOutputStream();
		PrintWriter pw = new PrintWriter(new OutputStreamWriter(baos,"ASCII"));
		pw.println("Hello");
		pw.flush();
		
		ByteArrayInputStream bais;
		
		bais = new ByteArrayInputStream(baos.toByteArray());
		doSetAsciiStream(bais, 0);

		bais = new ByteArrayInputStream(baos.toByteArray());
		doSetAsciiStream(bais, 6);

		bais = new ByteArrayInputStream(baos.toByteArray());
		doSetAsciiStream(bais, 100);
	}

	private void doSetBinaryStream(ByteArrayInputStream bais, int length) throws SQLException
	{
		PreparedStatement pstmt = conn.prepareStatement("INSERT INTO streamtable (bin,str) VALUES (?,?)");
		pstmt.setBinaryStream(1,bais, length);
		pstmt.setString(2,null);
		pstmt.executeUpdate();
		pstmt.close();
	}

	private void doSetAsciiStream(InputStream is, int length) throws SQLException
	{
		PreparedStatement pstmt = conn.prepareStatement("INSERT INTO streamtable (bin,str) VALUES (?,?)");
		pstmt.setBytes(1,null);
		pstmt.setAsciiStream(2, is, length);
		pstmt.executeUpdate();
		pstmt.close();
	}
}

package org.postgresql.test.jdbc2;
                                                                                                                                                                                     
import org.postgresql.test.TestUtil;
import junit.framework.TestCase;
import java.io.*;
import java.sql.*;

import java.io.ByteArrayInputStream;
import java.io.InputStream;
import java.util.Properties;
import java.sql.*;

/**
 * User: alexei
 * Date: 17-Dec-2003
 * Time: 11:01:44
 * @version $Id: OID74Test.java,v 1.2.2.3 2004/03/29 17:47:47 barry Exp $
 */
public class OID74Test  extends TestCase
{

	public OID74Test( String name )
	{
		super(name);
	}
	public void setUp() throws Exception
	{
	}
	public void tearDown() throws Exception
	{
	}
	public void testBinaryStream() throws SQLException
	{
		//set up conection here
		Properties props = new Properties();
		props.setProperty("compatible","7.1");
		Connection c = TestUtil.openDB(props);
		c.setAutoCommit(false);

		TestUtil.createTable(c,"temp","col oid");
    		
      		Statement st = null; 
      		
		PreparedStatement pstmt = null;
    		try 
		{
	      	
			pstmt = c.prepareStatement("INSERT INTO temp VALUES (?)");
		      	pstmt.setBinaryStream(1, new ByteArrayInputStream(new byte[]{1, 2, 3, 4, 5}), 5);
			assertTrue( (pstmt.executeUpdate() == 1) );
		      	pstmt.close();
    		
		      	pstmt = c.prepareStatement("SELECT col FROM temp LIMIT 1");
		      	ResultSet rs = pstmt.executeQuery();

		      	assertTrue("No results from query", rs.next() );

			InputStream in = rs.getBinaryStream(1);
		      	int data;
			int i = 1;
		      	while ((data = in.read()) != -1)
				assertEquals(data,i++);
		      	rs.close();
		      	pstmt.close();
			c.createStatement().executeUpdate("DELETE FROM temp");
			c.commit();
		}
		catch ( IOException ioex )
		{
			fail( ioex.getMessage() );
		}
		catch (SQLException ex)
		{
			fail( ex.getMessage() );
		}

		TestUtil.dropTable(c,"temp");
                c.commit();
		TestUtil.closeDB(c);
  	}	
}

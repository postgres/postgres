package org.postgresql.test.jdbc2;
                                                                                                                                                                                     
import org.postgresql.test.TestUtil;
import junit.framework.TestCase;
import java.io.*;
import java.sql.*;

import java.io.ByteArrayInputStream;
import java.io.InputStream;
import java.sql.*;

/**
 * User: alexei
 * Date: 17-Dec-2003
 * Time: 11:01:44
 * @version $Id: OID74Test.java,v 1.1 2003/12/17 15:38:42 davec Exp $
 */
public class OID74Test  extends TestCase
{
	private Connection con;
    

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
	public void testBinaryStream()
	{
		//set up conection here
		Connection c = null;
    		
      		Statement st = null; 
      		try 
		{
			c =  DriverManager.getConnection("jdbc:postgresql://localhost/test?compatible=7.1&user=test");
    			c.setAutoCommit(false);
			st = c.createStatement();
        		st.execute("CREATE TABLE temp (col oid)");
      		}
		 catch (SQLException e) 
		{
        		//another issue: when connecting to 7.3 database and this exception occurs because the table already exists,
		        //st.setBinaryStream throws internal error in LargeObjectManager initialisation code
		        fail("table creating error, probably already exists, code=" + e.getErrorCode());
      		}
		finally
		{
			try{ if (st != null) st.close(); }catch(SQLException ex){};
		}
      		
		PreparedStatement pstmt = null;
    		try 
		{
	      	
			pstmt = c.prepareStatement("INSERT INTO temp VALUES (?)");
			//in case of 7.4 server, should block here
		      	pstmt.setBinaryStream(1, new ByteArrayInputStream(new byte[]{1, 2, 3, 4, 5}), 5);
			assertTrue( (pstmt.executeUpdate() == 1) );
		      	pstmt.close();
    		
		      	pstmt = c.prepareStatement("SELECT col FROM temp LIMIT 1");
		      	ResultSet rs = pstmt.executeQuery();

		      	assertTrue("No results from query", rs.next() );

			//in case of 7.4 server, should block here
			InputStream in = rs.getBinaryStream(1);
		      	int data;
		      	while ((data = in.read()) != -1)
		        	System.out.println(data);
		      	rs.close();
		      	st.close();
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
		finally 
		{
			try
			{
				if ( c!=null) c.close();
			}
			catch( SQLException e1){}
		}
  	}	
}

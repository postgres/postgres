package org.postgresql.test.jdbc2;

import java.sql.*;
import junit.framework.TestCase;

import org.postgresql.test.TestUtil;
/**
 * <p>Title: </p>
 * <p>Description: </p>
 * <p>Copyright: Copyright (c) 2001</p>
 * <p>Company: </p>
 * @author unascribed
 * @version 1.0
 */

public class UpdateableResultTest extends TestCase
{
	private Connection con;

	public UpdateableResultTest( String name )
	{
		super( name );
	}

	protected void setUp() throws Exception
	{
		con = TestUtil.openDB();
		TestUtil.createTable(con, "updateable", "id int primary key, name text, notselected text");
		TestUtil.createTable(con, "second", "id1 int primary key, name1 text");

		// put some dummy data into second
		Statement st2 = con.createStatement();
		st2.execute( "insert into second values (1,'anyvalue' )");
		st2.close();
		
	}

	protected void tearDown() throws Exception
	{
		TestUtil.dropTable(con, "updateable");
		TestUtil.dropTable(con, "second");
		TestUtil.closeDB(con);
	}

	public void testCancelRowUpdates() throws Exception
	{
		Statement st = con.createStatement( ResultSet.TYPE_SCROLL_INSENSITIVE, ResultSet.CONCUR_UPDATABLE );
		ResultSet rs = st.executeQuery( "select * from second");

		// make sure we're dealing with the correct row.
		rs.first();
		assertEquals(1,rs.getInt(1));
		assertEquals("anyvalue",rs.getString(2));

		// update, cancel and make sure nothings changed.
		rs.updateInt(1,99);
		rs.cancelRowUpdates();
		assertEquals(1,rs.getInt(1));
		assertEquals("anyvalue",rs.getString(2));

		// real update
		rs.updateInt(1,999);
		rs.updateRow();
		assertEquals(999,rs.getInt(1));
		assertEquals("anyvalue",rs.getString(2));

		// scroll some and make sure the update is still there
		rs.beforeFirst();
		rs.next();
		assertEquals(999,rs.getInt(1));
		assertEquals("anyvalue",rs.getString(2));


		// make sure the update got to the db and the driver isn't lying to us.
		rs.close();
		rs = st.executeQuery( "select * from second");
		rs.first();
		assertEquals(999,rs.getInt(1));
		assertEquals("anyvalue",rs.getString(2));

		rs.close();
		st.close();
	}



	public void testUpdateable()
	{
		try
		{
			Statement st = con.createStatement( ResultSet.TYPE_SCROLL_INSENSITIVE, ResultSet.CONCUR_UPDATABLE );
			ResultSet rs = st.executeQuery( "select * from updateable");
			assertNotNull( rs );
			rs.moveToInsertRow();
			rs.updateInt( 1, 1 );
			rs.updateString( 2, "jake" );
			rs.updateString( 3, "avalue" );
			rs.insertRow();
			rs.first();

			rs.updateInt( "id", 2 );
			rs.updateString( "name", "dave" );
			rs.updateRow();

			assertTrue( rs.getInt("id") == 2 );
			assertTrue( rs.getString("name").equals("dave"));
			assertTrue( rs.getString("notselected").equals("avalue") );

			rs.deleteRow();
			rs.moveToInsertRow();
			rs.updateInt("id", 3);
			rs.updateString("name", "paul");

			rs.insertRow();
			rs.refreshRow();
			assertTrue( rs.getInt("id") == 3 );
			assertTrue( rs.getString("name").equals("paul"));
			assertTrue( rs.getString("notselected") == null );


			rs.close();

			rs = st.executeQuery("select id1, id, name, name1 from updateable, second" );
			try
			{
				while ( rs.next() )
				{
					rs.updateInt( "id", 2 );
					rs.updateString( "name", "dave" );
					rs.updateRow();
				}


				assertTrue( "should not get here, update should fail", false );
			}
			catch (SQLException ex)
			{}

			try
			{
				rs = st.executeQuery("select oid,* from updateable");
				if ( rs.first() )
				{
					rs.updateInt( "id", 3 );
					rs.updateString( "name", "dave3");
					rs.updateRow();
					assertTrue(rs.getInt("id") == 3 );
					assertTrue(rs.getString("name").equals("dave3"));

					rs.moveToInsertRow();
					rs.updateInt( "id", 4 );
					rs.updateString( "name", "dave4" );

					rs.insertRow();
					rs.updateInt("id", 5 );
					rs.updateString( "name", "dave5" );
					rs.insertRow();

					rs.moveToCurrentRow();
					assertTrue(rs.getInt("id") == 3 );
					assertTrue(rs.getString("name").equals("dave3"));

					assertTrue( rs.next() );
					assertTrue(rs.getInt("id") == 4 );
					assertTrue(rs.getString("name").equals("dave4"));

					assertTrue( rs.next() );
					assertTrue(rs.getInt("id") == 5 );
					assertTrue(rs.getString("name").equals("dave5"));

				}
			}
			catch (SQLException ex)
			{
				fail(ex.getMessage());
			}

			st.close();

		}
		catch (Exception ex)
		{
			ex.printStackTrace();
			fail(ex.getMessage());
		}
	}


}

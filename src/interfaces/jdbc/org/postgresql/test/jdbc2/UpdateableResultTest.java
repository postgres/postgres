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

	public void testDeleteRows() throws SQLException
	{
		Statement st = con.createStatement();
		st.executeUpdate("INSERT INTO second values (2,'two')");
		st.executeUpdate("INSERT INTO second values (3,'three')");
		st.executeUpdate("INSERT INTO second values (4,'four')");
		st.close();

		st = con.createStatement( ResultSet.TYPE_SCROLL_INSENSITIVE, ResultSet.CONCUR_UPDATABLE );
		ResultSet rs = st.executeQuery( "select id1,name1 from second order by id1");

		assertTrue(rs.next());
		assertEquals(1, rs.getInt("id1"));
		rs.deleteRow();
		assertTrue(rs.isBeforeFirst());

		assertTrue(rs.next());
		assertTrue(rs.next());
		assertEquals(3, rs.getInt("id1"));
		rs.deleteRow();
		assertEquals(2, rs.getInt("id1"));

		rs.close();
		st.close();
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



	public void testUpdateable() throws SQLException
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

		assertEquals(2, rs.getInt("id"));
		assertEquals("dave", rs.getString("name"));
		assertEquals("avalue", rs.getString("notselected"));

		rs.deleteRow();
		rs.moveToInsertRow();
		rs.updateInt("id", 3);
		rs.updateString("name", "paul");

		rs.insertRow();

		try {
			rs.refreshRow();
			fail("Can't refresh when on the insert row.");
		} catch (SQLException sqle) { }

		assertEquals(3, rs.getInt("id"));
		assertEquals("paul", rs.getString("name"));
		assertNull(rs.getString("notselected"));

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


			fail("should not get here, update should fail");
		}
		catch (SQLException ex)
		{}

		rs = st.executeQuery("select oid,* from updateable");
		assertTrue(rs.first());
		rs.updateInt( "id", 3 );
		rs.updateString( "name", "dave3");
		rs.updateRow();
		assertEquals(3, rs.getInt("id"));
		assertEquals("dave3", rs.getString("name"));

		rs.moveToInsertRow();
		rs.updateInt( "id", 4 );
		rs.updateString( "name", "dave4" );

		rs.insertRow();
		rs.updateInt("id", 5 );
		rs.updateString( "name", "dave5" );
		rs.insertRow();

		rs.moveToCurrentRow();
		assertEquals(3, rs.getInt("id"));
		assertEquals("dave3", rs.getString("name"));

		assertTrue( rs.next() );
		assertEquals(4, rs.getInt("id"));
		assertEquals("dave4", rs.getString("name"));

		assertTrue( rs.next() );
		assertEquals(5, rs.getInt("id"));
		assertEquals("dave5", rs.getString("name"));

		rs.close();
		st.close();
	}

}

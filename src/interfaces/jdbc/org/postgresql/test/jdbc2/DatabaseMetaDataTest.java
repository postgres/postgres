package org.postgresql.test.jdbc2;

import org.postgresql.test.JDBC2Tests;
import junit.framework.TestCase;
import java.sql.*;

/*
 * TestCase to test the internal functionality of org.postgresql.jdbc2.DatabaseMetaData
 *
 * PS: Do you know how difficult it is to type on a train? ;-)
 *
 * $Id: DatabaseMetaDataTest.java,v 1.10 2002/07/30 13:22:38 davec Exp $
 */

public class DatabaseMetaDataTest extends TestCase
{

	private Connection con;
	/*
	 * Constructor
	 */
	public DatabaseMetaDataTest(String name)
	{
		super(name);
	}

	protected void setUp() throws Exception
	{
		con = JDBC2Tests.openDB();
		JDBC2Tests.createTable( con, "testmetadata", "id int4, name text, updated timestamp" );
	}
	protected void tearDown() throws Exception
	{
		JDBC2Tests.dropTable( con, "testmetadata" );

		JDBC2Tests.closeDB( con );
	}
	/*
	 * The spec says this may return null, but we always do!
	 */
	public void testGetMetaData()
	{
		try
		{

			DatabaseMetaData dbmd = con.getMetaData();
			assertNotNull(dbmd);

			ResultSet rs = dbmd.getTables( null, null, "test%", new String[] {"TABLE"});
			assertTrue( rs.next() );
      String tableName = rs.getString("TABLE_NAME");
			assertTrue( tableName.equals("testmetadata") );

			rs.close();

			rs = dbmd.getColumns("", "", "test%", "%" );
			assertTrue( rs.next() );
			assertTrue( rs.getString("TABLE_NAME").equals("testmetadata") );
			assertTrue( rs.getString("COLUMN_NAME").equals("id") );
			assertTrue( rs.getInt("DATA_TYPE") == java.sql.Types.INTEGER );

			assertTrue( rs.next() );
			assertTrue( rs.getString("TABLE_NAME").equals("testmetadata") );
			assertTrue( rs.getString("COLUMN_NAME").equals("name") );
			assertTrue( rs.getInt("DATA_TYPE") == java.sql.Types.VARCHAR );

			assertTrue( rs.next() );
			assertTrue( rs.getString("TABLE_NAME").equals("testmetadata") );
			assertTrue( rs.getString("COLUMN_NAME").equals("updated") );
			assertTrue( rs.getInt("DATA_TYPE") == java.sql.Types.TIMESTAMP );

		}
		catch (SQLException ex)
		{
			fail(ex.getMessage());
		}
	}

	/*
	 * Test default capabilities
	 */
	public void testCapabilities()
	{
		try
		{

			DatabaseMetaData dbmd = con.getMetaData();
			assertNotNull(dbmd);

			assertTrue(dbmd.allProceduresAreCallable());
			assertTrue(dbmd.allTablesAreSelectable()); // not true all the time

			// This should always be false for postgresql (at least for 7.x)
			assertTrue(!dbmd.isReadOnly());

			// does the backend support this yet? The protocol does...
			assertTrue(!dbmd.supportsMultipleResultSets());

			// yes, as multiple backends can have transactions open
			assertTrue(dbmd.supportsMultipleTransactions());

			assertTrue(dbmd.supportsMinimumSQLGrammar());
			assertTrue(!dbmd.supportsCoreSQLGrammar());
			assertTrue(!dbmd.supportsExtendedSQLGrammar());
			assertTrue(dbmd.supportsANSI92EntryLevelSQL());
			assertTrue(!dbmd.supportsANSI92IntermediateSQL());
			assertTrue(!dbmd.supportsANSI92FullSQL());

			assertTrue(!dbmd.supportsIntegrityEnhancementFacility());

		}
		catch (SQLException ex)
		{
			fail(ex.getMessage());
		}
	}


	public void testJoins()
	{
		try
		{

			DatabaseMetaData dbmd = con.getMetaData();
			assertNotNull(dbmd);

			assertTrue(dbmd.supportsOuterJoins());
			assertTrue(dbmd.supportsFullOuterJoins());
			assertTrue(dbmd.supportsLimitedOuterJoins());

		}
		catch (SQLException ex)
		{
			fail(ex.getMessage());
		}
	}

	public void testCursors()
	{
		try
		{

			DatabaseMetaData dbmd = con.getMetaData();
			assertNotNull(dbmd);

			assertTrue(!dbmd.supportsPositionedDelete());
			assertTrue(!dbmd.supportsPositionedUpdate());

		}
		catch (SQLException ex)
		{
			fail(ex.getMessage());
		}
	}

	public void testNulls()
	{
		try
		{

			DatabaseMetaData dbmd = con.getMetaData();
			assertNotNull(dbmd);

			// We need to type cast the connection to get access to the
			// PostgreSQL-specific method haveMinimumServerVersion().
			// This is not available through the java.sql.Connection interface.
			assertTrue( con instanceof org.postgresql.PGConnection );

			assertTrue(!dbmd.nullsAreSortedAtStart());
			assertTrue( dbmd.nullsAreSortedAtEnd() !=
						((org.postgresql.jdbc2.AbstractJdbc2Connection)con).haveMinimumServerVersion("7.2"));
			assertTrue( dbmd.nullsAreSortedHigh() ==
						((org.postgresql.jdbc2.AbstractJdbc2Connection)con).haveMinimumServerVersion("7.2"));
			assertTrue(!dbmd.nullsAreSortedLow());

			assertTrue(dbmd.nullPlusNonNullIsNull());

			assertTrue(dbmd.supportsNonNullableColumns());

		}
		catch (SQLException ex)
		{
			fail(ex.getMessage());
		}
	}

	public void testLocalFiles()
	{
		try
		{

			DatabaseMetaData dbmd = con.getMetaData();
			assertNotNull(dbmd);

			assertTrue(!dbmd.usesLocalFilePerTable());
			assertTrue(!dbmd.usesLocalFiles());

		}
		catch (SQLException ex)
		{
			fail(ex.getMessage());
		}
	}

	public void testIdentifiers()
	{
		try
		{

			DatabaseMetaData dbmd = con.getMetaData();
			assertNotNull(dbmd);

			assertTrue(!dbmd.supportsMixedCaseIdentifiers()); // always false
			assertTrue(dbmd.supportsMixedCaseQuotedIdentifiers());	// always true

			assertTrue(!dbmd.storesUpperCaseIdentifiers());   // always false
			assertTrue(dbmd.storesLowerCaseIdentifiers());	  // always true
			assertTrue(!dbmd.storesUpperCaseQuotedIdentifiers()); // always false
			assertTrue(!dbmd.storesLowerCaseQuotedIdentifiers()); // always false
			assertTrue(!dbmd.storesMixedCaseQuotedIdentifiers()); // always false

			assertTrue(dbmd.getIdentifierQuoteString().equals("\""));


		}
		catch (SQLException ex)
		{
			fail(ex.getMessage());
		}
	}

  public void testCrossReference()
  {
		try
		{
		  Connection con1 = JDBC2Tests.openDB();

		  JDBC2Tests.createTable( con1, "vv", "a int not null, b int not null, primary key ( a, b )" );

		  JDBC2Tests.createTable( con1, "ww", "m int not null, n int not null, primary key ( m, n ), foreign key ( m, n ) references vv ( a, b )" );


			DatabaseMetaData dbmd = con.getMetaData();
			assertNotNull(dbmd);

      ResultSet rs = dbmd.getCrossReference(null, "", "vv", null, "", "ww" );

      for (int j=1; rs.next(); j++ )
      {

         String pkTableName = rs.getString( "PKTABLE_NAME" );
         assertTrue (  pkTableName.equals("vv")  );

         String pkColumnName = rs.getString( "PKCOLUMN_NAME" );
         assertTrue( pkColumnName.equals("a") || pkColumnName.equals("b"));

         String fkTableName = rs.getString( "FKTABLE_NAME" );
         assertTrue( fkTableName.equals( "ww" ) );

         String fkColumnName = rs.getString( "FKCOLUMN_NAME" );
         assertTrue( fkColumnName.equals( "m" ) || fkColumnName.equals( "n" ) ) ;

         String fkName = rs.getString( "FK_NAME" );
         assertTrue( fkName.equals( "<unnamed>") );

         String pkName = rs.getString( "PK_NAME" );
         assertTrue( pkName.equals("vv_pkey") );

         int keySeq  = rs.getInt( "KEY_SEQ" );
         assertTrue( keySeq == j );
      }


      JDBC2Tests.dropTable( con1, "vv" );
      JDBC2Tests.dropTable( con1, "ww" );

		}
		catch (SQLException ex)
		{
			fail(ex.getMessage());
		}
  }
  public void testForeignKeys()
  {
		try
		{
		  Connection con1 = JDBC2Tests.openDB();
		  JDBC2Tests.createTable( con1, "people", "id int4 primary key, name text" );
		  JDBC2Tests.createTable( con1, "policy", "id int4 primary key, name text" );

		  JDBC2Tests.createTable( con1, "users", "id int4 primary key, people_id int4, policy_id int4,"+
                                    "CONSTRAINT people FOREIGN KEY (people_id) references people(id),"+
                                    "constraint policy FOREIGN KEY (policy_id) references policy(id)" );


			DatabaseMetaData dbmd = con.getMetaData();
			assertNotNull(dbmd);

      ResultSet rs = dbmd.getImportedKeys(null, "", "users" );
      int j = 0;
      for (; rs.next(); j++ )
      {

         String pkTableName = rs.getString( "PKTABLE_NAME" );
	 assertTrue (  pkTableName.equals("people") || pkTableName.equals("policy")  );

         String pkColumnName = rs.getString( "PKCOLUMN_NAME" );
         assertTrue( pkColumnName.equals("id") );

         String fkTableName = rs.getString( "FKTABLE_NAME" );
         assertTrue( fkTableName.equals( "users" ) );

         String fkColumnName = rs.getString( "FKCOLUMN_NAME" );
         assertTrue( fkColumnName.equals( "people_id" ) || fkColumnName.equals( "policy_id" ) ) ;

         String fkName = rs.getString( "FK_NAME" );
         assertTrue( fkName.equals( "people") || fkName.equals( "policy" ) );

         String pkName = rs.getString( "PK_NAME" );
         assertTrue( pkName.equals( "people_pkey") || pkName.equals( "policy_pkey" ) );

      }

      assertTrue ( j== 2 );

      rs = dbmd.getExportedKeys( null, "", "people" );

      // this is hacky, but it will serve the purpose
      assertTrue ( rs.next() );

      assertTrue( rs.getString( "PKTABLE_NAME" ).equals( "people" ) );
      assertTrue( rs.getString( "PKCOLUMN_NAME" ).equals( "id" ) );

      assertTrue( rs.getString( "FKTABLE_NAME" ).equals( "users" ) );
      assertTrue( rs.getString( "FKCOLUMN_NAME" ).equals( "people_id" ) );

      assertTrue( rs.getString( "FK_NAME" ).equals( "people" ) );


      JDBC2Tests.dropTable( con1, "users" );
      JDBC2Tests.dropTable( con1, "people" );
      JDBC2Tests.dropTable( con1, "policy" );

		}
		catch (SQLException ex)
		{
			fail(ex.getMessage());
		}
  }
	public void testTables()
	{
		try
		{

			DatabaseMetaData dbmd = con.getMetaData();
			assertNotNull(dbmd);

			// we can add columns
			assertTrue(dbmd.supportsAlterTableWithAddColumn());

			// we can't drop columns (yet)
			assertTrue(!dbmd.supportsAlterTableWithDropColumn());

		}
		catch (SQLException ex)
		{
			fail(ex.getMessage());
		}
	}

	public void testSelect()
	{
		try
		{

			DatabaseMetaData dbmd = con.getMetaData();
			assertNotNull(dbmd);

			// yes we can?: SELECT col a FROM a;
			assertTrue(dbmd.supportsColumnAliasing());

			// yes we can have expressions in ORDERBY
			assertTrue(dbmd.supportsExpressionsInOrderBy());

			// Yes, an ORDER BY clause can contain columns that are not in the
			// SELECT clause.
			assertTrue(dbmd.supportsOrderByUnrelated());

			assertTrue(dbmd.supportsGroupBy());
			assertTrue(dbmd.supportsGroupByUnrelated());
			assertTrue(dbmd.supportsGroupByBeyondSelect()); // needs checking

		}
		catch (SQLException ex)
		{
			fail(ex.getMessage());
		}
	}

	public void testDBParams()
	{
		try
		{

			DatabaseMetaData dbmd = con.getMetaData();
			assertNotNull(dbmd);

			assertTrue(dbmd.getURL().equals(JDBC2Tests.getURL()));
			assertTrue(dbmd.getUserName().equals(JDBC2Tests.getUser()));

		}
		catch (SQLException ex)
		{
			fail(ex.getMessage());
		}
	}

	public void testDbProductDetails()
	{
		try
		{
		        assertTrue(con instanceof org.postgresql.PGConnection);
			org.postgresql.jdbc2.AbstractJdbc2Connection pc = (org.postgresql.jdbc2.AbstractJdbc2Connection) con;

			DatabaseMetaData dbmd = con.getMetaData();
			assertNotNull(dbmd);

			assertTrue(dbmd.getDatabaseProductName().equals("PostgreSQL"));
                        //The test below doesn't make sense to me, it tests that 
                        //the version of the driver = the version of the database it is connected to
                        //since the driver should be backwardly compatible this test is commented out 
			//assertTrue(dbmd.getDatabaseProductVersion().startsWith(
                        //         Integer.toString(pc.getDriver().getMajorVersion()) 
                        //         + "." 
                        //         + Integer.toString(pc.getDriver().getMinorVersion())));
			assertTrue(dbmd.getDriverName().equals("PostgreSQL Native Driver"));

		}
		catch (SQLException ex)
		{
			fail(ex.getMessage());
		}
	}

	public void testDriverVersioning()
	{
		try
		{
			assertTrue(con instanceof org.postgresql.PGConnection);
			org.postgresql.jdbc2.AbstractJdbc2Connection pc = (org.postgresql.jdbc2.AbstractJdbc2Connection) con;

			DatabaseMetaData dbmd = con.getMetaData();
			assertNotNull(dbmd);

			assertTrue(dbmd.getDriverVersion().equals(pc.getDriver().getVersion()));
			assertTrue(dbmd.getDriverMajorVersion() == pc.getDriver().getMajorVersion());
			assertTrue(dbmd.getDriverMinorVersion() == pc.getDriver().getMinorVersion());


		}
		catch (SQLException ex)
		{
			fail(ex.getMessage());
		}
	}
}

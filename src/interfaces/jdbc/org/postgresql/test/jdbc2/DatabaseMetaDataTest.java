package org.postgresql.test.jdbc2;

import org.postgresql.test.JDBC2Tests;
import junit.framework.TestCase;
import java.sql.*;

/**
 * TestCase to test the internal functionality of org.postgresql.jdbc2.DatabaseMetaData
 *
 * PS: Do you know how difficult it is to type on a train? ;-)
 *
 * $Id: DatabaseMetaDataTest.java,v 1.1 2001/02/13 16:39:05 peter Exp $
 */

public class DatabaseMetaDataTest extends TestCase {

  /**
   * Constructor
   */
  public DatabaseMetaDataTest(String name) {
    super(name);
  }

  /**
   * The spec says this may return null, but we always do!
   */
  public void testGetMetaData() {
    try {
      Connection con = JDBC2Tests.openDB();

      DatabaseMetaData dbmd = con.getMetaData();
      assert(dbmd!=null);

      JDBC2Tests.closeDB(con);
    } catch(SQLException ex) {
      assert(ex.getMessage(),false);
    }
  }

  /**
   * Test default capabilities
   */
  public void testCapabilities() {
    try {
      Connection con = JDBC2Tests.openDB();

      DatabaseMetaData dbmd = con.getMetaData();
      assert(dbmd!=null);

      assert(dbmd.allProceduresAreCallable()==true);
      assert(dbmd.allTablesAreSelectable()==true); // not true all the time

      // This should always be false for postgresql (at least for 7.x)
      assert(!dbmd.isReadOnly());

      // does the backend support this yet? The protocol does...
      assert(!dbmd.supportsMultipleResultSets());

      // yes, as multiple backends can have transactions open
      assert(dbmd.supportsMultipleTransactions());

      assert(dbmd.supportsMinimumSQLGrammar());
      assert(!dbmd.supportsCoreSQLGrammar());
      assert(!dbmd.supportsExtendedSQLGrammar());
      assert(!dbmd.supportsANSI92EntryLevelSQL());
      assert(!dbmd.supportsANSI92IntermediateSQL());
      assert(!dbmd.supportsANSI92FullSQL());

      assert(!dbmd.supportsIntegrityEnhancementFacility());

      JDBC2Tests.closeDB(con);
    } catch(SQLException ex) {
      assert(ex.getMessage(),false);
    }
  }


  public void testJoins() {
    try {
      Connection con = JDBC2Tests.openDB();

      DatabaseMetaData dbmd = con.getMetaData();
      assert(dbmd!=null);

      assert(dbmd.supportsOuterJoins());
      assert(dbmd.supportsFullOuterJoins());
      assert(dbmd.supportsLimitedOuterJoins());

      JDBC2Tests.closeDB(con);
    } catch(SQLException ex) {
      assert(ex.getMessage(),false);
    }
  }

  public void testCursors() {
    try {
      Connection con = JDBC2Tests.openDB();

      DatabaseMetaData dbmd = con.getMetaData();
      assert(dbmd!=null);

      assert(!dbmd.supportsPositionedDelete());
      assert(!dbmd.supportsPositionedUpdate());

      JDBC2Tests.closeDB(con);
    } catch(SQLException ex) {
      assert(ex.getMessage(),false);
    }
  }

  public void testNulls() {
    try {
      Connection con = JDBC2Tests.openDB();

      DatabaseMetaData dbmd = con.getMetaData();
      assert(dbmd!=null);

      // these need double checking
      assert(!dbmd.nullsAreSortedAtStart());
      assert(dbmd.nullsAreSortedAtEnd());
      assert(!dbmd.nullsAreSortedHigh());
      assert(!dbmd.nullsAreSortedLow());

      assert(dbmd.nullPlusNonNullIsNull());

      assert(dbmd.supportsNonNullableColumns());

      JDBC2Tests.closeDB(con);
    } catch(SQLException ex) {
      assert(ex.getMessage(),false);
    }
  }

  public void testLocalFiles() {
    try {
      Connection con = JDBC2Tests.openDB();

      DatabaseMetaData dbmd = con.getMetaData();
      assert(dbmd!=null);

      assert(!dbmd.usesLocalFilePerTable());
      assert(!dbmd.usesLocalFiles());

      JDBC2Tests.closeDB(con);
    } catch(SQLException ex) {
      assert(ex.getMessage(),false);
    }
  }

  public void testIdentifiers() {
    try {
      Connection con = JDBC2Tests.openDB();

      DatabaseMetaData dbmd = con.getMetaData();
      assert(dbmd!=null);

      assert(!dbmd.supportsMixedCaseIdentifiers()); // always false
      assert(dbmd.supportsMixedCaseQuotedIdentifiers());  // always true

      assert(!dbmd.storesUpperCaseIdentifiers());   // always false
      assert(dbmd.storesLowerCaseIdentifiers());    // always true
      assert(!dbmd.storesUpperCaseQuotedIdentifiers()); // always false
      assert(!dbmd.storesLowerCaseQuotedIdentifiers()); // always false
      assert(!dbmd.storesMixedCaseQuotedIdentifiers()); // always false

      assert(dbmd.getIdentifierQuoteString().equals("\""));


      JDBC2Tests.closeDB(con);
    } catch(SQLException ex) {
      assert(ex.getMessage(),false);
    }
  }

  public void testTables() {
    try {
      Connection con = JDBC2Tests.openDB();

      DatabaseMetaData dbmd = con.getMetaData();
      assert(dbmd!=null);

      // we can add columns
      assert(dbmd.supportsAlterTableWithAddColumn());

      // we can't drop columns (yet)
      assert(!dbmd.supportsAlterTableWithDropColumn());

      JDBC2Tests.closeDB(con);
    } catch(SQLException ex) {
      assert(ex.getMessage(),false);
    }
  }

  public void testSelect() {
    try {
      Connection con = JDBC2Tests.openDB();

      DatabaseMetaData dbmd = con.getMetaData();
      assert(dbmd!=null);

      // yes we can?: SELECT col a FROM a;
      assert(dbmd.supportsColumnAliasing());

      // yes we can have expressions in ORDERBY
      assert(dbmd.supportsExpressionsInOrderBy());

      assert(!dbmd.supportsOrderByUnrelated());

      assert(dbmd.supportsGroupBy());
      assert(dbmd.supportsGroupByUnrelated());
      assert(dbmd.supportsGroupByBeyondSelect()); // needs checking

      JDBC2Tests.closeDB(con);
    } catch(SQLException ex) {
      assert(ex.getMessage(),false);
    }
  }

  public void testDBParams() {
    try {
      Connection con = JDBC2Tests.openDB();

      DatabaseMetaData dbmd = con.getMetaData();
      assert(dbmd!=null);

      assert(dbmd.getURL().equals(JDBC2Tests.getURL()));
      assert(dbmd.getUserName().equals(JDBC2Tests.getUser()));

      JDBC2Tests.closeDB(con);
    } catch(SQLException ex) {
      assert(ex.getMessage(),false);
    }
  }

  public void testDbProductDetails() {
    try {
      Connection con = JDBC2Tests.openDB();
      assert(con instanceof org.postgresql.Connection);
      org.postgresql.Connection pc = (org.postgresql.Connection) con;

      DatabaseMetaData dbmd = con.getMetaData();
      assert(dbmd!=null);

      assert(dbmd.getDatabaseProductName().equals("PostgreSQL"));
      assert(dbmd.getDatabaseProductVersion().startsWith(Integer.toString(pc.this_driver.getMajorVersion())+"."+Integer.toString(pc.this_driver.getMinorVersion())));
      assert(dbmd.getDriverName().equals("PostgreSQL Native Driver"));

      JDBC2Tests.closeDB(con);
    } catch(SQLException ex) {
      assert(ex.getMessage(),false);
    }
  }

  public void testDriverVersioning() {
    try {
      Connection con = JDBC2Tests.openDB();
      assert(con instanceof org.postgresql.Connection);
      org.postgresql.Connection pc = (org.postgresql.Connection) con;

      DatabaseMetaData dbmd = con.getMetaData();
      assert(dbmd!=null);

      assert(dbmd.getDriverVersion().equals(pc.this_driver.getVersion()));
      assert(dbmd.getDriverMajorVersion()==pc.this_driver.getMajorVersion());
      assert(dbmd.getDriverMinorVersion()==pc.this_driver.getMinorVersion());


      JDBC2Tests.closeDB(con);
    } catch(SQLException ex) {
      assert(ex.getMessage(),false);
    }
  }
}
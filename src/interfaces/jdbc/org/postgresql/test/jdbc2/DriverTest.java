package org.postgresql.test.jdbc2;

import org.postgresql.test.JDBC2Tests;
import junit.framework.TestCase;
import java.sql.*;

/**
 * $Id: DriverTest.java,v 1.1 2001/02/07 09:13:20 peter Exp $
 *
 * Tests the dynamically created class org.postgresql.Driver
 *
 */
public class DriverTest extends TestCase {

  public DriverTest(String name) {
    super(name);
  }

  /**
   * This tests the acceptsURL() method with a couple of good and badly formed
   * jdbc urls
   */
  public void testAcceptsURL() {
    try {

      // Load the driver (note clients should never do it this way!)
      org.postgresql.Driver drv = new org.postgresql.Driver();
      assert(drv!=null);

      // These are always correct
      assert(drv.acceptsURL("jdbc:postgresql:test"));
      assert(drv.acceptsURL("jdbc:postgresql://localhost/test"));
      assert(drv.acceptsURL("jdbc:postgresql://localhost:5432/test"));
      assert(drv.acceptsURL("jdbc:postgresql://127.0.0.1/anydbname"));
      assert(drv.acceptsURL("jdbc:postgresql://127.0.0.1:5433/hidden"));

      // Badly formatted url's
      assert(!drv.acceptsURL("jdbc:postgres:test"));
      assert(!drv.acceptsURL("postgresql:test"));

    } catch(SQLException ex) {
      assert(ex.getMessage(),false);
    }
  }

  /**
   * Tests parseURL (internal)
   */
  /**
   * Tests the connect method by connecting to the test database
   */
  public void testConnect() {
    Connection con=null;
    try {
      Class.forName("org.postgresql.Driver");

      // Test with the url, username & password
      con = DriverManager.getConnection(JDBC2Tests.getURL(),JDBC2Tests.getUser(),JDBC2Tests.getPassword());
      assert(con!=null);
      con.close();

      // Test with the username in the url
      con = DriverManager.getConnection(JDBC2Tests.getURL()+"?user="+JDBC2Tests.getUser()+"&password="+JDBC2Tests.getPassword());
      assert(con!=null);
      con.close();

    } catch(ClassNotFoundException ex) {
      assert(ex.getMessage(),false);
    } catch(SQLException ex) {
      assert(ex.getMessage(),false);
    }
  }
}
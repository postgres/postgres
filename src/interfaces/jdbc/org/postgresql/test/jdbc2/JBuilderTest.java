package org.postgresql.test.jdbc2;

import org.postgresql.test.JDBC2Tests;
import junit.framework.TestCase;
import java.sql.*;
import java.math.BigDecimal;

/**
 * $Id: JBuilderTest.java,v 1.1 2001/02/13 16:39:05 peter Exp $
 *
 * Some simple tests to check that the required components needed for JBuilder
 * stay working
 *
 */
public class JBuilderTest extends TestCase {

  public JBuilderTest(String name) {
    super(name);
  }

  /**
   * This tests that Money types work. JDBCExplorer barfs if this fails.
   */
  public void testMoney() {
    try {
      Connection con = JDBC2Tests.openDB();

      Statement st=con.createStatement();
      ResultSet rs=st.executeQuery("select cost from test_c");
      assert(rs!=null);

      while(rs.next()){
        double bd = rs.getDouble(1);
      }

      rs.close();
      st.close();

      JDBC2Tests.closeDB(con);
    } catch(Exception ex) {
      assert(ex.getMessage(),false);
    }
  }
}

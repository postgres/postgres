package org.postgresql.test.jdbc2;

import org.postgresql.test.JDBC2Tests;
import junit.framework.TestCase;
import java.sql.*;

/**
 * $Id: TimeTest.java,v 1.1 2001/02/13 16:39:05 peter Exp $
 *
 * Some simple tests based on problems reported by users. Hopefully these will
 * help prevent previous problems from re-occuring ;-)
 *
 */
public class TimeTest extends TestCase {

  public TimeTest(String name) {
    super(name);
  }

  /**
   * Tests the time methods in ResultSet
   */
  public void testGetTime() {
    try {
      Connection con = JDBC2Tests.openDB();

      Statement st=con.createStatement();

      JDBC2Tests.createTable(con,"tm time");

      st.executeUpdate(JDBC2Tests.insert("'01:02:03'"));
      st.executeUpdate(JDBC2Tests.insert("'23:59:59'"));

      // Fall through helper
      checkTimeTest(con,st);

      st.close();

      JDBC2Tests.closeDB(con);
    } catch(Exception ex) {
      assert(ex.getMessage(),false);
    }
  }

  /**
   * Tests the time methods in PreparedStatement
   */
  public void testSetTime() {
    try {
      Connection con = JDBC2Tests.openDB();

      Statement st=con.createStatement();

      JDBC2Tests.createTable(con,"tm time");

      PreparedStatement ps = con.prepareStatement(JDBC2Tests.insert("?"));

      ps.setTime(1,getTime(1,2,3));
      assert(!ps.execute()); // false as its an update!

      ps.setTime(1,getTime(23,59,59));
      assert(!ps.execute()); // false as its an update!

      // Fall through helper
      checkTimeTest(con,st);

      ps.close();
      st.close();

      JDBC2Tests.closeDB(con);
    } catch(Exception ex) {
      assert(ex.getMessage(),false);
    }
  }

  /**
   * Helper for the TimeTests. It tests what should be in the db
   */
  private void checkTimeTest(Connection con,Statement st) throws SQLException {
    ResultSet rs=null;
    Time t=null;

    rs=st.executeQuery(JDBC2Tests.select("tm"));
    assert(rs!=null);

    assert(rs.next());
    t = rs.getTime(1);
    assert(t!=null);
    assert(getHours(t)==1);
    assert(getMinutes(t)==2);
    assert(getSeconds(t)==3);

    assert(rs.next());
    t = rs.getTime(1);
    assert(t!=null);
    assert(getHours(t)==23);
    assert(getMinutes(t)==59);
    assert(getSeconds(t)==59);

    assert(!rs.next());

    rs.close();
  }

  /**
   * These implement depreciated methods in java.sql.Time
   */
  private static long getHours(Time t) {
    return (t.getTime() % JDBC2Tests.DAYMILLIS)/3600000;
  }

  private static long getMinutes(Time t) {
    return ((t.getTime() % JDBC2Tests.DAYMILLIS)/60000)%60;
  }

  private static long getSeconds(Time t) {
    return ((t.getTime() % JDBC2Tests.DAYMILLIS)/1000)%60;
  }

  private Time getTime(int h,int m,int s) {
    return new Time(1000*(s+(m*60)+(h*3600)));
  }
}

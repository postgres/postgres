package org.postgresql.test.jdbc2.optional;

import junit.framework.TestCase;
import org.postgresql.test.JDBC2Tests;
import org.postgresql.jdbc2.optional.SimpleDataSource;
import org.postgresql.jdbc2.optional.BaseDataSource;

import java.sql.*;

/**
 * Common tests for all the BaseDataSource implementations.  This is
 * a small variety to make sure that a connection can be opened and
 * some basic queries run.  The different BaseDataSource subclasses
 * have different subclasses of this which add additional custom
 * tests.
 *
 * @author Aaron Mulder (ammulder@chariotsolutions.com)
 * @version $Revision: 1.1 $
 */
public abstract class BaseDataSourceTest extends TestCase {
    protected Connection con;
    protected BaseDataSource bds;

    /**
     * Constructor required by JUnit
     */
    public BaseDataSourceTest(String name) {
        super(name);
    }

    /**
     * Creates a test table using a standard connection (not from a
     * DataSource).
     */
    protected void setUp() throws Exception {
        con = JDBC2Tests.openDB();
        JDBC2Tests.createTable(con, "poolingtest", "id int4 not null primary key, name varchar(50)");
        Statement stmt = con.createStatement();
        stmt.executeUpdate("INSERT INTO poolingtest VALUES (1, 'Test Row 1')");
        stmt.executeUpdate("INSERT INTO poolingtest VALUES (2, 'Test Row 2')");
        JDBC2Tests.closeDB(con);
    }

    /**
     * Removes the test table using a standard connection (not from
     * a DataSource)
     */
    protected void tearDown() throws Exception {
        con = JDBC2Tests.openDB();
        JDBC2Tests.dropTable(con, "poolingtest");
        JDBC2Tests.closeDB(con);
    }

    /**
     * Gets a connection from the current BaseDataSource
     */
    protected Connection getDataSourceConnection() throws SQLException {
        initializeDataSource();
        return bds.getConnection();
    }

    /**
     * Creates an instance of the current BaseDataSource for
     * testing.  Must be customized by each subclass.
     */
    protected abstract void initializeDataSource();

    /**
     * Test to make sure you can instantiate and configure the
     * appropriate DataSource
     */
    public void testCreateDataSource() {
        initializeDataSource();
    }

    /**
     * Test to make sure you can get a connection from the DataSource,
     * which in turn means the DataSource was able to open it.
     */
    public void testGetConnection() {
        try {
            con = getDataSourceConnection();
            con.close();
        } catch (SQLException e) {
            fail(e.getMessage());
        }
    }

    /**
     * A simple test to make sure you can execute SQL using the
     * Connection from the DataSource
     */
    public void testUseConnection() {
        try {
            con = getDataSourceConnection();
            Statement st = con.createStatement();
            ResultSet rs = st.executeQuery("SELECT COUNT(*) FROM poolingtest");
            if(rs.next()) {
                int count = rs.getInt(1);
                if(rs.next()) {
                    fail("Should only have one row in SELECT COUNT result set");
                }
                if(count != 2) {
                    fail("Count returned "+count+" expecting 2");
                }
            } else {
                fail("Should have one row in SELECT COUNT result set");
            }
            rs.close();
            st.close();
            con.close();
        } catch (SQLException e) {
            fail(e.getMessage());
        }
    }

    /**
     * A test to make sure you can execute DDL SQL using the
     * Connection from the DataSource.
     */
    public void testDdlOverConnection() {
        try {
            con = getDataSourceConnection();
            JDBC2Tests.dropTable(con, "poolingtest");
            JDBC2Tests.createTable(con, "poolingtest", "id int4 not null primary key, name varchar(50)");
            con.close();
        } catch (SQLException e) {
            fail(e.getMessage());
        }
    }

    /**
     * A test to make sure the connections are not being pooled by the
     * current DataSource.  Obviously need to be overridden in the case
     * of a pooling Datasource.
     */
    public void testNotPooledConnection() {
        try {
            con = getDataSourceConnection();
            String name = con.toString();
            con.close();
            con = getDataSourceConnection();
            String name2 = con.toString();
            con.close();
            assertTrue(!name.equals(name2));
        } catch (SQLException e) {
            fail(e.getMessage());
        }
    }

    /**
     * Eventually, we must test stuffing the DataSource in JNDI and
     * then getting it back out and make sure it's still usable.  This
     * should ideally test both Serializable and Referenceable
     * mechanisms.  Will probably be multiple tests when implemented.
     */
    public void testJndi() {
        // TODO: Put the DS in JNDI, retrieve it, and try some of this stuff again
    }
}

package org.postgresql.test.jdbc2;

import org.postgresql.test.TestUtil;
import junit.framework.TestCase;
import java.sql.*;
import org.postgresql.util.Serialize;

public class SerializeTest extends TestCase {

	private Connection conn;
	private SerializeObject serobj;
	private Serialize ser;

	public SerializeTest(String name) {
		super(name);
	}

	protected void setUp() throws Exception {
		conn = TestUtil.openDB();
		serobj = new SerializeObject();
		serobj.intcol = 1;
		serobj.doublecol = 3.4;
		serobj.stringcol = "Hello";
		TestUtil.dropTable(conn,Serialize.toPostgreSQL(conn,serobj.getClass().getName()));
		Serialize.create(conn, serobj);
		Serialize.create(conn, serobj);
		ser = new Serialize(conn,serobj);
	}

	protected void tearDown() throws Exception {
		TestUtil.dropTable(conn,Serialize.toPostgreSQL(conn,serobj.getClass().getName()));
	}

	public void testCreateSerialize() {
		try {
			long oid = ser.storeObject(serobj);
			SerializeObject serobj2 = (SerializeObject)ser.fetch(oid);
			assertNotNull(serobj2);
			assertEquals(serobj.intcol,serobj2.intcol);
			assertTrue(Math.abs(serobj.doublecol-serobj2.doublecol) < 0.0001);
			assertTrue(serobj.stringcol.equals(serobj2.stringcol));
		} catch (SQLException sqle) {
			fail(sqle.getMessage());
		}
	}

}

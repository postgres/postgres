package org.postgresql;

import java.sql.Statement;
import java.sql.SQLException;

public abstract class PGStatement implements Statement {
    public ObjectPool inusemap_dim1[];
    public ObjectPool inusemap_dim2[];
    protected Connection connection;

    public PGStatement(Connection connection){
	this.connection = connection;
	inusemap_dim1 = connection.pg_stream.factory_dim1.getObjectPoolArr();
	inusemap_dim2 = connection.pg_stream.factory_dim2.getObjectPoolArr();
    }
 
    public void deallocate(){
	connection.pg_stream.deallocate(this);
    }

    public void close() throws SQLException {
	deallocate();
	connection.pg_stream.factory_dim1.releaseObjectPoolArr(inusemap_dim1);
	connection.pg_stream.factory_dim2.releaseObjectPoolArr(inusemap_dim2);
    }
}

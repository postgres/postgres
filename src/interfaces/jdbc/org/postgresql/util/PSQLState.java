/*-------------------------------------------------------------------------
 *
 * PSQLState.java
 *     This class is used for holding SQLState codes.
 *
 * Copyright (c) 2003, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
 
 package org.postgresql.util;
 
 public class PSQLState
 {
 	private String state;
	
	public String getState()
	{
		return this.state;
	}

	public PSQLState(String state)
	{
		this.state = state;
	}
	
	
	// begin constant state codes
	public final static PSQLState UNKNOWN_STATE = new PSQLState("");
	public final static PSQLState COMMUNICATION_ERROR = new PSQLState("08S01");
	public final static PSQLState DATA_ERROR = new PSQLState("22000");
	public final static PSQLState CONNECTION_DOES_NOT_EXIST = new PSQLState("08003");
	public final static PSQLState CONNECTION_REJECTED = new PSQLState("08004");
	public final static PSQLState CONNECTION_UNABLE_TO_CONNECT = new PSQLState("08001");
	public final static PSQLState CONNECTION_FAILURE = new PSQLState("08006");
	public final static PSQLState CONNECTION_CLOSED = new PSQLState("08003");
	public final static PSQLState NOT_IMPLEMENTED = new PSQLState("0A000");
	public final static PSQLState INVALID_PARAMETER_TYPE = new PSQLState("07006");
	public final static PSQLState PARAMETER_ERROR = new PSQLState("07001");
	public final static PSQLState TRANSACTION_STATE_INVALID = new PSQLState("25000");
	
}

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
 
 public class PSQLState implements java.io.Serializable
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
  public final static PSQLState NO_DATA = new PSQLState("02000");
  public final static PSQLState INVALID_PARAMETER_TYPE = new PSQLState("07006");
  public final static PSQLState CONNECTION_UNABLE_TO_CONNECT = new PSQLState("08001");
  public final static PSQLState CONNECTION_DOES_NOT_EXIST = new PSQLState("08003");
  public final static PSQLState CONNECTION_REJECTED = new PSQLState("08004");
  public final static PSQLState CONNECTION_FAILURE = new PSQLState("08006");
  public final static PSQLState CONNECTION_FAILURE_DURING_TRANSACTION = new PSQLState("08007");
	public final static PSQLState COMMUNICATION_ERROR = new PSQLState("08S01");
  public final static PSQLState NOT_IMPLEMENTED = new PSQLState("0A000");
  public final static PSQLState DATA_ERROR = new PSQLState("22000");
  public final static PSQLState NUMERIC_VALUE_OUT_OF_RANGE = new PSQLState("22003");
  public final static PSQLState BAD_DATETIME_FORMAT = new PSQLState("22007");
  public final static PSQLState MOST_SPECIFIC_TYPE_DOES_NOT_MATCH = new PSQLState("2200G");
  public final static PSQLState INVALID_PARAMETER_VALUE = new PSQLState("22023");
	public final static PSQLState TRANSACTION_STATE_INVALID = new PSQLState("25000");
  public final static PSQLState STATEMENT_NOT_ALLOWED_IN_FUNCTION_CALL = new PSQLState("2F003");
  public final static PSQLState NUMERIC_CONSTANT_OUT_OF_RANGE = new PSQLState("42820'");
  public final static PSQLState DATA_TYPE_MISMATCH = new PSQLState("42821");
  public final static PSQLState SYSTEM_ERROR = new PSQLState("60000");
  public final static PSQLState UNEXPECTED_ERROR = new PSQLState("99999");
	
}

/*-------------------------------------------------------------------------
 *
 * plerrcodes.h
 *	  PL/pgSQL error codes (mapping of exception labels to SQLSTATEs)
 *
 * Eventually this header file should be auto-generated from errcodes.h
 * with some sort of sed hackery, but no time for that now.  It's likely
 * that an exact mapping will not be what's wanted anyhow ...
 *
 * Copyright (c) 2003-2006, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/pl/plpgsql/src/plerrcodes.h,v 1.9 2006/06/16 23:29:27 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

/* Success and warnings can't be caught, so omit them from table */
{
	"sql_statement_not_yet_complete", ERRCODE_SQL_STATEMENT_NOT_YET_COMPLETE
},

{
	"connection_exception", ERRCODE_CONNECTION_EXCEPTION
},

{
	"connection_does_not_exist", ERRCODE_CONNECTION_DOES_NOT_EXIST
},

{
	"connection_failure", ERRCODE_CONNECTION_FAILURE
},

{
	"sqlclient_unable_to_establish_sqlconnection", ERRCODE_SQLCLIENT_UNABLE_TO_ESTABLISH_SQLCONNECTION
},

{
	"sqlserver_rejected_establishment_of_sqlconnection", ERRCODE_SQLSERVER_REJECTED_ESTABLISHMENT_OF_SQLCONNECTION
},

{
	"transaction_resolution_unknown", ERRCODE_TRANSACTION_RESOLUTION_UNKNOWN
},

{
	"protocol_violation", ERRCODE_PROTOCOL_VIOLATION
},

{
	"triggered_action_exception", ERRCODE_TRIGGERED_ACTION_EXCEPTION
},

{
	"feature_not_supported", ERRCODE_FEATURE_NOT_SUPPORTED
},

{
	"invalid_transaction_initiation", ERRCODE_INVALID_TRANSACTION_INITIATION
},

{
	"locator_exception", ERRCODE_LOCATOR_EXCEPTION
},

{
	"invalid_locator_specification", ERRCODE_L_E_INVALID_SPECIFICATION
},

{
	"invalid_grantor", ERRCODE_INVALID_GRANTOR
},

{
	"invalid_grant_operation", ERRCODE_INVALID_GRANT_OPERATION
},

{
	"invalid_role_specification", ERRCODE_INVALID_ROLE_SPECIFICATION
},

{
	"cardinality_violation", ERRCODE_CARDINALITY_VIOLATION
},

{
	"data_exception", ERRCODE_DATA_EXCEPTION
},

{
	"array_element_error", ERRCODE_ARRAY_ELEMENT_ERROR
},

{
	"array_subscript_error", ERRCODE_ARRAY_SUBSCRIPT_ERROR
},

{
	"character_not_in_repertoire", ERRCODE_CHARACTER_NOT_IN_REPERTOIRE
},

{
	"datetime_field_overflow", ERRCODE_DATETIME_FIELD_OVERFLOW
},

{
	"datetime_value_out_of_range", ERRCODE_DATETIME_VALUE_OUT_OF_RANGE
},

{
	"division_by_zero", ERRCODE_DIVISION_BY_ZERO
},

{
	"error_in_assignment", ERRCODE_ERROR_IN_ASSIGNMENT
},

{
	"escape_character_conflict", ERRCODE_ESCAPE_CHARACTER_CONFLICT
},

{
	"indicator_overflow", ERRCODE_INDICATOR_OVERFLOW
},

{
	"interval_field_overflow", ERRCODE_INTERVAL_FIELD_OVERFLOW
},

{
	"invalid_argument_for_logarithm", ERRCODE_INVALID_ARGUMENT_FOR_LOG
},

{
	"invalid_argument_for_power_function", ERRCODE_INVALID_ARGUMENT_FOR_POWER_FUNCTION
},

{
	"invalid_argument_for_width_bucket_function", ERRCODE_INVALID_ARGUMENT_FOR_WIDTH_BUCKET_FUNCTION
},

{
	"invalid_character_value_for_cast", ERRCODE_INVALID_CHARACTER_VALUE_FOR_CAST
},

{
	"invalid_datetime_format", ERRCODE_INVALID_DATETIME_FORMAT
},

{
	"invalid_escape_character", ERRCODE_INVALID_ESCAPE_CHARACTER
},

{
	"invalid_escape_octet", ERRCODE_INVALID_ESCAPE_OCTET
},

{
	"invalid_escape_sequence", ERRCODE_INVALID_ESCAPE_SEQUENCE
},

{
	"invalid_indicator_parameter_value", ERRCODE_INVALID_INDICATOR_PARAMETER_VALUE
},

{
	"invalid_limit_value", ERRCODE_INVALID_LIMIT_VALUE
},

{
	"invalid_parameter_value", ERRCODE_INVALID_PARAMETER_VALUE
},

{
	"invalid_regular_expression", ERRCODE_INVALID_REGULAR_EXPRESSION
},

{
	"invalid_time_zone_displacement_value", ERRCODE_INVALID_TIME_ZONE_DISPLACEMENT_VALUE
},

{
	"invalid_use_of_escape_character", ERRCODE_INVALID_USE_OF_ESCAPE_CHARACTER
},

{
	"most_specific_type_mismatch", ERRCODE_MOST_SPECIFIC_TYPE_MISMATCH
},

{
	"null_value_not_allowed", ERRCODE_NULL_VALUE_NOT_ALLOWED
},

{
	"null_value_no_indicator_parameter", ERRCODE_NULL_VALUE_NO_INDICATOR_PARAMETER
},

{
	"numeric_value_out_of_range", ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE
},

{
	"string_data_length_mismatch", ERRCODE_STRING_DATA_LENGTH_MISMATCH
},

{
	"string_data_right_truncation", ERRCODE_STRING_DATA_RIGHT_TRUNCATION
},

{
	"substring_error", ERRCODE_SUBSTRING_ERROR
},

{
	"trim_error", ERRCODE_TRIM_ERROR
},

{
	"unterminated_c_string", ERRCODE_UNTERMINATED_C_STRING
},

{
	"zero_length_character_string", ERRCODE_ZERO_LENGTH_CHARACTER_STRING
},

{
	"floating_point_exception", ERRCODE_FLOATING_POINT_EXCEPTION
},

{
	"invalid_text_representation", ERRCODE_INVALID_TEXT_REPRESENTATION
},

{
	"invalid_binary_representation", ERRCODE_INVALID_BINARY_REPRESENTATION
},

{
	"bad_copy_file_format", ERRCODE_BAD_COPY_FILE_FORMAT
},

{
	"untranslatable_character", ERRCODE_UNTRANSLATABLE_CHARACTER
},

{
	"integrity_constraint_violation", ERRCODE_INTEGRITY_CONSTRAINT_VIOLATION
},

{
	"restrict_violation", ERRCODE_RESTRICT_VIOLATION
},

{
	"not_null_violation", ERRCODE_NOT_NULL_VIOLATION
},

{
	"foreign_key_violation", ERRCODE_FOREIGN_KEY_VIOLATION
},

{
	"unique_violation", ERRCODE_UNIQUE_VIOLATION
},

{
	"check_violation", ERRCODE_CHECK_VIOLATION
},

{
	"invalid_cursor_state", ERRCODE_INVALID_CURSOR_STATE
},

{
	"invalid_transaction_state", ERRCODE_INVALID_TRANSACTION_STATE
},

{
	"active_sql_transaction", ERRCODE_ACTIVE_SQL_TRANSACTION
},

{
	"branch_transaction_already_active", ERRCODE_BRANCH_TRANSACTION_ALREADY_ACTIVE
},

{
	"held_cursor_requires_same_isolation_level", ERRCODE_HELD_CURSOR_REQUIRES_SAME_ISOLATION_LEVEL
},

{
	"inappropriate_access_mode_for_branch_transaction", ERRCODE_INAPPROPRIATE_ACCESS_MODE_FOR_BRANCH_TRANSACTION
},

{
	"inappropriate_isolation_level_for_branch_transaction", ERRCODE_INAPPROPRIATE_ISOLATION_LEVEL_FOR_BRANCH_TRANSACTION
},

{
	"no_active_sql_transaction_for_branch_transaction", ERRCODE_NO_ACTIVE_SQL_TRANSACTION_FOR_BRANCH_TRANSACTION
},

{
	"read_only_sql_transaction", ERRCODE_READ_ONLY_SQL_TRANSACTION
},

{
	"schema_and_data_statement_mixing_not_supported", ERRCODE_SCHEMA_AND_DATA_STATEMENT_MIXING_NOT_SUPPORTED
},

{
	"no_active_sql_transaction", ERRCODE_NO_ACTIVE_SQL_TRANSACTION
},

{
	"in_failed_sql_transaction", ERRCODE_IN_FAILED_SQL_TRANSACTION
},

{
	"invalid_sql_statement_name", ERRCODE_INVALID_SQL_STATEMENT_NAME
},

{
	"triggered_data_change_violation", ERRCODE_TRIGGERED_DATA_CHANGE_VIOLATION
},

{
	"invalid_authorization_specification", ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION
},

{
	"dependent_privilege_descriptors_still_exist", ERRCODE_DEPENDENT_PRIVILEGE_DESCRIPTORS_STILL_EXIST
},

{
	"dependent_objects_still_exist", ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST
},

{
	"invalid_transaction_termination", ERRCODE_INVALID_TRANSACTION_TERMINATION
},

{
	"sql_routine_exception", ERRCODE_SQL_ROUTINE_EXCEPTION
},

{
	"function_executed_no_return_statement", ERRCODE_S_R_E_FUNCTION_EXECUTED_NO_RETURN_STATEMENT
},

{
	"modifying_sql_data_not_permitted", ERRCODE_S_R_E_MODIFYING_SQL_DATA_NOT_PERMITTED
},

{
	"prohibited_sql_statement_attempted", ERRCODE_S_R_E_PROHIBITED_SQL_STATEMENT_ATTEMPTED
},

{
	"reading_sql_data_not_permitted", ERRCODE_S_R_E_READING_SQL_DATA_NOT_PERMITTED
},

{
	"invalid_cursor_name", ERRCODE_INVALID_CURSOR_NAME
},

{
	"external_routine_exception", ERRCODE_EXTERNAL_ROUTINE_EXCEPTION
},

{
	"containing_sql_not_permitted", ERRCODE_E_R_E_CONTAINING_SQL_NOT_PERMITTED
},

{
	"modifying_sql_data_not_permitted", ERRCODE_E_R_E_MODIFYING_SQL_DATA_NOT_PERMITTED
},

{
	"prohibited_sql_statement_attempted", ERRCODE_E_R_E_PROHIBITED_SQL_STATEMENT_ATTEMPTED
},

{
	"reading_sql_data_not_permitted", ERRCODE_E_R_E_READING_SQL_DATA_NOT_PERMITTED
},

{
	"external_routine_invocation_exception", ERRCODE_EXTERNAL_ROUTINE_INVOCATION_EXCEPTION
},

{
	"invalid_sqlstate_returned", ERRCODE_E_R_I_E_INVALID_SQLSTATE_RETURNED
},

{
	"null_value_not_allowed", ERRCODE_E_R_I_E_NULL_VALUE_NOT_ALLOWED
},

{
	"trigger_protocol_violated", ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED
},

{
	"srf_protocol_violated", ERRCODE_E_R_I_E_SRF_PROTOCOL_VIOLATED
},

{
	"savepoint_exception", ERRCODE_SAVEPOINT_EXCEPTION
},

{
	"invalid_savepoint_specification", ERRCODE_S_E_INVALID_SPECIFICATION
},

{
	"invalid_catalog_name", ERRCODE_INVALID_CATALOG_NAME
},

{
	"invalid_schema_name", ERRCODE_INVALID_SCHEMA_NAME
},

{
	"transaction_rollback", ERRCODE_TRANSACTION_ROLLBACK
},

{
	"transaction_integrity_constraint_violation", ERRCODE_T_R_INTEGRITY_CONSTRAINT_VIOLATION
},

{
	"serialization_failure", ERRCODE_T_R_SERIALIZATION_FAILURE
},

{
	"statement_completion_unknown", ERRCODE_T_R_STATEMENT_COMPLETION_UNKNOWN
},

{
	"deadlock_detected", ERRCODE_T_R_DEADLOCK_DETECTED
},

{
	"syntax_error_or_access_rule_violation", ERRCODE_SYNTAX_ERROR_OR_ACCESS_RULE_VIOLATION
},

{
	"syntax_error", ERRCODE_SYNTAX_ERROR
},

{
	"insufficient_privilege", ERRCODE_INSUFFICIENT_PRIVILEGE
},

{
	"cannot_coerce", ERRCODE_CANNOT_COERCE
},

{
	"grouping_error", ERRCODE_GROUPING_ERROR
},

{
	"invalid_foreign_key", ERRCODE_INVALID_FOREIGN_KEY
},

{
	"invalid_name", ERRCODE_INVALID_NAME
},

{
	"name_too_long", ERRCODE_NAME_TOO_LONG
},

{
	"reserved_name", ERRCODE_RESERVED_NAME
},

{
	"datatype_mismatch", ERRCODE_DATATYPE_MISMATCH
},

{
	"indeterminate_datatype", ERRCODE_INDETERMINATE_DATATYPE
},

{
	"wrong_object_type", ERRCODE_WRONG_OBJECT_TYPE
},

{
	"undefined_column", ERRCODE_UNDEFINED_COLUMN
},

{
	"undefined_cursor", ERRCODE_UNDEFINED_CURSOR
},

{
	"undefined_database", ERRCODE_UNDEFINED_DATABASE
},

{
	"undefined_function", ERRCODE_UNDEFINED_FUNCTION
},

{
	"undefined_pstatement", ERRCODE_UNDEFINED_PSTATEMENT
},

{
	"undefined_schema", ERRCODE_UNDEFINED_SCHEMA
},

{
	"undefined_table", ERRCODE_UNDEFINED_TABLE
},

{
	"undefined_parameter", ERRCODE_UNDEFINED_PARAMETER
},

{
	"undefined_object", ERRCODE_UNDEFINED_OBJECT
},

{
	"duplicate_column", ERRCODE_DUPLICATE_COLUMN
},

{
	"duplicate_cursor", ERRCODE_DUPLICATE_CURSOR
},

{
	"duplicate_database", ERRCODE_DUPLICATE_DATABASE
},

{
	"duplicate_function", ERRCODE_DUPLICATE_FUNCTION
},

{
	"duplicate_prepared_statement", ERRCODE_DUPLICATE_PSTATEMENT
},

{
	"duplicate_schema", ERRCODE_DUPLICATE_SCHEMA
},

{
	"duplicate_table", ERRCODE_DUPLICATE_TABLE
},

{
	"duplicate_alias", ERRCODE_DUPLICATE_ALIAS
},

{
	"duplicate_object", ERRCODE_DUPLICATE_OBJECT
},

{
	"ambiguous_column", ERRCODE_AMBIGUOUS_COLUMN
},

{
	"ambiguous_function", ERRCODE_AMBIGUOUS_FUNCTION
},

{
	"ambiguous_parameter", ERRCODE_AMBIGUOUS_PARAMETER
},

{
	"ambiguous_alias", ERRCODE_AMBIGUOUS_ALIAS
},

{
	"invalid_column_reference", ERRCODE_INVALID_COLUMN_REFERENCE
},

{
	"invalid_column_definition", ERRCODE_INVALID_COLUMN_DEFINITION
},

{
	"invalid_cursor_definition", ERRCODE_INVALID_CURSOR_DEFINITION
},

{
	"invalid_database_definition", ERRCODE_INVALID_DATABASE_DEFINITION
},

{
	"invalid_function_definition", ERRCODE_INVALID_FUNCTION_DEFINITION
},

{
	"invalid_prepared_statement_definition", ERRCODE_INVALID_PSTATEMENT_DEFINITION
},

{
	"invalid_schema_definition", ERRCODE_INVALID_SCHEMA_DEFINITION
},

{
	"invalid_table_definition", ERRCODE_INVALID_TABLE_DEFINITION
},

{
	"invalid_object_definition", ERRCODE_INVALID_OBJECT_DEFINITION
},

{
	"with_check_option_violation", ERRCODE_WITH_CHECK_OPTION_VIOLATION
},

{
	"insufficient_resources", ERRCODE_INSUFFICIENT_RESOURCES
},

{
	"disk_full", ERRCODE_DISK_FULL
},

{
	"out_of_memory", ERRCODE_OUT_OF_MEMORY
},

{
	"too_many_connections", ERRCODE_TOO_MANY_CONNECTIONS
},

{
	"program_limit_exceeded", ERRCODE_PROGRAM_LIMIT_EXCEEDED
},

{
	"statement_too_complex", ERRCODE_STATEMENT_TOO_COMPLEX
},

{
	"too_many_columns", ERRCODE_TOO_MANY_COLUMNS
},

{
	"too_many_arguments", ERRCODE_TOO_MANY_ARGUMENTS
},

{
	"object_not_in_prerequisite_state", ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE
},

{
	"object_in_use", ERRCODE_OBJECT_IN_USE
},

{
	"cant_change_runtime_param", ERRCODE_CANT_CHANGE_RUNTIME_PARAM
},

{
	"lock_not_available", ERRCODE_LOCK_NOT_AVAILABLE
},

{
	"operator_intervention", ERRCODE_OPERATOR_INTERVENTION
},

{
	"query_canceled", ERRCODE_QUERY_CANCELED
},

{
	"admin_shutdown", ERRCODE_ADMIN_SHUTDOWN
},

{
	"crash_shutdown", ERRCODE_CRASH_SHUTDOWN
},

{
	"cannot_connect_now", ERRCODE_CANNOT_CONNECT_NOW
},

{
	"io_error", ERRCODE_IO_ERROR
},

{
	"undefined_file", ERRCODE_UNDEFINED_FILE
},

{
	"duplicate_file", ERRCODE_DUPLICATE_FILE
},

{
	"config_file_error", ERRCODE_CONFIG_FILE_ERROR
},

{
	"lock_file_exists", ERRCODE_LOCK_FILE_EXISTS
},

{
	"plpgsql_error", ERRCODE_PLPGSQL_ERROR
},

{
	"raise_exception", ERRCODE_RAISE_EXCEPTION
},

{
	"no_data_found", ERRCODE_NO_DATA_FOUND
},

{
	"too_many_rows", ERRCODE_TOO_MANY_ROWS
},

{
	"internal_error", ERRCODE_INTERNAL_ERROR
},

{
	"data_corrupted", ERRCODE_DATA_CORRUPTED
},

{
	"index_corrupted", ERRCODE_INDEX_CORRUPTED
},

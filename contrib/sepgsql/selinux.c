/* -------------------------------------------------------------------------
 *
 * contrib/sepgsql/selinux.c
 *
 * Interactions between userspace and selinux in kernelspace,
 * using libselinux api.
 *
 * Copyright (c) 2010-2016, PostgreSQL Global Development Group
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lib/stringinfo.h"

#include "sepgsql.h"

/*
 * selinux_catalog
 *
 * This mapping table enables to translate the name of object classes and
 * access vectors to/from their own codes.
 * When we ask SELinux whether the required privileges are allowed or not,
 * we use security_compute_av(3). It needs us to represent object classes
 * and access vectors using 'external' codes defined in the security policy.
 * It is determinded in the runtime, not build time. So, it needs an internal
 * service to translate object class/access vectors which we want to check
 * into the code which kernel want to be given.
 */
static struct
{
	const char *class_name;
	uint16		class_code;
	struct
	{
		const char *av_name;
		uint32		av_code;
	}			av[32];
}	selinux_catalog[] =

{
	{
		"process", SEPG_CLASS_PROCESS,
		{
			{
				"transition", SEPG_PROCESS__TRANSITION
			},
			{
				"dyntransition", SEPG_PROCESS__DYNTRANSITION
			},
			{
				"setcurrent", SEPG_PROCESS__SETCURRENT
			},
			{
				NULL, 0UL
			}
		}
	},
	{
		"file", SEPG_CLASS_FILE,
		{
			{
				"read", SEPG_FILE__READ
			},
			{
				"write", SEPG_FILE__WRITE
			},
			{
				"create", SEPG_FILE__CREATE
			},
			{
				"getattr", SEPG_FILE__GETATTR
			},
			{
				"unlink", SEPG_FILE__UNLINK
			},
			{
				"rename", SEPG_FILE__RENAME
			},
			{
				"append", SEPG_FILE__APPEND
			},
			{
				NULL, 0UL
			}
		}
	},
	{
		"dir", SEPG_CLASS_DIR,
		{
			{
				"read", SEPG_DIR__READ
			},
			{
				"write", SEPG_DIR__WRITE
			},
			{
				"create", SEPG_DIR__CREATE
			},
			{
				"getattr", SEPG_DIR__GETATTR
			},
			{
				"unlink", SEPG_DIR__UNLINK
			},
			{
				"rename", SEPG_DIR__RENAME
			},
			{
				"search", SEPG_DIR__SEARCH
			},
			{
				"add_name", SEPG_DIR__ADD_NAME
			},
			{
				"remove_name", SEPG_DIR__REMOVE_NAME
			},
			{
				"rmdir", SEPG_DIR__RMDIR
			},
			{
				"reparent", SEPG_DIR__REPARENT
			},
			{
				NULL, 0UL
			}
		}
	},
	{
		"lnk_file", SEPG_CLASS_LNK_FILE,
		{
			{
				"read", SEPG_LNK_FILE__READ
			},
			{
				"write", SEPG_LNK_FILE__WRITE
			},
			{
				"create", SEPG_LNK_FILE__CREATE
			},
			{
				"getattr", SEPG_LNK_FILE__GETATTR
			},
			{
				"unlink", SEPG_LNK_FILE__UNLINK
			},
			{
				"rename", SEPG_LNK_FILE__RENAME
			},
			{
				NULL, 0UL
			}
		}
	},
	{
		"chr_file", SEPG_CLASS_CHR_FILE,
		{
			{
				"read", SEPG_CHR_FILE__READ
			},
			{
				"write", SEPG_CHR_FILE__WRITE
			},
			{
				"create", SEPG_CHR_FILE__CREATE
			},
			{
				"getattr", SEPG_CHR_FILE__GETATTR
			},
			{
				"unlink", SEPG_CHR_FILE__UNLINK
			},
			{
				"rename", SEPG_CHR_FILE__RENAME
			},
			{
				NULL, 0UL
			}
		}
	},
	{
		"blk_file", SEPG_CLASS_BLK_FILE,
		{
			{
				"read", SEPG_BLK_FILE__READ
			},
			{
				"write", SEPG_BLK_FILE__WRITE
			},
			{
				"create", SEPG_BLK_FILE__CREATE
			},
			{
				"getattr", SEPG_BLK_FILE__GETATTR
			},
			{
				"unlink", SEPG_BLK_FILE__UNLINK
			},
			{
				"rename", SEPG_BLK_FILE__RENAME
			},
			{
				NULL, 0UL
			}
		}
	},
	{
		"sock_file", SEPG_CLASS_SOCK_FILE,
		{
			{
				"read", SEPG_SOCK_FILE__READ
			},
			{
				"write", SEPG_SOCK_FILE__WRITE
			},
			{
				"create", SEPG_SOCK_FILE__CREATE
			},
			{
				"getattr", SEPG_SOCK_FILE__GETATTR
			},
			{
				"unlink", SEPG_SOCK_FILE__UNLINK
			},
			{
				"rename", SEPG_SOCK_FILE__RENAME
			},
			{
				NULL, 0UL
			}
		}
	},
	{
		"fifo_file", SEPG_CLASS_FIFO_FILE,
		{
			{
				"read", SEPG_FIFO_FILE__READ
			},
			{
				"write", SEPG_FIFO_FILE__WRITE
			},
			{
				"create", SEPG_FIFO_FILE__CREATE
			},
			{
				"getattr", SEPG_FIFO_FILE__GETATTR
			},
			{
				"unlink", SEPG_FIFO_FILE__UNLINK
			},
			{
				"rename", SEPG_FIFO_FILE__RENAME
			},
			{
				NULL, 0UL
			}
		}
	},
	{
		"db_database", SEPG_CLASS_DB_DATABASE,
		{
			{
				"create", SEPG_DB_DATABASE__CREATE
			},
			{
				"drop", SEPG_DB_DATABASE__DROP
			},
			{
				"getattr", SEPG_DB_DATABASE__GETATTR
			},
			{
				"setattr", SEPG_DB_DATABASE__SETATTR
			},
			{
				"relabelfrom", SEPG_DB_DATABASE__RELABELFROM
			},
			{
				"relabelto", SEPG_DB_DATABASE__RELABELTO
			},
			{
				"access", SEPG_DB_DATABASE__ACCESS
			},
			{
				"load_module", SEPG_DB_DATABASE__LOAD_MODULE
			},
			{
				NULL, 0UL
			},
		}
	},
	{
		"db_schema", SEPG_CLASS_DB_SCHEMA,
		{
			{
				"create", SEPG_DB_SCHEMA__CREATE
			},
			{
				"drop", SEPG_DB_SCHEMA__DROP
			},
			{
				"getattr", SEPG_DB_SCHEMA__GETATTR
			},
			{
				"setattr", SEPG_DB_SCHEMA__SETATTR
			},
			{
				"relabelfrom", SEPG_DB_SCHEMA__RELABELFROM
			},
			{
				"relabelto", SEPG_DB_SCHEMA__RELABELTO
			},
			{
				"search", SEPG_DB_SCHEMA__SEARCH
			},
			{
				"add_name", SEPG_DB_SCHEMA__ADD_NAME
			},
			{
				"remove_name", SEPG_DB_SCHEMA__REMOVE_NAME
			},
			{
				NULL, 0UL
			},
		}
	},
	{
		"db_table", SEPG_CLASS_DB_TABLE,
		{
			{
				"create", SEPG_DB_TABLE__CREATE
			},
			{
				"drop", SEPG_DB_TABLE__DROP
			},
			{
				"getattr", SEPG_DB_TABLE__GETATTR
			},
			{
				"setattr", SEPG_DB_TABLE__SETATTR
			},
			{
				"relabelfrom", SEPG_DB_TABLE__RELABELFROM
			},
			{
				"relabelto", SEPG_DB_TABLE__RELABELTO
			},
			{
				"select", SEPG_DB_TABLE__SELECT
			},
			{
				"update", SEPG_DB_TABLE__UPDATE
			},
			{
				"insert", SEPG_DB_TABLE__INSERT
			},
			{
				"delete", SEPG_DB_TABLE__DELETE
			},
			{
				"lock", SEPG_DB_TABLE__LOCK
			},
			{
				NULL, 0UL
			},
		}
	},
	{
		"db_sequence", SEPG_CLASS_DB_SEQUENCE,
		{
			{
				"create", SEPG_DB_SEQUENCE__CREATE
			},
			{
				"drop", SEPG_DB_SEQUENCE__DROP
			},
			{
				"getattr", SEPG_DB_SEQUENCE__GETATTR
			},
			{
				"setattr", SEPG_DB_SEQUENCE__SETATTR
			},
			{
				"relabelfrom", SEPG_DB_SEQUENCE__RELABELFROM
			},
			{
				"relabelto", SEPG_DB_SEQUENCE__RELABELTO
			},
			{
				"get_value", SEPG_DB_SEQUENCE__GET_VALUE
			},
			{
				"next_value", SEPG_DB_SEQUENCE__NEXT_VALUE
			},
			{
				"set_value", SEPG_DB_SEQUENCE__SET_VALUE
			},
			{
				NULL, 0UL
			},
		}
	},
	{
		"db_procedure", SEPG_CLASS_DB_PROCEDURE,
		{
			{
				"create", SEPG_DB_PROCEDURE__CREATE
			},
			{
				"drop", SEPG_DB_PROCEDURE__DROP
			},
			{
				"getattr", SEPG_DB_PROCEDURE__GETATTR
			},
			{
				"setattr", SEPG_DB_PROCEDURE__SETATTR
			},
			{
				"relabelfrom", SEPG_DB_PROCEDURE__RELABELFROM
			},
			{
				"relabelto", SEPG_DB_PROCEDURE__RELABELTO
			},
			{
				"execute", SEPG_DB_PROCEDURE__EXECUTE
			},
			{
				"entrypoint", SEPG_DB_PROCEDURE__ENTRYPOINT
			},
			{
				"install", SEPG_DB_PROCEDURE__INSTALL
			},
			{
				NULL, 0UL
			},
		}
	},
	{
		"db_column", SEPG_CLASS_DB_COLUMN,
		{
			{
				"create", SEPG_DB_COLUMN__CREATE
			},
			{
				"drop", SEPG_DB_COLUMN__DROP
			},
			{
				"getattr", SEPG_DB_COLUMN__GETATTR
			},
			{
				"setattr", SEPG_DB_COLUMN__SETATTR
			},
			{
				"relabelfrom", SEPG_DB_COLUMN__RELABELFROM
			},
			{
				"relabelto", SEPG_DB_COLUMN__RELABELTO
			},
			{
				"select", SEPG_DB_COLUMN__SELECT
			},
			{
				"update", SEPG_DB_COLUMN__UPDATE
			},
			{
				"insert", SEPG_DB_COLUMN__INSERT
			},
			{
				NULL, 0UL
			},
		}
	},
	{
		"db_tuple", SEPG_CLASS_DB_TUPLE,
		{
			{
				"relabelfrom", SEPG_DB_TUPLE__RELABELFROM
			},
			{
				"relabelto", SEPG_DB_TUPLE__RELABELTO
			},
			{
				"select", SEPG_DB_TUPLE__SELECT
			},
			{
				"update", SEPG_DB_TUPLE__UPDATE
			},
			{
				"insert", SEPG_DB_TUPLE__INSERT
			},
			{
				"delete", SEPG_DB_TUPLE__DELETE
			},
			{
				NULL, 0UL
			},
		}
	},
	{
		"db_blob", SEPG_CLASS_DB_BLOB,
		{
			{
				"create", SEPG_DB_BLOB__CREATE
			},
			{
				"drop", SEPG_DB_BLOB__DROP
			},
			{
				"getattr", SEPG_DB_BLOB__GETATTR
			},
			{
				"setattr", SEPG_DB_BLOB__SETATTR
			},
			{
				"relabelfrom", SEPG_DB_BLOB__RELABELFROM
			},
			{
				"relabelto", SEPG_DB_BLOB__RELABELTO
			},
			{
				"read", SEPG_DB_BLOB__READ
			},
			{
				"write", SEPG_DB_BLOB__WRITE
			},
			{
				"import", SEPG_DB_BLOB__IMPORT
			},
			{
				"export", SEPG_DB_BLOB__EXPORT
			},
			{
				NULL, 0UL
			},
		}
	},
	{
		"db_language", SEPG_CLASS_DB_LANGUAGE,
		{
			{
				"create", SEPG_DB_LANGUAGE__CREATE
			},
			{
				"drop", SEPG_DB_LANGUAGE__DROP
			},
			{
				"getattr", SEPG_DB_LANGUAGE__GETATTR
			},
			{
				"setattr", SEPG_DB_LANGUAGE__SETATTR
			},
			{
				"relabelfrom", SEPG_DB_LANGUAGE__RELABELFROM
			},
			{
				"relabelto", SEPG_DB_LANGUAGE__RELABELTO
			},
			{
				"implement", SEPG_DB_LANGUAGE__IMPLEMENT
			},
			{
				"execute", SEPG_DB_LANGUAGE__EXECUTE
			},
			{
				NULL, 0UL
			},
		}
	},
	{
		"db_view", SEPG_CLASS_DB_VIEW,
		{
			{
				"create", SEPG_DB_VIEW__CREATE
			},
			{
				"drop", SEPG_DB_VIEW__DROP
			},
			{
				"getattr", SEPG_DB_VIEW__GETATTR
			},
			{
				"setattr", SEPG_DB_VIEW__SETATTR
			},
			{
				"relabelfrom", SEPG_DB_VIEW__RELABELFROM
			},
			{
				"relabelto", SEPG_DB_VIEW__RELABELTO
			},
			{
				"expand", SEPG_DB_VIEW__EXPAND
			},
			{
				NULL, 0UL
			},
		}
	},
};

/*
 * sepgsql_mode
 *
 * SEPGSQL_MODE_DISABLED: Disabled on runtime
 * SEPGSQL_MODE_DEFAULT: Same as system settings
 * SEPGSQL_MODE_PERMISSIVE: Always permissive mode
 * SEPGSQL_MODE_INTERNAL: Same as permissive, except for no audit logs
 */
static int	sepgsql_mode = SEPGSQL_MODE_INTERNAL;

/*
 * sepgsql_is_enabled
 */
bool
sepgsql_is_enabled(void)
{
	return (sepgsql_mode != SEPGSQL_MODE_DISABLED ? true : false);
}

/*
 * sepgsql_get_mode
 */
int
sepgsql_get_mode(void)
{
	return sepgsql_mode;
}

/*
 * sepgsql_set_mode
 */
int
sepgsql_set_mode(int new_mode)
{
	int			old_mode = sepgsql_mode;

	sepgsql_mode = new_mode;

	return old_mode;
}

/*
 * sepgsql_getenforce
 *
 * It returns whether the current working mode tries to enforce access
 * control decision, or not. It shall be enforced when sepgsql_mode is
 * SEPGSQL_MODE_DEFAULT and system is running in enforcing mode.
 */
bool
sepgsql_getenforce(void)
{
	if (sepgsql_mode == SEPGSQL_MODE_DEFAULT &&
		selinux_status_getenforce() > 0)
		return true;

	return false;
}

/*
 * sepgsql_audit_log
 *
 * It generates a security audit record. In the default, it writes out
 * audit records into standard PG's logfile. It also allows to set up
 * external audit log receiver, such as auditd in Linux, using the
 * sepgsql_audit_hook.
 *
 * SELinux can control what should be audited and should not using
 * "auditdeny" and "auditallow" rules in the security policy. In the
 * default, all the access violations are audited, and all the access
 * allowed are not audited. But we can set up the security policy, so
 * we can have exceptions. So, it is necessary to follow the suggestion
 * come from the security policy. (av_decision.auditallow and auditdeny)
 *
 * Security audit is an important feature, because it enables us to check
 * what was happen if we have a security incident. In fact, ISO/IEC15408
 * defines several security functionalities for audit features.
 */
void
sepgsql_audit_log(bool denied,
				  const char *scontext,
				  const char *tcontext,
				  uint16 tclass,
				  uint32 audited,
				  const char *audit_name)
{
	StringInfoData buf;
	const char *class_name;
	const char *av_name;
	int			i;

	/* lookup name of the object class */
	Assert(tclass < SEPG_CLASS_MAX);
	class_name = selinux_catalog[tclass].class_name;

	/* lookup name of the permissions */
	initStringInfo(&buf);
	appendStringInfo(&buf, "%s {",
					 (denied ? "denied" : "allowed"));
	for (i = 0; selinux_catalog[tclass].av[i].av_name; i++)
	{
		if (audited & (1UL << i))
		{
			av_name = selinux_catalog[tclass].av[i].av_name;
			appendStringInfo(&buf, " %s", av_name);
		}
	}
	appendStringInfo(&buf, " }");

	/*
	 * Call external audit module, if loaded
	 */
	appendStringInfo(&buf, " scontext=%s tcontext=%s tclass=%s",
					 scontext, tcontext, class_name);
	if (audit_name)
		appendStringInfo(&buf, " name=\"%s\"", audit_name);

	ereport(LOG, (errmsg("SELinux: %s", buf.data)));
}

/*
 * sepgsql_compute_avd
 *
 * It actually asks SELinux what permissions are allowed on a pair of
 * the security contexts and object class. It also returns what permissions
 * should be audited on access violation or allowed.
 * In most cases, subject's security context (scontext) is a client, and
 * target security context (tcontext) is a database object.
 *
 * The access control decision shall be set on the given av_decision.
 * The av_decision.allowed has a bitmask of SEPG_<class>__<perms>
 * to suggest a set of allowed actions in this object class.
 */
void
sepgsql_compute_avd(const char *scontext,
					const char *tcontext,
					uint16 tclass,
					struct av_decision * avd)
{
	const char *tclass_name;
	security_class_t tclass_ex;
	struct av_decision avd_ex;
	int			i,
				deny_unknown = security_deny_unknown();

	/* Get external code of the object class */
	Assert(tclass < SEPG_CLASS_MAX);
	Assert(tclass == selinux_catalog[tclass].class_code);

	tclass_name = selinux_catalog[tclass].class_name;
	tclass_ex = string_to_security_class(tclass_name);

	if (tclass_ex == 0)
	{
		/*
		 * If the current security policy does not support permissions
		 * corresponding to database objects, we fill up them with dummy data.
		 * If security_deny_unknown() returns positive value, undefined
		 * permissions should be denied. Otherwise, allowed
		 */
		avd->allowed = (security_deny_unknown() > 0 ? 0 : ~0);
		avd->auditallow = 0U;
		avd->auditdeny = ~0U;
		avd->flags = 0;

		return;
	}

	/*
	 * Ask SELinux what is allowed set of permissions on a pair of the
	 * security contexts and the given object class.
	 */
	if (security_compute_av_flags_raw((security_context_t) scontext,
									  (security_context_t) tcontext,
									  tclass_ex, 0, &avd_ex) < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("SELinux could not compute av_decision: "
						"scontext=%s tcontext=%s tclass=%s: %m",
						scontext, tcontext, tclass_name)));

	/*
	 * SELinux returns its access control decision as a set of permissions
	 * represented in external code which depends on run-time environment. So,
	 * we need to translate it to the internal representation before returning
	 * results for the caller.
	 */
	memset(avd, 0, sizeof(struct av_decision));

	for (i = 0; selinux_catalog[tclass].av[i].av_name; i++)
	{
		access_vector_t av_code_ex;
		const char *av_name = selinux_catalog[tclass].av[i].av_name;
		uint32		av_code = selinux_catalog[tclass].av[i].av_code;

		av_code_ex = string_to_av_perm(tclass_ex, av_name);
		if (av_code_ex == 0)
		{
			/* fill up undefined permissions */
			if (!deny_unknown)
				avd->allowed |= av_code;
			avd->auditdeny |= av_code;

			continue;
		}

		if (avd_ex.allowed & av_code_ex)
			avd->allowed |= av_code;
		if (avd_ex.auditallow & av_code_ex)
			avd->auditallow |= av_code;
		if (avd_ex.auditdeny & av_code_ex)
			avd->auditdeny |= av_code;
	}

	return;
}

/*
 * sepgsql_compute_create
 *
 * It returns a default security context to be assigned on a new database
 * object. SELinux compute it based on a combination of client, upper object
 * which owns the new object and object class.
 *
 * For example, when a client (staff_u:staff_r:staff_t:s0) tries to create
 * a new table within a schema (system_u:object_r:sepgsql_schema_t:s0),
 * SELinux looks-up its security policy. If it has a special rule on the
 * combination of these security contexts and object class (db_table),
 * it returns the security context suggested by the special rule.
 * Otherwise, it returns the security context of schema, as is.
 *
 * We expect the caller already applies sanity/validation checks on the
 * given security context.
 *
 * scontext: security context of the subject (mostly, peer process).
 * tcontext: security context of the upper database object.
 * tclass: class code (SEPG_CLASS_*) of the new object in creation
 */
char *
sepgsql_compute_create(const char *scontext,
					   const char *tcontext,
					   uint16 tclass,
					   const char *objname)
{
	security_context_t ncontext;
	security_class_t tclass_ex;
	const char *tclass_name;
	char	   *result;

	/* Get external code of the object class */
	Assert(tclass < SEPG_CLASS_MAX);

	tclass_name = selinux_catalog[tclass].class_name;
	tclass_ex = string_to_security_class(tclass_name);

	/*
	 * Ask SELinux what is the default context for the given object class on a
	 * pair of security contexts
	 */
	if (security_compute_create_name_raw((security_context_t) scontext,
										 (security_context_t) tcontext,
										 tclass_ex,
										 objname,
										 &ncontext) < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("SELinux could not compute a new context: "
						"scontext=%s tcontext=%s tclass=%s: %m",
						scontext, tcontext, tclass_name)));

	/*
	 * libselinux returns malloc()'ed string, so we need to copy it on the
	 * palloc()'ed region.
	 */
	PG_TRY();
	{
		result = pstrdup(ncontext);
	}
	PG_CATCH();
	{
		freecon(ncontext);
		PG_RE_THROW();
	}
	PG_END_TRY();
	freecon(ncontext);

	return result;
}

/*
 * sepgsql_check_perms
 *
 * It makes access control decision without userspace caching mechanism.
 * If SELinux denied the required accesses on the pair of security labels,
 * it raises an error or returns false.
 *
 * scontext: security label of the subject (mostly, peer process)
 * tcontext: security label of the object being referenced
 * tclass: class code (SEPG_CLASS_*) of the object being referenced
 * required: a mask of required permissions (SEPG_<class>__<perm>)
 * audit_name: a human readable object name for audit logs, or NULL.
 * abort_on_violation: true, if error shall be raised on access violation
 */
bool
sepgsql_check_perms(const char *scontext,
					const char *tcontext,
					uint16 tclass,
					uint32 required,
					const char *audit_name,
					bool abort_on_violation)
{
	struct av_decision avd;
	uint32		denied;
	uint32		audited;
	bool		result = true;

	sepgsql_compute_avd(scontext, tcontext, tclass, &avd);

	denied = required & ~avd.allowed;

	if (sepgsql_get_debug_audit())
		audited = (denied ? denied : required);
	else
		audited = (denied ? (denied & avd.auditdeny)
				   : (required & avd.auditallow));

	if (denied &&
		sepgsql_getenforce() > 0 &&
		(avd.flags & SELINUX_AVD_FLAGS_PERMISSIVE) == 0)
		result = false;

	/*
	 * It records a security audit for the request, if needed. But, when
	 * SE-PgSQL performs 'internal' mode, it needs to keep silent.
	 */
	if (audited && sepgsql_mode != SEPGSQL_MODE_INTERNAL)
	{
		sepgsql_audit_log(denied,
						  scontext,
						  tcontext,
						  tclass,
						  audited,
						  audit_name);
	}

	if (!result && abort_on_violation)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("SELinux: security policy violation")));
	return result;
}

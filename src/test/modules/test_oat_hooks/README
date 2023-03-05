OVERVIEW
========

This test module, "test_oat_hooks", is an example of how to use the object
access hooks (OAT) to enforce mandatory access controls (MAC).

The testing strategy is as follows:  When this module loads, it registers hooks
of various types.  (See below.)  GUCs are defined to control each hook,
determining whether the hook allows or denies actions for which it fires.  A
single additional GUC controls the verbosity of the hooks.  GUCs default to
permissive/quiet, which allows the module to load without generating noise in
the log or denying any activity in the run-up to the regression test beginning.
When the test begins, it uses SET commands to turn on logging and to control
each hook's permissive/restrictive behavior.  Various SQL statements are run
under both superuser and ordinary user permissions.  The output is compared
against the expected output to verify that the hooks behaved and fired in the
order by expect.

Because users may care about the firing order of other system hooks relative to
OAT hooks, ProcessUtility hooks and ExecutorCheckPerms hooks are also
registered by this module, with their own logging and allow/deny behavior.


SUSET test configuration GUCs
=============================

The following configuration parameters (GUCs) control this test module's Object
Access Type (OAT), Process Utility and Executor Check Permissions hooks.  The
general pattern is that each hook has a corresponding GUC which controls
whether the hook will allow or deny operations for which the hook gets called.
A real-world OAT hook should certainly provide more fine-grained control than
merely "allow-all" vs. "deny-all", but for testing this is sufficient.

Note that even when these hooks allow an action, the core permissions system
may still refuse the action.  The firing order of the hooks relative to the
core permissions system can be inferred from which NOTICE messages get emitted
before an action is refused.

Each hook applies the allow vs. deny setting to all operations performed by
non-superusers.

- test_oat_hooks.deny_set_variable

  Controls whether the object_access_hook_str MAC function rejects attempts to
  set a configuration parameter.

- test_oat_hooks.deny_alter_system

  Controls whether the object_access_hook_str MAC function rejects attempts to
  alter system set a configuration parameter.

- test_oat_hooks.deny_object_access

  Controls whether the object_access_hook MAC function rejects all operations
  for which it is called.

- test_oat_hooks.deny_exec_perms

  Controls whether the exec_check_perms MAC function rejects all operations for
  which it is called.

- test_oat_hooks.deny_utility_commands

  Controls whether the ProcessUtility_hook function rejects all operations for
  which it is called.

- test_oat_hooks.audit

  Controls whether each hook logs NOTICE messages for each attempt, along with
  success or failure status.  Note that clearing or setting this GUC may itself
  generate NOTICE messages appearing before but not after, or after but not
  before, the new setting takes effect.


Functions
=========

The module registers hooks by the following names:

- REGRESS_object_access_hook

- REGRESS_object_access_hook_str

- REGRESS_exec_check_perms

- REGRESS_utility_command

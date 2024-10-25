# Basic isolation test for injection points.
#
# This checks the interactions between wakeup, wait and detach.
# Feel free to use it as a template when implementing an isolation
# test with injection points.

setup
{
	CREATE EXTENSION injection_points;
}
teardown
{
	DROP EXTENSION injection_points;
}

# Wait happens in the first session, wakeup in the second session.
session s1
setup	{
	SELECT injection_points_set_local();
	SELECT injection_points_attach('injection-points-wait', 'wait');
}
step wait1	{ SELECT injection_points_run('injection-points-wait'); }

session s2
step wakeup2	{ SELECT injection_points_wakeup('injection-points-wait'); }
step detach2	{ SELECT injection_points_detach('injection-points-wait'); }

# Detach after wait and wakeup.
# This permutation is proving to be unstable on FreeBSD, so disable for now.
#permutation wait1 wakeup2 detach2

# Detach before wakeup.  s1 waits until wakeup, ignores the detach.
permutation wait1 detach2 wakeup2

# Detach before wait does not cause a wait, wakeup produces an error.
permutation detach2 wait1 wakeup2

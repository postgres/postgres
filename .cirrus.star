"""Additional CI configuration, using the starlark language. See
https://cirrus-ci.org/guide/programming-tasks/#introduction-into-starlark

See also the starlark specification at
https://github.com/bazelbuild/starlark/blob/master/spec.md

See also .cirrus.yml and src/tools/ci/README
"""

load("cirrus", "env", "fs", "re", "yaml")


def main():
    """The main function is executed by cirrus-ci after loading .cirrus.yml and can
    extend the CI definition further.

    As documented in .cirrus.yml, the final CI configuration is composed of

    1) the contents of .cirrus.yml

    2) computed environment variables

    3) if defined, the contents of the file referenced by the, repository
       level, REPO_CI_CONFIG_GIT_URL variable (see
       https://cirrus-ci.org/guide/programming-tasks/#fs for the accepted
       format)

    4) .cirrus.tasks.yml
    """

    output = ""

    # 1) is evaluated implicitly


    # Add 2)
    additional_env = compute_environment_vars()
    env_fmt = """
###
# Computed environment variables start here
###
{0}
###
# Computed environment variables end here
###
"""
    output += env_fmt.format(yaml.dumps({'env': additional_env}))


    # Add 3)
    repo_config_url = env.get("REPO_CI_CONFIG_GIT_URL")
    if repo_config_url != None:
        print("loading additional configuration from \"{}\"".format(repo_config_url))
        output += config_from(repo_config_url)
    else:
        output += "\n# REPO_CI_CONFIG_URL was not set\n"


    # Add 4)
    output += config_from(".cirrus.tasks.yml")


    return output


def compute_environment_vars():
    cenv = {}

    ###
    # Some tasks are manually triggered by default because they might use too
    # many resources for users of free Cirrus credits, but they can be
    # triggered automatically by naming them in an environment variable e.g.
    # REPO_CI_AUTOMATIC_TRIGGER_TASKS="task_name other_task" under "Repository
    # Settings" on Cirrus CI's website.

    default_manual_trigger_tasks = []

    repo_ci_automatic_trigger_tasks = env.get('REPO_CI_AUTOMATIC_TRIGGER_TASKS', '')
    for task in default_manual_trigger_tasks:
        name = 'CI_TRIGGER_TYPE_' + task.upper()
        if repo_ci_automatic_trigger_tasks.find(task) != -1:
            value = 'automatic'
        else:
            value = 'manual'
        cenv[name] = value
    ###

    ###
    # Parse "ci-os-only:" tag in commit message and set
    # CI_{$OS}_ENABLED variable for each OS

    operating_systems = [
      'freebsd',
      'linux',
      'macos',
      'windows',
    ]
    commit_message = env.get('CIRRUS_CHANGE_MESSAGE')
    match_re = r"(^|.*\n)ci-os-only: ([^\n]+)($|\n.*)"

    # re.match() returns an array with a tuple of (matched-string, match_1, ...)
    m = re.match(match_re, commit_message)
    if m and len(m) > 0:
        os_only = m[0][2]
        os_only_list = re.split(r'[, ]+', os_only)
    else:
        os_only_list = operating_systems

    for os in operating_systems:
        os_enabled = os in os_only_list
        cenv['CI_{0}_ENABLED'.format(os.upper())] = os_enabled
    ###

    return cenv


def config_from(config_src):
    """return contents of config file `config_src`, surrounded by markers
    indicating start / end of the the included file
    """

    config_contents = fs.read(config_src)
    config_fmt = """

###
# contents of config file `{0}` start here
###
{1}
###
# contents of config file `{0}` end here
###
"""
    return config_fmt.format(config_src, config_contents)

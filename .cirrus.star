"""Additional CI configuration, using the starlark language. See
https://cirrus-ci.org/guide/programming-tasks/#introduction-into-starlark

See also the starlark specification at
https://github.com/bazelbuild/starlark/blob/master/spec.md

See also .cirrus.yml and src/tools/ci/README
"""

load("cirrus", "env", "fs")


def main():
    """The main function is executed by cirrus-ci after loading .cirrus.yml and can
    extend the CI definition further.

    As documented in .cirrus.yml, the final CI configuration is composed of

    1) the contents of .cirrus.yml

    2) if defined, the contents of the file referenced by the, repository
       level, REPO_CI_CONFIG_GIT_URL variable (see
       https://cirrus-ci.org/guide/programming-tasks/#fs for the accepted
       format)

    3) .cirrus.tasks.yml
    """

    output = ""

    # 1) is evaluated implicitly

    # Add 2)
    repo_config_url = env.get("REPO_CI_CONFIG_GIT_URL")
    if repo_config_url != None:
        print("loading additional configuration from \"{}\"".format(repo_config_url))
        output += config_from(repo_config_url)
    else:
        output += "\n# REPO_CI_CONFIG_URL was not set\n"

    # Add 3)
    output += config_from(".cirrus.tasks.yml")

    return output


def config_from(config_src):
    """return contents of config file `config_src`, surrounded by markers
    indicating start / end of the included file
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

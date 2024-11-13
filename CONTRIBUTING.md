# Contributing guide

Welcome to `pg_tde` - the Transparent Database Encryption for PostgreSQL!

We're glad that you would like to become a Percona community member and participate in keeping open source open.

You can contribute in one of the following ways:

1. Reach us on our [Forums](https://forums.percona.com/c/postgresql/pg-tde-transparent-data-encryption-tde/82).
2. [Submit a bug report or a feature request](#submit-a-bug-report-or-a-feature-request)
3. [Submit a pull request (PR) with the code patch](#submit-a-pull-request)
4. [Contribute to documentation](#contributing-to-documentation)

By contributing, you agree to the [Percona Community code of conduct](https://github.com/percona/community/blob/main/content/contribute/coc.md).


## Submit a bug report or a feature request

All bug reports, enhancements and feature requests are tracked in [Jira issue tracker](https://jira.percona.com/projects/PG). If you would like to suggest a new feature / an improvement or you found a bug in `pg_tde`, please submit the report to the [PG project](https://jira.percona.com/projects/PG/issues).

Start by searching the open tickets for a similar report. If you find that someone else has already reported your issue, then you can upvote that report to increase its visibility.

If there is no existing report, submit your report following these steps:

1. Sign in to [Jira issue tracker](https://jira.percona.com/projects/PG/issues). You will need to create an account if you do not have one.
2. In the _Summary_, _Description_, _Steps To Reproduce_, _Affects Version_ fields describe the problem you have detected or an idea that you have for a new feature or improvement.
3. As a general rule of thumb, try to create bug reports that are:

  * Reproducible: describe the steps to reproduce the problem.
  * Unique: check if there already exists a JIRA ticket to describe the problem.
  * Scoped to a Single Bug: only report one bug in one JIRA ticket

## Submit a pull request

Though not mandatory, we encourage you to first check for a bug report among Jira issues and in the PR list: perhaps the bug has already been addressed.

For feature requests and enhancements, we do ask you to create a Jira issue, describe your idea and discuss the design with us. This way we align your ideas with our vision for the product development.

If the bug hasn’t been reported / addressed, or we’ve agreed on the enhancement implementation with you, do the following:

1. [Fork](https://docs.github.com/en/github/getting-started-with-github/fork-a-repo) this repository
2. Clone this repository on your machine.
3. Create a separate branch for your changes. If you work on a Jira issue, please include the issue number in the branch name so it reads as `<JIRAISSUE>-my_branch`. This makes it easier to track your contribution.
4. Make your changes. Please follow the guidelines outlined in the [PostgreSQL Coding Standard](https://www.postgresql.org/docs/current/source.html) to improve code readability.
   <details>
    <summary>.vimrc configuration example</summary> 

    ```
    set nocompatible                " choose no compatibility with legacy vi
    syntax enableset 
    tabstop=4set 
    background=lightset 
    textwidth=80set 
    colorcolumn=80
    let g:filestyle_ignore_patterns = ['^\t* \{1,3}\S']
    highlight Normal ctermbg=15
    highlight ColorColumn ctermbg=52 
    ```
  </details>

5. Test your changes locally. See the [Running tests ](#running-tests) section for more information
6. Update the documentation describing your changes. See the [Contributing to documentation](#contributing-to-documentation) section for details
7. Commit the changes. Add the Jira issue number at the beginning of your message subject, so that is reads as `<JIRAISSUE> : My commit message`.  Follow this pattern for your commits:

    ```
    PG-1234:  Main commit message.
    <Blank line>
    Details of fix.
    ```

    The [commit message guidelines](https://gist.github.com/robertpainsi/b632364184e70900af4ab688decf6f53) will help you with writing great commit messages

8. Open a pull request to Percona
9. Our team will review your code and if everything is correct, will merge it. Otherwise, we will contact you for additional information or with the request to make changes.

### Building pg_tde

To build `pg_tde` from source code, you require the following:

* git
* make
* gcc
* pg_config

Refer to the [Building from source code](https://github.com/percona/pg_tde?tab=readme-ov-file#building-from-sources-for-community-postgresql) section for guidelines.


### Running tests 

When you work, you should periodically run tests to check that your changes don’t break existing code.

You can find the tests in the `sql` directory.

#### Run manually

1. Change directory to pg_tde

**NOTE**: Make sure `postgres` user is the owner of the `pg_tde` directory

2. Start the tests
    1. If you built PostgreSQL from PGDG, use the following command:

        ```sh
        make installcheck
        ```
        

    2. If you installed PostgreSQL server  from Percona Distribution for PostgreSQL, use the following command:

        ```sh
        sudo su postgres bash -c 'make installcheck USE_PGXS=1'
        ```
#### Run automatically       

The tests are run automatically with GitHub actions once you commit and push your changes. Make sure all tests are successfully passed before you proceed.


## Contributing to documentation

`pg_tde` documentation is maintained in the `documentation` directory. Please read the [Contributing guide](https://github.com/percona/pg_tde/blob/main/documentation/CONTRIBUTING.md) for guidelines how you can contribute to the docs.

## After your pull request is merged

Once your pull request is merged, you are an official Percona Community Contributor. Welcome to the community!

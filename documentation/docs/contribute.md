# Contributing guide

Welcome to `pg_tde` - the Transparent Data Encryption extension for PostgreSQL!

We're glad that you would like to become a community member and contribute to this project.

You can contribute in one of the following ways:

1. Reach us on our [Forums](https://forums.percona.com/c/postgresql/25).
2. Submit a bug report or a feature request
3. Submit a pull request (PR) with the code patch
4. Contribute to documentation

## Prerequisites

Before submitting code contributions, we ask you to complete the following prerequisites.

### 1. Sign the CLA

Before you can contribute, we kindly ask you to sign our [Contributor License Agreement](https://cla-assistant.percona.com/&lt;linktoCLA>) (CLA). You can do this in on click using your GitHub account.

**Note**:  You can sign it later, when submitting your first pull request. The CLA assistant validates the PR and asks you to sign the CLA to proceed.

### 2. Code of Conduct

Please make sure to read and agree to our [Code of Conduct](https://github.com/percona/community/blob/main/content/contribute/coc.md).

## Submitting a pull request

All bug reports, enhancements and feature requests are tracked in [GitHub issues](https://github.com/Percona-Lab/pg_tde/issues). Though not mandatory, we encourage you to first check for a bug report among the issues and in the PR list: perhaps the bug has already been addressed. 

For feature requests and enhancements, we do ask you to create a GitHub issue, describe your idea and discuss the design with us. This way we align your ideas with our vision for the product development.

If the bug hasn’t been reported / addressed, or we’ve agreed on the enhancement implementation with you, do the following:

1. [Fork](https://docs.github.com/en/github/getting-started-with-github/fork-a-repo) this repository
2. Clone this repository on your machine. 
3. Create a separate branch for your changes. If you work on a GitHub issue, please [create a branch from it](https://docs.github.com/en/issues/tracking-your-work-with-issues/linking-a-pull-request-to-an-issue#manually-linking-a-pull-request-or-branch-to-an-issue-using-the-issue-sidebar). This makes it easier to track your contribution.
4. Make your changes. Please follow the following guidelines to improve code readability:

    - [PostgreSQL coding conventions](https://www.postgresql.org/docs/current/source.html)
    - [C style and Coding rules](https://github.com/MaJerle/c-code-style) 

5. [Build `pg_tde`](https://github.com/Percona-Lab/pg_tde/wiki/Make-builds-for-developers) and [test your changes locally](#run-local-tests). 
6. Commit the changes. The [commit message guidelines](https://gist.github.com/robertpainsi/b632364184e70900af4ab688decf6f53) will help you with writing great commit messages
7. Open a pull request to Percona.
8. Our team will review your code and if everything is correct, will merge it. 
Otherwise, we will contact you for additional information or with the request to make changes.

### Run local tests

When you work, you should periodically run tests to check that your changes don’t break existing code.

To run the tests, use the following command:

```
cd pg_tde
./configure
make USE_PGXS=1 installcheck
```

You can run tests on your local machine with whatever operating system you have. After you submit the pull request, we will check your patch on multiple operating systems.

## Contribute to documentation

`pg_tde` documentation is written in Markdown language, so you can 
[edit it online via GitHub](#edit-documentation-online-vi-github). If you wish to have more control over the doc process, jump to how to [edit documentation locally](#edit-documentation-locally). 

Before you start, learn what [git], [MkDocs] and [Docker] are and what [Markdown] is and how to write it. For your convenience, there's also a cheat sheet to help you with the syntax. 

The doc files are in the `docs` directory.

### Edit documentation online via GitHub

1. Click the **Edit this page** icon next to the page title. The source `.md` file of the page opens in GitHub editor in your browser. If you haven’t worked with the repository before, GitHub creates a [fork](https://docs.github.com/en/github/getting-started-with-github/fork-a-repo) of it for you.
2. Edit the page. You can check your changes on the **Preview** tab. 
3. Commit your changes.
    * In the _Commit changes_ section, describe your changes.
    * Select the **Create a new branch for this commit** and start a pull request option
    * Click **Propose changes**.
4. GitHub creates a branch and a commit for your changes. It loads a new page on which you can open a pull request to Percona. The page shows the base branch - the one you offer your changes for, your commit message and a diff - a visual representation of your changes against the original page. This allows you to make a last-minute review. When you are ready, click the Create pull request button.
5. Someone from our team reviews the pull request and if everything is correct, merges it into the documentation. Then it gets published on the site.

### Edit documentation locally

This option is for users who prefer to work from their computer and / or have the full control over the documentation process.

The steps are the following:

1. Fork this repository
2. Clone the repository on your machine:

```sh
git clone git@github.com:<your-name>/pg_tde.git

3. Change the directory to ``pg_tde`` and add the remote upstream repository:

```sh
git remote add upstream git@github.com:Percona-Lab/pg_tde.git
```

4. Pull the latest changes from upstream

```sh
git fetch upstream
git merge upstream/main
```

5. Create a separate branch for your changes

```sh
git checkout -b <my_branch>
```

6. Make changes
7. Commit your changes. The [commit message guidelines](https://gist.github.com/robertpainsi/b632364184e70900af4ab688decf6f53) will help you with writing great commit messages

8. Open a pull request to Percona

#### Building the documentation

To verify how your changes look, generate the static site with the documentation. This process is called *building*. You can do it in these ways:
- [Use Docker](#use-docker)
- [Install MkDocs and build locally](#install-mkdocs-and-build-locally)

##### Use Docker

1. [Get Docker](https://docs.docker.com/get-docker/)
2. We use [our Docker image](https://hub.docker.com/repository/docker/perconalab/pmm-doc-md) to build documentation. Run the following command:

```sh
cd documentation
docker run --rm -v $(pwd):/docs perconalab/pmm-doc-md mkdocs build
```
   If Docker can't find the image locally, it first downloads the image, and then runs it to build the documentation.

3. Go to the ``site`` directory and open the ``index.html`` file to see the documentation.

If you want to see the changes as you edit the docs, use this command instead:

```sh
cd documentation
docker run --rm -v $(pwd):/docs -p 8000:8000 perconalab/pmm-doc-md mkdocs serve --dev-addr=0.0.0.0:8000
```

Wait until you see `INFO    -  Start detecting changes`, then enter `0.0.0.0:8000` in the browser's address bar. The documentation automatically reloads after you save the changes in source files.

##### Install MkDocs and build locally

1. Install [Python].

2. Install MkDocs and required extensions:

    ```sh
    pip install -r requirements.txt
    ```

3. Build the site:

    ```sh
    cd documentation
    mkdocs build
    ```

4. Open `site/index.html`

Or, to run the built-in web server:

```sh
cd documentation
mkdocs serve
```




View the site at <http://0.0.0.0:8000>

[MkDocs]: https://www.mkdocs.org/
[Markdown]: https://daringfireball.net/projects/markdown/
[Git]: https://git-scm.com
[Python]: https://www.python.org/downloads/
[Docker]: https://docs.docker.com/get-docker/

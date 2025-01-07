## Contribute to documentation

`pg_tde` documentation is written in Markdown language, so you can [edit it online via GitHub](#edit-documentation-online-vi-github). If you wish to have more control over the doc process, jump to how to [edit documentation locally](#edit-documentation-locally). 

Before you start, learn what [git], [MkDocs] and [Docker] are and what [Markdown] is and how to write it. For your convenience, there's also a [cheat sheet](https://www.markdownguide.org/cheat-sheet/) to help you with the syntax. 

The doc files are in the `documentation` directory.

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
git clone git@github.com:<your_name>/pg_tde.git
```

3. Change the directory to ``pg_tde`` and add the remote upstream repository:

```sh
git remote add upstream git@github.com:percona/pg_tde.git
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
- [Install MkDocs and build locally](#install-sphinx-and-build-locally)

##### Use Docker

1. [Get Docker](https://docs.docker.com/get-docker/)
2. We use [our Docker image](https://hub.docker.com/repository/docker/perconalab/pmm-doc-md) to build documentation. Run the following command:

```sh
docker run --rm -v $(pwd):/documentation perconalab/pmm-doc-md mkdocs build
```
   If Docker can't find the image locally, it first downloads the image, and then runs it to build the documentation.

3. Go to the ``site`` directory and open the ``index.html`` file to see the documentation.

If you want to see the changes as you edit the docs, use this command instead:

```sh
docker run --rm -v $(pwd):/documentation -p 8000:8000 perconalab/pmm-doc-md mkdocs serve --dev-addr=0.0.0.0:8000
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
    mkdocs build
    ```

4. Open `site/index.html`

Or, to run the built-in web server:

```sh
mkdocs serve
```


View the site at <http://0.0.0.0:8000>

[MkDocs]: https://www.mkdocs.org/
[Markdown]: https://daringfireball.net/projects/markdown/
[Git]: https://git-scm.com
[Python]: https://www.python.org/downloads/
[Docker]: https://docs.docker.com/get-docker/

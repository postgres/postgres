PostgreSQL Database Management System
=====================================

This directory contains the source code distribution of the PostgreSQL
database management system.

PostgreSQL is an advanced object-relational database management system
that supports an extended subset of the SQL standard, including
transactions, foreign keys, subqueries, triggers, user-defined types
and functions.  This distribution also contains C language bindings.

Copyright and license information can be found in the file COPYRIGHT.

General documentation about this version of PostgreSQL can be found at
<https://www.postgresql.org/docs/devel/>.  In particular, information
about building PostgreSQL from the source code can be found at
<https://www.postgresql.org/docs/devel/installation.html>.

The latest version of this software, and related software, may be
obtained at <https://www.postgresql.org/download/>.  For more information
look at our web site located at <https://www.postgresql.org/>.

## Table of Contents

- [Table of Contents](#table-of-contents)
- [How to build the project](#how-to-build-the-project)
    - [Some issues that may arise during the build process in MacOS](#some-issues-that-may-arise-during-the-build-process-in-macos)
        - [Missing tools to build the project and the documentation](#missing-tools-to-build-the-project-and-the-documentation)
        - [Missing icu4c](#missing-icu4c)


# How to build the project

Details of each step can be found in: https://www.postgresql.org/docs/devel/install-make.html

1. Clone the repository to your local machine.
2. Open the terminal and navigate to the project directory.
3. Run the following command to configure the project:
   ```
   ./configure
   ```
4. Run the following command to build the project:
   ```
   make all
   ```
5. Run the regression tests to check if the project is built correctly:
   ```
   make check
   ``` 
6. Install PostgreSQL:
    ```
    make install
    ```

## Some issues that may arise during the build process in MacOS

### Missing tools to build the project and the documentation

More details: https://www.postgresql.org/docs/current/docguide-toolsets.html#DOCGUIDE-TOOLSETS-INST-MACOS
```
brew install docbook docbook-xsl libxslt fop
```

Some of these tools require the following environment variable to be set. For Intel based machines, use this:
```
export XML_CATALOG_FILES=/usr/local/etc/xml/catalog
```

For Apple Silicon based machines, use this:
```
export XML_CATALOG_FILES=/opt/homebrew/etc/xml/catalog
```

### Missing icu4c

If you don't have icu4c installed, you can install it using Homebrew:
```
brew install icu4c
```

In some cases, you'll need to force the linking of icu4c:
```
brew link --force icu4c
```

To have icu4c in your PATH, add the following lines to your ~/.zshrc file:

```
export PATH="/opt/homebrew/opt/icu4c/bin:$PATH"
export PATH="/opt/homebrew/opt/icu4c/sbin:$PATH"
```

For compilers to find icu4c, set the following environment variables:
```
export LDFLAGS="-L/opt/homebrew/opt/icu4c/lib"
export CPPFLAGS="-I/opt/homebrew/opt/icu4c/include"
```

Finally, install pkg-config and update the PKG_CONFIG_PATH:
```
brew install pkg-config
export PKG_CONFIG_PATH="/opt/homebrew/opt/icu4c/lib/pkgconfig:$PKG_CONFIG_PATH"
```

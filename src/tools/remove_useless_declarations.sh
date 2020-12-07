#!/bin/sh

set -eu

cd "$(git rev-parse --show-toplevel)"
files=$(git ls-tree -r HEAD --name-only | grep '\.c$' | grep -v '/test/expected/')
while true; do
    # A visual version of this regex can be seen here (it is MUCH clearer):
    # https://www.debuggex.com/r/XodMNE9auT9e-bTx
    # This visual version only contains the search bit, the replacement bit is
    # quite simple when extracted from:
    # \n$+{code_between}\t$+{type}$+{variable} =
    # shellcheck disable=SC2086
    perl -i -p0e 's/\n\t(?!(return|static)\b)(?P<type>(\w+[\t ])+[\t *]*)(?>(?P<variable>\w+)( = [\w>\s\n-]*?)?;\n(?P<code_between>(?>(?P<comment_or_string_or_not_preprocessor>\/\*.*?\*\/|"(?>\\"|.)*?"|(?!goto)[^#]))*?)(\t)?(?=\b(?P=variable)\b))(?<=\n\t)(?<!:\n\t)(?P=variable) =(?![^;]*?[^>_]\b(?P=variable)\b[^_])/\n$+{code_between}\t$+{type}$+{variable} =/sg' $files
    # The following are simply the same regex, but repeated for different
    # indentation levels, i.e. finding declarations indented using 2, 3, 4, 5
    # and 6 tabs. More than 6 don't really occur in the wild.
    # (this is needed because variable sized backtracking is not supported in perl)
    # shellcheck disable=SC2086
    perl -i -p0e 's/\n\t\t(?!(return|static)\b)(?P<type>(\w+[\t ])+[\t *]*)(?>(?P<variable>\w+)( = [\w>\s\n-]*?)?;\n(?P<code_between>(?>(?P<comment_or_string_or_not_preprocessor>\/\*.*?\*\/|"(?>\\"|.)*?"|(?!goto)[^#]))*?)(\t\t)?(?=\b(?P=variable)\b))(?<=\n\t\t)(?<!:\n\t\t)(?P=variable) =(?![^;]*?[^>_]\b(?P=variable)\b[^_])/\n$+{code_between}\t\t$+{type}$+{variable} =/sg' $files
    # shellcheck disable=SC2086
    perl -i -p0e 's/\n\t\t\t(?!(return|static)\b)(?P<type>(\w+[\t ])+[\t *]*)(?>(?P<variable>\w+)( = [\w>\s\n-]*?)?;\n(?P<code_between>(?>(?P<comment_or_string_or_not_preprocessor>\/\*.*?\*\/|"(?>\\"|.)*?"|(?!goto)[^#]))*?)(\t\t\t)?(?=\b(?P=variable)\b))(?<=\n\t\t\t)(?<!:\n\t\t\t)(?P=variable) =(?![^;]*?[^>_]\b(?P=variable)\b[^_])/\n$+{code_between}\t\t\t$+{type}$+{variable} =/sg' $files
    # shellcheck disable=SC2086
    perl -i -p0e 's/\n\t\t\t\t(?!(return|static)\b)(?P<type>(\w+[\t ])+[\t *]*)(?>(?P<variable>\w+)( = [\w>\s\n-]*?)?;\n(?P<code_between>(?>(?P<comment_or_string_or_not_preprocessor>\/\*.*?\*\/|"(?>\\"|.)*?"|(?!goto)[^#]))*?)(\t\t\t\t)?(?=\b(?P=variable)\b))(?<=\n\t\t\t\t)(?<!:\n\t\t\t\t)(?P=variable) =(?![^;]*?[^>_]\b(?P=variable)\b[^_])/\n$+{code_between}\t\t\t\t$+{type}$+{variable} =/sg' $files
    # shellcheck disable=SC2086
    perl -i -p0e 's/\n\t\t\t\t\t(?!(return|static)\b)(?P<type>(\w+[\t ])+[\t *]*)(?>(?P<variable>\w+)( = [\w>\s\n-]*?)?;\n(?P<code_between>(?>(?P<comment_or_string_or_not_preprocessor>\/\*.*?\*\/|"(?>\\"|.)*?"|(?!goto)[^#]))*?)(\t\t\t\t\t)?(?=\b(?P=variable)\b))(?<=\n\t\t\t\t\t)(?<!:\n\t\t\t\t\t)(?P=variable) =(?![^;]*?[^>_]\b(?P=variable)\b[^_])/\n$+{code_between}\t\t\t\t\t$+{type}$+{variable} =/sg' $files
    # shellcheck disable=SC2086
    perl -i -p0e 's/\n\t\t\t\t\t\t(?!(return|static)\b)(?P<type>(\w+[\t ])+[\t *]*)(?>(?P<variable>\w+)( = [\w>\s\n-]*?)?;\n(?P<code_between>(?>(?P<comment_or_string_or_not_preprocessor>\/\*.*?\*\/|"(?>\\"|.)*?"|(?!goto)[^#]))*?)(\t\t\t\t\t\t)?(?=\b(?P=variable)\b))(?<=\n\t\t\t\t\t\t)(?<!:\n\t\t\t\t\t\t)(?P=variable) =(?![^;]*?[^>_]\b(?P=variable)\b[^_])/\n$+{code_between}\t\t\t\t\t\t$+{type}$+{variable} =/sg' $files
    # shellcheck disable=SC2086
    # shellcheck disable=SC2086
    git diff --quiet $files && break;
    # shellcheck disable=SC2086
    git add $files;
done

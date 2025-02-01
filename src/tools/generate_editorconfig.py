#!/usr/bin/env python3

import os


def cd_to_repo_root():
    abspath = os.path.abspath(__file__)
    dname = os.path.join(os.path.dirname(abspath), "..", "..")
    os.chdir(dname)


# Space based indentation levels are not tracked in .gitattributes, so
# we hardcode them here for the relevant filetypes.
space_based_indent_sizes = {
    "*.py": 4,
    "*.sgml": 1,
    "*.xsl": 1,
    "*.xml": 2,
}


def main():
    cd_to_repo_root()

    with open(".gitattributes", "r") as f:
        lines = f.read().splitlines()

    new_contents = """root = true

[*]
indent_size = tab
"""

    for line in lines:
        if line.startswith("#") or len(line) == 0:
            continue
        name, git_rules = line.split()
        if git_rules == "-whitespace":
            rules = [
                "indent_style = unset",
                "indent_size = unset",
                "trim_trailing_whitespace = unset",
                "insert_final_newline = unset",
            ]
        elif git_rules.startswith("whitespace="):
            git_whitespace_rules = git_rules.replace("whitespace=", "").split(",")
            rules = []
            if "-blank-at-eol" in git_whitespace_rules:
                rules += ["trim_trailing_whitespace = unset"]
            else:
                rules += ["trim_trailing_whitespace = true"]

            if "-blank-at-eof" in git_whitespace_rules:
                rules += ["insert_final_newline = unset"]
            else:
                rules += ["insert_final_newline = true"]

            if "tab-in-indent" in git_whitespace_rules:
                rules += ["indent_style = space"]
            elif "indent-with-non-tab" in git_whitespace_rules:
                rules += ["indent_style = tab"]
            elif name in ["*.pl", "*.pm"]:
                # We want editors to use tabs for indenting Perl
                # files, but we cannot add it such a rule to
                # .gitattributes, because certain lines are still
                # indented with spaces (e.g. SYNOPSIS blocks).  So we
                # hardcode the rule here.

                rules += ["indent_style = tab"]
            else:
                rules += ["indent_style = unset"]

            tab_width = "unset"
            for rule in git_whitespace_rules:
                if rule.startswith("tabwidth="):
                    tab_width = rule.replace("tabwidth=", "")
            rules += [f"tab_width = {tab_width}"]

            if name in space_based_indent_sizes:
                indent_size = space_based_indent_sizes[name]
                rules += [f"indent_size = {indent_size}"]

        else:
            continue

        rules = "\n".join(rules)
        new_contents += f"\n[{name}]\n{rules}\n"

    with open(".editorconfig", "w") as f:
        f.write(new_contents)


if __name__ == "__main__":
    main()

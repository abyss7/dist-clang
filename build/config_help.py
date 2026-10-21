#!/usr/bin/env python3
# Usage: config_help.py <output.h> <configuration.proto> [<imported.proto>...]
#
# Makes the reference of the configuration of the daemon from the comments of
# the protos - for "dist-clang --help-config". The comments follow the fields
# and the values of the enums, that they describe.

# For both Python 2 and 3: GN runs "python".
from __future__ import unicode_literals

import io
import re
import sys
import textwrap

WIDTH = 79

INTRO = """\
The configuration of the daemon is read from
$XDG_CONFIG_HOME/dist-clang/daemon.conf - ~/.config/dist-clang/daemon.conf by
default. The format is the text protobuf, e.g.:

  cache {
    size: 53687091200
    tags { name: "bench" isolated: true quota: 21474836480 }
  }

The daemon, that the client autostarts, has its own defaults, and the file
changes only the given fields: the socket in $XDG_RUNTIME_DIR, the cache of
20 GiB in $XDG_CACHE_HOME/dist-clang (~/.cache/dist-clang) with the direct
mode, and the number of threads twice the number of CPUs. A broken file is
ignored. "dist-clangd --config=<path>" reads only the given file - then the
required fields are needed. The paths are absolute: "~" isn't expanded. The
daemon reads the file at the start: stop it to apply the changes - the client
starts it again.

The fields below: "= value" is the default, "[repeated]" fields may be given
several times, "{ ... }" fields are groups of other fields, "<Name>" groups
are described at the end. The absorber, the collector, the coordinator and
the remotes of the emitter are for the distributed compilation.
"""


class Field:
    def __init__(self, label, type_name, name, default):
        self.label = label
        self.type_name = type_name
        self.name = name
        self.default = default
        self.comment = []


class EnumValue:
    def __init__(self, name):
        self.name = name
        self.comment = []


class Scope:
    def __init__(self, kind, name, parent):
        self.kind = kind  # "message", "enum" or "file".
        self.name = name
        self.parent = parent
        self.items = []  # Fields or enum values, in order.
        self.nested = {}  # Name -> Scope.


DECL_RE = re.compile(
    r"^(required|optional|repeated)\s+([\w.]+)\s+(\w+)\s*=\s*\d+\s*"
    r"(?:\[\s*default\s*=\s*([^\]]+?)\s*\])?\s*;$")
VALUE_RE = re.compile(r"^(\w+)\s*=\s*-?\d+\s*;$")
OPEN_RE = re.compile(r"^(message|enum)\s+(\w+)\s*\{$")


def Parse(path):
    root = Scope("file", "", None)
    scope = root
    last = None  # The declaration, that the next comment describes.
    for number, raw in enumerate(io.open(path, encoding="utf-8"), 1):
        line = raw.strip()
        if line.startswith("//"):
            if last is not None:
                last.comment.append(line[2:].strip())
            continue
        last = None
        if not line or re.match(r"^(syntax|import|package|option)\b", line):
            continue
        match = OPEN_RE.match(line)
        if match:
            nested = Scope(match.group(1), match.group(2), scope)
            scope.nested[nested.name] = nested
            scope = nested
            continue
        if line == "}":
            assert scope.parent, "%s:%d: unexpected }" % (path, number)
            scope = scope.parent
            continue
        match = DECL_RE.match(line)
        if match and scope.kind == "message":
            last = Field(*match.groups())
            scope.items.append(last)
            continue
        match = VALUE_RE.match(line)
        if match and scope.kind == "enum":
            last = EnumValue(match.group(1))
            scope.items.append(last)
            continue
        raise SystemExit("%s:%d: can't parse: %s" % (path, number, line))
    assert scope is root, "%s: unclosed scope" % path
    return root


def Resolve(scope, type_name, files):
    # Only the last part matters: the names don't repeat in the protos.
    name = type_name.split(".")[-1]
    while scope:
        if name in scope.nested:
            return scope.nested[name]
        scope = scope.parent
    for file in files:
        if name in file.nested:
            return file.nested[name]
    return None


def Text(comment):
    # "|name|" refers to the field "name".
    text = " ".join(line for line in comment if line)
    return re.sub(r"\|([\w:()]+)\|", r'"\1"', text)


def Wrap(text, indent, hanging=None):
    hanging = indent if hanging is None else hanging
    return textwrap.wrap(text, WIDTH, initial_indent=indent,
                         subsequent_indent=hanging, break_long_words=False,
                         break_on_hyphens=False)


class Writer:
    def __init__(self, files):
        self.files = files
        self.lines = []
        self.shared = []  # Messages, that several fields use.
        self.order = []  # Messages in the order of the first use.

    def CountUses(self, message, uses, seen):
        if message in seen:
            return
        seen.add(message)
        for field in message.items:
            target = Resolve(message, field.type_name, self.files)
            if target and target.kind == "message":
                if target not in uses:
                    uses[target] = 0
                    self.order.append(target)
                uses[target] += 1
                self.CountUses(target, uses, seen)

    def Message(self, message, indent):
        for field in message.items:
            self.Field(message, field, indent)

    def Field(self, message, field, indent):
        target = Resolve(message, field.type_name, self.files)
        label = "  [%s]" % field.label if field.label != "optional" else ""
        comment = Text(field.comment)
        if target and target.kind == "message":
            if target in self.shared:
                head = "%s%s { <%s> }" % (indent, field.name, target.name)
            else:
                head = "%s%s {" % (indent, field.name)
            self.lines.append(head + label)
            if comment:
                self.lines.extend(Wrap(comment, indent + "    "))
            if target not in self.shared:
                if comment:
                    self.lines.append("")
                self.Message(target, indent + "  ")
                while not self.lines[-1]:
                    self.lines.pop()
                self.lines.append(indent + "}")
            self.lines.append("")
            return

        if target and target.kind == "enum":
            type_name = " | ".join(value.name for value in target.items)
        else:
            type_name = field.type_name
        head = "%s%s: %s" % (indent, field.name, type_name)
        if field.default is not None:
            head += " = " + field.default
        self.lines.append(head + label)
        if comment:
            self.lines.extend(Wrap(comment, indent + "    "))
        if target and target.kind == "enum":
            for value in target.items:
                text = Text(value.comment)
                if text:
                    first = "%s    %s - " % (indent, value.name)
                    self.lines.extend(
                        Wrap(text, first, " " * len(first)))
        if comment or (target and target.kind == "enum"):
            self.lines.append("")

    def Write(self, root_message):
        uses = {}
        self.CountUses(root_message, uses, set())
        self.shared = [m for m in self.order if uses[m] > 1]
        self.lines.extend(INTRO.splitlines())
        self.lines.append("")
        self.Message(root_message, "")
        for message in self.shared:
            self.lines.append("<%s>:" % message.name)
            self.Message(message, "  ")
        while self.lines and not self.lines[-1]:
            self.lines.pop()
        # No double empty lines.
        result = []
        for line in self.lines:
            line = line.rstrip()
            if line or (result and result[-1]):
                result.append(line)
        return "\n".join(result) + "\n"


def main():
    output, config, imported = sys.argv[1], sys.argv[2], sys.argv[3:]
    files = [Parse(config)] + [Parse(path) for path in imported]
    root = files[0].nested["Configuration"]
    text = Writer(files).Write(root)
    assert ")help\"" not in text
    with io.open(output, "w", encoding="utf-8") as f:
        f.write("#pragma once\n\n"
                "// Generated by build/config_help.py - don't edit.\n\n"
                "namespace dist_clang::daemon {\n\n"
                "inline constexpr char kConfigHelp[] = R\"help(" + text +
                ")help\";\n\n"
                "}  // namespace dist_clang::daemon\n")


if __name__ == "__main__":
    main()

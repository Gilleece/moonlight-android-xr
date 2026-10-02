#!/usr/bin/env python3
#
# Checks every translation in app/src/main/res against the default strings
# and arrays: missing and extra keys, placeholders that do not match, line
# breaks lost, apostrophes and double quotes aapt would eat, emdashes, and
# anything marked translatable="false" that a locale overrides. The locales
# the app promises to be complete in have to have every key and be in the
# language setting; the rest are upstream's partial ones and are only
# checked for what they have.
#
#   python3 tools/check_strings.py [-v]
#
# Standard library only. Exits 1 on any error, which is what CI looks at.

import os
import re
import sys
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))
RES = os.path.normpath(os.path.join(HERE, "..", "app", "src", "main", "res"))

# Folder suffixes after "values-" of the locales that must be complete. Each
# joins the list as it is filled in.
COMPLETE = [
    "fr", "zh-rCN", "zh-rTW", "de", "es", "it", "pt-rBR", "ja", "ko", "nl", "pl", "sv", "da",
    "nb-rNO", "fi", "cs", "tr", "ru", "uk",
]

# A Java format specifier. No space flag, so "0 % est" in prose is not one.
PLACEHOLDER = re.compile(r"%(?:\d+\$)?[-#+0,(]*\d*(?:\.\d+)?[bBhHsScCdoxXeEfgGaAtTn%]")
EMDASH = "\u2014"


class Resources:
    def __init__(self, folder):
        self.folder = folder
        self.strings = {}
        self.arrays = {}
        self.fixed = set()
        self.lines = {}
        self.errors = []
        for name in ("strings.xml", "arrays.xml"):
            path = os.path.join(folder, name)
            if os.path.exists(path):
                self.read(path)

    def read(self, path):
        with open(path, encoding="utf-8") as f:
            raw = f.read()
        try:
            root = ET.fromstring(raw)
        except ET.ParseError as e:
            self.errors.append("%s: not well formed: %s" % (rel(path), e))
            return
        for el in root:
            key = el.get("name")
            if key is None:
                continue
            where = "%s:%d" % (rel(path), line_of(raw, el.tag, key))
            if el.get("translatable") == "false":
                self.fixed.add(key)
            if el.tag == "string":
                if key in self.strings:
                    self.errors.append("%s: %s defined twice" % (where, key))
                self.strings[key] = "".join(el.itertext())
            elif el.tag == "string-array":
                if key in self.arrays:
                    self.errors.append("%s: %s defined twice" % (where, key))
                self.arrays[key] = ["".join(item.itertext()) for item in el.findall("item")]
            else:
                continue
            self.lines[key] = where


def rel(path):
    return os.path.relpath(path, os.path.join(RES, "..", "..", "..", ".."))


def line_of(raw, tag, key):
    at = raw.find('<%s name="%s"' % (tag, key))
    return raw.count("\n", 0, at) + 1 if at >= 0 else 0


def placeholders(text):
    return sorted(PLACEHOLDER.findall(text))


def line_breaks(text):
    return len(re.findall(r"(?<!\\)\\n", text))


def escaping(text):
    """What aapt would do wrong with the text, or None. A string wholly in
    double quotes keeps its apostrophes; any other quote is a toggle aapt
    strips, and a bare apostrophe ends the string there."""
    body = text.strip()
    if len(body) >= 2 and body[0] == '"' and body[-1] == '"' and '"' not in body[1:-1].replace('\\"', ""):
        return None
    escaped = False
    for c in body:
        if escaped:
            escaped = False
        elif c == "\\":
            escaped = True
        elif c == "'":
            return "unescaped apostrophe"
        elif c == '"':
            return "unescaped double quote"
    return None


def check_text(where, key, text, errors):
    problem = escaping(text)
    if problem:
        errors.append("%s: %s has an %s" % (where, key, problem))
    if EMDASH in text:
        errors.append("%s: %s has an emdash" % (where, key))


def main():
    verbose = "-v" in sys.argv[1:]
    default = Resources(os.path.join(RES, "values"))
    errors = list(default.errors)
    translatable = [k for k in default.strings if k not in default.fixed]
    for key in default.strings:
        check_text(default.lines[key], key, default.strings[key], errors)

    folders = sorted(d for d in os.listdir(RES)
                     if d.startswith("values-")
                     and os.path.exists(os.path.join(RES, d, "strings.xml")))
    for missing in sorted(set(COMPLETE) - set(d[len("values-"):] for d in folders)):
        errors.append("values-%s: no strings.xml for a locale that has to be complete" % missing)

    for folder in folders:
        locale = folder[len("values-"):]
        res = Resources(os.path.join(RES, folder))
        errors.extend(res.errors)
        complete = locale in COMPLETE

        missing = [k for k in translatable if k not in res.strings]
        if complete:
            for key in missing:
                errors.append("%s: %s is missing" % (folder, key))
        for key in res.strings:
            where = res.lines[key]
            if key not in default.strings:
                errors.append("%s: %s is not in the default strings" % (where, key))
                continue
            if key in default.fixed:
                errors.append("%s: %s is translatable=\"false\"" % (where, key))
                continue
            text = res.strings[key]
            check_text(where, key, text, errors)
            if placeholders(text) != placeholders(default.strings[key]):
                errors.append("%s: %s placeholders %s, default has %s" % (
                    where, key, " ".join(placeholders(text)) or "none",
                    " ".join(placeholders(default.strings[key])) or "none"))
            # A translation may break a long paragraph where English does
            # not, but may not run two together
            if line_breaks(text) < line_breaks(default.strings[key]):
                errors.append("%s: %s has %d \\n, default has %d" % (
                    where, key, line_breaks(text), line_breaks(default.strings[key])))

        for key, items in res.arrays.items():
            where = res.lines[key]
            if key not in default.arrays:
                errors.append("%s: array %s is not in the default arrays" % (where, key))
            elif key in default.fixed:
                errors.append("%s: array %s is translatable=\"false\"" % (where, key))
            elif len(items) != len(default.arrays[key]):
                errors.append("%s: array %s has %d items, default has %d" % (
                    where, key, len(items), len(default.arrays[key])))
            else:
                for item in items:
                    check_text(where, key, item, errors)

        done = len(translatable) - len(missing)
        if complete or verbose:
            print("%-8s %3d/%d%s" % (locale, done, len(translatable),
                                     "" if complete else " (partial, not checked for gaps)"))

    # Every complete locale has to be one the language setting can pick
    arrays = Resources(os.path.join(RES, "values")).arrays
    values = set(arrays.get("language_values", []))
    with open(os.path.join(RES, "xml", "locales_config.xml"), encoding="utf-8") as f:
        config = set(re.findall(r'android:name="([^"]+)"', f.read()))
    for locale in COMPLETE:
        tag = locale.replace("-r", "-")
        if tag not in values:
            errors.append("values/arrays.xml: language_values has no %s" % tag)
        if tag not in config:
            errors.append("xml/locales_config.xml: no %s" % tag)
    if len(arrays.get("language_names", [])) != len(arrays.get("language_values", [])):
        errors.append("values/arrays.xml: language_names and language_values differ in length")

    for error in errors:
        print("ERROR " + error)
    print("%d translatable strings, %d errors" % (len(translatable), len(errors)))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Turn an mcs target Makefile into a CMakeLists.txt.

This is a one-shot migration aid, not part of the build.  It handles the ~130
directories that are pure variable assignments plus simple profile
conditionals; anything with a real recipe is left for a human and reported at
the end.

The conditionals are *evaluated*, not translated.  Every knob the mobile
profiles used to set (MOBILE_PROFILE, NO_SRE, AOT_FRIENDLY_PROFILE, ...) is
false for the profiles that survive, so those branches collapse at conversion
time instead of becoming CMake `if()`s.
"""

import os
import re
import sys

# Profiles this build still has, and the profile-scoped variables each sets.
PROFILES = ["build", "net_4_x", "xbuild_12", "xbuild_14", "unityjit"]

# Knobs that are unset for every surviving profile.  `ifdef` on one of these is
# dead; `ifndef` is always taken.
FALSE_KNOBS = {
    "MOBILE_PROFILE", "NO_SRE", "AOT_FRIENDLY_PROFILE", "MOBILE_DYNAMIC",
    "NO_THREAD_ABORT", "NO_THREAD_SUSPEND_RESUME", "NO_MULTIPLE_APPDOMAINS",
    "NO_PROCESS_START", "NO_CONSOLE", "DISABLE_REMOTING", "NO_MONO_SECURITY",
    "MONO_FEATURE_APPLETLS", "ONLY_APPLETLS", "MONO_FEATURE_APPLE_X509",
    "CC_PROFILE", "XAMMAC_4_5", "MCS_MODE", "NO_TASK_DELAY", "MANAGED_INTERP",
    "PROFILE_DISABLE_BTLS", "NO_GSS", "NO_RESGEN", "INCLUDE_DISABLED",
    "NO_SYSTEM_WEB_DEPENDENCY", "NO_SYSTEM_DRAWING_DEPENDENCY",
    "NO_SYSTEM_WEB_APPSERVICES_DEPENDENCY", "NO_WINFORMS_DEPENDENCY",
    "NO_SYSTEM_SERVICEMODEL_ACTIVATION_DEPENDENCY", "NO_WINDOWS_BASE",
    "NO_SYSTEM_DIRECTORY_SERVICES_DEPENDENCY", "NO_SYSTEM_DESIGN_DEPENDENCY",
    "TRACE", "DEBUG", "DEVEL", "MONO_TRACE", "PIPELINE_TIMER", "COVERAGE",
}
TRUE_KNOBS = {"ENABLE_GSS", "NET_4_5"}

# Variables that only matter to targets this conversion does not build (tests,
# dist tarballs, corcompare, monodoc).
IGNORED = {
    "SUBDIRS", "thisdir", "DISTFILES", "EXTRA_DISTFILES", "NO_TEST",
    "TEST_MCS_FLAGS", "TEST_LIB_REFS", "XTEST_LIB_REFS", "XTEST_MCS_FLAGS",
    "XTEST_LIB_FLAGS", "TEST_RESOURCE_FILES", "TEST_ARCHIVE", "TESTNUM",
    "TESTS", "USE_XTEST_REMOTE_EXECUTOR", "RESX_RESOURCE_STRING",
    "RESX_EXTRA_ARGUMENTS", "XTEST_RESX_RESOURCE_STRING", "TXT_RESOURCE_STRINGS",
    "TEST_NUNITLITE_APP_CONFIG_RUNTIME", "TEST_SPLIT_ASSEMBLIES",
    "XTEST_SPLIT_ASSEMBLIES", "LIBRARY_WARN_AS_ERROR", "MCS_BUILD_DIR",
    "TEST_FILES", "TEST_COMPILE", "RUN_STANDALONE", "CLEAN_FILES",
    "MONO_DISABLE_MANAGED_COLLATION", "TEST_HARNESS_EXCLUDES",
}

# What config.make and the profile fragments contribute.  Every surviving
# profile is FRAMEWORK_VERSION 4.5; mono_libdir is written prefix-relative
# because that is the form CMake's install() wants.
CONFIG_VALUES = {
    "mono_libdir": "lib",
    "prefix": "",
    "sysconfdir": "etc",
    "FRAMEWORK_VERSION": "4.5",
    "FRAMEWORK_VERSION_MAJOR": "4",
}

# Profiles that build no Facades (mcs/class/Makefile NO_FACADES_PROFILE).
NO_FACADES = {"xbuild_12", "xbuild_14", "binary_reference_assemblies"}

ASSIGN = re.compile(r"^\s*([A-Za-z_][A-Za-z0-9_]*)\s*(:=|\+=|\?=|=)\s*(.*)$")
COND = re.compile(r"^\s*(ifdef|ifndef|ifeq|ifneq|else|endif)\b\s*(.*)$")
# Directives that carry no information this conversion needs.  The `include`s
# pull in rules.make/library.make, whose content is the CMake module instead.
# A make rule, as opposed to an assignment.  Nearly all of these build test
# fixtures or install data, neither of which this conversion covers -- but a
# rule on $(the_lib)/$(build_lib) adds inputs to the assembly itself and has to
# be looked at by a human.
RULE = re.compile(r"^[^\s=]+\s*:(?!=)")
LIB_RULE = re.compile(r"\$\((the_lib|build_lib|the_assembly)\)\s*:")

DIRECTIVE = re.compile(
    r"^\s*(-?include|export|unexport|vpath|\.PHONY|\.DEFAULT|\.SUFFIXES|define|endef)\b")


class Skip(Exception):
    """The file needs a human."""


LENIENT = False


def get(values, name):
    """Read a variable, expanding it now -- assignments store raw text."""
    return expand(values.get(name, ""), values)


def truthy(name, values):
    if name in FALSE_KNOBS:
        return False
    if name in TRUE_KNOBS:
        return True
    return bool(get(values, name).strip())


def parse(path, profile):
    """Evaluate a Makefile for one profile, returning its variables."""
    values = dict(CONFIG_VALUES)
    values["PROFILE"] = profile
    values["PROFILE_DIRECTORY"] = profile
    values["XBUILD_VERSION"] = {"xbuild_12": "12.0", "xbuild_14": "14.0"}.get(
        profile, "4.0")
    # Stack of (taken_now, taken_already) so `else` works.
    stack = []
    with open(path, encoding="utf-8", errors="replace") as fh:
        lines = fh.read().splitlines()

    in_recipe = False
    i = 0
    while i < len(lines):
        line = lines[i]
        i += 1
        # Join backslash continuations before anything else -- most of the
        # "has a recipe" false positives in this tree are continuation lines.
        while line.endswith("\\") and i < len(lines):
            line = line[:-1] + " " + lines[i].strip()
            i += 1

        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue

        # Conditionals are tracked even inside a branch that is not taken, so
        # nesting stays balanced.
        m = COND.match(line)
        if m:
            kind, rest = m.group(1), m.group(2).strip()
            if kind == "endif":
                if stack:
                    stack.pop()
                continue
            if kind == "else":
                if stack:
                    now, ever = stack[-1]
                    stack[-1] = (not ever, True)
                continue
            if not all(now for now, _ in stack):
                # Inside dead code; the condition itself may not be evaluable.
                stack.append((False, True))
                continue
            if kind == "ifdef":
                cond = truthy(rest, values)
            elif kind == "ifndef":
                cond = not truthy(rest, values)
            else:
                cond = evaluate_eq(rest, values, kind)
            stack.append((cond, cond))
            continue

        if not all(now for now, _ in stack):
            continue

        if line.startswith("\t"):
            # Continuations were joined above, so a tab here is a real recipe.
            # If it belongs to a rule we already decided to drop -- a test
            # fixture, an install step, a maintainer target -- it goes with it.
            if in_recipe or LENIENT:
                continue
            raise Skip("recipe")

        if DIRECTIVE.match(line):
            continue

        if RULE.match(line):
            if LIB_RULE.search(line):
                raise Skip("rule on the library: " + line.strip()[:50])
            in_recipe = True
            continue

        in_recipe = False

        m = ASSIGN.match(line)
        if not m:
            if LENIENT:
                continue
            raise Skip("unparsed line: " + stripped[:60])
        name, op, val = m.group(1), m.group(2), m.group(3).strip()
        if name in IGNORED:
            continue
        if op == "+=":
            values[name] = (values.get(name, "") + " " + val).strip()
        elif op == "?=":
            values.setdefault(name, val)
        else:
            values[name] = val
    return values


def evaluate_eq(rest, values, kind):
    m = re.match(r"^\(\s*(.*?)\s*,\s*(.*?)\s*\)$", rest)
    if not m:
        raise Skip("unparsed condition: " + rest[:50])
    a = expand(m.group(1), values).strip()
    b = expand(m.group(2), values).strip()
    return (a == b) if kind == "ifeq" else (a != b)


def expand(text, values):
    """Expand the handful of make constructs these files actually use."""
    for _ in range(8):
        before = text
        # $(filter a b,$(PROFILE)) and friends, used for VALID_PROFILE guards.
        def do_filter(m):
            wanted = m.group(1).split()
            subject = expand(m.group(2), values).split()
            return " ".join(w for w in subject if w in wanted)

        def do_filter_out(m):
            unwanted = expand(m.group(1), values).split()
            subject = expand(m.group(2), values).split()
            return " ".join(w for w in subject if w not in unwanted)

        def do_subst(m):
            words = expand("$(" + m.group(1) + ")", values).split()
            pat, repl = m.group(2), m.group(3)
            out = []
            for w in words:
                if "%" in pat:
                    rx = "^" + re.escape(pat).replace("%", "(.*)") + "$"
                    mm = re.match(rx, w)
                    out.append(repl.replace("%", mm.group(1)) if mm else w)
                elif w.endswith(pat):
                    out.append(w[: -len(pat)] + repl)
                else:
                    out.append(w)
            return " ".join(out)

        text = re.sub(r"\$\(([A-Za-z_][A-Za-z0-9_]*):([^=()]*)=([^()]*)\)", do_subst, text)
        text = re.sub(r"\$\(filter\s+([^,()]*),([^()]*)\)", do_filter, text)
        text = re.sub(r"\$\(filter-out\s+([^,()]*),([^()]*)\)", do_filter_out, text)
        text = re.sub(r"\$[\({]([A-Za-z_][A-Za-z0-9_]*)[\)}]",
                      lambda m: values.get(m.group(1), ""), text)
        if text == before:
            break
    if "$(" in text:
        if LENIENT:
            return re.sub(r"\$\([^)]*\)", "", text)
        raise Skip("unexpanded: " + text[:60])
    return text


# ---------------------------------------------------------------------------
# Which directories each profile builds
# ---------------------------------------------------------------------------
# The target Makefile says *how* to build an assembly but not *whether* a
# profile builds it -- that lives in the per-profile SUBDIRS lists in
# mcs/Makefile, mcs/class/Makefile, mcs/tools/Makefile and Facades/subdirs.make.
SUBDIR_SOURCES = [
    ("", "Makefile"),
    ("class", "class/Makefile"),
    ("tools", "tools/Makefile"),
    ("class/Facades", "class/Facades/subdirs.make"),
]


def profile_dirs(topdir, profile):
    """Directories `make PROFILE=<profile>` would recurse into, relative to mcs/."""
    found = set()
    for prefix, rel in SUBDIR_SOURCES:
        if prefix == "class/Facades" and profile in NO_FACADES:
            continue
        path = os.path.join(topdir, rel)
        if not os.path.exists(path):
            continue
        values = parse_tolerant(path, profile)
        names = []
        for key in (f"{profile}_SUBDIRS", f"{profile}_PARALLEL_SUBDIRS"):
            names += get(values, key).split()
        if not names and prefix == "class/Facades":
            names = get(values, "common_SUBDIRS").split()
        for n in names:
            n = os.path.normpath(os.path.join(prefix, n)) if prefix else n
            found.add(n.lstrip("./"))
    return found


def parse_tolerant(path, profile):
    """parse(), but skipping recipe lines instead of giving up on them."""
    global LENIENT
    LENIENT = True
    try:
        return _parse_tolerant(path, profile)
    finally:
        LENIENT = False


def _parse_tolerant(path, profile):
    return parse(path, profile)


# ---------------------------------------------------------------------------
# Emitting
# ---------------------------------------------------------------------------
def settings(values):
    """The subset of a Makefile's variables that shapes the CMake declaration."""
    program = get(values, "PROGRAM").strip()
    library = get(values, "LIBRARY").strip()
    flags = " ".join(f for f in (get(values, "LOCAL_MCS_FLAGS"),
                                 get(values, "LIB_MCS_FLAGS")) if f).split()
    return {
        "program": bool(program),
        "name": os.path.basename(program or library),
        "output_name": get(values, "LIBRARY_NAME").strip(),
        "subdir": get(values, "LIBRARY_SUBDIR").strip(),
        "refs": get(values, "LIB_REFS").split(),
        "api_bin_refs": get(values, "API_BIN_REFS").split(),
        "target_net_reference": get(values, "TARGET_NET_REFERENCE").strip(),
        "flags": flags,
        "keyfile": get(values, "KEYFILE").strip(),
        "package": get(values, "LIBRARY_PACKAGE").strip(),
        "install_dir": (get(values, "LIBRARY_INSTALL_DIR")
                        or get(values, "PROGRAM_INSTALL_DIR")).strip(),
        "intermediate": bool(get(values, "LIBRARY_USE_INTERMEDIATE_FILE").strip()
                             or get(values, "PROGRAM_USE_INTERMEDIATE_FILE").strip()),
        "no_install": bool(get(values, "NO_INSTALL").strip()),
        "no_sign": bool(get(values, "NO_SIGN_ASSEMBLY").strip()),
        # 135 makefiles clear this to drop /debug:portable and sourcelink.
        "no_debug": "PLATFORM_DEBUG_FLAGS" in values
                    and not values["PLATFORM_DEBUG_FLAGS"].strip(),
        "no_build": bool(get(values, "NO_BUILD").strip()),
    }


def quote(word):
    return f'"{word}"' if re.search(r"[\s;\"$]", word) else word


def emit(profiles_settings, makefile_rel):
    """One CMakeLists.txt body for a directory, grouping identical profiles."""
    groups = {}
    for profile, s in profiles_settings.items():
        if s is None or s["no_build"] or not s["name"]:
            continue
        groups.setdefault(repr(sorted(s.items(), key=str)), (s, []))[1].append(profile)
    if not groups:
        return None

    out = [f"# Converted from {makefile_rel}.", ""]
    for _, (s, profiles) in sorted(groups.items(), key=lambda kv: kv[1][1]):
        out.append("mono_declare_managed(")
        if s["program"]:
            out.append("  PROGRAM")
        out.append(f"  NAME       {s['name']}")
        out.append(f"  PROFILES   {' '.join(sorted(profiles))}")
        for key, arg in (("output_name", "OUTPUT_NAME"), ("subdir", "SUBDIR"),
                         ("keyfile", "KEYFILE"), ("package", "PACKAGE"),
                         ("install_dir", "INSTALL_DIR"),
                         ("target_net_reference", "TARGET_NET_REFERENCE")):
            if s[key]:
                out.append(f"  {arg:<10} {quote(s[key])}")
        for key, arg in (("refs", "REFS"), ("api_bin_refs", "API_BIN_REFS"),
                         ("flags", "FLAGS")):
            if s[key]:
                out.append(f"  {arg:<10} {' '.join(quote(w) for w in s[key])}")
        for key, arg in (("intermediate", "INTERMEDIATE"), ("no_install", "NO_INSTALL"),
                         ("no_sign", "NO_SIGN"), ("no_debug", "NO_DEBUG")):
            if s[key]:
                out.append(f"  {arg}")
        out.append(")")
        out.append("")
    return "\n".join(out)


def write_subdirs(topdir):
    """Regenerate mcs/CMakeLists.txt's add_subdirectory list.

    Kept in sync here rather than by hand: a directory that is declared but
    never added is silently absent from the build, which shows up much later as
    an unresolved reference.
    """
    dirs = sorted(
        os.path.relpath(root, topdir)
        for root, _, files in os.walk(topdir)
        if "CMakeLists.txt" in files and os.path.relpath(root, topdir) != "."
    )
    body = "\n".join(f"add_subdirectory({d})" for d in dirs)
    with open(os.path.join(topdir, "CMakeLists.txt"), "w") as fh:
        fh.write(HEADER + body + FOOTER)


HEADER = """# The class libraries.
#
# Each subdirectory declares the assemblies it produces; nothing is built until
# mono_managed_materialize() runs at the end, because a reference is a bare
# assembly name and cannot be resolved to a target before every directory has
# been read.  Order in this list is therefore irrelevant.  See
# cmake/MonoManaged.cmake.
#
# Regenerated by build/convert-makefiles.py along with the declarations.

"""

FOOTER = """

mono_managed_materialize()

# `mcs` keeps its old meaning: build everything the enabled profiles produce.
add_custom_target(mcs ALL)
foreach(_p IN LISTS MONO_MANAGED_PROFILES)
  add_dependencies(mcs mcs-${_p})
endforeach()
"""


def main():
    topdir = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else ".")
    dirs_for = {p: profile_dirs(topdir, p) for p in PROFILES}

    written, skipped = [], []
    for root, _, files in os.walk(topdir):
        if "Makefile" not in files:
            continue
        rel = os.path.relpath(root, topdir)
        rel = "" if rel == "." else rel
        path = os.path.join(root, "Makefile")
        with open(path, encoding="utf-8", errors="replace") as fh:
            text = fh.read()
        # The Facades spell the include as $(MCS_BUILD_DIR)/library.make.
        if "library.make" not in text and "executable.make" not in text:
            continue

        per_profile = {}
        reason = None
        for profile in PROFILES:
            if rel not in dirs_for[profile]:
                continue
            try:
                per_profile[profile] = settings(parse(path, profile))
            except Skip as exc:
                reason = str(exc)
                per_profile = {}
                break
        if reason:
            skipped.append((rel, reason))
            continue
        if not per_profile:
            skipped.append((rel, "in no surviving profile"))
            continue
        # Never clobber a hand-written file: the awkward directories (corlib,
        # ilasm, the bootstrap tools) are converted by hand and this script
        # cannot express what they do.
        existing = os.path.join(root, "CMakeLists.txt")
        if os.path.exists(existing):
            with open(existing) as fh:
                if not fh.readline().startswith("# Converted from"):
                    skipped.append((rel, "hand-written, kept"))
                    continue
        body = emit(per_profile, os.path.join(rel, "Makefile"))
        if body is None:
            skipped.append((rel, "nothing to build"))
            continue
        with open(os.path.join(root, "CMakeLists.txt"), "w") as fh:
            fh.write(body)
        written.append(rel)

    write_subdirs(topdir)
    print(f"wrote {len(written)} CMakeLists.txt")
    print(f"skipped {len(skipped)}:")
    for rel, why in sorted(skipped):
        print(f"  {rel or '.'}: {why}")


if __name__ == "__main__":
    main()

"""Shared plumbing for the checks that assert what a tier-1 pass decided.

A corpus that computes the right answers looks green whether or not anything
was inlined, devirtualized or co-located, so these checks read a pass's own
trace instead.  They all have the same shape: run the corpus under a
deterministic tier-1 configuration, capture a trace off stderr, and compare it
against expectations -- either written next to the fixtures in the corpus
source, or structural and stated in the check itself.
"""

import argparse
import os
import subprocess
import sys

# Threshold 0 is the only deterministic mode: above it promotion is handed to a
# background worker, so which methods reach tier 1 before the process exits --
# and therefore which decisions the trace even contains -- varies from run to
# run.
EAGER_TIER1 = {
    "MONO_TIERED": "1",
    "MONO_TIERED_CALL_THRESHOLD": "0",
}


def argument_parser(description, *, source=False):
    """A parser carrying the arguments every check takes.

    MONO_PATH and friends come in through the environment rather than as a
    command prefix, so the runtime really is just a path here.
    """
    parser = argparse.ArgumentParser(description=description)
    parser.add_argument("runtime", help="the mono binary, or the mono-wrapper script")
    parser.add_argument("corpus", help="the corpus .exe to run")
    if source:
        parser.add_argument("source", help="the corpus source carrying the expectations")
    return parser


def run(runtime, corpus, *, args=(), env=None):
    """Run the corpus and hand back the completed process, text-mode."""
    child = dict(os.environ)
    child.update(env or {})
    return subprocess.run([runtime, *args, corpus],
                          env=child, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          universal_newlines=True)


def die(message, *details):
    """Report a problem with the run itself, rather than with what it decided."""
    prog = os.path.basename(sys.argv[0])
    print(f"{prog}: {message}", file=sys.stderr)
    for detail in details:
        print(f"  {detail}", file=sys.stderr)
    sys.exit(1)


def traced_run(runtime, corpus, trace_var, *, args=("--llvm",)):
    """Run the corpus at tier 1 with one trace switch on and return its stderr.

    Bails out if the corpus itself failed, so callers only ever see a trace
    from a run that got all the way through.
    """
    env = dict(EAGER_TIER1)
    env[trace_var] = "1"
    proc = run(runtime, corpus, args=args, env=env)
    if proc.returncode != 0:
        die(f"the corpus run failed (exit {proc.returncode})",
            *proc.stderr.splitlines()[-20:])
    return proc.stderr


def require_trace(trace, prefix, *hints):
    """Insist the run produced a trace at all, so an inert check cannot pass."""
    lines = [line for line in trace.splitlines() if line.startswith(prefix)]
    if not lines:
        die(f"the run produced no {prefix!r} trace at all.", *hints)
    return lines


def read_expectations(source, marker):
    """Yield (verb, subject) for every `<marker>: <verb> <subject>` in a source.

    Every line carrying the marker is read, so a comment restating the syntax
    would be picked up as a real expectation -- document the syntax in the
    check, not in the corpus.
    """
    with open(source, encoding="utf-8") as handle:
        for line in handle:
            _, sep, spec = line.partition(marker + ":")
            if not sep:
                continue
            spec = spec.strip()
            if not spec:
                continue
            verb, _, subject = spec.partition(" ")
            yield verb, subject.strip()


class Report:
    """Prints a line per check and turns the tally into an exit code."""

    def __init__(self):
        self.checks = 0
        self.problems = 0

    def ok(self, text):
        self.checks += 1
        print(f"  ok   {text}")

    def fail(self, text):
        self.checks += 1
        self.problems += 1
        print(f"  FAIL {text}")

    def problem(self, text):
        """A failure that is not a check of its own; several can land on one."""
        self.problems += 1
        print(f"  FAIL {text}")

    def finish(self, noun):
        print(f"{self.checks} {noun}, {self.problems} failed")
        return 1 if self.problems else 0

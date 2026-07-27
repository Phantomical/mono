Acceptance tests
================

This directory contains acceptance tests, which are third party test suites and frameworks that are used to validate Mono against a wider range of test cases that go beyond the Mono unit tests that run as part of CI.

In order to make checking out those test suites optional we don't use traditional git submodules, but instead clone them on demand when needed. The custom submodule repositories are checked out into the acceptance-tests/external/ directory.

## Usage

Running all test suites is possible via "make check-full". There are also targets for running individual test suites, see below.

Some of the test suites require an installed Mono (i.e. they don't work with the in-tree build), those will ask you to pass in the PREFIX variable pointing to the installation directory when invoking make. Note that this directory needs to be writable as we overwrite some files there as part of testing.

## Individual test suites and targets

* `make check-ms-test-suite` - Runs tests that were shared with Xamarin, those are not available publically and will be skipped when the repository is not accessible.
* `make check-roslyn` - Runs the Roslyn test suite.
* `make check-coreclr` - Runs the CoreCLR test suite.
  * `make coreclr-runtest-coremanglib` - Runs only the CoreMangLib portion of the CoreCLR tests, those tests mostly target the BCL behavior.
  * `make coreclr-runtest-basic` - Runs only the CoreCLR tests that target runtime behavior and stability.
  * `make coreclr-compile-tests` - Convenience target that precompiles all the test cases in parallel.
  * `make coreclr-gcstress` - Runs the CoreCLR GC stress tests.

## The corpus submodules

The test corpora under `external/` are git submodules of this repository, pinned
by gitlink like any other. All of them carry `update = none` in `.gitmodules`,
so an ordinary `git submodule update --init --recursive` skips them: together
they are well over a gigabyte, and nothing outside these suites reads them.
Ask for one by name:

```
git submodule update --init --checkout acceptance-tests/external/coreclr
```

`ms-test-suite` needs that treatment for a second reason — it is Xamarin-internal
and not readable from outside, so skipping it by default keeps a bare `--init`
from failing rather than merely saving a download.

To see what is checked out and what each one is pinned to, configure with
`-D MONO_ENABLE_ACCEPTANCE_TESTS=ON` and build the `print-versions` target.

Re-pinning is plain git: check the submodule out at the revision you want, then
commit the changed gitlink in the superproject.

```
git -C acceptance-tests/external/coreclr checkout <rev>
git add acceptance-tests/external/coreclr
```

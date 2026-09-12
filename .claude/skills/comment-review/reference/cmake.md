# CMake files

Every rule and pass in `SKILL.md` applies to CMake files too. The differences:

**The marker rules do not.** CMake has one comment marker. A comment above a
`function ()`, `macro ()` or `option ()` is a doc comment and gets the doc rules — verb
first, contract not mechanism, `\param` has no counterpart so name the arguments in
prose. Everything else is a remark and gets the remark rules. A **separator banner**
(`# -------`) carries nothing and goes.

**The identity proof is different.** There is nothing to preprocess. Prove the change is
comment-only by reading the diff:

```bash
git diff -U0 -- <path> | grep -E '^[-+]' | grep -vE '^(\+\+\+|---)' |
  grep -vE '^[-+][[:space:]]*(#|$)'
```

Anything that prints is a line you changed that is not a whole-line comment. A trailing
comment on a code line prints too, so read what comes out rather than requiring silence.

**The names to grep are different, and this is where the yield is.** A build file's
comments name variables, cache variables, targets, test labels, options, generated files
and paths. Every one of those is greppable, and a build system that has been rewritten
carries comments describing the one before it. Check:

| the name is | where it lives |
| --- | --- |
| a variable or cache variable | `git grep -n '<name>' -- '*.cmake' '*CMakeLists.txt'` |
| a target | the `add_*` call that makes it, anywhere in the tree |
| a test or a label | `ctest --test-dir build -N`, and `--print-labels` |
| a file the build writes | the `add_custom_command` `OUTPUT` that writes it |
| an autotools artefact | `configure`, `Makefile.am` and `autogen.sh` are **gone** |

**The house failure mode here is teaching CMake.** A comment explaining what
`set (... PARENT_SCOPE)` does, what a generator expression is, or how ctest picks tests
fails the governing test: the reader has the manual. Keep what is local — why *this*
build makes that choice, which upstream defect a flag works around, what a magic number
was measured at.

Run `scripts/register-check-cmake.sh <file>...` instead of `register-check.sh`.

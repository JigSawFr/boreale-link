<!--
The title must be a conventional commit with a scope (feat(session): …, fix(json): …):
CI checks it, and the squash merge makes it the commit subject.
-->

## What this changes, and why

<!-- The diff says what; this says what it is for. -->

## How it was verified

<!-- Beyond CI: which homebrew it was tried in, and whether on a console. -->

## Checklist

- [ ] `make test` passes
- [ ] Nothing allocates, nothing knows a particular homebrew
- [ ] A change to an interface says which homebrews must follow

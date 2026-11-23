<!--
The title must be a conventional commit with a scope (feat(session): …, fix(json): …):
CI checks it, and the squash merge makes it the commit subject.
-->

## ✨ At a glance

<!-- One or two lines: what changes for someone reading the log, and whether an interface moves. -->

## 🤔 Why

<!-- The problem or the need. The diff says what; this says what it is for. -->

## 🔧 What changes

<!-- The notable parts, module by module when several are touched. -->

## ✅ Checked

<!-- Beyond CI (make test, the Switch build and the title check run there): which homebrew it was tried in, and whether on a console. Write "not run" for what was not. -->

- [ ] Nothing allocates, nothing knows a particular homebrew
- [ ] A change to an interface says which homebrews must follow

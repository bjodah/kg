# The hosted CI box, and what is not true of it

What differs between the Woodpecker box and a development one, written
down because every one of these has already cost a wrong diagnosis.

## It has no route out

The box resolves internal names only.  `cont-reg.bjodah.se` answers;
`github.com`, `deb.debian.org` and `repo.maven.apache.org` all time out --
on the host itself, not merely inside a container.  The Woodpecker
environment carries an http proxy, and it reaches curl, cargo, go and
rustup.  It does NOT reach a JVM: Java reads neither `http_proxy` nor
`https_proxy`, so Maven, and any language server that shells out to it,
fetches nothing here whatever the shell around it can do.

So a test fixture must need nothing downloaded.  `requires_tool:` covers
what has to be installed and `requires_python_module:` what has to be
importable; nothing covers what a tool would fetch at run time, and that
failure does not look like a missing dependency.  It looks like a slow
box timing out.

### The worked example: dap-nbcode-java-smoke (fixed 2026-08-14)

The case planted a `pom.xml`, and nbcode debugs a file belonging to a
Maven project by running the PROJECT -- `mvn ... exec:java`, resolving
`org.codehaus.mojo:exec-maven-plugin` at launch time.  Straced on this
box, the adapter's own `output` events say it plainly:

    Scanning for projects...
    BUILD FAILURE
    Plugin org.codehaus.mojo:exec-maven-plugin:3.5.1 or one of its
      dependencies could not be resolved ... (absent)

36 s after `launch`, then `terminated`: no debuggee, no `stopped`, and
the case's two bounded waits spend themselves for nothing.  The image's
own `~/.m2` carries the receipt -- a `.lastUpdated` for that exact
artifact recording `Temporary failure in name resolution`.

An earlier note in this file read "not the network", on the strength of a
`mvn compile` that downloaded and succeeded through the proxy from a
shell in that container.  That was not the run the case makes: the proxy
was in the shell's environment and nbcode's Maven is a JVM.

Two further things that mislead here, both measured:

- The PTY harness gives every case a throwaway `HOME`, so a warm
  `/root/.m2` on a development box is invisible to the tests.  Seeding
  one would not have helped; a `settings.xml` naming an absolute
  `<localRepository>` would be needed, planted per case.
- The development box does not take the Maven path at all.  Straced
  there, `launch` is answered in 110 ms with `User program running` and
  no Maven whatsoever: nbcode's single-file launcher wins a race that a
  slow box lets the project import finish first.  Insert a 25 s pause
  before `dap-debug` and the same fixture starts scanning for projects.
  A fixture whose launch path depends on who wins that race is not a
  fixture.

The fix is in the case and nowhere else: no `pom.xml`, so nbcode compiles
and runs the single file itself (NetBeans' java.file.launcher, a launch
shape `doc/plans/dap/03-java.md` had already measured).  After it, on
this box: three runs, 15.3-15.6 s each, and the whole `dap-*`/`lsp-*` set
59 PASS / 2 SKIP / 0 FAIL.  `lsp-nbcode-definition.yaml` keeps its pom
and passes -- OPENING a Maven project is offline work, and only running
one is not.

## What the image carries (triceratops-9:10)

- `pmccabe` and `scc`, so `.woodpecker.yaml` installs nothing on top.
- A tree-sitter prefix in `TREE_SITTER_ROOT`
  (`/opt-2/tree-sitter-v0.26.12-release`), so `.ci/ci-13` finds an
  install instead of building the core and grammars from the pins'
  hosts -- which it could not reach.
- debugpy, importable by the `python3` the activated environment puts
  first, which is the interpreter kg spawns: `dap-debugpy-smoke` runs
  rather than skipping.
- `nbcode` at `/usr/local/bin/nbcode`, and `lldb-dap`, `clangd`, `ty`,
  `delve`.

`/usr/bin/python3` has PyYAML but not pexpect, so the PTY suite depends
on `.woodpecker.yaml` sourcing the `/opt-3/cpython-*` environment before
the runner.  Installing `python3-pexpect` in the image would remove that
dependency on ordering; nothing else is missing.

## Three cores, and what that is not

`.ci/ci-env.sh` caps `PTY_JOBS` at `nproc` for this box, because the
cases that wait on a JVM, a `go build` or rust-analyzer's sysroot do not
overlap for free on three cores.  That cap is the answer to genuine
contention failures.  It is not the answer to a case that fails here when
run entirely alone on an idle box -- that one is not timing, and looking
for a race first is how the Maven diagnosis above took two sessions.

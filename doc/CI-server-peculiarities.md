The one that's left: dap-nbcode-java-smoke

This is a different bug and I could not fix it. Evidence, all from your box:

- Not timing. Budgets raised from 150/60 s to 600/180 s: still fails, identical screen.
- Not the network. Same failure with Maven proxied properly (I verified mvn compile with a cold local repo downloads and succeeds through your proxy from inside that container).
- Not the rlimit failure mode the case's own comment warns about — ulimit -v is unlimited, overcommit_memory is 0.
- nbcode's own log reports Debugger session fixture started at localhost for Java, exactly as it does on the passing dev box, and its logs are otherwise identical (same two benign ClassNotFoundExceptions on both).
- The debuggee JVM never appears. With the run stuck at 150 s I checked the process table: kg, one nbcode JVM, no second java, no mvn. nbcode's in-process Maven build simply never produces a program to debug.

So nbcode opens the project (1,986 ms vs 234 ms here) and starts the JPDA listener, then nothing launches. Next step would be capturing kg's DAP traffic during a stuck run to see whether launch is answered, errored, or dropped — worth its own session.

Your image question

The honest answer: pre-seeding Maven won't buy you a green CI today, because the one case that touches Maven fails for the reason above, and the jdtls case is already offline-safe by design. Two things are worth baking in, both verified available in trixie:

RUN apt-get update && apt-get install -y --no-install-recommends \
        python3-debugpy python3-pexpect \
    && rm -rf /var/lib/apt/lists/*

- python3-debugpy — the only tool actually missing. dap-debugpy-smoke SKIPs on your box with python3 cannot import debugpy, and kg spawns the literal python3 -m debugpy.adapter, so it must be that interpreter.
- python3-pexpect — /usr/bin/python3 has PyYAML but not pexpect. The Makefile picks the first of python3, python that can import both and otherwise falls back to python3; neither qualifies in the image, so the PTY suite currently only runs because something in the
bash -l environment supplies an interpreter. Installing it removes that dependency on profile ordering.

Also worth considering: bake a tree-sitter prefix (utils/build-tree-sitter.sh, then ENV TREE_SITTER_PREFIX=…), since .ci/ci-13 otherwise builds the core and grammars from the pins' hosts on every run.

If you do want the Maven cache anyway, one caveat that will bite: the PTY harness gives every case a fresh HOME, so a warm /root/.m2/repository is invisible to the tests. It has to live at a fixed path a case's planted settings.xml names via <localRepository>. And
separately — the JVM ignores http_proxy/https_proxy, so your WOODPECKER_ENVIRONMENT proxy reaches curl, cargo, go and rustup but not Maven or any language server that shells out to it.

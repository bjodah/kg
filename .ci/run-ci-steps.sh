#!/bin/bash
set -euxo pipefail
source $(compgen -G "/opt-?/cpython-v3.*-apt-deb/bin/activate")
VALGRIND="valgrind --quiet --tool=memcheck --leak-check=full --show-leak-kinds=all --errors-for-leak-kinds=definite,possible --error-exitcode=1"
make complexity-check
make coverage CC="ccache gcc" PTY_TIMEOUT=20 PTY_STARTUP_DELAY_ADD=1.5 PTY_KEY_DELAY_ADD=0.05
make -B check CC="ccache gcc -Werror -fanalyzer" TEST_RUNNER="${VALGRIND}" KG_RUNNER="${VALGRIND}" PTY_TIMEOUT=20 PTY_STARTUP_DELAY_ADD=1.5 PTY_KEY_DELAY_ADD=0.05
make -B check CC="ccache clang" CFLAGS="-Werror -Wall -Wextra -pedantic -fsanitize=address,undefined -fno-omit-frame-pointer -fno-optimize-sibling-calls -O0 -g"
make -B check CC="ccache clang" CFLAGS="-Werror -Wall -Wextra -pedantic -fsanitize=memory -fsanitize-memory-track-origins=2 -fsanitize-memory-param-retval -fno-omit-frame-pointer -fno-optimize-sibling-calls -O0 -g"

# https://btorpey.github.io/blog/2015/04/27/static-analysis-with-clang/
bear -- make CC="ccache clang" -B
export COMPILE_DB=$(/bin/pwd);
make iwyu

python3 - <<'PY' |
import json
with open("compile_commands.json", "r", encoding="utf-8") as fp:
    for entry in json.load(fp):
        print(entry["file"])
PY
while read FILE; do
  (cd $(dirname ${FILE});
   OUT=$(mktemp)
   trap 'rm -f "${OUT}"' RETURN
   clang-check -analyze -p ${COMPILE_DB} $(basename ${FILE}) 2>&1 | tee "${OUT}"
   if rg -q "warning:" "${OUT}"; then
     exit 1
   fi
  );
done

cppcheck --quiet --error-exitcode=1 --suppress=normalCheckLevelMaxBranches src/*.c

python3 - <<'PY' |
import json
with open("compile_commands.json", "r", encoding="utf-8") as fp:
    for entry in json.load(fp):
        print(entry["file"])
PY
while read FILE; do
  OUT=$(mktemp)
  trap 'rm -f "${OUT}"' RETURN
  if ! clang-tidy --quiet -p ${COMPILE_DB} ${FILE} >"${OUT}" 2>&1; then
    cat "${OUT}"
    exit 1
  fi
done

make format-check

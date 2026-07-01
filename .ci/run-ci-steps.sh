#!/bin/bash
set -euxo pipefail
source $(compgen -G "/opt-?/cpython-v3.*-apt-deb/bin/activate")
make -B check CC="ccache gcc -Werror -fanalyzer" 
make -B check CC="ccache clang" CFLAGS="-Werror -Wall -Wextra -pedantic -fsanitize=address,undefined -fno-omit-frame-pointer -fno-optimize-sibling-calls -O0 -g"
make -B check CC="ccache clang" CFLAGS="-Werror -Wall -Wextra -pedantic -fsanitize=memory -fsanitize-memory-track-origins=2 -fsanitize-memory-param-retval -fno-omit-frame-pointer -fno-optimize-sibling-calls -O0 -g"

# https://btorpey.github.io/blog/2015/04/27/static-analysis-with-clang/
bear -- make CC="ccache clang" -B
export COMPILE_DB=$(/bin/pwd);
grep file compile_commands.json |
awk '{ print $2; }' |
sed 's/\"//g' |
while read FILE; do
  (cd $(dirname ${FILE});
   clang-check -analyze -p ${COMPILE_DB} $(basename ${FILE})
  );
done

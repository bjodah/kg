# Fe upstream

kg embeds the core of [Fe](https://github.com/bjodah/fe) through the `fe/`
git submodule. It is pinned to commit `0dc79f2a9db6f4646c11200c65d7f315e3ce27e0`
on the `analyzers-etc` branch.
The supported embedding interface is `FE_API_VERSION 1`.

Fe is MIT licensed. Copyright belongs to rxi and Chris Palmer; the complete
license text is in `fe/LICENSE`.

kg compiles only `fe/fe.c` and its public header `fe/fe.h`. The `fex*` files,
`auto.*`, and `main.c` are deliberately excluded so Fe's optional I/O,
process, regular-expression, math, and time extensions are not exposed.

To update Fe:

1. Bump the `fe/` submodule to the intended upstream commit or tag.
2. Review the complete submodule diff.
3. Confirm `FE_API_VERSION` and adapt kg if it changed.
4. Rerun kg's full CI pipeline.

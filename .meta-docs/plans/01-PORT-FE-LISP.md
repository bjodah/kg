Integrating the `fe` Lisp interpreter into the `kg` text editor is an excellent project! It will transform `kg` from a static editor into a customizable, programmable one, much like Emacs. 

Here is a detailed, step-by-step implementation plan tailored for a junior engineer.

---

## Architecture Overview
*   **The Interpreter (`fe`)**: Will live inside the editor as an embedded subsystem. It requires a fixed block of memory (an "arena") and a context (`FeContext`).
*   **The Editor (`kg`)**: Will own the `FeContext`, initializing it on startup and shutting it down on exit.
*   **The Bridge**: We will write C functions (`FeNativeFn`) that expose `kg`'s internals (inserting text, moving the cursor, setting the status) to the Lisp environment.

---

## Phase 1: Build System & File Integration

First, we need to bring the `fe` source files into the `kg` project and get them to compile together.

1.  **Copy Files**: Copy `fe.c` and `fe.h` from the `fe` repository into `kg/src/`.
    *   *(Optional)*: You can also copy the `fex_*.c/h` files if you want regular expressions and math built-in, but starting with just `fe.c/h` keeps things simpler.
2.  **Update `Makefile`**:
    *   Add `fe.c` to the `SRCS` variable.
    *   Add `$(OBJDIR)/fe.o` to the `OBJS` variable (it will be handled automatically if you add it to `SRCS`).
3.  **Adjust the C Standard**: 
    *   `kg` is currently built with `-std=c99`, but `fe` uses some modern C features (`-std=c2x`). 
    *   In the `Makefile`, change `CFLAGS ?= -Wall -W -pedantic -std=c99 -Os` to use **`-std=gnu11`** or **`-std=c2x`**. (Try `gnu11` first, as it balances compatibility well).

---

## Phase 2: Lisp State Initialization

We need to give the editor a Lisp context and manage its memory lifecycle.

1.  **Update `src/def.h`**:
    *   Include the `fe` header near the top: `#include "fe.h"`
    *   Add the Lisp context to the global state. Inside `struct editor_config`, add:
        ```c
        void *lisp_arena;
        FeContext *lisp_ctx;
        ```
2.  **Initialize Lisp in `src/main.c`**:
    *   Locate `init_editor(void)`.
    *   Allocate memory for Lisp (e.g., 2 Megabytes) and open the context:
        ```c
        editor.lisp_arena = malloc(2 * 1024 * 1024);
        editor.lisp_ctx = FeOpenContext(editor.lisp_arena, 2 * 1024 * 1024);
        ```
3.  **Clean up Lisp in `src/tty.c`**:
    *   Locate `editor_at_exit(void)`.
    *   Add cleanup code to prevent memory leaks:
        ```c
        if (editor.lisp_ctx) {
            FeCloseContext(editor.lisp_ctx);
            free(editor.lisp_arena);
        }
        ```

---

## Phase 3: Safe Evaluation & Error Handling

By default, if `fe` encounters a Lisp error, it prints to `stderr` and calls `exit(1)`. This is unacceptable for a text editora typo in a Lisp script shouldn't crash the editor! We need to catch errors using C's `setjmp`/`longjmp`.

1.  **Create `src/lisp.c` (and `src/lisp.h`)**:
    *   Add `lisp.c` to your `Makefile` `SRCS`.
2.  **Implement the Error Handler (`src/lisp.c`)**:
    ```c
    #include "def.h"
    #include <setjmp.h>

    static jmp_buf lisp_err_jmp;

    static void lisp_error_handler(FeContext* ctx, const char* err, FeObject* cl) {
        /* Tell kg to show the error in the status bar */
        editor_set_status_message("Lisp error: %s", err);
        /* Jump back to safety instead of crashing */
        longjmp(lisp_err_jmp, 1);
    }
    ```
3.  **Implement a Safe `eval` Function (`src/lisp.c`)**:
    ```c
    void lisp_init_handlers(void) {
        FeGetHandlers(editor.lisp_ctx)->error = lisp_error_handler;
    }

    void lisp_eval_string(const char *code) {
        FeContext *ctx = editor.lisp_ctx;
        size_t gc_save = FeSaveGC(ctx);
        
        /* If longjmp is called, execution returns here and setjmp returns 1 */
        if (setjmp(lisp_err_jmp) == 0) {
            /* Try to read and evaluate the string */
            FeObject *obj = FeRead(ctx, /* implement a string reader fn here, or use FeEvalute directly on lists */ ...); 
            // Note: Since FeRead requires a callback, a simple helper to read from a string is needed.
            // Alternatively, write a temporary file or implement a memory reader callback.
            
            // Assuming you parse `code` into `obj`:
            FeObject *result = FeEvaluate(ctx, obj);
            
            // Print result to status bar
            char buf[128];
            FeToString(ctx, result, buf, sizeof(buf));
            editor_set_status_message("=> %s", buf);
        }
        
        /* Clean up garbage collector stack */
        FeRestoreGC(ctx, gc_save);
    }
    ```
    *Call `lisp_init_handlers()` right after `FeOpenContext` in `main.c`.*

---

## Phase 4: Connecting the Editor to Lisp (The Bridge)

Now, we give Lisp the power to control the editor. We do this by writing C functions that conform to `FeNativeFn` and binding them to Lisp symbols.

1.  **Write Native Wrappers (`src/lisp.c`)**:
    Let's expose `editor_set_status_message` and `editor_insert_text_raw`.
    ```c
    static FeObject* lisp_kg_message(FeContext* ctx, FeObject* arg) {
        char msg[256];
        /* Get first argument */
        FeObject *val = FeGetNextArgument(ctx, &arg);
        FeToString(ctx, val, msg, sizeof(msg));
        
        editor_set_status_message("%s", msg);
        return &nil; /* Return Lisp 'nil' */
    }

    static FeObject* lisp_kg_insert(FeContext* ctx, FeObject* arg) {
        char text[1024];
        FeObject *val = FeGetNextArgument(ctx, &arg);
        
        if (FeGetType(val) != FeTString) {
            FeHandleError(ctx, "kg-insert expects a string");
        }
        
        size_t len = FeToString(ctx, val, text, sizeof(text));
        editor_insert_text_raw(text, len);
        return &nil;
    }
    ```
2.  **Register Native Functions (`src/lisp.c`)**:
    ```c
    void lisp_register_api(void) {
        FeContext *ctx = editor.lisp_ctx;
        size_t gc_save = FeSaveGC(ctx);

        FeSet(ctx, FeMakeSymbol(ctx, "kg-message"), FeMakeNativeFn(ctx, lisp_kg_message));
        FeSet(ctx, FeMakeSymbol(ctx, "kg-insert"), FeMakeNativeFn(ctx, lisp_kg_insert));

        FeRestoreGC(ctx, gc_save);
    }
    ```
    *Call `lisp_register_api()` in `main.c` during initialization.*

---

## Phase 5: Executing Lisp from the UI

We want to let users type Lisp commands directly in the editor using `M-x eval-expression`.

1.  **Add to Command Table (`src/cmd.c`)**:
    *   Write a new command function:
        ```c
        static void cmd_eval_expression(int fd) {
            char expr[256];
            if (editor_read_line(fd, "Eval: ", expr, sizeof(expr)) < 0 || !expr[0]) {
                return;
            }
            lisp_eval_string(expr);
        }
        ```
    *   Add it to `cmdtable`:
        ```c
        { "eval-expression", cmd_eval_expression },
        ```
    *   Now you can press `M-x`, type `eval-expression`, and evaluate `(kg-insert "Hello from Lisp!")`.

---

## Phase 6: Loading the `init.fe` Config File

To truly make it customizable, the editor should run a script on startup.

1.  **Load Init File (`src/lisp.c`)**:
    ```c
    void lisp_load_init_file(void) {
        char path[512];
        snprintf(path, sizeof(path), "%s/.config/kg/init.fe", getenv("HOME"));
        
        FILE *fp = fopen(path, "rb");
        if (!fp) return; /* No init file, that's fine */

        FeContext *ctx = editor.lisp_ctx;
        size_t gc_save = FeSaveGC(ctx);

        if (setjmp(lisp_err_jmp) == 0) {
            while (1) {
                FeObject *obj = FeReadFile(ctx, fp);
                if (!obj) break; /* EOF */
                FeEvaluate(ctx, obj);
                FeRestoreGC(ctx, gc_save); /* prevent GC stack overflow during loop */
            }
        } else {
            /* Error occurred loading init.fe */
            /* Message is already set by lisp_error_handler */
        }
        
        fclose(fp);
    }
    ```
2.  **Call it at startup**: Add `lisp_load_init_file();` at the end of `init_editor()` in `main.c`.

---

## Junior Engineer Tips & Gotchas
*   **Garbage Collection (GC)**: Notice the `FeSaveGC` and `FeRestoreGC` patterns? `fe` expects you to manually mark where you started creating objects in C, and "restore" that state so temporary objects can be freed. **Always** wrap Lisp API calls in this pattern.
*   **String Reader**: `FeRead` in `fe` takes a callback `FeReadFn`. To parse a `char *` from memory in `lisp_eval_string` (Phase 3), you'll need a tiny state struct and a callback that returns one character at a time until `\0`.
    ```c
    struct str_reader { const char *str; size_t pos; };
    static char string_read_fn(FeContext* ctx, void* udata) {
        struct str_reader *r = udata;
        return r->str[r->pos++];
    }
    // usage:
    struct str_reader r = { code, 0 };
    FeObject *obj = FeRead(ctx, string_read_fn, &r);
    ```
*   **Compiler Errors**: If `gcc` complains about `static_assert` or `bool` in `fe.h`, ensure your Makefile's `CFLAGS` allows modern C features and that `<stdbool.h>` and `<assert.h>` are included.
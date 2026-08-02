#include <stddef.h>
#include <string.h>

#include "../fe/fe.h"
#include "lisp_internal.h"

/* Internal trampoline: kg_lisp_run_command evaluates
 * "(internal--run-pending-command)" under the normal step budget, so the
 * FeCall below inherits that budget.  Calling it directly from user code
 * just runs the pending command again and is harmless. */
static FeObject *native_run_pending(FeContext *context, FeObject *arguments)
{
	FeRequireNoArguments(context, arguments);
	if (!state.pending_command) {
		FeHandleError(context, "no pending command");
	}
	return FeCall(context, state.pending_command, nullptr, 0);
}

struct native_binding {
	const char *name;
	FeNativeFn *fn;
};

/* Every name is the Emacs one wherever Emacs has a matching form; the
 * rest are unprefixed and descriptive. */
static const struct native_binding native_bindings[] = {
	{ "message", native_message },
	{ "insert", native_insert },
	{ "buffer-name", native_buffer_name },
	{ "load", native_load },
	{ "global-set-key", native_bind_key },
	{ "global-unset-key", native_unbind_key },
	{ "point", native_point_offset },
	{ "point-min", native_point_min },
	{ "point-max", native_point_max },
	{ "goto-char", native_goto_char },
	{ "goto-line", native_goto_line },
	{ "line-number-at-pos", native_line_number },
	{ "current-column", native_current_column },
	{ "mark", native_mark },
	{ "set-mark", native_set_mark },
	{ "deactivate-mark", native_deactivate_mark },
	{ "region-beginning", native_region_beginning },
	{ "region-end", native_region_end },
	{ "buffer-substring", native_buffer_substring },
	{ "char-after", native_char_after },
	{ "forward-word", native_forward_word },
	{ "backward-word", native_backward_word },
	{ "bounds-of-thing-at-point", native_bounds_of_thing },
	{ "string-length", native_string_length },
	{ "substring", native_substring },
	{ "concat", native_concat },
	{ "format", native_format },
	{ "string=", native_string_equal },
	{ "char-to-string", native_char_to_string },
	{ "string-to-char", native_string_to_char },
	{ "type-of", native_type_of },
	{ "stringp", native_stringp },
	{ "symbolp", native_symbolp },
	{ "numberp", native_numberp },
	{ "consp", native_consp },
	{ "functionp", native_functionp },
	{ "command-execute", native_command },
	/* Emacs defines commands with defun plus (interactive); kg keeps a
	 * name -> function registry, so these two have no Emacs analogue. */
	{ "define-command", native_define_command },
	{ "remove-command", native_remove_command },
	{ "internal--run-pending-command", native_run_pending },
};

void register_natives(FeContext *context)
{
	size_t i;

	for (i = 0; i < sizeof(native_bindings) / sizeof(native_bindings[0]);
	    i++) {
		FeDefineNative(
		    context, native_bindings[i].name, native_bindings[i].fn);
	}
}

/* Forms kg provides that upstream fe does not: the Emacs Lisp surface,
 * written in Fe and evaluated at startup so it is available before any
 * init file runs.
 *
 * Three rules hold everywhere below.
 *
 * 1. Ordering is load-bearing.  An alias of a primitive must be taken
 *    before anything shadows that name (only `let` is shadowed), and a
 *    macro must not expand into a name that shadows what it meant.
 * 2. No macro may expand to bare nil.  Fe splices the expansion over the
 *    caller's cons cell, and its nil test is pointer equality, so the copy
 *    would be a nil-shaped truthy object.  Expand to (quote nil) instead.
 * 3. Nothing here recurses over a list spine.  Fe's GC stack caps
 *    recursion at a few hundred frames, so list walks are `while` loops.
 *
 * Macros also expand exactly once per call site, so every macro here is a
 * pure function of its unevaluated argument forms.
 *
 * The prelude bootstraps itself with the primitives it is about to wrap:
 * definitions use `=` rather than the `setq` and `defun` defined further
 * down, so nothing here depends on an expansion happening first.
 *
 * The parts are separate string literals only to stay inside the 4095-byte
 * literal C guarantees; they are evaluated in order and the split points
 * carry no meaning beyond the section comments. */
static const char *const lisp_prelude[] = {
	/* Emacs spellings for Fe primitives.  `internal--let` keeps Fe's
	 * one-binding `let` reachable after the Emacs `let` shadows it; the
	 * function bodies below use it. */
	"(= internal--let let)\n"
	"(= progn do)\n"
	"(= null not)\n"
	"(= eq is)\n"
	"(= function (lambda (f) f))\n"
	/* (setq A 1 B 2 ...): Fe's `=` silently drops the extra pairs, and it
	 * returns nil where Emacs returns the value just assigned. */
	"(= setq (macro pairs\n"
	"  (if (null pairs)\n"
	"      (list 'quote nil)\n"
	"    (if (null (cdr (cdr pairs)))\n"
	"        (list 'do (list '= (car pairs) (car (cdr pairs)))\n"
	"          (car pairs))\n"
	"      (list 'do (list '= (car pairs) (car (cdr pairs)))\n"
	"        (cons 'setq (cdr (cdr pairs))))))))\n"
	"(= 1+ (lambda (n) (+ n 1)))\n"
	"(= 1- (lambda (n) (- n 1)))\n"
	"(= caar (lambda (x) (car (car x))))\n"
	"(= cadr (lambda (x) (car (cdr x))))\n"
	"(= cddr (lambda (x) (cdr (cdr x))))\n"
	"(= listp (lambda (x) (if (null x) t (consp x))))\n"
	/* --- list library, all iterative --- */
	"(= reverse (lambda (lst)\n"
	"  (internal--let res nil)\n"
	"  (while lst\n"
	"    (setq res (cons (car lst) res))\n"
	"    (setq lst (cdr lst)))\n"
	"  res))\n"
	"(= internal--append2 (lambda (a b)\n"
	"  (internal--let res b)\n"
	"  (internal--let r (reverse a))\n"
	"  (while r\n"
	"    (setq res (cons (car r) res))\n"
	"    (setq r (cdr r)))\n"
	"  res))\n"
	"(= append (lambda lists\n"
	"  (internal--let r (reverse lists))\n"
	"  (internal--let res (car r))\n"
	"  (setq r (cdr r))\n"
	"  (while r\n"
	"    (setq res (internal--append2 (car r) res))\n"
	"    (setq r (cdr r)))\n"
	"  res))\n"
	"(= length (lambda (x)\n"
	"  (if (stringp x)\n"
	"      (string-length x)\n"
	"    (internal--let n 0)\n"
	"    (while x\n"
	"      (setq n (+ n 1))\n"
	"      (setq x (cdr x)))\n"
	"    n)))\n"
	"(= nthcdr (lambda (n lst)\n"
	"  (while (and (< 0 n) lst)\n"
	"    (setq n (- n 1))\n"
	"    (setq lst (cdr lst)))\n"
	"  lst))\n"
	"(= nth (lambda (n lst) (car (nthcdr n lst))))\n"
	"(= last (lambda (lst)\n"
	"  (while (cdr lst) (setq lst (cdr lst)))\n"
	"  lst))\n"
	/* Structural on lists; Fe's `is` compares pairs by identity.  Only the
	 * car descends, so the spine cost is a loop, not a frame. */
	"(= equal (lambda (a b)\n"
	"  (internal--let same t)\n"
	"  (while (and same (consp a) (consp b))\n"
	"    (if (equal (car a) (car b)) nil (setq same nil))\n"
	"    (setq a (cdr a))\n"
	"    (setq b (cdr b)))\n"
	"  (and same (not (consp a)) (not (consp b)) (is a b))))\n"
	"(= mapcar (lambda (f lst)\n"
	"  (internal--let res nil)\n"
	"  (while lst\n"
	"    (setq res (cons (f (car lst)) res))\n"
	"    (setq lst (cdr lst)))\n"
	"  (reverse res)))\n"
	"(= member (lambda (elt lst)\n"
	"  (while (and lst (not (equal elt (car lst))))\n"
	"    (setq lst (cdr lst)))\n"
	"  lst))\n"
	"(= memq (lambda (elt lst)\n"
	"  (while (and lst (not (eq elt (car lst))))\n"
	"    (setq lst (cdr lst)))\n"
	"  lst))\n"
	"(= assoc (lambda (key alist)\n"
	"  (internal--let hit nil)\n"
	"  (while (and alist (null hit))\n"
	"    (if (equal key (car (car alist))) (setq hit (car alist)))\n"
	"    (setq alist (cdr alist)))\n"
	"  hit))\n",
	/* --- control macros --- */
	"(= cond (macro clauses\n"
	"  (if clauses\n"
	"      (list 'if (car (car clauses))\n"
	"        (cons 'progn (cdr (car clauses)))\n"
	"        (cons 'cond (cdr clauses)))\n"
	"    (list 'quote nil))))\n"
	"(= when (macro (test . body)\n"
	"  (list 'if test (cons 'progn body))))\n"
	"(= unless (macro (test . body)\n"
	"  (cons 'if (cons test (cons nil body)))))\n"
	"(= internal--first (lambda args (car args)))\n"
	"(= prog1 (macro (first . body)\n"
	"  (cons 'internal--first (cons first body))))\n"
	/* --- binding forms --- */
	"(= internal--bind-name (lambda (b) (if (atom b) b (car b))))\n"
	"(= internal--bind-value (lambda (b)\n"
	"  (if (atom b) nil (car (cdr b)))))\n"
	/* Parallel, via immediate application: the value forms are evaluated
	 * as arguments, in the environment outside the new bindings. */
	"(= let (macro (bindings . body)\n"
	"  (cons (cons 'lambda\n"
	"          (cons (mapcar internal--bind-name bindings) body))\n"
	"    (mapcar internal--bind-value bindings))))\n"
	"(= let* (macro (bindings . body)\n"
	"  (internal--let forms nil)\n"
	"  (while bindings\n"
	"    (setq forms (cons (list 'internal--let\n"
	"                        (internal--bind-name (car bindings))\n"
	"                        (internal--bind-value (car bindings)))\n"
	"                  forms))\n"
	"    (setq bindings (cdr bindings)))\n"
	"  (cons 'progn (internal--append2 (reverse forms) body))))\n"
	/* --- iteration macros --- */
	"(= internal--dolist (lambda (items body)\n"
	"  (while items\n"
	"    (body (car items))\n"
	"    (setq items (cdr items)))))\n"
	"(= dolist (macro (spec . body)\n"
	"  (list 'progn\n"
	"    (list 'internal--dolist (car (cdr spec))\n"
	"      (cons 'lambda (cons (list (car spec)) body)))\n"
	"    (car (cdr (cdr spec))))))\n"
	"(= internal--dotimes (lambda (count body)\n"
	"  (internal--let i 0)\n"
	"  (while (< i count)\n"
	"    (body i)\n"
	"    (setq i (+ i 1)))))\n"
	"(= dotimes (macro (spec . body)\n"
	"  (list 'progn\n"
	"    (list 'internal--dotimes (car (cdr spec))\n"
	"      (cons 'lambda (cons (list (car spec)) body)))\n"
	"    (car (cdr (cdr spec))))))\n"
	"(= push (macro (item place)\n"
	"  (list 'setq place (list 'cons item place))))\n"
	"(= pop (macro (place)\n"
	"  (list 'prog1 (list 'car place)\n"
	"    (list 'setq place (list 'cdr place)))))\n"
	/* --- quasiquote: `x , ,@ read as (quasiquote x) etc. --- */
	"(= internal--qq (lambda (form)\n"
	"  (if (atom form)\n"
	"      (list 'quote form)\n"
	"    (if (eq (car form) 'unquote)\n"
	"        (car (cdr form))\n"
	"      (internal--qq-list form)))))\n"
	"(= internal--qq-list (lambda (form)\n"
	"  (internal--let segs nil)\n"
	"  (while (consp form)\n"
	"    (internal--let e (car form))\n"
	"    (if (and (consp e) (eq (car e) 'unquote-splicing))\n"
	"        (setq segs (cons (car (cdr e)) segs))\n"
	"      (setq segs (cons (list 'list (internal--qq e)) segs)))\n"
	"    (setq form (cdr form)))\n"
	"  (if form (setq segs (cons (internal--qq form) segs)))\n"
	"  (cons 'append (reverse segs))))\n"
	"(= quasiquote (macro (form) (internal--qq form)))\n",
	/* --- definition forms --- */
	/* Argument lists go to Fe unchanged: its binder reads &optional and
	 * &rest itself, as well as its own dotted and bare-symbol forms.  kg
	 * used to lower "(a &optional b &rest r)" to "(a b . r)" here, which
	 * worked only because the binder accepted any argument count. */
	"(= internal--interactive-p (lambda (form)\n"
	"  (if (atom form) nil (eq (car form) 'interactive))))\n"
	"(= internal--has-interactive (lambda (body)\n"
	"  (internal--let hit nil)\n"
	"  (while body\n"
	"    (if (internal--interactive-p (car body)) (setq hit t))\n"
	"    (setq body (cdr body)))\n"
	"  hit))\n"
	"(= internal--strip-interactive (lambda (body)\n"
	"  (internal--let out nil)\n"
	"  (while body\n"
	"    (if (internal--interactive-p (car body))\n"
	"        nil\n"
	"      (setq out (cons (car body) out)))\n"
	"    (setq body (cdr body)))\n"
	"  (reverse out)))\n"
	/* A body form (interactive) registers the function as a command, the
	 * way Emacs makes a defun interactive; define-command takes the
	 * symbol.  defun returns the name, as Emacs does. */
	"(= defun (macro (name params . body)\n"
	"  (internal--let f (cons 'lambda (cons params\n"
	"                     (internal--strip-interactive body))))\n"
	"  (if (internal--has-interactive body)\n"
	"      (list 'progn (list 'setq name f)\n"
	"        (list 'define-command (list 'quote name) name)\n"
	"        (list 'quote name))\n"
	"    (list 'progn (list 'setq name f) (list 'quote name)))))\n"
	"(= defmacro (macro (name params . body)\n"
	"  (list 'progn\n"
	"    (list 'setq name\n"
	"      (cons 'macro (cons params body)))\n"
	"    (list 'quote name))))\n"
	/* Fe distinguishes an unassigned symbol from one holding nil, so
	 * defvar asks boundp rather than reading the variable -- which would
	 * now raise void-variable -- and a variable holding nil stays nil. */
	"(= defvar (macro (name . rest)\n"
	"  (list 'progn\n"
	"    (list 'if (list 'boundp (list 'quote name))\n"
	"      (list 'quote nil)\n"
	"      (list 'setq name (car rest)))\n"
	"    (list 'quote name))))\n"
	"(= defconst (macro (name . rest)\n"
	"  (list 'progn (list 'setq name (car rest)) (list 'quote name))))\n"
	/* Inert outside defun: a stray top-level (interactive) is harmless. */
	"(= interactive (macro args (list 'quote nil)))\n"
	/* --- editor helpers --- */
	"(= string-empty-p (lambda (s) (string= s \"\")))\n"
	"(= thing-at-point (lambda (thing)\n"
	"  (internal--let bounds (bounds-of-thing-at-point thing))\n"
	"  (if bounds (buffer-substring (car bounds) (cdr bounds)))))\n",
};

/* The prelude gets its own evaluation, so it neither consumes nor shares
 * the budget of later user evaluations. */
void evaluate_prelude(FeContext *context)
{
	size_t i;

	for (i = 0; i < sizeof(lisp_prelude) / sizeof(lisp_prelude[0]); i++) {
		(void)FeEvaluateStringWithOptions(context, "prelude",
		    lisp_prelude[i], strlen(lisp_prelude[i]), &eval_options);
	}
}

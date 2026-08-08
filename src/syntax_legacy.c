/* ===================== Legacy syntax highlighting backend =================
 *
 * The bespoke row scanners kg inherited from kilo and grew: one keyword
 * database per language plus five hand-written per-mode scanners, all of
 * them working a row at a time and carrying whatever cross-row state they
 * need in row->hl_oc.
 *
 * This file is one implementation of src/syntax_backend.h, selected by the
 * Makefile's source list rather than by a preprocessor branch, so a build
 * that installs a different backend simply does not compile it.  Nothing
 * here is reachable by name from src/syntax.c: the facade owns mode
 * identity, mode selection, colour mapping and hl[] allocation, and calls
 * exactly one function here.
 *
 * To add a language: define its keyword list below, its file-name patterns
 * and registry row in src/syntax.c, and -- if it needs more than the
 * generic keyword/comment/string/number scan -- a scanner function and a
 * legacy_syntax_spec row naming it.  A keyword with a trailing '|' is
 * highlighted in the second keyword colour.  The characters for single and
 * multi line comments must be exactly two, or exactly one. */

#include <ctype.h>
#include <string.h>

#include "def.h"
#include "syntax.h"
#include "syntax_backend.h"
#include "syntax_legacy.h"

/* Enable string and number highlighting for a mode scanned generically.
 * Recorded per mode below for the same reason the comment delimiters are:
 * it is the scanner's configuration, not the mode's identity.  The generic
 * scanner does not consult them today -- it highlights both unconditionally
 * -- so they are documentation of intent, kept where a scanner that grows a
 * switch for them would look. */
#define HL_HIGHLIGHT_STRINGS (1 << 0)
#define HL_HIGHLIGHT_NUMBERS (1 << 1)

/* C / C++ */
static char *C_HL_keywords[] = {
	/* C Keywords */
	"auto", "break", "case", "continue", "default", "do", "else", "enum",
	"extern", "for", "goto", "if", "register", "return", "sizeof", "static",
	"struct", "switch", "typedef", "union", "volatile", "while", "NULL",

	/* C++ Keywords */
	"alignas", "alignof", "and", "and_eq", "asm", "bitand", "bitor",
	"class", "compl", "constexpr", "const_cast", "deltype", "delete",
	"dynamic_cast", "explicit", "export", "false", "friend", "inline",
	"mutable", "namespace", "new", "noexcept", "not", "not_eq", "nullptr",
	"operator", "or", "or_eq", "private", "protected", "public",
	"reinterpret_cast", "static_assert", "static_cast", "template", "this",
	"thread_local", "throw", "true", "try", "typeid", "typename", "virtual",
	"xor", "xor_eq",

	/* C types */
	"int|", "long|", "double|", "float|", "char|", "unsigned|", "signed|",
	"void|", "short|", "auto|", "const|", "bool|", NULL
};

/* Python */
static char *PYTHON_HL_keywords[] = {
	/* Python Keywords */
	"and", "as", "assert", "break", "class", "continue", "def", "del",
	"elif", "else", "except", "exec", "finally", "for", "from", "global",
	"if", "import", "in", "is", "lambda", "not", "or", "pass", "print",
	"raise", "return", "try", "while", "with", "yield", "async", "await",
	"nonlocal", "True", "False", "None",

	/* Python Built-ins */
	"abs|", "all|", "any|", "bin|", "bool|", "bytearray|", "bytes|",
	"callable|", "chr|", "classmethod|", "compile|", "complex|", "delattr|",
	"dict|", "dir|", "divmod|", "enumerate|", "eval|", "exec|", "filter|",
	"float|", "format|", "frozenset|", "getattr|", "globals|", "hasattr|",
	"hash|", "help|", "hex|", "id|", "input|", "int|", "isinstance|",
	"issubclass|", "iter|", "len|", "list|", "locals|", "map|", "max|",
	"memoryview|", "min|", "next|", "object|", "oct|", "open|", "ord|",
	"pow|", "property|", "range|", "repr|", "reversed|", "round|", "set|",
	"setattr|", "slice|", "sorted|", "staticmethod|", "str|", "sum|",
	"super|", "tuple|", "type|", "vars|", "zip|", "self|", "cls|", NULL
};

/* Shell */
static char *SHELL_HL_keywords[] = {
	/* Shell Keywords */
	"if", "then", "else", "elif", "fi", "case", "esac", "for", "while",
	"until", "do", "done", "select", "function", "in", "time", "coproc",

	/* Common commands */
	"alias|", "bg|", "bind|", "break|", "builtin|", "caller|", "cd|",
	"command|", "compgen|", "complete|", "continue|", "declare|", "dirs|",
	"disown|", "echo|", "enable|", "eval|", "exec|", "exit|", "export|",
	"false|", "fc|", "fg|", "getopts|", "hash|", "help|", "history|",
	"jobs|", "kill|", "let|", "local|", "logout|", "mapfile|", "popd|",
	"printf|", "pushd|", "pwd|", "read|", "readarray|", "readonly|",
	"return|", "set|", "shift|", "shopt|", "source|", "suspend|", "test|",
	"times|", "trap|", "true|", "type|", "typeset|", "ulimit|", "umask|",
	"unalias|", "unset|", "wait|",

	/* System utilities */
	"awk|", "cat|", "chmod|", "chown|", "cp|", "curl|", "cut|", "date|",
	"df|", "diff|", "dig|", "du|", "find|", "grep|", "head|", "ln|", "ls|",
	"mkdir|", "mv|", "ping|", "ps|", "rm|", "rsync|", "scp|", "sed|",
	"ssh|", "sudo|", "tail|", "tar|", "top|", "touch|", "tr|", "uniq|",
	"wc|", "wget|", "which|", "xargs|",

	/* Special variables */
	"$BASH|", "$BASHOPTS|", "$BASHPID|", "$BASH_ALIASES|", "$BASH_ARGC|",
	"$BASH_ARGV|", "$BASH_CMDS|", "$BASH_COMMAND|", "$BASH_ENV|",
	"$BASH_LINENO|", "$BASH_SOURCE|", "$BASH_SUBSHELL|", "$BASH_VERSION|",
	"$DIRSTACK|", "$EUID|", "$FUNCNAME|", "$GROUPS|", "$HOME|",
	"$HOSTNAME|", "$HOSTTYPE|", "$IFS|", "$LINENO|", "$MACHTYPE|",
	"$OLDPWD|", "$OPTARG|", "$OPTIND|", "$OSTYPE|", "$PATH|",
	"$PIPESTATUS|", "$PPID|", "$PS1|", "$PS2|", "$PS3|", "$PS4|", "$PWD|",
	"$RANDOM|", "$REPLY|", "$SECONDS|", "$SHELL|", "$SHELLOPTS|", "$SHLVL|",
	"$UID|", NULL
};

/* JavaScript */
static char *JS_HL_keywords[] = {
	/* JavaScript Keywords */
	"break", "case", "catch", "class", "const", "continue", "debugger",
	"default", "delete", "do", "else", "export", "extends", "finally",
	"for", "function", "if", "import", "in", "instanceof", "let", "new",
	"return", "super", "switch", "this", "throw", "try", "typeof", "var",
	"void", "while", "with", "yield", "async", "await", "of", "true",
	"false", "null", "undefined",

	/* JavaScript Built-ins */
	"Array|", "Object|", "String|", "Number|", "Boolean|", "Date|", "Math|",
	"RegExp|", "Error|", "JSON|", "console|", "window|", "document|",
	"setTimeout|", "setInterval|", "clearTimeout|", "clearInterval|",
	"parseInt|", "parseFloat|", "isNaN|", "isFinite|", "encodeURI|",
	"decodeURI|", "Promise|", "Map|", "Set|", "WeakMap|", "WeakSet|",
	"Symbol|", "Proxy|", "Reflect|", "Generator|", NULL
};

/* Rust */
static char *RUST_HL_keywords[] = {
	/* Rust Keywords */
	"as", "async", "await", "break", "const", "continue", "crate", "dyn",
	"else", "enum", "extern", "false", "fn", "for", "if", "impl", "in",
	"let", "loop", "match", "mod", "move", "mut", "pub", "ref", "return",
	"self", "Self", "static", "struct", "super", "trait", "true", "type",
	"unsafe", "use", "where", "while", "abstract", "become", "box", "do",
	"final", "macro", "override", "priv", "typeof", "unsized", "virtual",
	"yield", "try", "union", "catch", "default",

	/* Rust Types */
	"i8|", "i16|", "i32|", "i64|", "i128|", "isize|", "u8|", "u16|", "u32|",
	"u64|", "u128|", "usize|", "f32|", "f64|", "bool|", "char|", "str|",
	"String|", "Vec|", "HashMap|", "HashSet|", "BTreeMap|", "BTreeSet|",
	"Option|", "Result|", "Box|", "Rc|", "Arc|", "RefCell|", "Cell|",
	"Mutex|", "RwLock|", "thread|", "Clone|", "Copy|", "Send|", "Sync|",
	"Drop|", "Display|", "Debug|", "Default|", "PartialEq|", "Eq|",
	"PartialOrd|", "Ord|", "Hash|", "Iterator|", "IntoIterator|", NULL
};

/* Java */
static char *JAVA_HL_keywords[] = {
	/* Java Keywords */
	"abstract", "assert", "boolean", "break", "byte", "case", "catch",
	"char", "class", "const", "continue", "default", "do", "double", "else",
	"enum", "extends", "final", "finally", "float", "for", "goto", "if",
	"implements", "import", "instanceof", "int", "interface", "long",
	"native", "new", "package", "private", "protected", "public", "return",
	"short", "static", "strictfp", "super", "switch", "synchronized",
	"this", "throw", "throws", "transient", "try", "void", "volatile",
	"while", "true", "false", "null",

	/* Java Types and Common Classes */
	"String|", "Object|", "Class|", "System|", "Thread|", "Runnable|",
	"Exception|", "RuntimeException|", "ArrayList|", "HashMap|", "List|",
	"Map|", "Set|", "Collection|", "Iterator|", "Comparable|",
	"Serializable|", NULL
};

/* TypeScript */
static char *TS_HL_keywords[] = {
	/* TypeScript Keywords (includes JavaScript) */
	"break", "case", "catch", "class", "const", "continue", "debugger",
	"default", "delete", "do", "else", "export", "extends", "finally",
	"for", "function", "if", "import", "in", "instanceof", "let", "new",
	"return", "super", "switch", "this", "throw", "try", "typeof", "var",
	"void", "while", "with", "yield", "async", "await", "of", "true",
	"false", "null", "undefined",

	/* TypeScript Specific */
	"interface", "type", "enum", "namespace", "module", "declare",
	"abstract", "implements", "private", "protected", "public", "readonly",
	"static", "get", "set", "as", "keyof", "infer", "is", "asserts",

	/* TypeScript Types */
	"string|", "number|", "boolean|", "object|", "any|", "unknown|",
	"never|", "void|", "bigint|", "symbol|", "Array|", "Promise|",
	"Record|", "Partial|", "Required|", "Pick|", "Omit|", "Exclude|",
	"Extract|", "NonNullable|", NULL
};

/* C# */
static char *CSHARP_HL_keywords[] = {
	/* C# Keywords */
	"abstract", "as", "base", "bool", "break", "byte", "case", "catch",
	"char", "checked", "class", "const", "continue", "decimal", "default",
	"delegate", "do", "double", "else", "enum", "event", "explicit",
	"extern", "false", "finally", "fixed", "float", "for", "foreach",
	"goto", "if", "implicit", "in", "int", "interface", "internal", "is",
	"lock", "long", "namespace", "new", "null", "object", "operator", "out",
	"override", "params", "private", "protected", "public", "readonly",
	"ref", "return", "sbyte", "sealed", "short", "sizeof", "stackalloc",
	"static", "string", "struct", "switch", "this", "throw", "true", "try",
	"typeof", "uint", "ulong", "unchecked", "unsafe", "ushort", "using",
	"virtual", "void", "volatile", "while", "async", "await", "var",
	"dynamic", "yield", "where", "when", "nameof",

	/* C# Types */
	"String|", "Object|", "Int32|", "Boolean|", "Double|", "DateTime|",
	"List|", "Dictionary|", "Array|", "IEnumerable|", "ICollection|",
	"IList|", "Task|", "Exception|", "ArgumentException|",
	"NullReferenceException|", NULL
};

/* PHP */
static char *PHP_HL_keywords[] = {
	/* PHP Keywords */
	"abstract", "and", "array", "as", "break", "callable", "case", "catch",
	"class", "clone", "const", "continue", "declare", "default", "die",
	"do", "echo", "else", "elseif", "empty", "enddeclare", "endfor",
	"endforeach", "endif", "endswitch", "endwhile", "eval", "exit",
	"extends", "final", "finally", "for", "foreach", "function", "global",
	"goto", "if", "implements", "include", "include_once", "instanceof",
	"insteadof", "interface", "isset", "list", "namespace", "new", "or",
	"print", "private", "protected", "public", "require", "require_once",
	"return", "static", "switch", "throw", "trait", "try", "unset", "use",
	"var", "while", "xor", "yield", "true", "false", "null",

	/* PHP Built-ins */
	"$_GET|", "$_POST|", "$_SESSION|", "$_COOKIE|", "$_SERVER|", "$_FILES|",
	"$_ENV|", "$_REQUEST|", "$GLOBALS|", "strlen|", "substr|", "strpos|",
	"explode|", "implode|", "array_merge|", "array_push|", "array_pop|",
	"count|", "sizeof|", "is_array|", "is_string|", "is_numeric|", "empty|",
	"isset|", "unset|", "die|", "exit|", "echo|", "print|", "var_dump|",
	NULL
};

/* Ruby */
static char *RUBY_HL_keywords[] = {
	/* Ruby Keywords */
	"alias", "and", "begin", "break", "case", "class", "def", "defined",
	"do", "else", "elsif", "end", "ensure", "false", "for", "if", "in",
	"module", "next", "nil", "not", "or", "redo", "rescue", "retry",
	"return", "self", "super", "then", "true", "undef", "unless", "until",
	"when", "while", "yield", "require", "include", "extend", "attr_reader",
	"attr_writer", "attr_accessor",

	/* Ruby Built-ins */
	"puts|", "print|", "p|", "gets|", "chomp|", "strip|", "length|",
	"size|", "empty|", "nil|", "class|", "new|", "initialize|", "to_s|",
	"to_i|", "to_f|", "to_a|", "each|", "map|", "select|", "reject|",
	"find|", "inject|", "reduce|", "Array|", "Hash|", "String|", "Integer|",
	"Float|", "Symbol|", "Proc|", "Lambda|", "Method|", "Class|", "Module|",
	"Object|", "Kernel|", NULL
};

/* Swift */
static char *SWIFT_HL_keywords[] = {
	/* Swift Keywords */
	"associatedtype", "class", "deinit", "enum", "extension", "fileprivate",
	"func", "import", "init", "inout", "internal", "let", "open",
	"operator", "private", "protocol", "public", "static", "struct",
	"subscript", "typealias", "var", "break", "case", "continue", "default",
	"defer", "do", "else", "fallthrough", "for", "guard", "if", "in",
	"repeat", "return", "switch", "where", "while", "as", "catch", "false",
	"is", "nil", "rethrows", "super", "self", "Self", "throw", "throws",
	"true", "try", "async", "await", "some", "any",

	/* Swift Types */
	"Int|", "Double|", "Float|", "Bool|", "String|", "Character|", "Array|",
	"Dictionary|", "Set|", "Optional|", "Result|", "Error|", "AnyObject|",
	"AnyClass|", "Protocol|", "Codable|", "Hashable|", "Equatable|",
	"Comparable|", "Collection|", "Sequence|", NULL
};

/* SQL */
static char *SQL_HL_keywords[] = {
	/* SQL Keywords */
	"SELECT", "FROM", "WHERE", "INSERT", "UPDATE", "DELETE", "CREATE",
	"DROP", "ALTER", "TABLE", "INDEX", "VIEW", "DATABASE", "SCHEMA",
	"COLUMN", "PRIMARY", "FOREIGN", "KEY", "REFERENCES", "CONSTRAINT",
	"UNIQUE", "NOT", "NULL", "DEFAULT", "AUTO_INCREMENT", "IDENTITY",
	"SERIAL", "BOOLEAN", "TINYINT", "SMALLINT", "MEDIUMINT", "INT",
	"INTEGER", "BIGINT", "DECIMAL", "NUMERIC", "FLOAT", "DOUBLE", "REAL",
	"BIT", "DATE", "TIME", "DATETIME", "TIMESTAMP", "YEAR", "CHAR",
	"VARCHAR", "BINARY", "VARBINARY", "TINYBLOB", "BLOB", "MEDIUMBLOB",
	"LONGBLOB", "TINYTEXT", "TEXT", "MEDIUMTEXT", "LONGTEXT", "ENUM", "SET",
	"JSON", "GEOMETRY", "POINT", "LINESTRING", "POLYGON", "MULTIPOINT",
	"MULTILINESTRING", "MULTIPOLYGON", "GEOMETRYCOLLECTION", "AND", "OR",
	"IN", "BETWEEN", "LIKE", "IS", "EXISTS", "ANY", "ALL", "SOME", "UNION",
	"INTERSECT", "EXCEPT", "INNER", "LEFT", "RIGHT", "FULL", "OUTER",
	"JOIN", "ON", "USING", "GROUP", "BY", "HAVING", "ORDER", "ASC", "DESC",
	"LIMIT", "OFFSET", "DISTINCT", "AS", "CASE", "WHEN", "THEN", "ELSE",
	"END", "IF", "IFNULL", "ISNULL", "COALESCE", "NULLIF", "CAST",
	"CONVERT", "SUBSTRING", "LENGTH", "UPPER", "LOWER", "TRIM", "LTRIM",
	"RTRIM", "REPLACE", "CONCAT", "CURRENT_DATE", "CURRENT_TIME",
	"CURRENT_TIMESTAMP", "NOW", "COUNT", "SUM", "AVG", "MIN", "MAX",
	"STDDEV", "VARIANCE", "BEGIN", "COMMIT", "ROLLBACK", "TRANSACTION",
	"SAVEPOINT", "GRANT", "REVOKE", "LOCK", "UNLOCK",

	/* SQL Functions and Operators */
	"TRUE|", "FALSE|", "UNKNOWN|", NULL
};

/* Dart */
static char *DART_HL_keywords[] = {
	/* Dart Keywords */
	"abstract", "as", "assert", "async", "await", "break", "case", "catch",
	"class", "const", "continue", "covariant", "default", "deferred", "do",
	"dynamic", "else", "enum", "export", "extends", "extension", "external",
	"factory", "false", "final", "finally", "for", "Function", "get",
	"hide", "if", "implements", "import", "in", "interface", "is", "late",
	"library", "mixin", "new", "null", "on", "operator", "part", "required",
	"rethrow", "return", "set", "show", "static", "super", "switch", "sync",
	"this", "throw", "true", "try", "typedef", "var", "void", "while",
	"with", "yield",

	/* Dart Types */
	"int|", "double|", "num|", "String|", "bool|", "List|", "Map|", "Set|",
	"Object|", "dynamic|", "var|", "void|", "Future|", "Stream|",
	"Iterable|", "Iterator|", "Comparable|", "Duration|", "DateTime|",
	"Uri|", "RegExp|", "StringBuffer|", "Symbol|", "Type|", "Function|",
	"Null|", NULL
};

/* HTML */
static char *HTML_HL_keywords[] = {
	/* Opening tags */
	"<a>", "<abbr>", "<address>", "<article>", "<aside>", "<audio>", "<b>",
	"<bdi>", "<bdo>", "<blockquote>", "<body>", "<br>", "<button>",
	"<canvas>", "<caption>", "<cite>", "<code>", "<colgroup>", "<datalist>",
	"<dd>", "<del>", "<details>", "<dfn>", "<dialog>", "<div>", "<dl>",
	"<dt>", "<em>", "<embed>", "<fieldset>", "<figcaption>", "<figure>",
	"<footer>", "<form>", "<h1>", "<h2>", "<h3>", "<h4>", "<h5>", "<h6>",
	"<head>", "<header>", "<hr>", "<html>", "<i>", "<iframe>", "<img>",
	"<input>", "<ins>", "<kbd>", "<label>", "<legend>", "<li>", "<link>",
	"<main>", "<map>", "<mark>", "<meta>", "<meter>", "<nav>", "<noscript>",
	"<object>", "<ol>", "<optgroup>", "<option>", "<output>", "<p>",
	"<picture>", "<pre>", "<progress>", "<q>", "<s>", "<samp>", "<script>",
	"<section>", "<select>", "<small>", "<source>", "<span>", "<strong>",
	"<style>", "<sub>", "<summary>", "<sup>", "<svg>", "<table>", "<tbody>",
	"<td>", "<template>", "<textarea>", "<tfoot>", "<th>", "<thead>",
	"<time>", "<title>", "<tr>", "<track>", "<u>", "<ul>", "<var>",
	"<video>",

	/* Closing tags */
	"</a>", "</abbr>", "</address>", "</article>", "</aside>", "</audio>",
	"</b>", "</bdi>", "</bdo>", "</blockquote>", "</body>", "</button>",
	"</canvas>", "</caption>", "</cite>", "</code>", "</colgroup>",
	"</datalist>", "</dd>", "</del>", "</details>", "</dfn>", "</dialog>",
	"</div>", "</dl>", "</dt>", "</em>", "</fieldset>", "</figcaption>",
	"</figure>", "</footer>", "</form>", "</h1>", "</h2>", "</h3>", "</h4>",
	"</h5>", "</h6>", "</head>", "</header>", "</html>", "</i>",
	"</iframe>", "</ins>", "</kbd>", "</label>", "</legend>", "</li>",
	"</main>", "</map>", "</mark>", "</meter>", "</nav>", "</noscript>",
	"</object>", "</ol>", "</optgroup>", "</option>", "</output>", "</p>",
	"</picture>", "</pre>", "</progress>", "</q>", "</s>", "</samp>",
	"</script>", "</section>", "</select>", "</small>", "</span>",
	"</strong>", "</style>", "</sub>", "</summary>", "</sup>", "</svg>",
	"</table>", "</tbody>", "</td>", "</template>", "</textarea>",
	"</tfoot>", "</th>", "</thead>", "</time>", "</title>", "</tr>", "</u>",
	"</ul>", "</var>", "</video>",

	/* Common attributes */
	"class=|", "id=|", "style=|", "src=|", "href=|", "alt=|", "title=|",
	"width=|", "height=|", "type=|", "name=|", "value=|", "placeholder=|",
	NULL
};

/* React/JSX - extends JavaScript with React-specific features */
static char *REACT_HL_keywords[] = {
	/* All JavaScript keywords first */
	"async", "await", "break", "case", "catch", "class", "const",
	"continue", "debugger", "default", "delete", "do", "else", "export",
	"extends", "finally", "for", "from", "function", "if", "import", "in",
	"instanceof", "let", "new", "return", "static", "super", "switch",
	"this", "throw", "try", "typeof", "var", "void", "while", "with",
	"yield",

	/* React Hooks */
	"useState|", "useEffect|", "useContext|", "useReducer|", "useCallback|",
	"useMemo|", "useRef|", "useImperativeHandle|", "useLayoutEffect|",
	"useDebugValue|", "useDeferredValue|", "useTransition|", "useId|",
	"useSyncExternalStore|", "useInsertionEffect|",

	/* React API */
	"React|", "Component|", "PureComponent|", "Fragment|", "StrictMode|",
	"Suspense|", "createElement|", "createContext|", "forwardRef|", "lazy|",
	"memo|", "createRef|", "isValidElement|", "Children|", "cloneElement|",

	/* Common JSX/React patterns */
	"props|", "state|", "key|", "ref|", "defaultProps|", "propTypes|",
	"className|", "onClick|", "onChange|", "onSubmit|", "useState|",

	/* Common values */
	"true|", "false|", "null|", "undefined|", "NaN|", "Infinity|",

	/* Built-in objects */
	"Array|", "Object|", "String|", "Number|", "Boolean|", "Date|", "Math|",
	"JSON|", "Promise|", "Map|", "Set|", "WeakMap|", "WeakSet|", "Symbol|",
	"BigInt|", "RegExp|", "Error|", "console|", NULL
};

/* Vue.js - single file component syntax */
static char *VUE_HL_keywords[] = {
	/* JavaScript keywords */
	"async", "await", "break", "case", "catch", "class", "const",
	"continue", "debugger", "default", "delete", "do", "else", "export",
	"extends", "finally", "for", "from", "function", "if", "import", "in",
	"instanceof", "let", "new", "return", "static", "super", "switch",
	"this", "throw", "try", "typeof", "var", "void", "while", "with",
	"yield",

	/* Vue Composition API */
	"ref|", "reactive|", "computed|", "watch|", "watchEffect|",
	"onMounted|", "onUpdated|", "onUnmounted|", "onBeforeMount|",
	"onBeforeUpdate|", "onBeforeUnmount|", "onActivated|", "onDeactivated|",
	"onErrorCaptured|", "provide|", "inject|", "defineProps|",
	"defineEmits|", "defineExpose|", "useSlots|", "useAttrs|", "toRef|",
	"toRefs|", "isRef|", "unref|", "shallowRef|", "triggerRef|",
	"customRef|", "shallowReactive|", "readonly|", "shallowReadonly|",
	"toRaw|", "markRaw|",

	/* Vue Options API */
	"data|", "props|", "methods|", "computed|", "watch|", "emits|",
	"components|", "directives|", "mixins|", "extends|", "setup|",
	"beforeCreate|", "created|", "beforeMount|", "mounted|",
	"beforeUpdate|", "updated|", "beforeUnmount|", "unmounted|",

	/* Vue Directives */
	"v-if|", "v-else|", "v-else-if|", "v-for|", "v-show|", "v-bind|",
	"v-on|", "v-model|", "v-slot|", "v-pre|", "v-once|", "v-memo|",
	"v-cloak|", "v-html|", "v-text|",

	/* Vue Special Attributes */
	"key|", "ref|", "is|",

	/* Common values */
	"true|", "false|", "null|", "undefined|", "NaN|", "Infinity|",

	/* Built-in objects */
	"Array|", "Object|", "String|", "Number|", "Boolean|", "Date|", "Math|",
	"JSON|", "Promise|", "Map|", "Set|", "console|",

	/* Vue template tags */
	"<template>", "</template>", "<script>", "</script>", "<style>",
	"</style>", NULL
};

/* Angular - TypeScript with Angular decorators and directives */
static char *ANGULAR_HL_keywords[] = {
	/* TypeScript/JavaScript keywords */
	"abstract", "async", "await", "break", "case", "catch", "class",
	"const", "continue", "debugger", "default", "delete", "do", "else",
	"enum", "export", "extends", "finally", "for", "from", "function", "if",
	"implements", "import", "in", "instanceof", "interface", "let", "new",
	"private", "protected", "public", "readonly", "return", "static",
	"super", "switch", "this", "throw", "try", "type", "typeof", "var",
	"void", "while", "with", "yield",

	/* Angular Decorators */
	"@Component|", "@NgModule|", "@Injectable|", "@Directive|", "@Pipe|",
	"@Input|", "@Output|", "@ViewChild|", "@ViewChildren|",
	"@ContentChild|", "@ContentChildren|", "@HostBinding|",
	"@HostListener|",

	/* Angular Core */
	"OnInit|", "OnDestroy|", "OnChanges|", "DoCheck|", "AfterContentInit|",
	"AfterContentChecked|", "AfterViewInit|", "AfterViewChecked|",
	"ChangeDetectorRef|", "ElementRef|", "Renderer2|", "ViewContainerRef|",
	"TemplateRef|", "EventEmitter|", "Injector|",
	"ComponentFactoryResolver|",

	/* Angular Common */
	"ngFor|", "ngIf|", "ngSwitch|", "ngClass|", "ngStyle|", "ngModel|",
	"FormControl|", "FormGroup|", "FormBuilder|", "Validators|",
	"HttpClient|", "HttpHeaders|", "Observable|", "Subject|",
	"BehaviorSubject|", "Router|", "ActivatedRoute|", "RouterModule|",
	"Routes|",

	/* Common values */
	"true|", "false|", "null|", "undefined|", "NaN|", "Infinity|",

	/* Built-in types */
	"string|", "number|", "boolean|", "any|", "void|", "never|", "unknown|",
	"Array|", "Object|", "Promise|", "Map|", "Set|", NULL
};

/* Svelte - single file component with reactive syntax */
static char *SVELTE_HL_keywords[] = {
	/* JavaScript keywords */
	"async", "await", "break", "case", "catch", "class", "const",
	"continue", "debugger", "default", "delete", "do", "else", "export",
	"extends", "finally", "for", "from", "function", "if", "import", "in",
	"instanceof", "let", "new", "return", "static", "super", "switch",
	"this", "throw", "try", "typeof", "var", "void", "while", "with",
	"yield",

	/* Svelte Lifecycle */
	"onMount|", "onDestroy|", "beforeUpdate|", "afterUpdate|", "tick|",
	"setContext|", "getContext|", "hasContext|", "getAllContexts|",

	/* Svelte Stores */
	"writable|", "readable|", "derived|", "get|",

	/* Svelte Motion */
	"tweened|", "spring|",

	/* Svelte Transitions */
	"fade|", "blur|", "fly|", "slide|", "scale|", "draw|", "crossfade|",

	/* Svelte Actions */
	"use|",

	/* Svelte Bindings */
	"bind|", "on|", "class|",

	/* Svelte Special Elements */
	"svelte:component|", "svelte:window|", "svelte:body|", "svelte:head|",
	"svelte:options|", "svelte:fragment|", "svelte:self|",

	/* Svelte Blocks */
	"{#if", "{:else", "{:else if", "{/if}", "{#each", "{/each}", "{#await",
	"{:then", "{:catch", "{/await}", "{#key", "{/key}",

	/* Common values */
	"true|", "false|", "null|", "undefined|", "NaN|", "Infinity|",

	/* Built-in objects */
	"Array|", "Object|", "String|", "Number|", "Boolean|", "Date|", "Math|",
	"JSON|", "Promise|", "Map|", "Set|", "console|",

	/* Svelte template tags */
	"<script>", "</script>", "<style>", "</style>", NULL
};

/* kg's Lisp, which is Emacs-shaped; "let*" precedes "let" because the
 * first match wins. */
static char *LISP_HL_keywords[] = { "defun", "defmacro", "defvar", "defconst",
	"interactive", "lambda", "fn", "macro", "let*", "let", "setq", "progn",
	"if", "cond", "when", "unless", "while", "dolist", "dotimes", "quote",
	"and", "or", "not", "nil", "t", NULL };

static void markdown_syntax(struct editor_buffer *b, erow *row);
static void makefile_syntax(struct editor_buffer *b, erow *row);
static void gitcommit_syntax(struct editor_buffer *b, erow *row);
static void gitrebase_syntax(struct editor_buffer *b, erow *row);
static void yaml_syntax(struct editor_buffer *b, erow *row);

/* How this backend highlights one mode, keyed by the mode's stable
 * identity.  This is the legacy scanner's private configuration: it used to
 * be four more columns on struct editor_syntax, where every consumer of a
 * mode record could see it and mode semantics could accidentally come to
 * depend on it.  A mode with no row here is scanned generically with no
 * keywords and no multi-line comment -- which is what the synthetic modes
 * (Text, IBuffer, Compilation, Lisp Interaction) got from their all-empty
 * records before the split, so their highlighting is unchanged.
 *
 * The single-line comment starter is deliberately NOT here: it stays on the
 * mode record because comment-dwim (src/word.c) inserts it, which is
 * command semantics that outlives any highlighting backend. */
struct legacy_syntax_spec {
	enum kg_mode_id mode;
	char **keywords;
	char multiline_comment_start[5];
	char multiline_comment_end[5];
	int flags;
	/* NULL => the generic keyword scanner. */
	void (*scan)(struct editor_buffer *b, erow *row);
};

static const struct legacy_syntax_spec legacy_specs[] = {
	{ KG_MODE_C, C_HL_keywords, "/*", "*/",
	    HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_NUMBERS, NULL },
	{ KG_MODE_PYTHON, PYTHON_HL_keywords, "", "",
	    HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_NUMBERS, NULL },
	{ KG_MODE_SHELL, SHELL_HL_keywords, "", "",
	    HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_NUMBERS, NULL },
	{ KG_MODE_JAVASCRIPT, JS_HL_keywords, "/*", "*/",
	    HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_NUMBERS, NULL },
	{ KG_MODE_RUST, RUST_HL_keywords, "/*", "*/",
	    HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_NUMBERS, NULL },
	{ KG_MODE_JAVA, JAVA_HL_keywords, "/*", "*/",
	    HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_NUMBERS, NULL },
	{ KG_MODE_TYPESCRIPT, TS_HL_keywords, "/*", "*/",
	    HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_NUMBERS, NULL },
	{ KG_MODE_CSHARP, CSHARP_HL_keywords, "/*", "*/",
	    HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_NUMBERS, NULL },
	{ KG_MODE_PHP, PHP_HL_keywords, "/*", "*/",
	    HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_NUMBERS, NULL },
	{ KG_MODE_RUBY, RUBY_HL_keywords, "", "",
	    HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_NUMBERS, NULL },
	{ KG_MODE_SWIFT, SWIFT_HL_keywords, "/*", "*/",
	    HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_NUMBERS, NULL },
	{ KG_MODE_SQL, SQL_HL_keywords, "/*", "*/",
	    HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_NUMBERS, NULL },
	{ KG_MODE_DART, DART_HL_keywords, "/*", "*/",
	    HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_NUMBERS, NULL },
	{ KG_MODE_HTML, HTML_HL_keywords, "", "-->",
	    HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_NUMBERS, NULL },
	{ KG_MODE_REACT, REACT_HL_keywords, "/*", "*/",
	    HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_NUMBERS, NULL },
	{ KG_MODE_VUE, VUE_HL_keywords, "/*", "*/",
	    HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_NUMBERS, NULL },
	{ KG_MODE_ANGULAR, ANGULAR_HL_keywords, "/*", "*/",
	    HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_NUMBERS, NULL },
	{ KG_MODE_SVELTE, SVELTE_HL_keywords, "/*", "*/",
	    HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_NUMBERS, NULL },
	{ KG_MODE_MAKEFILE, NULL, "", "", 0, makefile_syntax },
	{ KG_MODE_MARKDOWN, NULL, "", "", 0, markdown_syntax },
	{ KG_MODE_LISP, LISP_HL_keywords, "", "",
	    HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_NUMBERS, NULL },
	{ KG_MODE_GIT_COMMIT, NULL, "", "", 0, gitcommit_syntax },
	{ KG_MODE_GIT_REBASE, NULL, "", "", 0, gitrebase_syntax },
	{ KG_MODE_YAML, NULL, "", "", 0, yaml_syntax },
};

#define LEGACY_SPEC_ENTRIES                                                    \
	((int)(sizeof(legacy_specs) / sizeof(legacy_specs[0])))

/* What a mode with no row above is scanned with: keywords and multi-line
 * comments off, generic scan on. */
static const struct legacy_syntax_spec legacy_spec_plain
    = { KG_MODE_TEXT, NULL, "", "", 0, NULL };

static const struct legacy_syntax_spec *legacy_spec_for(enum kg_mode_id id)
{
	int i;

	for (i = 0; i < LEGACY_SPEC_ENTRIES; i++) {
		if (legacy_specs[i].mode == id) {
			return &legacy_specs[i];
		}
	}
	return &legacy_spec_plain;
}

int is_separator(int c)
{
	return c == '\0' || ascii_is_space(c)
	    || strchr(",.()+-/*=~%[];", c) != NULL;
}

/* Return true if the specified row last char is part of a multi line comment
 * that starts at this row or at one before, and does not end at the end
 * of the row but spawns to the next row. */
int editor_row_has_open_comment(erow *row)
{
	if (row->hl && row->rsize && row->hl[row->rsize - 1] == HL_MLCOMMENT
	    && (row->rsize < 2
		|| (row->render[row->rsize - 2] != '*'
		    || row->render[row->rsize - 1] != '/'))) {
		return 1;
	}
	return 0;
}

/* Return 1 if every character in p is '=' or every character is '-' (and
 * len > 0).  Used to detect setext heading underlines. */
static int is_setext_line(const char *p, int len)
{
	int all_eq = 1, all_dash = 1, i;

	if (len == 0) {
		return 0;
	}
	for (i = 0; i < len; i++) {
		if (p[i] != '=') {
			all_eq = 0;
		}
		if (p[i] != '-') {
			all_dash = 0;
		}
		if (!all_eq && !all_dash) {
			return 0;
		}
	}
	return 1;
}

/* Markdown syntax highlighter.  Uses hl_oc to track fenced code block state
 * across rows (1 = inside a fenced block). */
static void markdown_syntax(struct editor_buffer *b, erow *row)
{
	char *p = row->render;
	int len = row->rsize, i, j, oc;
	int in_block = (row->idx > 0 && b->row[row->idx - 1].hl_oc);

	/* Fenced code block fence line (```). */
	if (len >= 3 && memcmp(p, "```", 3) == 0) {
		memset(row->hl, HL_STRING, len);
		oc = in_block ? 0 : 1;
		goto done;
	}

	/* Body of a fenced code block. */
	if (in_block) {
		if (len > 0) {
			memset(row->hl, HL_STRING, len);
		}
		oc = 1;
		goto done;
	}

	oc = 0;

	/* Setext heading underline (===== or -----).
	 * Re-trigger the row above so it gets heading colour too. */
	if (is_setext_line(p, len)) {
		memset(row->hl, HL_KEYWORD1, len);
		if (row->idx > 0) {
			syntax_update_row_only(b, &b->row[row->idx - 1]);
		}
		goto done;
	}

	/* Setext heading text: next row is the underline. */
	if (len > 0 && row->idx + 1 < b->numrows
	    && is_setext_line(
		b->row[row->idx + 1].render, b->row[row->idx + 1].rsize)) {
		memset(row->hl, HL_KEYWORD1, len);
		goto done;
	}

	if (len > 0 && p[0] == '#') {
		memset(row->hl, HL_KEYWORD1, len);
		goto done;
	}

	/* Blockquote: line starts with '>'. */
	if (len > 0 && p[0] == '>') {
		memset(row->hl, HL_COMMENT, len);
		goto done;
	}

	/* Inline: inline code (`...`), bold (**...**), link text ([...]). */
	for (i = 0; i < len; i++) {
		if (p[i] == '`') {
			for (j = i + 1; j < len && p[j] != '`'; j++) { }
			if (j < len) {
				memset(row->hl + i, HL_STRING, j - i + 1);
				i = j;
			}
		} else if (i + 1 < len && p[i] == '*' && p[i + 1] == '*') {
			for (j = i + 2;
			    j + 1 < len && !(p[j] == '*' && p[j + 1] == '*');
			    j++)
				;
			if (j + 1 < len) {
				memset(row->hl + i, HL_KEYWORD2, j - i + 2);
				i = j + 1;
			}
		} else if (p[i] == '[') {
			for (j = i + 1; j < len && p[j] != ']'; j++) { }
			if (j < len) {
				memset(row->hl + i, HL_KEYWORD2, j - i + 1);
				i = j;
			}
		}
	}

done:
	row->hl_oc = oc;
}

/* Highlight variable references $(...), ${...}, and single-char $X.
 * Scans render buffer from position *ip to end, marking HL_STRING.
 * Also stops at '#' and marks the rest as HL_COMMENT.
 */
static void make_var_and_comment(erow *row, int start)
{
	char *p = row->render;
	int len = row->rsize;
	int i, j;
	char close;

	for (i = start; i < len; i++) {
		if (p[i] == '#') {
			memset(row->hl + i, HL_COMMENT, len - i);
			return;
		}
		if (p[i] == '$') {
			row->hl[i] = HL_STRING;
			if (i + 1 >= len) {
				continue;
			}
			i++;
			if (p[i] == '(' || p[i] == '{') {
				close = (p[i] == '(') ? ')' : '}';
				for (j = i; j < len && p[j] != close; j++) {
					row->hl[j] = HL_STRING;
				}
				if (j < len) {
					row->hl[j] = HL_STRING;
				}
				i = (j < len) ? j : j - 1;
			} else {
				row->hl[i] = HL_STRING; /* $@, $<, $^, etc. */
			}
		}
	}
}

/* Makefile syntax highlighter.
 * Highlights:
 *   - Recipe lines (tab-indented): variable refs and trailing comments
 *   - Directives (include, ifdef, define, ...): HL_KEYWORD1
 *   - Rule targets (before ':'): HL_KEYWORD1
 *   - Assignment LHS (before =, :=, +=, ?=, !=): HL_KEYWORD2
 *   - Assignment operator: HL_KEYWORD1
 *   - Variable references $(VAR), ${VAR}, $@, $<, ...: HL_STRING
 *   - Comments (#): HL_COMMENT
 */
static void makefile_syntax(struct editor_buffer *b, erow *row)
{
	(void)b;
	static const char *directives[] = { "include", "-include", "sinclude",
		"define", "endef", "ifdef", "ifndef", "ifeq", "ifneq", "else",
		"endif", "override", "export", "unexport", "vpath", NULL };
	char *p = row->render;
	int len = row->rsize;
	int i, j, d, dlen, colon, eq, tend, name_end, op_start, op_len;

	/* Recipe line: starts with a hard tab */
	if (len > 0 && p[0] == '\t') {
		make_var_and_comment(row, 0);
		return;
	}

	/* Skip leading spaces */
	i = 0;
	while (i < len && p[i] == ' ') {
		i++;
	}
	if (i >= len) {
		return;
	}

	/* Comment line */
	if (p[i] == '#') {
		memset(row->hl + i, HL_COMMENT, len - i);
		return;
	}

	/* Directives at the start of a non-recipe line */
	for (d = 0; directives[d]; d++) {
		dlen = strlen(directives[d]);
		if (!strncmp(p + i, directives[d], dlen)
		    && (i + dlen >= len
			|| isspace((unsigned char)p[i + dlen]))) {
			memset(row->hl + i, HL_KEYWORD1, dlen);
			make_var_and_comment(row, i + dlen);
			return;
		}
	}

	/* Scan ahead for ':' (rule) or '=' (assignment) */
	colon = -1;
	eq = -1;
	for (j = i; j < len; j++) {
		if (p[j] == '#') {
			break;
		}
		if (p[j] == ':' && (j + 1 >= len || p[j + 1] != '=')) {
			colon = j;
			break;
		}
		if (p[j] == '=') {
			eq = j;
			/* Back up over compound operators := ?= += != .
			 * The NUL check is strchr()'s: it finds the byte
			 * that ends the set. */
			if (j > i && p[j - 1] && strchr(":?+!", p[j - 1])) {
				eq = j - 1;
			}
			break;
		}
	}

	if (colon > i) {
		/* Rule: highlight target (before ':') as KEYWORD1 */
		tend = colon;
		while (tend > i && p[tend - 1] == ' ') {
			tend--;
		}
		if (tend > i) {
			memset(row->hl + i, HL_KEYWORD1, tend - i);
		}
		make_var_and_comment(row, colon + 1);
		return;
	}

	if (eq >= i) {
		/* Assignment: variable name as KEYWORD2, operator as KEYWORD1
		 */
		name_end = eq;
		while (name_end > i && p[name_end - 1] == ' ') {
			name_end--;
		}
		if (name_end > i) {
			memset(row->hl + i, HL_KEYWORD2, name_end - i);
		}
		op_start = eq;
		op_len = (p[op_start] == '=') ? 1 : 2;
		memset(row->hl + op_start, HL_KEYWORD1, op_len);
		make_var_and_comment(row, op_start + op_len);
		return;
	}

	/* Fallback: highlight variable refs and comments */
	make_var_and_comment(row, i);
}

#define GITCOMMIT_SUBJECT_LIMIT 50

/* True if `row` is the subject line: itself non-blank/non-comment, and
 * every already-existing earlier row (0..row->idx-1) is blank or a '#'
 * comment.  Deliberately does not consult b->numrows: buffer.c's
 * editor_insert_row() calls editor_update_row() (and thus this
 * highlighter) *before* incrementing numrows, so a numrows-bounded scan
 * could never match a row against itself on first insertion, and a
 * freshly opened commit file would never show the subject warning until
 * the user edited that line.  Walking only the rows that are already in
 * the array sidesteps that ordering entirely. */
static void gitcommit_syntax(struct editor_buffer *b, erow *row)
{
	int has_subject_above = (row->idx > 0 && b->row[row->idx - 1].hl_oc);
	int oc = has_subject_above;

	if (row->rsize > 0 && row->render[0] == '#') {
		memset(row->hl, HL_COMMENT, row->rsize);
	} else if (row->rsize > 0 && !has_subject_above) {
		oc = 1;
		if (row->rsize > GITCOMMIT_SUBJECT_LIMIT) {
			memset(row->hl + GITCOMMIT_SUBJECT_LIMIT, HL_WARNING,
			    row->rsize - GITCOMMIT_SUBJECT_LIMIT);
		}
	}

	row->hl_oc = oc;
}

/* True if p[0..len) looks like an abbreviated commit hash. */
static int gitrebase_is_hash(const char *p, int len)
{
	int i;

	if (len < 4) {
		return 0;
	}
	for (i = 0; i < len; i++) {
		if (!isxdigit((unsigned char)p[i])) {
			return 0;
		}
	}
	return 1;
}

/* Git rebase todo highlighter.  Row-local: '#' comments dimmed, known
 * action words as keywords, the hash after a commit-taking action (or
 * merge's -C) as KEYWORD2, an exec command body as string, and an
 * unknown first word or an option flag the action cannot take as a
 * warning (either typo would make git fail the whole rebase).  The action
 * vocabulary itself is the facade's (syntax_git_rebase_action_name): the
 * C-c action keys need it with any backend installed. */
static void gitrebase_syntax(struct editor_buffer *b, erow *row)
{
	(void)b;
	char *p = row->render;
	int len = row->rsize;
	int i = 0, w, flags_ok, takes_commit = 0;
	const char *action;

	if (len == 0) {
		return;
	}
	if (p[0] == '#') {
		memset(row->hl, HL_COMMENT, len);
		return;
	}
	i = syntax_git_rebase_skip_ws(p, len, 0);
	w = syntax_git_rebase_skip_word(p, len, i);
	if (w == i) {
		return;
	}
	action = syntax_git_rebase_action_name(p + i, w - i, &takes_commit);
	if (!action) {
		memset(row->hl + i, HL_WARNING, w - i);
		return;
	}
	memset(row->hl + i, HL_KEYWORD1, w - i);

	i = syntax_git_rebase_skip_ws(p, len, w);
	if (strcmp(action, "exec") == 0) {
		if (i < len) {
			memset(row->hl + i, HL_STRING, len - i);
		}
		return;
	}
	flags_ok = strcmp(action, "fixup") == 0 || strcmp(action, "merge") == 0;
	if (!takes_commit && !flags_ok) {
		return;
	}
	/* Option words before the hash: only fixup and merge take -C/-c,
	 * so any other flag would make git reject the whole todo -- warn. */
	while (i < len && p[i] == '-') {
		w = syntax_git_rebase_skip_word(p, len, i);
		if (!flags_ok || w - i != 2 || !strchr("Cc", p[i + 1])) {
			memset(row->hl + i, HL_WARNING, w - i);
		}
		i = syntax_git_rebase_skip_ws(p, len, w);
	}
	w = syntax_git_rebase_skip_word(p, len, i);
	if (gitrebase_is_hash(p + i, w - i)) {
		memset(row->hl + i, HL_KEYWORD2, w - i);
	}
}

/* YAML syntax highlighter.
 *
 * This is a lexical highlighter, not a YAML parser: it does not validate
 * documents, infer schemas, or track flow-collection nesting depth. It
 * highlights mapping keys, comments, quoted and block scalars, core-schema
 * values (booleans/null), numbers, and structural markers (document
 * markers, sequence markers, anchors/aliases/tags, directives).
 *
 * hl_oc packs the cross-row block-scalar state: a "kind" byte plus the
 * indentation level the block is anchored to.  YAML_STATE_BLOCK_PENDING
 * means a '|' or '>' was seen but the content indentation has not been
 * inferred yet; YAML_STATE_BLOCK_CONTENT means it has, and rows at or
 * beyond that indentation are scalar body. */
#define YAML_STATE_KIND_MASK 0xff000000
#define YAML_STATE_INDENT_MASK 0x00ffffff
#define YAML_STATE_BLOCK_PENDING 0x01000000
#define YAML_STATE_BLOCK_CONTENT 0x02000000

/* Number of leading space characters (YAML indentation; a leading tab is
 * not valid YAML indentation, so it does not count). */
static int yaml_indent(const erow *row)
{
	int i = 0;
	while (i < row->rsize && row->render[i] == ' ') {
		i++;
	}
	return i;
}

/* True if a '#' at p[pos] starts a comment: outside quotes, and either at
 * the first column or preceded by whitespace. */
static int yaml_is_comment_start(const char *p, int pos)
{
	return pos == 0 || isspace((unsigned char)p[pos - 1]);
}

/* True if the ':' at p[pos] acts as a YAML mapping separator: followed by
 * whitespace, end of line, or one of ",}]". */
static int yaml_colon_is_separator(const char *p, int pos, int len)
{
	int next = pos + 1;
	if (next >= len) {
		return 1;
	}
	return isspace((unsigned char)p[next]) || p[next] == ','
	    || p[next] == '}' || p[next] == ']';
}

/* Index just past the matching close quote for a quoted scalar starting
 * at p[start] (p[start] is '"' or '\''); len when unterminated.  '' is an
 * embedded quote in single-quoted scalars; \\ escapes the next byte in
 * double-quoted scalars. */
static int yaml_quote_span_end(const char *p, int start, int len)
{
	char q = p[start];
	int i = start + 1;

	while (i < len) {
		if (p[i] == q) {
			if (q == '\'' && i + 1 < len && p[i + 1] == '\'') {
				i += 2;
				continue;
			}
			return i + 1;
		}
		if (q == '"' && p[i] == '\\' && i + 1 < len) {
			i += 2;
			continue;
		}
		i++;
	}
	return len;
}

static int yaml_scan_quoted(erow *row, int start)
{
	int end = yaml_quote_span_end(row->render, start, row->rsize);
	memset(row->hl + start, HL_STRING, end - start);
	return end;
}

static int yaml_is_flow_punct(char c)
{
	return c == '{' || c == '}' || c == '[' || c == ']' || c == ',';
}

/* Anchor (&name), alias (*name) or tag (!tag, !!str) starting at
 * p[start]: consumes up to whitespace or flow punctuation. */
static int yaml_scan_anchor_or_tag(erow *row, int start)
{
	char *p = row->render;
	int len = row->rsize;
	int i = start + 1;

	while (i < len && !isspace((unsigned char)p[i])
	    && !yaml_is_flow_punct(p[i]) && p[i] != ':') {
		i++;
	}
	memset(row->hl + start, HL_KEYWORD2, i - start);
	return i;
}

/* Index just past a plain scalar token starting at p[start]: consumes up
 * to whitespace, flow punctuation, a comment start, or a mapping-colon. */
static int yaml_scalar_token_end(const char *p, int start, int len)
{
	int i = start;

	while (i < len) {
		char c = p[i];
		if (isspace((unsigned char)c) || yaml_is_flow_punct(c)) {
			break;
		}
		if (c == '#' && yaml_is_comment_start(p, i)) {
			break;
		}
		if (c == ':' && yaml_colon_is_separator(p, i, len)) {
			break;
		}
		i++;
	}
	return i;
}

/* True/false/null spellings and '~' recognised by the YAML 1.2 core
 * schema.  Deliberately excludes the YAML 1.1 yes/no/on/off spellings. */
static int yaml_is_core_scalar_keyword(const char *tok, int toklen)
{
	static const char *words[] = { "true", "True", "TRUE", "false", "False",
		"FALSE", "null", "Null", "NULL", "~", NULL };
	int i;

	for (i = 0; words[i]; i++) {
		if ((int)strlen(words[i]) == toklen
		    && !memcmp(tok, words[i], toklen)) {
			return 1;
		}
	}
	return 0;
}

/* True if tok[0..toklen) is a complete YAML core-schema number: a decimal
 * int/float with an optional exponent, a 0x/0o prefixed literal, or
 * (+/-).inf / .nan. */
static int yaml_is_number_token(const char *tok, int toklen)
{
	int i = 0, has_digit = 0;

	if (toklen == 0) {
		return 0;
	}
	if (tok[i] == '+' || tok[i] == '-') {
		i++;
	}
	if (i >= toklen) {
		return 0;
	}

	if (tok[i] == '.' && toklen - i == 4
	    && (!memcmp(tok + i, ".inf", 4) || !memcmp(tok + i, ".nan", 4))) {
		return 1;
	}

	if (tok[i] == '0' && i + 1 < toklen && strchr("xo", tok[i + 1])) {
		int is_hex = tok[i + 1] == 'x';
		i += 2;
		if (i >= toklen) {
			return 0;
		}
		for (; i < toklen; i++) {
			if (is_hex ? !isxdigit((unsigned char)tok[i])
				   : (tok[i] < '0' || tok[i] > '7')) {
				return 0;
			}
		}
		return 1;
	}

	for (; i < toklen; i++) {
		char c = tok[i];
		if (isdigit((unsigned char)c)) {
			has_digit = 1;
			continue;
		}
		if (c == '.') {
			continue;
		}
		if ((c == 'e' || c == 'E') && has_digit) {
			i++;
			if (i < toklen && (tok[i] == '+' || tok[i] == '-')) {
				i++;
			}
			if (i >= toklen || !isdigit((unsigned char)tok[i])) {
				return 0;
			}
			for (; i < toklen; i++) {
				if (!isdigit((unsigned char)tok[i])) {
					return 0;
				}
			}
			return 1;
		}
		return 0;
	}
	return has_digit;
}

/* Block scalar indicator ('|' or '>') at p[start]: an optional chomping
 * modifier (-/+) and an optional explicit indentation digit, in either
 * order.  Returns the index just past the indicator when it is followed
 * by whitespace, a comment, or end of line (a valid indicator); returns
 * start otherwise.  *explicit_indent is set to the indentation digit, or
 * 0 when none was given. */
static int yaml_block_indicator(
    const char *p, int start, int len, int *explicit_indent)
{
	int i = start + 1;

	*explicit_indent = 0;
	if (i < len && (p[i] == '-' || p[i] == '+')) {
		i++;
	}
	if (i < len && p[i] >= '1' && p[i] <= '9') {
		*explicit_indent = p[i] - '0';
		i++;
		if (i < len && (p[i] == '-' || p[i] == '+')) {
			i++;
		}
	}
	if (i < len && !isspace((unsigned char)p[i]) && p[i] != '#') {
		return start;
	}
	return i;
}

/* Scan p[start..rsize) as YAML "value" content: quoted/block scalars,
 * anchors/aliases/tags, core-schema keywords, numbers, and a trailing
 * comment.  Sets row->hl_oc when a block scalar indicator is found. */
static void yaml_scan_value(erow *row, int start)
{
	char *p = row->render;
	int len = row->rsize;
	int i = start;

	while (i < len) {
		if (isspace((unsigned char)p[i])) {
			i++;
			continue;
		}
		if (p[i] == '#' && yaml_is_comment_start(p, i)) {
			memset(row->hl + i, HL_COMMENT, len - i);
			return;
		}
		if (p[i] == '"' || p[i] == '\'') {
			i = yaml_scan_quoted(row, i);
			continue;
		}
		if (p[i] && strchr("&*!", p[i])) {
			i = yaml_scan_anchor_or_tag(row, i);
			continue;
		}
		if (p[i] == '|' || p[i] == '>') {
			int explicit_indent;
			int end
			    = yaml_block_indicator(p, i, len, &explicit_indent);
			if (end > i) {
				memset(row->hl + i, HL_KEYWORD2, end - i);
				int header_indent = yaml_indent(row);
				row->hl_oc = explicit_indent > 0
				    ? (YAML_STATE_BLOCK_CONTENT
					  | (header_indent + explicit_indent))
				    : (YAML_STATE_BLOCK_PENDING
					  | header_indent);
				i = end;
				continue;
			}
		}

		int end = yaml_scalar_token_end(p, i, len);
		if (end == i) {
			i++; /* lone flow punctuation; leave HL_NORMAL */
			continue;
		}
		if (yaml_is_core_scalar_keyword(p + i, end - i)) {
			memset(row->hl + i, HL_KEYWORD2, end - i);
		} else if (yaml_is_number_token(p + i, end - i)) {
			memset(row->hl + i, HL_NUMBER, end - i);
		}
		i = end;
	}
}

/* Mapping key at p[start..len): a plain, single- or double-quoted key, or
 * "<<", followed by a mapping colon.  Returns the colon index, or -1 when
 * no mapping key is present at this position. */
static int yaml_find_mapping_colon(const char *p, int start, int len)
{
	int i = start;

	if (i < len && (p[i] == '"' || p[i] == '\'')) {
		i = yaml_quote_span_end(p, i, len);
	} else {
		while (i < len && p[i] != ':' && p[i] != '#') {
			i++;
		}
	}
	while (i < len && p[i] == ' ') {
		i++;
	}
	if (i < len && p[i] == ':' && yaml_colon_is_separator(p, i, len)) {
		return i;
	}
	return -1;
}

/* Top-level per-row entry point.  Handles cross-row block-scalar state
 * first (inherited via the previous row's hl_oc), then parses the row as
 * ordinary YAML: directives, document markers, sequence markers, a
 * mapping key, and finally the value via yaml_scan_value(). */
static void yaml_syntax(struct editor_buffer *b, erow *row)
{
	char *p = row->render;
	int len = row->rsize;
	int prev_oc = (row->idx > 0) ? b->row[row->idx - 1].hl_oc : 0;
	int kind = prev_oc & YAML_STATE_KIND_MASK;
	int state_indent = prev_oc & YAML_STATE_INDENT_MASK;
	int cur_indent = yaml_indent(row);
	int blank = (cur_indent == len);
	int i, colon;

	if (kind == YAML_STATE_BLOCK_PENDING) {
		if (blank) {
			row->hl_oc = prev_oc;
			return;
		}
		if (cur_indent > state_indent) {
			memset(
			    row->hl + cur_indent, HL_STRING, len - cur_indent);
			row->hl_oc = YAML_STATE_BLOCK_CONTENT | cur_indent;
			return;
		}
		/* Never got content deep enough: block never started.  Fall
		 * through and parse this row as ordinary YAML. */
	} else if (kind == YAML_STATE_BLOCK_CONTENT) {
		if (blank) {
			row->hl_oc = prev_oc;
			return;
		}
		if (cur_indent >= state_indent) {
			memset(
			    row->hl + cur_indent, HL_STRING, len - cur_indent);
			row->hl_oc = prev_oc;
			return;
		}
		/* Dedent: block ends.  Fall through. */
	}

	if (blank) {
		return;
	}

	i = cur_indent;

	/* Directive: '%YAML', '%TAG', ... */
	if (p[i] == '%') {
		int end = yaml_scalar_token_end(p, i, len);
		memset(row->hl + i, HL_KEYWORD1, end - i);
		return;
	}

	/* Document markers only count at the first non-indentation column. */
	if (i + 3 <= len
	    && (!memcmp(p + i, "---", 3) || !memcmp(p + i, "...", 3))
	    && (i + 3 == len || isspace((unsigned char)p[i + 3]))) {
		memset(row->hl + i, HL_KEYWORD2, 3);
		yaml_scan_value(row, i + 3);
		return;
	}

	/* Sequence markers, repeatable for nested "- - value". */
	while (i < len && p[i] == '-'
	    && (i + 1 >= len || isspace((unsigned char)p[i + 1]))) {
		row->hl[i] = HL_KEYWORD2;
		i++;
		if (i < len && p[i] == ' ') {
			i++;
		}
	}
	if (i >= len) {
		return;
	}

	colon = yaml_find_mapping_colon(p, i, len);
	if (colon >= 0) {
		if (p[i] == '"' || p[i] == '\'') {
			yaml_scan_quoted(row, i);
		} else {
			memset(row->hl + i, HL_KEYWORD1, colon - i);
		}
		yaml_scan_value(row, colon + 1);
		return;
	}

	yaml_scan_value(row, i);
}

/* The non-decimal integer literal prefixes: the byte that may follow the
 * leading `0`, in both spellings, and the digits that radix admits.  One
 * table rather than three near-identical scanner arms. */
static const struct {
	const char *marks;
	const char *digits;
} radix_prefixes[] = {
	{ "bB", "01" },
	{ "oO", "01234567" },
	{ "xX", "0123456789abcdefABCDEF" },
	{ NULL, NULL },
};

/* The digits a `0`-prefixed literal admits, or NULL when the byte after
 * the leading zero names no radix.  render's NUL is a digit in no radix,
 * which is what stops such a literal at the end of the row. */
static const char *radix_digits(char c)
{
	for (int k = 0; radix_prefixes[k].marks; k++) {
		if (c && strchr(radix_prefixes[k].marks, c)) {
			return radix_prefixes[k].digits;
		}
	}
	return NULL;
}

static void generic_keyword_scan(
    struct editor_buffer *b, erow *row, const struct legacy_syntax_spec *spec)
{
	int in_string = 0; /* Are we inside "" or '' ? */
	int in_comment = 0; /* Are we inside multi-line comment? */
	int prev_sep = 1; /* Tell the parser if 'i' points to start of word. */
	char *p = row->render;
	int i = 0; /* Current char offset */
	char **keywords = spec->keywords;
	const char *mcs = spec->multiline_comment_start;
	const char *mce = spec->multiline_comment_end;
	char *scs = b->syntax->singleline_comment_start;

	/* Point to the first non-space char. */
	while (*p && ascii_is_space((unsigned char)*p)) {
		p++;
		i++;
	}

	/* If the previous line has an open comment, this line starts
	 * with an open comment state. */
	if (row->idx > 0 && b->row[row->idx - 1].hl_oc) {
		in_comment = 1;
	}

	while (*p) {
		/* Handle single-line comments (1- or 2-char starter). */
		if (scs[0] && prev_sep && *p == scs[0]
		    && (!scs[1] || *(p + 1) == scs[1])) {
			/* From here to end is a comment.  `i` and row->hl
			 * are both in render bytes, so the run ends at
			 * row->rsize: row->size is the chars length, and on
			 * a row holding a tab it stops the colour short by
			 * however much the tab expanded. */
			KG_ASSERT_RENDER_OFF(row, i);
			memset(row->hl + i, HL_COMMENT, row->rsize - i);
			goto done;
		}

		/* Handle multi line comments. */
		if (in_comment) {
			row->hl[i] = HL_MLCOMMENT;
			if (*p == mce[0] && *(p + 1) == mce[1]) {
				/* Safe as hl[i + 1] only while every spec
				 * mce is two bytes: a one-byte closer would
				 * match with p[1] the render NUL and write
				 * one past hl, exactly the trailing-'\\'
				 * overflow below.  Same offset shape, so the
				 * code no longer assumes the invariant. */
				int two = mce[1] ? 1 : 0;

				row->hl[i + two] = HL_MLCOMMENT;
				p += 1 + two;
				i += 1 + two;
				in_comment = 0;
				prev_sep = 1;
				continue;
			} else {
				prev_sep = 0;
				p++;
				i++;
				continue;
			}
		} else if (*p == mcs[0] && *(p + 1) == mcs[1]) {
			/* Mirror of the mce guard above, for the opener. */
			int two = mcs[1] ? 1 : 0;

			row->hl[i] = HL_MLCOMMENT;
			row->hl[i + two] = HL_MLCOMMENT;
			p += 1 + two;
			i += 1 + two;
			in_comment = 1;
			prev_sep = 0;
			continue;
		}

		/* Handle "" and '' */
		if (in_string) {
			row->hl[i] = HL_STRING;
			if (*p == '\\') {
				/* A backslash claims the byte after it too --
				 * but only when there is one.  In an
				 * unterminated string whose last render byte
				 * is a lone '\\', p[1] is render's NUL
				 * terminator, so hl[i + 1] is one past hl's
				 * allocation (row_hl_reserve() sizes hl to
				 * exactly rsize for a row below
				 * KG_ROW_SLACK_MIN): a one-byte heap overflow.
				 * `esc' is the escape's second byte where it
				 * exists and 0 where it does not, so a lone
				 * trailing backslash is repainted as the
				 * ordinary in-string byte it is and the scan
				 * advances by one to the end of the row --
				 * yaml_quote_span_end()'s `i + 1 < len' guard,
				 * spelled as an offset. */
				int esc = p[1] ? 1 : 0;

				row->hl[i + esc] = HL_STRING;
				p += 1 + esc;
				i += 1 + esc;
				prev_sep = 0;
				continue;
			}
			if (*p == in_string) {
				in_string = 0;
			}
			p++;
			i++;
			continue;
		} else {
			if (*p == '"' || *p == '\'') {
				in_string = (unsigned char)*p;
				row->hl[i] = HL_STRING;
				p++;
				i++;
				prev_sep = 0;
				continue;
			}
		}

		/* Handle non printable chars.  ASCII only: isprint() of a
		 * signed char 0xC3 was 0 in the "C" locale, so this used to
		 * mark every byte of every ordinary UTF-8 character in a C,
		 * Python or shell buffer HL_NONPRINT.  What a byte >= 0x80
		 * looks like is display_glyph_at()'s question, not the
		 * scanner's. */
		if ((unsigned char)*p < 0x80 && !ascii_is_print(*p)) {
			row->hl[i] = HL_NONPRINT;
			p++;
			i++;
			prev_sep = 0;
			continue;
		}

		/* Handle non-base-10 integer literals (0b/0B, 0o/0O, 0x/0X).
		 * Only highlight the prefix when at least one valid digit
		 * follows, so a bare 0b/0o/0x falls through to the decimal
		 * handler. */
		if (prev_sep && *p == '0') {
			const char *digits = radix_digits(p[1]);

			if (digits && p[2] && strchr(digits, p[2])) {
				int n = 2;

				while (p[n] && strchr(digits, p[n])) {
					n++;
				}
				KG_ASSERT_RENDER_OFF(row, i + n);
				memset(row->hl + i, HL_NUMBER, (size_t)n);
				p += n;
				i += n;
				prev_sep = 0;
				continue;
			}
		}

		/* Handle numbers */
		if ((ascii_is_digit((unsigned char)*p)
			&& (prev_sep || row->hl[i - 1] == HL_NUMBER))
		    || (*p == '.' && i > 0 && row->hl[i - 1] == HL_NUMBER)) {
			row->hl[i] = HL_NUMBER;
			p++;
			i++;
			prev_sep = 0;
			continue;
		}

		/* Handle keywords and lib calls */
		if (prev_sep && keywords) {
			int j;
			for (j = 0; keywords[j]; j++) {
				int klen = strlen(keywords[j]);
				int kw2 = keywords[j][klen - 1] == '|';
				if (kw2) {
					klen--;
				}

				/* Skip keywords that would read past
				 * render[rsize]. */
				if (i + klen > row->rsize) {
					continue;
				}

				if (!memcmp(p, keywords[j], klen)
				    && is_separator(*(p + klen))) {
					/* Keyword */
					memset(row->hl + i,
					    kw2 ? HL_KEYWORD2 : HL_KEYWORD1,
					    klen);
					p += klen;
					i += klen;
					break;
				}
			}
			if (keywords[j] != NULL) {
				prev_sep = 0;
				continue; /* We had a keyword match */
			}
		}

		/* Not special chars */
		prev_sep = is_separator(*p);
		p++;
		i++;
	}

done:;
	int oc = editor_row_has_open_comment(row);
	row->hl_oc = oc;
}

/* The backend contract (src/syntax_backend.h): highlight one non-empty row
 * of a buffer whose mode installs no highlighter of its own.  row->hl is
 * already reserved and HL_NORMAL-filled; row->hl_oc is this backend's own
 * cross-row state, cleared here before a scanner runs so a scanner that
 * only sets it on some paths still starts from a known value (the generic
 * scan sets it unconditionally from the row's open comment). */
void syntax_backend_update_row(struct editor_buffer *b, struct erow *row)
{
	const struct legacy_syntax_spec *spec = legacy_spec_for(b->syntax->id);

	if (spec->scan) {
		row->hl_oc = 0;
		spec->scan(b, row);
		return;
	}
	generic_keyword_scan(b, row, spec);
}

/* The other half of the backend contract: derive what this backend keeps
 * about a whole document.  It keeps nothing -- every row's colour comes
 * from the row above it and lives in that row -- so the state is NULL and
 * the work is the colouring the facade's contract asks for in passing.
 *
 * The scan walks its own copy of the staged record and raises the copy's
 * row count one row at a time, rather than colouring rows 0..N of a
 * document that already says it has N+1.  That is not a detail: a scanner
 * carrying state into the next row (an open block comment) must not be let
 * propagate into rows this pass has not reached, which have no render yet
 * and would be read as empty; and markdown's setext heading, which asks
 * whether the row *below* is an underline, has to find that row absent
 * until it arrives -- exactly as it did when a file load rendered and
 * highlighted one row at a time.  The copy is what keeps the caller's
 * record untouched, as syntax_backend_prepare()'s contract requires.
 *
 * Failure is the facade's per-row failure: syntax_update_row_only() clears
 * `running` when it cannot reserve a row's hl.  Rows coloured before that
 * point stay coloured; the caller of a failed prepare abandons the staged
 * array whole. */
struct kg_syntax_state *syntax_backend_prepare(
    struct editor_buffer *staged, int *ok)
{
	struct editor_buffer scan = *staged;
	int saved_running = running;
	int i;

	*ok = 1;
	for (i = 0; i < staged->numrows; i++) {
		scan.numrows = i + 1;
		syntax_update_row_only(&scan, &scan.row[i]);
		if (running != saved_running) {
			running = saved_running;
			*ok = 0;
			break;
		}
	}
	return NULL;
}

void syntax_backend_state_free(struct kg_syntax_state *st) { (void)st; }

/* =========================== Syntax highlights DB =========================
 *
 * In order to add a new syntax, define two arrays with a list of file name
 * matches and keywords. The file name matches are used in order to match
 * a given syntax with a given file name: if a match pattern starts with a
 * dot, it is matched as the last past of the filename, for example ".c".
 * Otherwise the pattern is just searched inside the filenme, like "Makefile").
 *
 * The list of keywords to highlight is just a list of words, however if they
 * a trailing '|' character is added at the end, they are highlighted in
 * a different color, so that you can have two different sets of keywords.
 *
 * Finally add a stanza in the HLDB global variable with two two arrays
 * of strings, and a set of flags in order to enable highlighting of
 * comments and numbers.
 *
 * The characters for single and multi line comments must be exactly two
 * and must be provided as well (see the C language example).
 *
 * There is no support to highlight patterns currently. */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "def.h"
#include "localvars.h"

/* C / C++ */
char *C_HL_extensions[] = { ".c", ".h", ".cpp", ".hpp", ".cc", NULL };
char *C_HL_keywords[] = {
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
char *PYTHON_HL_extensions[] = { ".py", ".pyw", ".pyi", ".pyx", NULL };
char *PYTHON_HL_keywords[] = {
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
char *SHELL_HL_extensions[] = { ".sh", ".bash", ".zsh", ".ksh", ".csh", ".tcsh",
	".profile", ".bashrc", ".bash_profile", ".bash_login", ".zshrc",
	".zshenv", ".zlogin", ".zprofile", NULL };

char *SHELL_HL_keywords[] = {
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
char *JS_HL_extensions[] = { ".js", ".jsx", ".mjs", ".cjs", NULL };
char *JS_HL_keywords[] = {
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
char *RUST_HL_extensions[] = { ".rs", ".rlib", NULL };
char *RUST_HL_keywords[] = {
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
char *JAVA_HL_extensions[] = { ".java", ".class", NULL };
char *JAVA_HL_keywords[] = {
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
char *TS_HL_extensions[] = { ".ts", ".tsx", ".d.ts", NULL };
char *TS_HL_keywords[] = {
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
char *CSHARP_HL_extensions[] = { ".cs", ".csx", NULL };
char *CSHARP_HL_keywords[] = {
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
char *PHP_HL_extensions[]
    = { ".php", ".phtml", ".php3", ".php4", ".php5", ".phps", NULL };
char *PHP_HL_keywords[] = {
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
char *RUBY_HL_extensions[] = { ".rb", ".rbw", ".rake", ".gemspec", NULL };
char *RUBY_HL_keywords[] = {
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
char *SWIFT_HL_extensions[] = { ".swift", NULL };
char *SWIFT_HL_keywords[] = {
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
char *SQL_HL_extensions[] = { ".sql", ".ddl", ".dml", NULL };
char *SQL_HL_keywords[] = {
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
char *DART_HL_extensions[] = { ".dart", NULL };
char *DART_HL_keywords[] = {
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
char *HTML_HL_extensions[] = { ".html", ".htm", ".xhtml", NULL };
char *HTML_HL_keywords[] = {
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
char *REACT_HL_extensions[] = { ".jsx", NULL };
char *REACT_HL_keywords[] = {
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
char *VUE_HL_extensions[] = { ".vue", NULL };
char *VUE_HL_keywords[] = {
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
char *ANGULAR_HL_extensions[] = { ".component.ts", ".service.ts", ".module.ts",
	".directive.ts", ".pipe.ts", ".guard.ts", NULL };
char *ANGULAR_HL_keywords[] = {
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
char *SVELTE_HL_extensions[] = { ".svelte", NULL };
char *SVELTE_HL_keywords[] = {
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

/* Makefile */
char *MAKE_HL_extensions[]
    = { "Makefile", "makefile", "GNUmakefile", ".mk", ".mak", NULL };

/* Markdown */
char *MD_HL_extensions[] = { ".md", ".markdown", ".mkd", NULL };
char *MD_HL_keywords[] = { NULL };

/* YAML */
char *YAML_HL_extensions[] = { ".yaml", ".yml", NULL };

/* Lisp */
char *LISP_HL_extensions[] = { ".fe", ".lisp", ".lsp", NULL };
char *LISP_HL_keywords[] = { "if", "while", "quote", "and", "or", "set", "fn",
	"mac", "define", "lambda", "let", "cond", "nil", "t", NULL };

/* Git message files: matched as substrings anywhere in the path, so
 * .git/COMMIT_EDITMSG works (see the filematch rule at the top). */
char *GITCOMMIT_HL_extensions[] = { "COMMIT_EDITMSG", "MERGE_MSG", "SQUASH_MSG",
	"TAG_EDITMSG", "NOTES_EDITMSG", "EDIT_DESCRIPTION", NULL };

static void markdown_syntax(erow *row);
static void makefile_syntax(erow *row);
static void gitcommit_syntax(erow *row);
static void yaml_syntax(erow *row);
static void editor_update_syntax_row_only(erow *row);

struct editor_syntax HLDB[] = {
	{ "C", C_HL_extensions, C_HL_keywords, "//", "/*", "*/",
	    HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_NUMBERS, NULL },
	{ "Python", PYTHON_HL_extensions, PYTHON_HL_keywords, "#", "", "",
	    HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_NUMBERS, NULL },
	{ "Shell", SHELL_HL_extensions, SHELL_HL_keywords, "#", "", "",
	    HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_NUMBERS, NULL },
	{ "JavaScript", JS_HL_extensions, JS_HL_keywords, "//", "/*", "*/",
	    HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_NUMBERS, NULL },
	{ "Rust", RUST_HL_extensions, RUST_HL_keywords, "//", "/*", "*/",
	    HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_NUMBERS, NULL },
	{ "Java", JAVA_HL_extensions, JAVA_HL_keywords, "//", "/*", "*/",
	    HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_NUMBERS, NULL },
	{ "TypeScript", TS_HL_extensions, TS_HL_keywords, "//", "/*", "*/",
	    HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_NUMBERS, NULL },
	{ "C#", CSHARP_HL_extensions, CSHARP_HL_keywords, "//", "/*", "*/",
	    HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_NUMBERS, NULL },
	{ "PHP", PHP_HL_extensions, PHP_HL_keywords, "//", "/*", "*/",
	    HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_NUMBERS, NULL },
	{ "Ruby", RUBY_HL_extensions, RUBY_HL_keywords, "#", "", "",
	    HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_NUMBERS, NULL },
	{ "Swift", SWIFT_HL_extensions, SWIFT_HL_keywords, "//", "/*", "*/",
	    HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_NUMBERS, NULL },
	{ "SQL", SQL_HL_extensions, SQL_HL_keywords, "--", "/*", "*/",
	    HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_NUMBERS, NULL },
	{ "Dart", DART_HL_extensions, DART_HL_keywords, "//", "/*", "*/",
	    HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_NUMBERS, NULL },
	{ "HTML", HTML_HL_extensions, HTML_HL_keywords, "<!--", "", "-->",
	    HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_NUMBERS, NULL },
	{ "React", REACT_HL_extensions, REACT_HL_keywords, "//", "/*", "*/",
	    HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_NUMBERS, NULL },
	{ "Vue", VUE_HL_extensions, VUE_HL_keywords, "//", "/*", "*/",
	    HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_NUMBERS, NULL },
	{ "Angular", ANGULAR_HL_extensions, ANGULAR_HL_keywords, "//", "/*",
	    "*/", HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_NUMBERS, NULL },
	{ "Svelte", SVELTE_HL_extensions, SVELTE_HL_keywords, "//", "/*", "*/",
	    HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_NUMBERS, NULL },
	{ "Makefile", MAKE_HL_extensions, NULL, "", "", "", 0,
	    makefile_syntax },
	{ "Markdown", MD_HL_extensions, MD_HL_keywords, "", "", "", 0,
	    markdown_syntax },
	{ "Lisp", LISP_HL_extensions, LISP_HL_keywords, ";", "", "",
	    HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_NUMBERS, NULL },
	{ "Git commit", GITCOMMIT_HL_extensions, NULL, "", "", "", 0,
	    gitcommit_syntax },
	{ "YAML", YAML_HL_extensions, NULL, "#", "", "", 0, yaml_syntax },
};

#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0]))

/* ====================== Syntax highlight color scheme  ==================== */

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

int is_separator(int c)
{
	return c == '\0' || isspace(c) || strchr(",.()+-/*=~%[];", c) != NULL;
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

/* Markdown syntax highlighter.  Uses hl_oc to track fenced code block state
 * across rows (1 = inside a fenced block). */
static void markdown_syntax(erow *row)
{
	char *p = row->render;
	int len = row->rsize, i, j, oc;
	int in_block = (row->idx > 0 && editor.row[row->idx - 1].hl_oc);

	/* Fenced code block fence line (```). */
	if (len >= 3 && p[0] == '`' && p[1] == '`' && p[2] == '`') {
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
			editor_update_syntax_row_only(
			    &editor.row[row->idx - 1]);
		}
		goto done;
	}

	/* Setext heading text: next row is the underline. */
	if (len > 0 && row->idx + 1 < editor.numrows
	    && is_setext_line(editor.row[row->idx + 1].render,
		editor.row[row->idx + 1].rsize)) {
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
static void makefile_syntax(erow *row)
{
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
			/* Back up over compound operators := ?= += != */
			if (j > i
			    && (p[j - 1] == ':' || p[j - 1] == '?'
				|| p[j - 1] == '+' || p[j - 1] == '!')) {
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

/* True when the current buffer is a git commit/merge/tag message. */
int syntax_is_git_commit(void)
{
	return editor.syntax && (editor.syntax->highlight == gitcommit_syntax);
}

/* Row index of the commit subject: the first non-blank row that is not
 * a '#' comment.  Returns -1 when the buffer has no subject yet. */
int syntax_git_commit_subject(void)
{
	int j;

	for (j = 0; j < editor.numrows; j++) {
		erow *r = &editor.row[j];

		if (r->rsize == 0 || r->render[0] == '#') {
			continue;
		}
		return j;
	}
	return -1;
}

/* True if `row` is the subject line: itself non-blank/non-comment, and
 * every already-existing earlier row (0..row->idx-1) is blank or a '#'
 * comment.  Deliberately does not consult editor.numrows: buffer.c's
 * editor_insert_row() calls editor_update_row() (and thus this
 * highlighter) *before* incrementing numrows, so a numrows-bounded scan
 * could never match a row against itself on first insertion, and a
 * freshly opened commit file would never show the subject warning until
 * the user edited that line.  Walking only the rows that are already in
 * the array sidesteps that ordering entirely. */
static void gitcommit_syntax(erow *row)
{
	int has_subject_above
	    = (row->idx > 0 && editor.row[row->idx - 1].hl_oc);
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

	if (tok[i] == '0' && i + 1 < toklen
	    && (tok[i + 1] == 'x' || tok[i + 1] == 'o')) {
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
		if (p[i] == '&' || p[i] == '*' || p[i] == '!') {
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
static void yaml_syntax(erow *row)
{
	char *p = row->render;
	int len = row->rsize;
	int prev_oc = (row->idx > 0) ? editor.row[row->idx - 1].hl_oc : 0;
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

static void generic_keyword_scan(erow *row)
{
	int in_string = 0; /* Are we inside "" or '' ? */
	int in_comment = 0; /* Are we inside multi-line comment? */
	int prev_sep = 1; /* Tell the parser if 'i' points to start of word. */
	char *p = row->render;
	int i = 0; /* Current char offset */
	char **keywords = editor.syntax->keywords;
	char *mcs = editor.syntax->multiline_comment_start;
	char *mce = editor.syntax->multiline_comment_end;
	char *scs = editor.syntax->singleline_comment_start;

	/* Point to the first non-space char. */
	while (*p && isspace(*p)) {
		p++;
		i++;
	}

	/* If the previous line has an open comment, this line starts
	 * with an open comment state. */
	if (row->idx > 0 && editor.row[row->idx - 1].hl_oc) {
		in_comment = 1;
	}

	while (*p) {
		/* Handle single-line comments (1- or 2-char starter). */
		if (scs[0] && prev_sep && *p == scs[0]
		    && (!scs[1] || *(p + 1) == scs[1])) {
			/* From here to end is a comment */
			memset(row->hl + i, HL_COMMENT, row->size - i);
			goto done;
		}

		/* Handle multi line comments. */
		if (in_comment) {
			row->hl[i] = HL_MLCOMMENT;
			if (*p == mce[0] && *(p + 1) == mce[1]) {
				row->hl[i + 1] = HL_MLCOMMENT;
				p += 2;
				i += 2;
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
			row->hl[i] = HL_MLCOMMENT;
			row->hl[i + 1] = HL_MLCOMMENT;
			p += 2;
			i += 2;
			in_comment = 1;
			prev_sep = 0;
			continue;
		}

		/* Handle "" and '' */
		if (in_string) {
			row->hl[i] = HL_STRING;
			if (*p == '\\') {
				row->hl[i + 1] = HL_STRING;
				p += 2;
				i += 2;
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

		/* Handle non printable chars. */
		if (!isprint(*p)) {
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
			switch (p[1]) {
			case 'b':
			case 'B':
				if (p[2] == '0' || p[2] == '1') {
					row->hl[i] = HL_NUMBER;
					p++;
					i++;
					row->hl[i] = HL_NUMBER;
					p++;
					i++;
					while (*p == '0' || *p == '1') {
						row->hl[i] = HL_NUMBER;
						p++;
						i++;
					}
					prev_sep = 0;
					continue;
				}
				break;
			case 'o':
			case 'O':
				if (p[2] >= '0' && p[2] <= '7') {
					row->hl[i] = HL_NUMBER;
					p++;
					i++;
					row->hl[i] = HL_NUMBER;
					p++;
					i++;
					while (*p >= '0' && *p <= '7') {
						row->hl[i] = HL_NUMBER;
						p++;
						i++;
					}
					prev_sep = 0;
					continue;
				}
				break;
			case 'x':
			case 'X':
				if (isxdigit((unsigned char)p[2])) {
					row->hl[i] = HL_NUMBER;
					p++;
					i++;
					row->hl[i] = HL_NUMBER;
					p++;
					i++;
					while (isxdigit((unsigned char)*p)) {
						row->hl[i] = HL_NUMBER;
						p++;
						i++;
					}
					prev_sep = 0;
					continue;
				}
				break;
			}
		}

		/* Handle numbers */
		if ((isdigit(*p) && (prev_sep || row->hl[i - 1] == HL_NUMBER))
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

static void editor_update_syntax_row_only(erow *row)
{
	unsigned char *newhl;

	if (row->rsize == 0) {
		free(row->hl);
		row->hl = NULL;
		row->hl_oc = 0;
		if (editor.syntax && editor.syntax->highlight) {
			editor.syntax->highlight(
			    row); /* state propagation only */
		} else {
			row->hl_oc = (row->idx > 0)
			    ? editor.row[row->idx - 1].hl_oc
			    : 0;
		}
		return;
	}

	newhl = realloc(row->hl, row->rsize);
	if (!newhl) {
		editor_set_status_message("Out of memory");
		running = 0;
		return;
	}
	row->hl = newhl;
	memset(row->hl, HL_NORMAL, row->rsize);

	if (editor.syntax == NULL) {
		return; /* No syntax, everything is HL_NORMAL. */
	}

	if (editor.syntax->highlight) {
		row->hl_oc = 0;
		editor.syntax->highlight(row);
	} else {
		generic_keyword_scan(row);
	}
}

static void syntax_propagate_downstream(erow *row, int old_oc)
{
	int idx = row->idx;
	while (row->hl_oc != old_oc && idx + 1 < editor.numrows) {
		idx++;
		row = &editor.row[idx];
		old_oc = row->hl_oc;
		editor_update_syntax_row_only(row);
	}
}

/* Set every byte of row->hl (that corresponds to every character in the line)
 * to the right syntax highlight type (HL_* defines). */
void editor_update_syntax(erow *row)
{
	int old_oc = row->hl_oc;
	editor_update_syntax_row_only(row);
	syntax_propagate_downstream(row, old_oc);
}

void editor_rehighlight_from(int start_idx)
{
	if (start_idx < 0 || start_idx >= editor.numrows) {
		return;
	}
	editor_update_syntax(&editor.row[start_idx]);
}

struct editor_syntax *syntax_find_by_name(const char *name)
{
	int j;
	for (j = 0; j < (int)HLDB_ENTRIES; j++) {
		if (strcmp(HLDB[j].name, name) == 0) {
			return &HLDB[j];
		}
	}
	return NULL;
}

void editor_rehighlight_all(void)
{
	int j;
	for (j = 0; j < editor.numrows; j++) {
		editor.row[j].hl_oc = 0;
	}
	for (j = 0; j < editor.numrows; j++) {
		editor_update_syntax_row_only(&editor.row[j]);
	}
}

void editor_set_syntax(struct editor_syntax *syntax)
{
	editor.syntax = syntax;
	if (buf_current >= 0 && buf_current < MAX_BUFFERS) {
		buflist[buf_current].syntax = syntax;
	}
	editor_rehighlight_all();
}

/* Maps syntax highlight token types to terminal colors. */
int editor_syntax_to_color(int hl)
{
	switch (hl) {
	case HL_COMMENT:
	case HL_MLCOMMENT:
		return 36; /* cyan */
	case HL_KEYWORD1:
		return 33; /* yellow */
	case HL_KEYWORD2:
		return 32; /* green */
	case HL_STRING:
		return 35; /* magenta */
	case HL_NUMBER:
		return 31; /* red */
	case HL_MATCH:
		return 34; /* blue */
	case HL_WARNING:
		return 31; /* red */
	default:
		return 37; /* white */
	}
}

/* Map a shebang interpreter name to a file extension for syntax lookup.
 * Supports versioned names (python3, python3.11) since shebangs often
 * pin specific versions.
 * Returns a dot-prefixed extension string, or NULL if unknown. */
static const char *shebang_interp_to_ext(const char *interp)
{
	static const struct {
		const char *name;
		const char *ext;
	} table[] = {
		{ "sh", ".sh" },
		{ "bash", ".bash" },
		{ "zsh", ".zsh" },
		{ "ksh", ".ksh" },
		{ "csh", ".csh" },
		{ "tcsh", ".tcsh" },
		{ "dash", ".sh" },
		{ "python", ".py" },
		{ "pypy", ".py" },
		{ "ruby", ".rb" },
		{ "node", ".js" },
		{ "nodejs", ".js" },
		{ "perl", ".pl" },
		{ NULL, NULL },
	};
	int i;

	for (i = 0; table[i].name; i++) {
		size_t len = strlen(table[i].name);
		if (strncmp(interp, table[i].name, len) == 0
		    && (interp[len] == '\0'
			|| isdigit((unsigned char)interp[len])
			|| interp[len] == '.')) {
			return table[i].ext;
		}
	}
	return NULL;
}

/* Try to select syntax by reading a hash-bang (#!) on the first line of
 * filename, falling back to extension matching via shebang_interp_to_ext(). */
static void select_syntax_by_shebang(const char *filename)
{
	char line[256];
	char *interp, *slash, *end;
	const char *ext;
	unsigned int j, i;
	FILE *fp;

	fp = fopen(filename, "r");
	if (!fp) {
		return;
	}
	char *got = fgets(line, sizeof(line), fp);
	fclose(fp);
	if (!got) {
		return;
	}

	if (line[0] != '#' || line[1] != '!') {
		return;
	}

	/* Strip leading spaces after #! */
	interp = line + 2;
	while (*interp == ' ') {
		interp++;
	}

	/* Take the last path component: "/usr/bin/bash" → "bash" */
	slash = strrchr(interp, '/');
	if (slash) {
		interp = slash + 1;
	}

	/* If the component is "env", the real interpreter is the next word.
	 * Handles "#!/usr/bin/env python3" and "#!env bash". */
	if (strncmp(interp, "env", 3) == 0
	    && (interp[3] == '\0' || isspace((unsigned char)interp[3]))) {
		interp += 3;
		while (*interp == ' ') {
			interp++;
		}
	}

	/* Trim at whitespace (strips newline and any arguments) */
	end = interp;
	while (*end && !isspace((unsigned char)*end)) {
		end++;
	}
	*end = '\0';

	if (*interp == '\0') {
		return;
	}

	ext = shebang_interp_to_ext(interp);
	if (!ext) {
		return;
	}

	for (j = 0; j < HLDB_ENTRIES; j++) {
		struct editor_syntax *s = HLDB + j;
		for (i = 0; s->filematch[i]; i++) {
			if (strcmp(s->filematch[i], ext) == 0) {
				editor.syntax = s;
				return;
			}
		}
	}
}

/* Select the syntax highlight scheme depending on the filename,
 * setting it in the global state editor.syntax. */
void editor_select_syntax_highlight(char *filename)
{
	unsigned int j;

	for (j = 0; j < HLDB_ENTRIES; j++) {
		struct editor_syntax *s = HLDB + j;
		unsigned int i = 0;
		while (s->filematch[i]) {
			int patlen = strlen(s->filematch[i]);
			char *p = strstr(filename, s->filematch[i]);
			if (p
			    && (s->filematch[i][0] != '.'
				|| p[patlen] == '\0')) {
				editor.syntax = s;
				return;
			}
			i++;
		}
	}

	/* No extension match — try hash-bang on first line of file */
	select_syntax_by_shebang(filename);
}

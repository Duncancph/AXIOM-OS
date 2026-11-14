# AXIOM JavaScript engine

A freestanding JavaScript engine in C for the AXIOM web browser app. It implements every
function of `js.h`, runs ES5 completely and most of ES2015–ES2024, and is checked against
node v22 by a test corpus (`tests/js/`).

## Files

| file | contents |
|---|---|
| `js.h` | public API (the browser side codes against it; three additions at the end) |
| `js_port.h` | the only place that knows about the platform: freestanding prototypes, setjmp, C-stack budget, `sqrtsd` |
| `js_int.h` | internal types (values, strings, objects, frames, runtime) and cross-file prototypes |
| `js_ops.h` | bytecode opcode list (X-macro: name, operand bytes) |
| `js_parse.h`, `js_parse.c` | lexer, parser to AST, scopes/bindings, early errors |
| `js_compile.c` | binding resolution (locals vs heap environments, TDZ) and bytecode generation |
| `js_vm.c` | interpreter, calls/frames, exceptions, generators/async frames, eval, job queue, stack traces |
| `js_obj.c` | objects, properties, the [[Get]]/[[Set]]/[[DefineOwnProperty]]… algorithms, conversions, errors |
| `js_str.c` | strings (Latin-1/UTF-16, ropes), atoms, UTF-8, string builder, Unicode case/space tables |
| `js_num.c` | math library (fdlibm ports), number⇄string (shortest round trip, toFixed/Precision/Exponential, radix, strtod) |
| `js_gc.c` | precise mark & sweep collector |
| `js_api.c` | `js.h` implementation |
| `js_b_core.c` | Object, Function, Symbol, Error types, Boolean, Reflect, Proxy, global functions |
| `js_b_array.c` | Array |
| `js_b_string.c` | String |
| `js_b_number.c` | Number, Math, BigInt |
| `js_b_json.c` | JSON, node-style value formatting (`util.inspect` subset), console |
| `js_regex.c` | backtracking RegExp engine and RegExp builtins |
| `js_b_date.c` | Date |
| `js_b_promise.c` | Promise, microtasks, async functions/generators |
| `js_b_iter.c` | iterators, generators |
| `js_b_coll.c` | Map, Set, WeakMap, WeakSet, WeakRef, FinalizationRegistry, ArrayBuffer, typed arrays, DataView, structuredClone |

## Building

Target (each file separately, must stay warning-free):

    gcc -std=gnu11 -O2 -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie -m64 \
        -mcmodel=large -mno-red-zone -msse2 -mfpmath=sse -fno-tree-loop-distribute-patterns \
        -Wall -Wextra -Wno-unused-parameter -c js_xxx.c

The objects only need `memset memcpy memmove memcmp strlen strcmp strncmp malloc free realloc calloc`
(check: `ld -r -o all.o js_*.o && nm -u all.o`). No `<math.h>`/`<stdio.h>`, no `long double`/x87.

Host test build and corpus (from the repository root):

    gcc -O1 -g -fsanitize=address,undefined -DJS_HOST_TEST -o build/test/jsrun tests/js/jsrun.c user/apps/web/js/*.c
    python3 tests/js/run.py            # cases vs node, then perf table
    python3 tests/js/run.py --no-perf -v 12_regexp
    gcc -O2 -DJS_HOST_TEST -o build/test/jsrun_fast tests/js/jsrun.c user/apps/web/js/*.c
    python3 tests/js/run.py --jsrun build/test/jsrun_fast --perf

`run.py` runs every `tests/js/cases/*.js` with node (through `node_script.js`, which runs the file as a
classic script like a browser does, with `TZ=UTC`) and with `jsrun`, and requires identical stdout and
agreeing exit status. `jsrun` prints console levels 0/1 to stdout and 2/3 to stderr, runs jobs after the
script, exits 1 on an uncaught exception, and honours `JSRUN_TIMEOUT_MS` (tests the interrupt hook).

## Language support

* ES5 except `with` (strict and sloppy mode, getters/setters, `arguments` incl. mapped arguments in
  sloppy functions with simple parameter lists, labels, ASI, Annex B function-in-block semantics, …).
* ES2015+: `let`/`const` with TDZ (per-iteration bindings in loops), arrow functions, default/rest
  parameters, spread, destructuring (all forms), template literals and tagged templates (`String.raw`,
  cached template objects), classes (fields, static fields/blocks, `#private` fields/methods/accessors,
  `#x in o`, `super`, `new.target`, derived constructors), computed/shorthand properties, `for…of`,
  iterators, generators, Promise, `async`/`await`, async generators and `for await`, optional chaining,
  `??`, `??=`, `||=`, `&&=`, `**`, numeric separators, BigInt literals, optional catch binding,
  `globalThis`, hashbang, HTML-like comments, labelled continue/break, getters in classes, Symbols
  (well-known symbols honoured by the builtins: iterator, asyncIterator, hasInstance, toPrimitive,
  toStringTag, species, isConcatSpreadable, match/matchAll/replace/search/split, unscopables).
* Builtins: Object (incl. groupBy, fromEntries, hasOwn, getOwnPropertyDescriptors, …), Function
  (constructor, bind, toString with source text), Array (all ES2023 methods incl. toSorted/toSpliced/with,
  findLast, flat/flatMap, at, copyWithin, Array.from/of), String (all incl. replaceAll, matchAll, at,
  padStart/End, normalize (NFC/NFD for precomposed Latin, Greek and Cyrillic letters; no canonical
  reordering of multiple marks; NFKC/NFKD behave as NFC/NFD), isWellFormed, localeCompare approximation, HTML methods),
  Number (toFixed/toPrecision/toExponential/toString(radix)/toLocaleString en-US), Math (all functions,
  bit-identical with V8 on random tests), JSON (parse with reviver, stringify with replacer/indent/toJSON,
  V8-identical error messages), Date (UTC is the local time zone; ISO and common free-form parsing,
  all getters/setters, toISOString/toString/toUTCString/toLocale*String en-US), RegExp (see below),
  Error types with `stack`, `cause`, AggregateError, Map/Set (+ ES2025 set methods), WeakMap/WeakSet,
  WeakRef, FinalizationRegistry, Promise (all/allSettled/any/race/withResolvers/finally), Reflect, Proxy
  (all traps and invariants), ArrayBuffer/typed arrays/DataView (incl. BigInt64), BigInt (arbitrary
  precision, asIntN/asUintN), structuredClone, queueMicrotask, URI functions, escape/unescape, `eval`.
* RegExp: backtracking engine with flags `dgimsuyv`, lastIndex/sticky semantics, named groups (and
  `$<name>` substitutions), lookahead and lookbehind, backreferences (named too), lazy/possessive-free
  quantifiers, Unicode escapes and property escapes (`\p{L}`, `\p{Lu}`, `\p{Ll}`, `\p{N}`, `\p{Nd}`,
  `\p{P}`, `\p{S}`, `\p{Z}`, `\p{M}`, `\p{C}`, `\p{Alphabetic}`, `\p{White_Space}`, `\p{ASCII}`,
  `\p{Any}`, `\p{Emoji}`, `\p{Script=Latin|Greek|Cyrillic|Han|Hiragana|Katakana|Arabic|Hebrew|Hangul|Thai}`),
  case-insensitive matching with Unicode case folding, catastrophic patterns stay interruptible.
* console: log/info/debug/warn/error/dir/table/trace/assert/group/groupEnd/count/countReset/time/
  timeEnd/timeLog with node-style formatting (`%s %d %i %f %o %O %j %c %%`, util.inspect rules for
  nesting depth, line breaking at 80 columns, array grouping, Map/Set/typed arrays/class instances,
  circular references, getters, symbols).
* Error messages follow V8's wording ("Cannot read properties of undefined (reading 'x')",
  "o.f is not a function", "x is not iterable", "Cannot destructure property 'q' of 'null' as it is
  null.", JSON positions, …) so pages behave as in Chrome.

### Not supported / differences

* No ES modules (`import`/`export`), no `with` statement (SyntaxError "'with' statements are not
  supported"), no tail calls.
* `eval` always evaluates in the global scope (direct eval cannot see the caller's local variables);
  `let`/`const`/`class` declared inside eval stay local to that eval.
* Functions whose parameter list has default values do not get the separate parameter scope, so a
  `var` in the body that redeclares a parameter shares it with closures in the defaults.
* `yield*` inside async generators is a SyntaxError.
* Intl, Atomics/SharedArrayBuffer, resizable ArrayBuffers, Iterator helpers (`Iterator.prototype.map`
  etc.), `Array.fromAsync`, `JSON.rawJSON`, legacy RegExp statics (`RegExp.$1`), `__lookupGetter__`,
  `Error.captureStackTrace`/`prepareStackTrace`, explicit resource management (`using`).
* `toLocaleString` and friends always use en-US; local time zone is UTC.
* WeakRef holds its target strongly and FinalizationRegistry callbacks never run (no weak references
  in the collector). WeakMap/WeakSet entries live on the key object, so they are collected with it.
* console `%o` is formatted like `%O` (no hidden properties).
* No browser APIs (setTimeout, DOM, fetch…); those belong to the browser side.

## Design

**Values.** NaN-boxed 64-bit words: doubles as themselves (NaNs canonicalised); tags in the top 16 bits
(≥ 0xFFF9) for undefined/null/booleans and internal markers (hole, uninitialised binding, exception),
strings, objects, symbols, BigInts and internal cells. Integers are just doubles.

**Strings** are immutable, either Latin-1 (1 byte/char) or UTF-16, with lazily flattened ropes for
concatenation. Property keys are atoms (interned strings in a weak hash table), so key comparison is a
pointer compare. UTF-8 is converted at the API boundary only; lone surrogates become U+FFFD there.

**Objects** hold an array of (key, value, flags) properties in insertion order, plus an open-addressed
hash index once they have more than 8 properties. Arrays keep dense elements (with holes) and fall back
to properties when sparse. Functions create `length`/`name`/`prototype` lazily. Exotic objects (arrays,
arguments, strings, typed arrays, proxies, host objects) take a slow path flagged per object.

**Compiler/VM.** Source → AST → a resolution pass that decides for every binding whether it lives in a
frame slot or (when captured by a closure) in a heap environment, and where TDZ checks are needed →
stack bytecode. The interpreter uses computed goto, inline caches for property loads/stores and global
lookups, and does not recurse in C for JS→JS calls (frames live in an array), so deep JS recursion
(≈4500 levels) needs no C stack. Generators and async functions save their frame and operand stack
into the generator object. Natives that call back into JS (Array methods, getters, Proxy traps,
toString/valueOf, …) do recurse in C; that recursion is bounded by measuring the C stack
(`JS_C_STACK_LIMIT`, 192 KiB of the 256 KiB user stack) and by a frame limit, and throws
`RangeError: Maximum call stack size exceeded`. The parser and compiler check the same budget.

**Interrupts.** `host->interrupt` is polled every ~10k backward jumps/calls and inside the regex
matcher; when it returns true the script is terminated with an uncatchable
"Script execution was interrupted" error (catch/finally blocks do not run).

**GC.** Precise mark & sweep with an explicit mark stack. Roots: the global object and global lexical
scope, interpreter value stack and frames, exception handlers, `js_root` slots, pending jobs and
unhandled rejections, intrinsics. Host objects are traced through `js_class.trace` and finalised
through `js_class.finalize` (also at `js_free`). The collector only runs at safe points: when the
interpreter is at a call or loop back-edge and the allocation counter passed its threshold, or in
`js_gc`. It never runs inside an allocation, so C code may keep fresh values in local variables until it
next runs JavaScript.

**Numbers.** Shortest round-trip formatting (Burger–Dybvig with bignums), exact decimal expansion for
toFixed/toPrecision/toExponential, correctly rounded string→double (bignum-verified), radix conversion as
in V8; the math functions are fdlibm ports with V8's adjustments, so results equal node's bit for bit.

## Host interface notes (`js.h`)

* **Additions** (appended; existing declarations unchanged): `js_is_constructing(rt)` — inside a
  native, true when called with `new`; `js_has_exception(rt)`; `js_construct(rt, ctor, argc, argv,
  &result)`.
* **Host objects.** `cls->get` is called (key as UTF-8) before the ordinary own-property lookup of that
  object, for every host object met along the prototype chain, and also by `in`/`hasOwnProperty`/
  `getOwnPropertyDescriptor`; symbol keys skip the hook. `cls->set` is called only when the host object is
  the receiver itself, before the ordinary set. Returning false falls back to ordinary properties.
* **Natives** (`js_function`, accessors): `this_`/`argv` are rooted for the call; `argv` has at least
  `length` entries (missing ones are undefined). Functions made by `js_function` are constructible: with
  `new`, `this_` is a fresh object inheriting from `fn.prototype` and returning a non-object yields it.
  To throw, `return js_throw(...)` / `js_throw_value(...)`; returning any value while an exception is
  pending also throws it. If `js_get`/`js_set`/`js_to_*` run a throwing getter or `valueOf` inside a
  native, the exception stays pending (see `js_has_exception`) and is thrown when the native returns;
  outside natives such exceptions are reported to the console (level 3) and the call returns
  undefined/0/"".
* **Re-entrancy.** `js_eval`, `js_call`, `js_construct`, `js_run_jobs` may be called from natives;
  they return -1 on a throw, leaving `js_exception`/`js_error_text` set but the exception *not*
  pending (to propagate it: `return js_throw_value(rt, js_exception(rt))`). They fail immediately with
  -1 if an exception is already pending.
* **Rooting.** GC can run in `js_eval`, `js_call`, `js_construct`, `js_run_jobs`, `js_gc`, and any call
  that may run JS code (getters, setters, valueOf/toString, proxies): `js_get`, `js_set`,
  `js_get_index`, `js_set_index`, `js_length`, `js_to_number`, `js_to_int`, `js_to_string`,
  `js_to_cstring`, `js_get_prototype`, `js_set_prototype`, `js_define`. Pure constructors (`js_str`,
  `js_object`, `js_array`, `js_function`, `js_host_object`, `js_new_error`, …) never collect. Any value
  the C side keeps across such a call must be in a `js_root`ed slot (or reachable from a rooted value
  or a traced host object). Values passed as `fn`/`this_`/`argv` to `js_call` are rooted during it.
* `js_to_string` returns -1 if conversion threw; `js_to_cstring` returns a malloc'd "" then.
* `js_error_text` gives "Type: message (file:line)".

## Status

* Corpus: 32 case files, 1316 output lines identical to node v22 (`tests/js/cases`), run under
  ASan/UBSan. Includes realistic app-style scripts (todo app model, tokenizer/parser, JSON
  transformation, async pipelines, class hierarchies).
* Performance (`tests/js/perf/bench.js`, -O2 host build, ms, node in parentheses): fib(25) 5 (6),
  1e6-iteration loop 17 (2), sort 100k 68 (43), string building 15 (6), join 19 (16), object
  literals 37 (2), class instances 84 (6), closures 19 (30), Map/Set 22 (7), JSON round trips 36 (15),
  regex 19 (2), array methods 22 (7), generators 46 (7), builtin calls 10 (1). An interpreter is 1–20×
  slower than V8's JIT on hot loops, on par for library-heavy code.

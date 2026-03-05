# NovaLang Toolchain Bootstrap

This repository contains an ANTLR4 grammar (`nova.g4`) for the NovaLang surface
syntax and now includes a C++-based toolchain prototype. The codebase covers
lexing, parsing, semantic analysis with type inference and effect tracking, a
typed intermediate representation, native code generation, and lightweight
developer tooling.

## What Exists Today

* `nova.g4` — source grammar that remains the authoritative description of the
  language syntax.
* `scripts/generate_tokens.py` — lightweight generator that extracts token
  metadata from `nova.g4` and emits `include/nova/generated_tokens.h`, ensuring
  the handwritten lexer stays aligned with the grammar.
* `include/` & `src/` — C headers and implementations for:
  * Token infrastructure (`nova/token.h`, `src/token.cpp`).
  * Lexer (`nova/lexer.h`, `src/lexer.cpp`) translating NovaLang source into a
    stream of `NovaToken` structures.
  * Expanded AST data structures (`nova/ast.h`, `src/ast.cpp`) that faithfully
    capture variants, match arms, pipelines, async/await, blocks, and literal
    forms described in `nova.g4`.
  * A fault-tolerant recursive-descent parser (`nova/parser.h`, `src/parser.cpp`)
    with diagnostics and recovery that mirrors the grammar and produces a
    `NovaProgram` tree.
  * A richer semantic analysis engine (`nova/semantic.h`, `src/semantic.cpp`)
    featuring scope management, type inference, effect tracking, variant
    exhaustiveness checking, and per-expression type/effect metadata.
  * A typed intermediate representation (`nova/ir.h`, `src/ir.cpp`) lowered from
    the AST with help from semantic results.
  * A low-latency incremental mark/sweep garbage collector runtime (`nova/gc.h`,
    `src/gc.cpp`) with pluggable allocators for performance tuning and tests.
  * A native code generator (`nova/codegen.h`, `src/codegen.cpp`) that emits C
    and drives the system compiler to produce object files.
* Developer tooling under `tools/`:
  * `nova-fmt` — simple formatter that reflows NovaLang source while validating
    syntax.
  * `nova-check` — end-to-end stability checker that parses, runs semantic
    analysis, lowers to IR, and optionally runs native code generation. It can
    now also emit ahead-of-time (AOT) native executables.
  * `nova-repl` — interactive shell that reports the inferred type of
    expressions.
  * `nova-new` — scaffolds a new NovaLang project with a manifest and sample
    entry point that compiles end-to-end.
* `docs/language.md` — methodised language reference covering syntax and core
  semantics.
* `examples/` — small NovaLang programs that exercise pipelines, matches, and
  loops.
* `tests/parser_tests.cpp` — end-to-end tests that cover parsing, semantics,
  exhaustiveness warnings, IR generation, and native code emission.
* `Makefile` — builds a reusable static library plus tests/tools with `g++` for faster incremental builds.

## Running the Tests

```
make
./build/tests
```

You can also validate a single source file with the stability checker:

```
./build/nova-check path/to/file.nova
```

Try one of the shipped examples:

```
./build/nova-check examples/pipeline.nova
```


You can also ask `nova-check` to produce an ahead-of-time executable from a
Nova source module. By default it uses the `main` Nova function as the entry
point, but you can override that with `--entry`:

```
./build/nova-check --emit-aot build/demo-app --entry app_entry path/to/file.nova
```

The native backend uses an aggressive low-latency profile (`-O3 -flto
-fno-plt -fomit-frame-pointer -DNDEBUG`) and supports overriding the compiler
binary through the `NOVA_CC` environment variable.

The Makefile supports `NOVA_COMPAT=0` for stricter C++ builds (disabling `-fpermissive`) once all legacy C-style conversions are eliminated.

The Makefile also uses per-file dependency generation (`-MMD -MP`) so incremental rebuilds are both faster and more reliable after header edits.

You can run a strict C++ conformance build (without compatibility mode) using:

```
make strict
```

The test suite parses representative NovaLang snippets, runs semantic analysis
to validate inference and diagnostics, lowers to the intermediate
representation, and exercises the native code generator.

## Building Release Artifacts

To produce a set of precompiled binaries and package them for distribution,
run the release helper script. You can optionally provide a version number that
will be baked into the archive name; if omitted, the script falls back to
`git describe --tags --always`.

```
./scripts/build_release.sh v0.1.0
```

To produce Windows archives, pass `--target windows-x86_64` while running the
script on a Windows machine (or in a Windows GitHub Actions runner):

```
./scripts/build_release.sh --target windows-x86_64 v0.1.0
```

Each invocation builds the NovaLang tools, assembles them under
`dist/v0.1.0/<target>`, and creates an archive plus a SHA-256 checksum file
ready to be attached to a GitHub release. You can also use the Makefile wrapper:

```
make release VERSION=v0.1.0 RELEASE_TARGET=windows-x86_64
```

Push a tag that matches the version (for example, `git tag v0.1.0 && git push
origin v0.1.0`) to trigger the included GitHub Actions workflow, which will
build both Linux and Windows archives and upload them to a GitHub Release
automatically.

For continuous verification, the repository now includes a CI workflow that
builds the toolchain and runs the test suite on every push.

## Next Steps

The next milestones focus on broadening expression lowering in the IR,
covering more host-side optimisations, expanding the formatter, and layering in
language-server features.

## Recent Enhancements

* The IR lowerer now understands conditional expressions, pipelines, match
  arms, list literals, and unit values, providing richer inputs for later
  compilation stages.
* Host-side code generation folds `if` expressions with constant boolean
  conditions and emits ternary expressions, reducing generated code size.
* `nova-fmt` gained tighter spacing rules around commas, arrows, and braces for
  more idiomatic output.
* A new `nova-lsp` binary speaks a minimal subset of the Language Server
  Protocol, supporting initialise/shutdown and hover requests that surface the
  inferred type at a cursor position.
* The code generator now invokes `cc` with `-O3` for more aggressive native
  optimisations, and the IR lowerer performs constant-folding on `if` and simple
  `match` expressions to eliminate dead branches before emission.

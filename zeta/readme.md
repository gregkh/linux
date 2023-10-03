## Zeta

Auxiliary directory for tools and configuration files.


### Branches and History

**Premise**: The `cf-linux-rolling-stable` branch keeps track of the
`linux-rolling-stable` branch.

```
o
| <- zeta (commit)
| <- linux-rolling-stable, cf-linux-rolling-stable
|
=
```

Definitions:
- Zeta: Auxiliary commits for devel/testing (do not touch the kernel code).
- hacl: All the commits that include new cryptographic code from hacl.

```
o
| <- hacl-hash (commits)
| <- cf-zeta (commits)
| <- linux-rolling-stable, cf-linux-rolling-stable,
|
=
```

Naturally, the `linux-rolling-stable` branch moves due to the upstream activity.

```
o
| <- linux-rolling-stable
|
|   o
|   | <- hacl-hash (commits)
|   | <- cf-zeta (commit)
|---/ <- cf-linux-rolling-stable,
|
=
```

Rebase: to catch up with upstream, commits must be rebased.
It is expected that this is an easy rebase (merge strategy), because the
contributed files will barely touch the kernel code.
Thus, we aim for a linear history.

```
                            o
                            | <- hacl-hash (commits)
o                           | <- cf-zeta (commit)
| <- linux-rolling-stable---/ <- cf-linux-rolling-stable,
|
|
=
```

### Code Style and Formatting

[F.1]: https://www.kernel.org/doc/html/latest/process/coding-style.html
[F.2]: https://www.kernel.org/doc/html/latest/process/clang-format.html
[F.asytle]: https://astyle.sourceforge.net/
[F.clangf]: https://clang.llvm.org/docs/ClangFormat.html
[F.c99]: https://en.cppreference.com/w/c/types/integer

The following are (non-exhuastive) guidelines for code style taken from
[Kernel coding style][F.1] and the [Clang kernel documentation][F.2].

- 8chars identation and 80 columns.
- Pointer is attached to name, so `int *x`.
- Use [C99][F.c99] fixed-size types.
- Avoid the use of typedef on structs and standard types.
- Comment Style
```c
/*
 * lorem ipsum dolor sit amet
 * lorem ipsum dolor sit amet
 * lorem ipsum dolor sit amet
 */
```

- Defines are ALLCAPS and consecutive defines are aligned:
```c
#define CONSTANT_LARGE_NAME_SIZE 2
#define CONSTANT_MEDIUM_SIZE     3
#define CONSTANT_SMALL           4
```

#### Formatting

To format files. run

```sh
$ make -C zeta format FILES="/crypto/sha256_generic.c /crypto/sha512_generic.c"
```

Pass in `FILES` the files to be formatted. Note the leading `/` (slash), which
indicates the position of the files with respect to the kernel source code.

This will invoke [astyle][F.asytle] followed by [clang-format][F.clangf] commands.
AStyle is used to prepare the files for clang-format, as the latter does
not catch issues that astyle does.

### Linting

[L.cpplint]: https://github.com/cpplint/cpplint

Zeta contains a linter script to be used as a reference to spot coding errors.
The [cpplinter][L.cpplint] could be verbose as it targets C++, however it helps
to remove minor issues.

```sh
$ make -C zeta lint FILES="/crypto/sha256_generic.c /crypto/sha512_generic.c"
```

Pass in `FILES` the files to be linted. Note the leading `/` (slash), which
indicates the position of the files with respect to the kernel source code.

# libtrash

A small, dependency-free C++17 library for moving files and directories to the
operating system's trash / recycle bin instead of permanently deleting them.

```cpp
#include <libtrash/trash.hpp>

std::error_code ec;
if (!libtrash::trash("path/to/file-or-dir", ec))
    std::fprintf(stderr, "%s\n", ec.message().c_str());

// ...or the throwing overload:
libtrash::trash("path/to/file-or-dir");   // throws std::filesystem::filesystem_error
```

- **`trash()`** in non-throwing and throwing overloads, mirroring `<filesystem>`.
- **`trash_available()`** to ask first, for a UI that should only offer trashing where it works.
- **Files and directories**: a directory and its contents move as a single unit.
- **Never permanently deletes**: if an item can't be trashed, the call fails and leaves it in place.
- **No third-party dependencies**: only the platform SDK.

Backends: Linux/BSD implement the
[FreeDesktop.org Trash specification v1.0](https://specifications.freedesktop.org/trash-spec/trashspec-1.0.html)
directly (no glib/Qt/GTK); Windows uses `IFileOperation`; macOS uses
`-[NSFileManager trashItemAtURL:...]`.

## Asking first

`trash_available()` reports whether a trash usable for a given path exists,
without moving or creating anything — for greying out a "Move to Trash" command,
or for telling a user which of the two things a Remove button is about to do.

```cpp
if (libtrash::trash_available("/mnt/backups"))
    // ...offer "Move to Trash" for items under /mnt/backups

std::error_code ec;                                  // ...or ask why not
if (!libtrash::trash_available("/mnt/backups", ec))
    std::fprintf(stderr, "%s\n", ec.message().c_str());
```

The one-argument form returns false for every reason it is not a yes and throws
nothing, so a menu item's enabled state can come straight from the result.

Pass the path you actually intend to trash, or the directory it lives in: every
platform decides per volume, so there is no machine-wide answer. A network
share, a volume with recycling switched off, and a home directory that has run
out of quota all differ from the disk beside them.

The answer is advisory in both directions. Because nothing is created, a `true`
can still be followed by a `trash()` that fails — a full disk, a permission
change, a race. And a `false` is not authority to delete permanently: `trash()`
will never do that, so the fallback is the caller's decision to make and to say
out loud.

How each platform is asked:

| Platform | Query |
| --- | --- |
| Linux/BSD | replays the spec's own directory selection (home trash, then `$topdir/.Trash{,-$uid}`) without creating anything |
| macOS | `-[NSFileManager URLForDirectory:NSTrashDirectory ... appropriateForURL:create:NO]` |
| Windows | drive type, `SHQueryRecycleBin`, and Explorer's `NoRecycleFiles` / `NukeOnDelete` settings |

## Error handling

Failures are reported through `libtrash::errc`, which integrates with
`std::error_code` (`libtrash::error_category()`):

| `libtrash::errc` | Meaning |
| --- | --- |
| `invalid_argument` | empty path, embedded NUL, or invalid UTF-8 |
| `not_found` | the path does not exist |
| `permission_denied` | could not create the trash directory or move the item |
| `cross_device` | the item's filesystem has no usable trash |
| `platform_error` | underlying OS API failure |
| `unsupported` | no backend for this platform |

```cpp
if (ec == libtrash::errc::not_found) { /* ... */ }
```

`not_found`, `invalid_argument`, and `permission_denied` behave the same on every
platform; the others are advisory and may vary by OS, so prefer the
success/failure result for control flow.

## Behavior details

- **Encoding.** Paths are UTF-8 on every platform. On Windows a
  `std::filesystem::path`'s narrow `.string()` is the local code page, *not*
  UTF-8:  convert accordingly before calling.
- **Relative paths** are resolved against the current working directory. Don't
  change the CWD from another thread during a call.
- **Symlinks** are trashed as the link itself, never their target; symlinks and
  `.`/`..` in the *directory* part of the path are resolved.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Add `-DLIBTRASH_SHARED=ON` for a shared library (static by default).

## Use as a submodule

```cmake
add_subdirectory(third-party/libtrash)   # tests/example are skipped automatically
target_link_libraries(your_app PRIVATE libtrash::trash)
```

The Foundation framework (macOS) and `ole32` / `shell32` (Windows) are linked
automatically.

## License

MIT. See [LICENSE](LICENSE).

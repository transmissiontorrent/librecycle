// SPDX-License-Identifier: MIT
// This file Copyright © Mnemosaic LLC.
//
// libtrash - cross-platform "move a file/directory to the trash" library.
//
// Two entry points: libtrash::trash() moves a file or directory to the trash,
// and libtrash::trash_available() reports whether it could. No glib/Qt/GTK
// dependency.
//
//   - Linux/*BSD : FreeDesktop.org Trash specification v1.0
//   - Windows    : IFileOperation (FOFX_RECYCLEONDELETE)
//   - macOS      : -[NSFileManager trashItemAtURL:resultingItemURL:error:]
//
// Error handling mirrors <filesystem>: a throwing overload and a non-throwing
// overload taking std::error_code&. Failures use the libtrash::errc enum,
// which plugs into std::error_code via the standard make_error_code
// customization.

#ifndef LIBTRASH_TRASH_HPP
#define LIBTRASH_TRASH_HPP

#include <string_view>
#include <system_error>

namespace libtrash
{

// Portable failure reasons. Values are stable; 0 is reserved for "no error".
enum class errc
{
    invalid_argument = 1, // empty path, embedded NUL, or invalid encoding
    not_found, // the path does not exist
    permission_denied, // could not create trash dirs or move the item
    cross_device, // no usable trash directory on the item's filesystem
    platform_error, // underlying OS API failure
    unsupported, // no backend for this platform
};

// The error_category for libtrash::errc.
std::error_category const& error_category() noexcept;

// Enables std::error_code{libtrash::errc::...} and ec == errc::... checks.
std::error_code make_error_code(errc e) noexcept;

// Move a file or directory to the OS trash (recycle bin) instead of deleting it.
//
//     std::error_code ec;
//     if (!libtrash::trash("notes.txt", ec)) { /* inspect ec */ }  // non-throwing
//     libtrash::trash("notes.txt");                                // throws on failure
//
// A directory is trashed together with all of its contents in a single operation
// (there is no separate "trash_all"). libtrash never permanently deletes: if the
// item cannot be moved to the trash, the call fails and leaves it in place.
//
// Edge cases:
//   * Paths are UTF-8 on every platform. Empty, containing a NUL, or invalid
//     UTF-8 -> errc::invalid_argument.
//   * A relative path is resolved against the current working directory; do not
//     change the CWD from another thread during the call.
//   * Symlinks and '.'/'..' in the *directory* part of the path are resolved,
//     but the final component is not: trashing a symlink moves the link itself,
//     not its target.
//
// Non-throwing: returns true on success; on failure returns false and sets `ec`.
bool trash(std::string_view utf8_path, std::error_code& ec) noexcept;

// Throwing overload: returns normally on success, throws
// std::filesystem::filesystem_error (carrying the path and a libtrash::errc) on
// failure.
void trash(std::string_view utf8_path);

// Whether a trash usable for `utf8_path` exists, without moving or creating
// anything. This is the question a UI needs answered before it offers to trash
// something -- and it is per-path, because every platform decides per volume:
// ask about the item (or the directory) you actually intend to trash.
//
//     if (libtrash::trash_available(path)) { /* offer "Move to Trash" */ }
//
// Returns true when trash() would have somewhere to put the item, and false for
// every reason it would not -- including a missing or malformed path. Nothing is
// thrown, so a menu item can be greyed out straight from the result: "this
// filesystem has no trash" is an answer, not an error.
//
// The answer is advisory in both directions. Because it creates nothing, a
// "yes" can still be followed by a trash() that fails -- a directory that was
// creatable a moment ago, a full disk, a race with another process. And a "no"
// is never authority to delete the item permanently: trash() will not do that,
// and neither should a caller without saying so in its own voice.
bool trash_available(std::string_view utf8_path) noexcept;

// As above, and on false sets `ec` to the reason: cross_device when the item's
// filesystem offers no usable trash, unsupported when this build has no
// backend, not_found when `utf8_path` does not exist, invalid_argument for a
// malformed path. Only the boolean is dependable across platforms; the reason is
// advisory, as for trash().
bool trash_available(std::string_view utf8_path, std::error_code& ec) noexcept;

} // namespace libtrash

template <>
struct std::is_error_code_enum<libtrash::errc> : std::true_type
{
};

#endif // LIBTRASH_TRASH_HPP

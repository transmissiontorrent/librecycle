// SPDX-License-Identifier: MIT
// This file Copyright © Mnemosaic LLC.
//
// Internal Linux/*BSD-only helpers (NOT part of the public API). Kept in a
// header so they can be unit-tested directly.

#ifndef LIBTRASH_TRASH_NIX_DETAIL_HPP
#define LIBTRASH_TRASH_NIX_DETAIL_HPP

#include <cerrno>
#include <filesystem>
#include <string>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace libtrash
{
namespace detail
{

// Create `path` as a private directory (mode, default 0700) owned by `uid`, or,
// if it already exists, verify it is a real directory (NOT a symlink) owned by
// `uid`. Never follows a symlink at `path`.
//
// This guards the top-directory trash ($topdir/.Trash-$uid and
// $topdir/.Trash/$uid) on a writable shared mount: a co-user who pre-plants a
// symlink (or a directory they own) there to redirect our trashed files is
// rejected, because mkdir fails with EEXIST and the follow-up lstat sees the
// symlink / foreign owner. Returns true only when we can safely use `path`.
inline bool make_or_verify_owned_dir(std::string const& path, uid_t uid, mode_t mode = 0700) noexcept
{
    if (::mkdir(path.c_str(), mode) == 0)
    {
        return true; // freshly created by us
    }
    if (errno != EEXIST)
    {
        return false;
    }
    struct stat st = {};
    if (::lstat(path.c_str(), &st) != 0)
    {
        return false;
    }
    return S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode) && st.st_uid == uid;
}

// Whether `path` is a directory, or is absent and could be created as one.
// Creates nothing: this is what trash_available() uses where trash() would
// mkdir -p.
//
// "Could be created" means the nearest existing ancestor is a directory we may
// write into and search, which is what mkdir needs. A prediction, not a
// promise: permissions can change, and an ACL or a read-only remount can still
// refuse the mkdir afterwards.
inline bool dir_exists_or_creatable(std::string const& path) noexcept
{
    struct stat st = {};
    if (::stat(path.c_str(), &st) == 0)
    {
        return S_ISDIR(st.st_mode);
    }

    // Walk up to the nearest existing ancestor: one mkdir -p would create
    // everything below it, so that ancestor's permissions decide the answer.
    std::filesystem::path cur(path);
    for (;;)
    {
        std::filesystem::path const parent = cur.parent_path();
        if (parent.empty() || parent == cur)
        {
            return false; // ran out of path without finding anything that exists
        }
        cur = parent;
        struct stat pst = {};
        if (::stat(cur.c_str(), &pst) == 0)
        {
            return S_ISDIR(pst.st_mode) && ::access(cur.c_str(), W_OK | X_OK) == 0;
        }
    }
}

// Whether make_or_verify_owned_dir() would succeed, creating nothing.
inline bool owned_dir_exists_or_creatable(std::string const& path, uid_t uid) noexcept
{
    struct stat st = {};
    if (::lstat(path.c_str(), &st) == 0)
    {
        // lstat + S_ISDIR already rejects the planted symlink that
        // make_or_verify_owned_dir() guards against.
        return S_ISDIR(st.st_mode) && st.st_uid == uid;
    }
    if (errno != ENOENT)
    {
        return false;
    }
    std::filesystem::path const parent = std::filesystem::path(path).parent_path();
    return !parent.empty() && ::access(parent.c_str(), W_OK | X_OK) == 0;
}

} // namespace detail
} // namespace libtrash

#endif // LIBTRASH_TRASH_NIX_DETAIL_HPP

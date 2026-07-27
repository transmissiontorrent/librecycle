// SPDX-License-Identifier: MIT
// This file Copyright © Mnemosaic LLC.
//
// macOS backend: -[NSFileManager trashItemAtURL:resultingItemURL:error:].
//
// Compiled with ARC (see CMakeLists.txt); Foundation objects are autoreleased,
// so there is nothing to release by hand.
//
// NOTE: not compiled or exercised on the development host (Linux); reviewed
// against the Foundation API docs.

#include "libtrash/trash.hpp"

#if defined(__APPLE__)

#include "trash_detail.hpp"

#import <Foundation/Foundation.h>

#include <filesystem>
#include <string_view>
#include <system_error>

#include <sys/stat.h>

namespace libtrash
{

bool trash(std::string_view path, std::error_code& ec) noexcept
{
    ec.clear();

    // Validate + resolve to an absolute path using the same rule as every other
    // backend (relative paths, '.'/'..', UTF-8 validation, symlink handling).
    std::filesystem::path resolved;
    if (!detail::resolve_input(path, resolved, ec))
    {
        return false;
    }

    @autoreleasepool
    {
        NSString* ns_path = [NSString stringWithUTF8String:resolved.c_str()];
        if (ns_path == nil)
        {
            ec = make_error_code(errc::invalid_argument);
            return false;
        }

        NSURL* url = [NSURL fileURLWithPath:ns_path];
        NSURL* resulting = nil;
        NSError* error = nil;
        BOOL const ok = [[NSFileManager defaultManager] trashItemAtURL:url
                                                      resultingItemURL:&resulting
                                                                 error:&error];
        if (ok)
        {
            return true;
        }

        if (error != nil && [error.domain isEqualToString:NSCocoaErrorDomain])
        {
            if (error.code == NSFileNoSuchFileError || error.code == NSFileReadNoSuchFileError)
            {
                ec = make_error_code(errc::not_found);
                return false;
            }
            if (error.code == NSFileWriteNoPermissionError)
            {
                ec = make_error_code(errc::permission_denied);
                return false;
            }
        }
        ec = make_error_code(errc::platform_error);
        return false;
    }
}

bool trash_available(std::string_view path, std::error_code& ec) noexcept
{
    ec.clear();

    std::filesystem::path resolved;
    if (!detail::resolve_input(path, resolved, ec))
    {
        return false;
    }

    // trash() reports a missing item as not_found; keep the probe's contract
    // identical instead of letting Foundation answer about a path that is not
    // there. lstat, so a dangling symlink counts as present -- trash() can move
    // one.
    struct stat st = {};
    if (::lstat(resolved.c_str(), &st) != 0)
    {
        ec = make_error_code(errc::not_found);
        return false;
    }

    @autoreleasepool
    {
        NSString* ns_path = [NSString stringWithUTF8String:resolved.c_str()];
        if (ns_path == nil)
        {
            ec = make_error_code(errc::invalid_argument);
            return false;
        }

        // appropriateForURL: asks for the trash serving *this item's* volume,
        // which is the per-volume question trash_available() answers; create:NO
        // keeps the probe non-mutating. A volume with no trash has no such
        // directory to hand back, so the call fails.
        NSError* error = nil;
        NSURL* trash_dir = [[NSFileManager defaultManager] URLForDirectory:NSTrashDirectory
                                                                 inDomain:NSUserDomainMask
                                                        appropriateForURL:[NSURL fileURLWithPath:ns_path]
                                                                   create:NO
                                                                    error:&error];
        if (trash_dir != nil)
        {
            return true;
        }

        if (error != nil && [error.domain isEqualToString:NSCocoaErrorDomain]
            && error.code == NSFileWriteNoPermissionError)
        {
            ec = make_error_code(errc::permission_denied);
            return false;
        }

        // Anything else means we could not obtain a trash for this volume.
        // Which Cocoa code says "this volume has no trash" is not contractual,
        // so the false return is the dependable part and cross_device is a
        // best-effort reason.
        ec = make_error_code(errc::cross_device);
        return false;
    }
}

} // namespace libtrash

#endif // __APPLE__

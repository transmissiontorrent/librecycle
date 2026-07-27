// SPDX-License-Identifier: MIT
// This file Copyright © Mnemosaic LLC.
//
// Fallback backend for platforms with no native implementation. It provides the
// definition of trash() so the library still links everywhere; the call simply
// reports errc::unsupported. Compiled unconditionally, but only emits a
// definition when no real backend claims the platform.

#include "libtrash/trash.hpp"

#if !defined(_WIN32) && !defined(__APPLE__) && !defined(__linux__) && !defined(__FreeBSD__) \
    && !defined(__NetBSD__) && !defined(__OpenBSD__)

#include "trash_detail.hpp"

#include <string_view>
#include <system_error>

namespace libtrash
{

namespace
{

// A malformed path is rejected as such even here, so that argument checking is
// the one thing callers can rely on from every platform.
bool validate_only(std::string_view path, std::error_code& ec)
{
    ec.clear();
    if (path.empty() || path.find('\0') != std::string_view::npos || !detail::is_valid_utf8(path))
    {
        ec = make_error_code(errc::invalid_argument);
        return false;
    }
    ec = make_error_code(errc::unsupported);
    return false;
}

} // namespace

bool trash(std::string_view path, std::error_code& ec) noexcept
{
    return validate_only(path, ec);
}

bool trash_available(std::string_view path, std::error_code& ec) noexcept
{
    return validate_only(path, ec);
}

} // namespace libtrash

#endif // no native backend

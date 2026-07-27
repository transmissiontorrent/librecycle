// SPDX-License-Identifier: MIT
// This file Copyright © Mnemosaic LLC.
//
// Platform-agnostic pieces: the trash error_category, the make_error_code
// customization point, and the two overloads that are defined in terms of the
// per-platform ones.

#include "libtrash/trash.hpp"

#include <filesystem>
#include <string>

namespace libtrash
{
namespace
{

class category_impl final : public std::error_category
{
public:
    char const* name() const noexcept override
    {
        return "trash";
    }

    std::string message(int ev) const override
    {
        switch (static_cast<errc>(ev))
        {
        case errc::invalid_argument:
            return "invalid argument";
        case errc::not_found:
            return "file or directory not found";
        case errc::permission_denied:
            return "permission denied";
        case errc::cross_device:
            return "no usable trash directory on the item's filesystem";
        case errc::platform_error:
            return "platform error";
        case errc::unsupported:
            return "operation not supported on this platform";
        }
        return "unknown trash error";
    }
};

} // namespace

std::error_category const& error_category() noexcept
{
    static category_impl const instance;
    return instance;
}

std::error_code make_error_code(errc e) noexcept
{
    return { static_cast<int>(e), error_category() };
}

void trash(std::string_view utf8_path)
{
    std::error_code ec;
    if (!trash(utf8_path, ec))
    {
        throw std::filesystem::filesystem_error("libtrash::trash", std::filesystem::path(utf8_path), ec);
    }
}

bool trash_available(std::string_view utf8_path) noexcept
{
    std::error_code ec;
    return trash_available(utf8_path, ec);
}

} // namespace libtrash

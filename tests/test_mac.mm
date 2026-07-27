// SPDX-License-Identifier: MIT
// This file Copyright © Mnemosaic LLC.
//
// macOS smoke test. Trashes a temp file on the home volume and asserts the
// source is gone. We avoid asserting an exact location in ~/.Trash to keep the
// test robust.

#include "libtrash/trash.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

int main()
{
    int failures = 0;
    auto check = [&](bool cond, char const* what) {
        std::fprintf(stderr, "%s - %s\n", cond ? "ok  " : "FAIL", what);
        if (!cond)
        {
            ++failures;
        }
    };

    fs::path const victim = fs::temp_directory_path() / "libtrash_mac_smoke.txt";
    {
        std::ofstream(victim) << "data";
    }

    // Ask before acting: the temp directory is on a volume with a trash, so the
    // probe must say yes and then be borne out by the trash() below.
    std::error_code avail;
    check(libtrash::trash_available(victim.string(), avail), "trash_available says yes for a temp file");
    check(!avail, "trash_available leaves ec clear when the answer is yes");

    std::error_code avail_missing;
    check(
        !libtrash::trash_available((fs::temp_directory_path() / "nope-xyz").string(), avail_missing)
            && avail_missing == libtrash::errc::not_found,
        "trash_available: missing path -> errc::not_found");

    check(libtrash::trash_available(victim.string()), "trash_available(path): yes needs no error_code");
    check(
        !libtrash::trash_available((fs::temp_directory_path() / "nope-xyz").string()),
        "trash_available(path): a missing path is a no, not a throw");

    std::error_code ec;
    bool const ok = libtrash::trash(victim.string(), ec);
    check(ok && !ec, "trash returns success");
    check(!fs::exists(victim), "original file is gone");

    std::error_code missing;
    check(!libtrash::trash((fs::temp_directory_path() / "nope-xyz").string(), missing), "missing path fails");
    check(missing == libtrash::errc::not_found, "missing path -> errc::not_found");

    std::error_code empty;
    check(
        !libtrash::trash("", empty) && empty == libtrash::errc::invalid_argument,
        "empty path -> errc::invalid_argument");

    return failures == 0 ? 0 : 1;
}

// SPDX-License-Identifier: MIT
// This file Copyright © Mnemosaic LLC.
//
// Linux / *BSD backend: FreeDesktop.org Trash specification v1.0.
// https://specifications.freedesktop.org/trash-spec/trashspec-1.0.html
//
// Correctness reference: arsenetar/send2trash (plat_other.py) and
// andreafrancia/trash-cli.

#include "libtrash/trash.hpp"

#if defined(__linux__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)

#include "trash_detail.hpp"
#include "trash_nix_detail.hpp"

#include <cerrno>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace libtrash
{
namespace
{

std::string get_home()
{
    if (char const* h = std::getenv("HOME"); h != nullptr && *h != '\0')
    {
        return h;
    }
    // getpwuid_r is the reentrant form; getpwuid() returns a pointer into a
    // shared static buffer that another thread could overwrite mid-use.
    long want = ::sysconf(_SC_GETPW_R_SIZE_MAX);
    std::string buf(want > 0 ? static_cast<std::size_t>(want) : std::size_t{ 16384 }, '\0');
    passwd pw = {};
    passwd* result = nullptr;
    if (::getpwuid_r(::getuid(), &pw, buf.data(), buf.size(), &result) == 0 && result != nullptr
        && result->pw_dir != nullptr)
    {
        return result->pw_dir;
    }
    return {};
}

// $XDG_DATA_HOME if set to an absolute path, else ~/.local/share. Empty on failure.
std::string xdg_data_home()
{
    if (char const* x = std::getenv("XDG_DATA_HOME"); x != nullptr && x[0] == '/')
    {
        return x;
    }
    std::string const home = get_home();
    if (home.empty())
    {
        return {};
    }
    return home + "/.local/share";
}

bool lstat_dev(std::string const& path, dev_t& out_dev)
{
    struct stat st = {};
    if (::lstat(path.c_str(), &st) != 0)
    {
        return false;
    }
    out_dev = st.st_dev;
    return true;
}

// Device of the nearest existing ancestor of `path` (used for not-yet-created
// trash directories).
bool nearest_existing_dev(std::string path, dev_t& out_dev)
{
    while (!path.empty())
    {
        struct stat st = {};
        if (::stat(path.c_str(), &st) == 0)
        {
            out_dev = st.st_dev;
            return true;
        }
        auto parent = fs::path(path).parent_path();
        if (parent.empty() || parent == fs::path(path))
        {
            break;
        }
        path = parent.string();
    }
    return false;
}

// The mount point containing `abs_path`: walk up while the parent stays on the
// same device.
std::string find_mount_point(std::string const& abs_path)
{
    dev_t dev = 0;
    if (!lstat_dev(abs_path, dev))
    {
        return {};
    }
    fs::path cur(abs_path);
    for (;;)
    {
        fs::path parent = cur.parent_path();
        if (parent.empty() || parent == cur)
        {
            break; // reached "/"
        }
        dev_t pdev = 0;
        if (!lstat_dev(parent.string(), pdev) || pdev != dev)
        {
            break; // parent is on another device -> cur is the mount point
        }
        cur = parent;
    }
    return cur.string();
}

// mkdir -p with the given mode applied to directories we create.
bool ensure_dir(std::string const& path, mode_t mode)
{
    struct stat st = {};
    if (::stat(path.c_str(), &st) == 0)
    {
        return S_ISDIR(st.st_mode);
    }
    fs::path const p(path);
    fs::path const parent = p.parent_path();
    if (!parent.empty() && parent != p && !ensure_dir(parent.string(), mode))
    {
        return false;
    }
    return ::mkdir(path.c_str(), mode) == 0 || errno == EEXIST;
}

// A shared "administrator" trash ($topdir/.Trash) is only safe if it is a real
// directory (not a symlink) with the sticky bit set. This guards against a
// hostile user planting a symlink on a shared mount.
bool is_safe_admin_trash(std::string const& path)
{
    struct stat st = {};
    if (::lstat(path.c_str(), &st) != 0)
    {
        return false;
    }
    return S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode) && (st.st_mode & S_ISVTX) != 0;
}

bool is_unreserved(unsigned char c)
{
    // Matches Python's urllib.parse.quote default never-quote set, which
    // send2trash uses with safe='/': ALWAYS_SAFE + '/'. Uses a fixed ASCII test
    // rather than std::isalnum so encoding never depends on the process locale
    // (under a single-byte locale isalnum() would leave UTF-8 high bytes raw).
    bool const alnum = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    return alnum || c == '_' || c == '.' || c == '-' || c == '~';
}

std::string percent_encode(std::string_view s)
{
    static char const* const kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (unsigned char const c : s)
    {
        if (is_unreserved(c) || c == '/')
        {
            out.push_back(static_cast<char>(c));
        }
        else
        {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

std::string deletion_date_now()
{
    std::time_t const t = std::time(nullptr);
    struct tm tm = {};
    if (::localtime_r(&t, &tm) == nullptr)
    {
        return {}; // record an empty DeletionDate rather than a bogus 0000-00-00 one
    }
    char buf[32] = {};
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm); // local time, no TZ
    return buf;
}

bool path_exists(std::string const& p)
{
    struct stat st = {};
    return ::lstat(p.c_str(), &st) == 0;
}

struct TrashName
{
    std::string files_path;
    std::string info_path;
};

// The Nth collision-candidate name: base, then base_2, base_3, ... with the
// counter inserted before the extension (foo_2.txt, not foo.txt_2).
std::string nth_name(std::string const& base, int counter)
{
    if (counter <= 1)
    {
        return base;
    }
    auto const dot = base.rfind('.');
    if (dot != std::string::npos && dot != 0)
    {
        return base.substr(0, dot) + "_" + std::to_string(counter) + base.substr(dot);
    }
    return base + "_" + std::to_string(counter);
}

// Write all of `content` to fd, retrying short writes and EINTR.
bool write_all(int fd, std::string const& content)
{
    char const* data = content.data();
    auto remaining = static_cast<ssize_t>(content.size());
    while (remaining > 0)
    {
        ssize_t const w = ::write(fd, data, static_cast<size_t>(remaining));
        if (w < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return false;
        }
        data += w;
        remaining -= w;
    }
    return true;
}

// Map a POSIX errno from a filesystem operation to a libtrash::errc.
errc errc_from_errno(int e)
{
    if (e == EACCES || e == EPERM || e == EROFS)
    {
        return errc::permission_denied;
    }
    return errc::platform_error;
}

// Claim a name free in both files/ and info/, writing info/NAME.trashinfo and
// claiming the slot atomically with O_EXCL. If a concurrent trasher wins the
// race for a name (EEXIST), advance the collision counter and retry rather than
// failing. On success fills `out`; on failure sets ec.
bool claim_and_write_info(
    std::string const& files_dir,
    std::string const& info_dir,
    std::string const& base,
    std::string const& encoded_path,
    std::string const& date,
    TrashName& out,
    std::error_code& ec)
{
    std::string const content = "[Trash Info]\nPath=" + encoded_path + "\nDeletionDate=" + date + "\n";
    for (int counter = 1; counter <= 100000; ++counter)
    {
        std::string const name = nth_name(base, counter);
        TrashName const cand{ files_dir + "/" + name, info_dir + "/" + name + ".trashinfo" };
        if (path_exists(cand.files_path))
        {
            continue; // files/ slot already taken
        }
        int const fd = ::open(cand.info_path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0)
        {
            if (errno == EEXIST)
            {
                continue; // lost the race for this name -> try the next counter
            }
            ec = make_error_code(errc_from_errno(errno));
            return false;
        }
        bool const wrote = write_all(fd, content);
        int const werr = errno;
        // close() can surface a deferred write-back error (e.g. ENOSPC on NFS).
        bool const closed = ::close(fd) == 0;
        if (!wrote || !closed)
        {
            ::unlink(cand.info_path.c_str()); // don't leave a truncated .trashinfo
            ec = make_error_code(wrote ? errc::platform_error : errc_from_errno(werr));
            return false;
        }
        out = cand;
        return true;
    }
    ec = make_error_code(errc::platform_error);
    return false;
}

enum class Mode
{
    Create, // create the trash directories as needed -- trash()
    Probe, // predict whether they could be created, touching nothing -- trash_available()
};

struct TrashDirs
{
    std::string files_dir;
    std::string info_dir;
    std::string original_for_info; // absolute (home trash) or topdir-relative
};

bool dir_usable(std::string const& path, Mode mode)
{
    return mode == Mode::Probe ? detail::dir_exists_or_creatable(path) : ensure_dir(path, 0700);
}

bool owned_dir_usable(std::string const& path, uid_t uid, Mode mode)
{
    return mode == Mode::Probe ? detail::owned_dir_exists_or_creatable(path, uid) :
                                 detail::make_or_verify_owned_dir(path, uid);
}

// Pick the trash directories for `resolved`, whose filesystem is `file_dev`.
// Both entry points share this so a probe cannot answer by a different rule
// than the move it predicts. On failure sets `ec`.
bool select_trash_dirs(fs::path const& resolved, dev_t file_dev, Mode mode, TrashDirs& out, std::error_code& ec)
{
    std::string const abs_path = resolved.string();

    std::string home_trash = xdg_data_home();
    if (home_trash.empty())
    {
        ec = make_error_code(errc::platform_error);
        return false;
    }
    home_trash += "/Trash";

    dev_t home_dev = 0;
    bool const have_home_dev = nearest_existing_dev(home_trash, home_dev);

    // Prefer the home trash when the item is on the home filesystem. If its
    // directories cannot be created (e.g. a read-only or over-quota home),
    // fall back to the top-directory trash on the item's own filesystem.
    if (have_home_dev && file_dev == home_dev)
    {
        std::string files_dir = home_trash + "/files";
        std::string info_dir = home_trash + "/info";
        if (dir_usable(files_dir, mode) && dir_usable(info_dir, mode))
        {
            out = { std::move(files_dir), std::move(info_dir), abs_path };
            return true;
        }
    }

    std::string const topdir = find_mount_point(abs_path);
    if (topdir.empty())
    {
        ec = make_error_code(errc::cross_device);
        return false;
    }
    uid_t const uid = ::getuid();
    std::string const uid_s = std::to_string(static_cast<unsigned long>(uid));

    // Select the per-user trash directory, refusing to follow a symlink or to
    // reuse a directory owned by someone else (a co-user's hijack attempt on a
    // shared mount). Prefer the sticky admin trash $topdir/.Trash/$uid;
    // otherwise fall back to $topdir/.Trash-$uid.
    std::string const admin = topdir + "/.Trash";
    std::string const admin_dir = admin + "/" + uid_s;
    std::string const fallback_dir = topdir + "/.Trash-" + uid_s;
    std::string trash_dir;
    if (is_safe_admin_trash(admin) && owned_dir_usable(admin_dir, uid, mode))
    {
        trash_dir = admin_dir;
    }
    else if (owned_dir_usable(fallback_dir, uid, mode))
    {
        trash_dir = fallback_dir;
    }
    else
    {
        ec = make_error_code(errc::permission_denied);
        return false;
    }

    std::string files_dir = trash_dir + "/files";
    std::string info_dir = trash_dir + "/info";
    if (!dir_usable(files_dir, mode) || !dir_usable(info_dir, mode))
    {
        ec = make_error_code(errc::permission_denied);
        return false;
    }

    // For topdir trashes the recorded path is relative to the top directory.
    out = { std::move(files_dir), std::move(info_dir), resolved.lexically_relative(topdir).string() };
    return true;
}

// Validate `path_sv` and stat the item, yielding the resolved path and the
// filesystem it lives on -- the inputs select_trash_dirs() needs.
bool resolve_and_stat(std::string_view path_sv, fs::path& resolved, dev_t& file_dev, std::error_code& ec)
{
    if (!detail::resolve_input(path_sv, resolved, ec))
    {
        return false;
    }
    if (!lstat_dev(resolved.string(), file_dev))
    {
        ec = make_error_code(errc::not_found);
        return false;
    }
    return true;
}

} // namespace

bool trash(std::string_view path_sv, std::error_code& ec) noexcept
{
    ec.clear();
    try
    {
        fs::path resolved;
        dev_t file_dev = 0;
        if (!resolve_and_stat(path_sv, resolved, file_dev, ec))
        {
            return false;
        }
        std::string const abs_path = resolved.string();

        TrashDirs dirs;
        if (!select_trash_dirs(resolved, file_dev, Mode::Create, dirs, ec))
        {
            return false;
        }

        std::string base = resolved.filename().string();
        if (base.empty())
        {
            base = "file";
        }
        TrashName target;
        if (!claim_and_write_info(
                dirs.files_dir,
                dirs.info_dir,
                base,
                percent_encode(dirs.original_for_info),
                deletion_date_now(),
                target,
                ec))
        {
            return false;
        }

        if (::rename(abs_path.c_str(), target.files_path.c_str()) != 0)
        {
            int const e = errno;
            ::unlink(target.info_path.c_str()); // release the claimed slot
            ec = make_error_code(e == EXDEV ? errc::cross_device : errc_from_errno(e));
            return false;
        }

        return true;
    }
    catch (...)
    {
        ec = make_error_code(errc::platform_error);
        return false;
    }
}

bool trash_available(std::string_view path_sv, std::error_code& ec) noexcept
{
    ec.clear();
    try
    {
        fs::path resolved;
        dev_t file_dev = 0;
        if (!resolve_and_stat(path_sv, resolved, file_dev, ec))
        {
            return false;
        }

        TrashDirs dirs;
        return select_trash_dirs(resolved, file_dev, Mode::Probe, dirs, ec);
    }
    catch (...)
    {
        ec = make_error_code(errc::platform_error);
        return false;
    }
}

} // namespace libtrash

#endif // platform

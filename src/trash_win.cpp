// SPDX-License-Identifier: MIT
// This file Copyright © Mnemosaic LLC.
//
// Windows backend: IFileOperation with FOFX_RECYCLEONDELETE.
//
// NOTE: not compiled or exercised on the development host (Linux); reviewed
// against the Windows Shell API docs and robertguetzkow/libtrashcan.

#include "libtrash/trash.hpp"

#if defined(_WIN32)

#include "trash_detail.hpp"

// clang-format off
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <shobjidl_core.h>
// clang-format on

#include <filesystem>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>

namespace libtrash
{

bool trash(std::string_view path, std::error_code& ec) noexcept
{
    ec.clear();
    try
    {
        // Validate + resolve to an absolute native path (this also handles
        // relative paths and forward-slash separators, which the shell parsing
        // API below does not accept on its own).
        std::filesystem::path resolved;
        if (!detail::resolve_input(path, resolved, ec))
        {
            return false;
        }

        HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        // RPC_E_CHANGED_MODE: COM is already initialized on this thread with another
        // model. That is fine; we just must not call CoUninitialize ourselves.
        bool const need_uninit = SUCCEEDED(hr);
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
        {
            ec = make_error_code(errc::platform_error);
            return false;
        }

        auto const fail = [&](errc code) {
            ec = make_error_code(code);
            if (need_uninit)
            {
                ::CoUninitialize();
            }
            return false;
        };

        IFileOperation* op = nullptr;
        hr = ::CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op));
        if (FAILED(hr) || op == nullptr)
        {
            return fail(errc::platform_error);
        }

        // FOFX_RECYCLEONDELETE recycles instead of permanently deleting.
        // FOF_WANTNUKEWARNING is critical: without it, an item that cannot be
        // recycled (too large for the bin, recycling disabled, or a
        // network/removable drive) would be silently *permanently deleted*.
        // With it, such an item is refused instead, which we detect via
        // GetAnyOperationsAborted below and report as a failure -- so trash()
        // never destroys data behind the caller's back.
        hr = op->SetOperationFlags(
            FOFX_RECYCLEONDELETE | FOF_NO_UI | FOF_NOCONFIRMATION | FOF_SILENT | FOF_WANTNUKEWARNING);
        if (FAILED(hr))
        {
            op->Release();
            return fail(errc::platform_error);
        }

        IShellItem* item = nullptr;
        hr = ::SHCreateItemFromParsingName(resolved.c_str(), nullptr, IID_PPV_ARGS(&item));
        if (FAILED(hr) || item == nullptr)
        {
            op->Release();
            if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND))
            {
                return fail(errc::not_found);
            }
            return fail(errc::platform_error);
        }

        hr = op->DeleteItem(item, nullptr);
        item->Release();
        if (FAILED(hr))
        {
            op->Release();
            return fail(errc::platform_error);
        }

        hr = op->PerformOperations();
        if (FAILED(hr))
        {
            op->Release();
            if (hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED))
            {
                return fail(errc::permission_denied);
            }
            return fail(errc::platform_error);
        }

        BOOL aborted = FALSE;
        op->GetAnyOperationsAborted(&aborted);
        op->Release();
        if (aborted)
        {
            // The operation was refused -- e.g. the item could not be recycled
            // and FOF_WANTNUKEWARNING blocked the permanent delete. Report
            // failure rather than pretend it was trashed.
            return fail(errc::platform_error);
        }

        if (need_uninit)
        {
            ::CoUninitialize();
        }
        return true;
    }
    catch (...)
    {
        ec = make_error_code(errc::platform_error);
        return false;
    }
}

namespace
{

// A registry DWORD, or `fallback` when it is absent or another type.
DWORD reg_dword(HKEY root, wchar_t const* subkey, wchar_t const* value, DWORD fallback)
{
    DWORD data = 0;
    DWORD size = sizeof(data);
    if (::RegGetValueW(root, subkey, value, RRF_RT_REG_DWORD, nullptr, &data, &size) == ERROR_SUCCESS)
    {
        return data;
    }
    return fallback;
}

// The {GUID} under which the recycle-bin settings for `volume_root` are filed,
// or empty if it cannot be determined.
std::wstring volume_guid(wchar_t const* volume_root)
{
    wchar_t name[MAX_PATH] = {}; // "\\?\Volume{...}\" needs 50
    if (::GetVolumeNameForVolumeMountPointW(volume_root, name, static_cast<DWORD>(std::size(name))) == 0)
    {
        return {};
    }
    std::wstring const s(name);
    auto const open = s.find(L'{');
    auto const close = s.find(L'}', open == std::wstring::npos ? 0 : open);
    if (open == std::wstring::npos || close == std::wstring::npos)
    {
        return {};
    }
    return s.substr(open, close - open + 1);
}

// Whether recycling is switched off -- by policy for the whole machine or user,
// or by this volume's own "delete immediately" setting. Both live in Explorer's
// registry state rather than behind an API, so this is a heuristic: it is here
// to avoid promising a bin that Explorer will bypass.
bool recycling_disabled(wchar_t const* volume_root)
{
    // Group policy "Do not move deleted files to the Recycle Bin".
    wchar_t const* const policy = L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer";
    if (reg_dword(HKEY_CURRENT_USER, policy, L"NoRecycleFiles", 0) != 0
        || reg_dword(HKEY_LOCAL_MACHINE, policy, L"NoRecycleFiles", 0) != 0)
    {
        return true;
    }

    std::wstring const guid = volume_guid(volume_root);
    if (guid.empty())
    {
        return false; // unknown volume -> let SHQueryRecycleBin have the last word
    }
    std::wstring const key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\BitBucket\\Volume\\" + guid;
    return reg_dword(HKEY_CURRENT_USER, key.c_str(), L"NukeOnDelete", 0) != 0;
}

} // namespace

bool trash_available(std::string_view path, std::error_code& ec) noexcept
{
    ec.clear();
    try
    {
        std::filesystem::path resolved;
        if (!detail::resolve_input(path, resolved, ec))
        {
            return false;
        }

        // trash() reports a missing item as not_found; keep the probe's contract
        // identical instead of answering about the volume of a path that is not
        // there.
        if (::GetFileAttributesW(resolved.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            DWORD const err = ::GetLastError();
            bool const missing = err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND;
            ec = make_error_code(missing ? errc::not_found : errc::permission_denied);
            return false;
        }

        // The volume serving the item is what has (or lacks) a bin.
        // GetVolumePathName finds it even when the item sits under a mounted
        // folder, which taking the leading "C:\" would not.
        wchar_t root[MAX_PATH + 1] = {};
        if (::GetVolumePathNameW(resolved.c_str(), root, static_cast<DWORD>(std::size(root))) == 0)
        {
            ec = make_error_code(errc::platform_error);
            return false;
        }

        // Network shares and optical media have no recycle bin at all. Removable
        // drives are left to the two checks below, which know more than the
        // drive type does.
        switch (::GetDriveTypeW(root))
        {
        case DRIVE_REMOTE:
        case DRIVE_CDROM:
        case DRIVE_NO_ROOT_DIR:
        case DRIVE_UNKNOWN:
            ec = make_error_code(errc::cross_device);
            return false;
        default:
            break;
        }

        if (recycling_disabled(root))
        {
            ec = make_error_code(errc::cross_device);
            return false;
        }

        // Succeeds for a volume that has a bin; fails for one that does not.
        SHQUERYRBINFO info = {};
        info.cbSize = sizeof(info);
        if (FAILED(::SHQueryRecycleBinW(root, &info)))
        {
            ec = make_error_code(errc::cross_device);
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

} // namespace libtrash

#endif // _WIN32

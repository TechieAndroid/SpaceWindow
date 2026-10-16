// SpaceWindow - Windows Shell operations.
//
// Replaces parts of SpaceMonger.cpp and DriveDialog.cpp of SpaceMonger 1.4.0
// (Copyright (c) 1998-2020 Sean Werkema, MIT License).
#include "pch.h"
#include "ShellOps.h"

namespace sm::shell
{
    namespace
    {
        std::wstring ParentFolder(std::wstring const& path)
        {
            std::size_t const slash = path.find_last_of(L'\\', path.size() >= 2 ? path.size() - 2 : 0);
            return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash + 1);
        }

        // Keeps Windows from asking for a disk when a removable drive is empty.
        class QuietErrors
        {
        public:
            QuietErrors() noexcept { SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &previous_); }
            ~QuietErrors() { SetThreadErrorMode(previous_, nullptr); }
            QuietErrors(QuietErrors const&) = delete;
            QuietErrors& operator=(QuietErrors const&) = delete;

        private:
            DWORD previous_ = 0;
        };

        // COM for the shell calls on whatever thread we are on; balanced only if we initialized it.
        class ComScope
        {
        public:
            ComScope() noexcept : initialized_(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {}
            ~ComScope()
            {
                if (initialized_)
                {
                    CoUninitialize();
                }
            }
            ComScope(ComScope const&) = delete;
            ComScope& operator=(ComScope const&) = delete;

        private:
            bool initialized_;
        };
    }

    bool Open(HWND owner, std::wstring const& path)
    {
        std::wstring const folder = ParentFolder(path);
        SHELLEXECUTEINFOW info{};
        info.cbSize = sizeof(info);
        info.hwnd = owner;
        info.lpFile = path.c_str();
        info.lpDirectory = folder.empty() ? nullptr : folder.c_str();
        info.nShow = SW_SHOWNORMAL;
        return ShellExecuteExW(&info) != FALSE;
    }

    bool ShowProperties(HWND owner, std::wstring const& path)
    {
        SHELLEXECUTEINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = SEE_MASK_INVOKEIDLIST;
        info.hwnd = owner;
        info.lpVerb = L"properties";
        info.lpFile = path.c_str();
        info.nShow = SW_SHOW;
        return ShellExecuteExW(&info) != FALSE;
    }

    bool ShowInExplorer(std::wstring const& path)
    {
        PIDLIST_ABSOLUTE item = nullptr;
        if (FAILED(SHParseDisplayName(path.c_str(), nullptr, &item, 0, nullptr)))
        {
            return false;
        }
        auto const free = wil::scope_exit([item] { CoTaskMemFree(item); });
        return SUCCEEDED(SHOpenFolderAndSelectItems(item, 0, nullptr, 0));
    }

    DeleteResult Recycle(HWND owner, std::wstring const& path)
    {
        wil::com_ptr_nothrow<IFileOperation> operation;
        wil::com_ptr_nothrow<IShellItem> item;
        if (FAILED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&operation))) ||
            FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item))))
        {
            return DeleteResult::Failed;
        }

        // Recycle without asking again (we already asked), but let Windows warn
        // if the item is too big for the Recycle Bin and would be destroyed.
        DWORD const flags =
            FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_WANTNUKEWARNING | FOFX_RECYCLEONDELETE | FOFX_EARLYFAILURE;
        if (FAILED(operation->SetOwnerWindow(owner)) || FAILED(operation->SetOperationFlags(flags)) ||
            FAILED(operation->DeleteItem(item.get(), nullptr)))
        {
            return DeleteResult::Failed;
        }

        HRESULT const hr = operation->PerformOperations();
        BOOL aborted = FALSE;
        (void)operation->GetAnyOperationsAborted(&aborted);
        if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED) || hr == COPYENGINE_E_USER_CANCELLED || aborted)
        {
            return DeleteResult::Cancelled;
        }
        return SUCCEEDED(hr) ? DeleteResult::Deleted : DeleteResult::Failed;
    }

    std::vector<DriveInfo> EnumerateDrives(int iconSize)
    {
        ComScope const com;
        QuietErrors const quiet;
        std::vector<DriveInfo> drives;
        DWORD const mask = GetLogicalDrives();
        for (int letter = 0; letter < 26; ++letter)
        {
            if ((mask & (1u << letter)) == 0)
            {
                continue;
            }
            DriveInfo drive;
            drive.root = { static_cast<wchar_t>(L'A' + letter), L':', L'\\' };
            drive.type = GetDriveTypeW(drive.root.c_str());
            if (drive.type == DRIVE_UNKNOWN || drive.type == DRIVE_NO_ROOT_DIR)
            {
                continue;
            }

            ULARGE_INTEGER available{};
            ULARGE_INTEGER total{};
            ULARGE_INTEGER free{};
            drive.ready = GetDiskFreeSpaceExW(drive.root.c_str(), &available, &total, &free) != FALSE;
            if (drive.ready)
            {
                drive.totalBytes = total.QuadPart;
                drive.freeBytes = free.QuadPart;
            }

            SHFILEINFOW info{};
            if (SHGetFileInfoW(drive.root.c_str(), 0, &info, sizeof(info), SHGFI_DISPLAYNAME) != 0 &&
                info.szDisplayName[0] != L'\0')
            {
                drive.displayName = info.szDisplayName;
            }
            else
            {
                drive.displayName = drive.root;
            }
            drive.icon = GetIcon(drive.root, iconSize);
            drives.push_back(std::move(drive));
        }
        return drives;
    }

    IconPixels GetIcon(std::wstring const& path, int size)
    {
        ComScope const com;  // on a background thread; the UI thread has COM already
        IconPixels icon;
        wil::com_ptr_nothrow<IShellItemImageFactory> factory;
        if (size <= 0 || FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&factory))))
        {
            return icon;
        }
        wil::unique_hbitmap bitmap;
        if (FAILED(factory->GetImage(SIZE{ size, size }, SIIGBF_ICONONLY, bitmap.put())))
        {
            return icon;
        }

        DIBSECTION section{};
        if (GetObjectW(bitmap.get(), sizeof(section), &section) != sizeof(section) || section.dsBm.bmBitsPixel != 32 ||
            section.dsBm.bmBits == nullptr)
        {
            return icon;
        }

        icon.width = section.dsBm.bmWidth;
        icon.height = std::abs(section.dsBm.bmHeight);
        auto const stride = static_cast<std::size_t>(section.dsBm.bmWidthBytes);
        auto const rowBytes = static_cast<std::size_t>(icon.width) * 4;
        bool const bottomUp = section.dsBmih.biHeight > 0;
        auto const* const source = static_cast<std::uint8_t const*>(section.dsBm.bmBits);
        icon.bgra.resize(rowBytes * static_cast<std::size_t>(icon.height));
        for (int y = 0; y < icon.height; ++y)
        {
            int const sourceRow = bottomUp ? icon.height - 1 - y : y;
            std::memcpy(icon.bgra.data() + rowBytes * static_cast<std::size_t>(y),
                        source + stride * static_cast<std::size_t>(sourceRow), rowBytes);
        }

        // Some icons come without an alpha channel; treat those as opaque.
        bool hasAlpha = false;
        for (std::size_t i = 3; i < icon.bgra.size(); i += 4)
        {
            if (icon.bgra[i] != 0)
            {
                hasAlpha = true;
                break;
            }
        }
        if (!hasAlpha)
        {
            for (std::size_t i = 3; i < icon.bgra.size(); i += 4)
            {
                icon.bgra[i] = 0xFF;
            }
        }
        return icon;
    }

    std::optional<std::wstring> PickFolder(HWND owner)
    {
        wil::com_ptr_nothrow<IFileOpenDialog> dialog;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        {
            return std::nullopt;
        }
        FILEOPENDIALOGOPTIONS options = 0;
        (void)dialog->GetOptions(&options);
        (void)dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        if (FAILED(dialog->Show(owner)))
        {
            return std::nullopt;  // includes the user cancelling
        }

        wil::com_ptr_nothrow<IShellItem> item;
        wil::unique_cotaskmem_string path;
        if (FAILED(dialog->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        {
            return std::nullopt;
        }
        return std::wstring(path.get());
    }

    std::vector<std::wstring> ThemeFolders()
    {
        std::vector<std::wstring> folders;
        wil::unique_cotaskmem_string appData;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_DEFAULT, nullptr, appData.put())))
        {
            folders.push_back(std::wstring(appData.get()) + L"\\SpaceWindow\\Themes");
        }
        std::wstring program(MAX_PATH, L'\0');
        for (;;)
        {
            DWORD const length = GetModuleFileNameW(nullptr, program.data(), static_cast<DWORD>(program.size()));
            if (length == 0)
            {
                return folders;
            }
            if (length < program.size())
            {
                program.resize(length);
                break;
            }
            program.resize(program.size() * 2);  // a path longer than MAX_PATH
        }
        folders.push_back(program.substr(0, program.find_last_of(L'\\') + 1) + L"Themes");
        return folders;
    }

    bool OpenFolder(std::wstring const& path)
    {
        int const created = SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
        if (created != ERROR_SUCCESS && created != ERROR_ALREADY_EXISTS && created != ERROR_FILE_EXISTS)
        {
            return false;
        }
        return Open(nullptr, path);
    }
}

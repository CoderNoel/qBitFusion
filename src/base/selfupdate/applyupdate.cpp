/*
 * Bittorrent Client using Qt and libtorrent.
 * Copyright (C) 2026  qBitFusion contributors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * In addition, as a special exception, the copyright holders give permission to
 * link this program with the OpenSSL project's "OpenSSL" library (or with
 * modified versions of it that use the same license as the "OpenSSL" library),
 * and distribute the linked executables. You must obey the GNU General Public
 * License in all respects for all of the code used other than "OpenSSL".  If you
 * modify file(s), you may extend this exception to your version of the file(s),
 * but you are not obligated to do so. If you do not wish to do so, delete this
 * exception statement from your version.
 */

#include "applyupdate.h"

#include <QtSystemDetection>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <fstream>
#include <system_error>
#include <thread>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>
#endif

#include <QString>

#include "updatemanifest.h"

namespace fs = std::filesystem;

namespace
{
    const std::wstring NEW_SUFFIX = L".qbfnew";

    std::wstring lowercase(std::wstring text)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](const wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
        return text;
    }

    // Antivirus and indexers briefly open new files; retry instead of failing on the first try.
    template <typename Operation>
    bool retry(Operation operation, std::error_code &error)
    {
        for (int attempt = 0; attempt < 20; ++attempt)
        {
            error.clear();
            if (operation(error))
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
        return false;
    }

    std::wstring describe(const std::wstring &what, const fs::path &path, const std::error_code &error)
    {
        return what + L" " + path.wstring() + L": " + QString::fromStdString(error.message()).toStdWString();
    }
}

SelfUpdate::ApplyResult SelfUpdate::applyFiles(const fs::path &from, const fs::path &to, const fs::path &backup
        , const std::vector<std::wstring> &excluded)
{
    ApplyResult result;
    std::error_code error;

    std::vector<std::wstring> excludedLower;
    for (const std::wstring &name : excluded)
        excludedLower.push_back(lowercase(name));

    // ---- what to install
    std::vector<fs::path> files;
    for (auto it = fs::recursive_directory_iterator(from, error); !error && (it != fs::recursive_directory_iterator()); it.increment(error))
    {
        const fs::path relative = fs::relative(it->path(), from, error);
        if (error)
            break;
        const std::wstring top = lowercase(relative.begin()->wstring());
        if (std::find(excludedLower.cbegin(), excludedLower.cend(), top) != excludedLower.cend())
        {
            if (it->is_directory())
                it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file())
            files.push_back(relative);
    }
    if (error)
    {
        result.error = describe(L"Cannot read", from, error);
        return result;
    }
    if (files.empty())
    {
        result.error = L"The update contains no files.";
        return result;
    }

    // ---- phase 1: write every new file next to its target
    std::vector<fs::path> written;
    const auto discardWritten = [&to, &written]()
    {
        std::error_code ignored;
        for (const fs::path &relative : written)
            fs::remove(to / (relative.wstring() + NEW_SUFFIX), ignored);
    };
    for (const fs::path &relative : files)
    {
        const fs::path target = to / (relative.wstring() + NEW_SUFFIX);
        fs::create_directories(target.parent_path(), error);
        const bool copied = retry([&](std::error_code &e)
        {
            return fs::copy_file(from / relative, target, fs::copy_options::overwrite_existing, e);
        }, error);
        if (!copied)
        {
            result.error = describe(L"Cannot write", target, error);
            discardWritten();
            return result;
        }
        written.push_back(relative);
    }

    // ---- phase 2: move each old file to the backup and the new one into place
    fs::remove_all(backup, error);
    std::vector<std::pair<fs::path, bool>> swapped;   // relative path, whether an old file was backed up
    const auto rollback = [&]()
    {
        std::error_code ignored;
        for (auto it = swapped.rbegin(); it != swapped.rend(); ++it)
        {
            fs::remove(to / it->first, ignored);
            if (it->second)
                fs::rename(backup / it->first, to / it->first, ignored);
        }
        discardWritten();
    };
    for (const fs::path &relative : files)
    {
        const fs::path target = to / relative;
        const bool existed = fs::exists(target, error);
        if (existed)
        {
            fs::create_directories((backup / relative).parent_path(), error);
            if (!retry([&](std::error_code &e) { fs::rename(target, backup / relative, e); return !e; }, error))
            {
                result.error = describe(L"Cannot replace", target, error);
                rollback();
                return result;
            }
        }
        if (!retry([&](std::error_code &e) { fs::rename(to / (relative.wstring() + NEW_SUFFIX), target, e); return !e; }, error))
        {
            if (existed)
            {
                std::error_code ignored;
                fs::rename(backup / relative, target, ignored);
            }
            result.error = describe(L"Cannot install", target, error);
            rollback();
            return result;
        }
        swapped.emplace_back(relative, existed);
    }

    result.ok = true;
    result.filesReplaced = static_cast<int>(swapped.size());
    return result;
}

std::optional<int> SelfUpdate::runApplyModeIfRequested()
{
#ifdef Q_OS_WIN
    int argc = 0;
    LPWSTR *argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (!argv)
        return std::nullopt;
    const std::vector<std::wstring> args {argv, argv + argc};
    ::LocalFree(argv);

    // qbitfusion.exe --qbf-apply-update <pid> <install dir> -- <arguments to restart with>
    if ((args.size() < 4) || (args[1] != APPLY_SWITCH))
        return std::nullopt;

    const DWORD parentPid = static_cast<DWORD>(std::wcstoul(args[2].c_str(), nullptr, 10));
    const fs::path installDir = args[3];
    std::vector<std::wstring> passThrough;
    for (std::size_t i = 4; i < args.size(); ++i)
    {
        if ((i == 4) && (args[i] == L"--"))
            continue;
        passThrough.push_back(args[i]);
    }

    // Wait for the program being replaced to exit.
    if (HANDLE process = ::OpenProcess(SYNCHRONIZE, FALSE, parentPid))
    {
        ::WaitForSingleObject(process, 120'000);
        ::CloseHandle(process);
    }

    wchar_t ownPath[MAX_PATH * 4] {};
    ::GetModuleFileNameW(nullptr, ownPath, static_cast<DWORD>(std::size(ownPath)));
    const fs::path ownExe = ownPath;
    const fs::path updateDir = installDir / L"update";

    const ApplyResult result = applyFiles(ownExe.parent_path(), installDir, updateDir / L"previous", {L"profile", L"update"});

    {
        std::error_code ignored;
        fs::remove(updateDir / L"staged" / L"applying", ignored);
        std::ofstream report {updateDir / L"result.txt", std::ios::binary | std::ios::trunc};
        report << (result.ok ? "ok" : "failed") << '\n'
               << SelfUpdate::currentVersion().toString().toStdString() << '\n'
               << QString::fromStdWString(result.error).toUtf8().toStdString() << '\n';
    }

    // Start the installed program again, with the arguments it was started with.
    std::wstring commandLine = SelfUpdate::quoteWindowsArgument(QString::fromStdWString((installDir / ownExe.filename()).wstring())).toStdWString();
    for (const std::wstring &arg : passThrough)
        commandLine += L" " + SelfUpdate::quoteWindowsArgument(QString::fromStdWString(arg)).toStdWString();

    STARTUPINFOW startup {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info {};
    if (::CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, installDir.c_str(), &startup, &info))
    {
        ::CloseHandle(info.hThread);
        ::CloseHandle(info.hProcess);
    }
    return result.ok ? 0 : 1;
#else
    return std::nullopt;
#endif
}

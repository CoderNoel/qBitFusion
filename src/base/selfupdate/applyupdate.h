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

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// Installing a downloaded update. This runs in a second copy of the program, started from the staged
// update folder, after the running program has exited: Windows cannot replace an executable or DLL
// that is in use. It deliberately uses no Qt objects, because it runs before any are set up.

namespace SelfUpdate
{
    struct ApplyResult
    {
        bool ok = false;
        int filesReplaced = 0;
        std::wstring error;
    };

    // Copies every file under `from` into `to`, except top-level entries named in `excluded` (compared
    // case-insensitively). Each replaced file is kept in `backup`. Two phases: all new files are first
    // written next to their targets, then swapped in; if any step fails, everything already swapped is
    // put back, so `to` is left either fully updated or unchanged.
    ApplyResult applyFiles(const std::filesystem::path &from, const std::filesystem::path &to
            , const std::filesystem::path &backup, const std::vector<std::wstring> &excluded);

    // If this process was started to install an update (--qbf-apply-update), does that and returns the
    // process exit code. Otherwise returns nothing, and the program starts normally.
    std::optional<int> runApplyModeIfRequested();

    inline constexpr wchar_t APPLY_SWITCH[] = L"--qbf-apply-update";
}

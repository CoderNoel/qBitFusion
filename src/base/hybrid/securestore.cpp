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

#include "securestore.h"

#include <QHash>
#include <QMutex>

#ifdef Q_OS_WIN
#include <windows.h>
#include <wincred.h>
#endif

using namespace Qt::Literals::StringLiterals;

namespace
{
    QString targetName(const QString &key)
    {
        return u"qBitFusion/"_s + key;
    }

#ifndef Q_OS_WIN
    QMutex g_lock;
    QHash<QString, QByteArray> g_memory;
#endif
}

bool Hybrid::SecureStore::isPersistent()
{
#ifdef Q_OS_WIN
    return true;
#else
    return false;
#endif
}

bool Hybrid::SecureStore::write(const QString &key, const QByteArray &secret)
{
#ifdef Q_OS_WIN
    if (secret.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE)
        return false;
    const std::wstring target = targetName(key).toStdWString();
    CREDENTIALW cred {};
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = const_cast<LPWSTR>(target.c_str());
    cred.CredentialBlobSize = static_cast<DWORD>(secret.size());
    cred.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char *>(secret.constData()));
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
    static const std::wstring user = L"qBitFusion";
    cred.UserName = const_cast<LPWSTR>(user.c_str());
    return ::CredWriteW(&cred, 0) == TRUE;
#else
    const QMutexLocker locker {&g_lock};
    g_memory.insert(key, secret);
    return true;
#endif
}

std::optional<QByteArray> Hybrid::SecureStore::read(const QString &key)
{
#ifdef Q_OS_WIN
    const std::wstring target = targetName(key).toStdWString();
    PCREDENTIALW cred = nullptr;
    if (::CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &cred) != TRUE)
        return std::nullopt;
    QByteArray data {reinterpret_cast<const char *>(cred->CredentialBlob), static_cast<qsizetype>(cred->CredentialBlobSize)};
    ::SecureZeroMemory(cred->CredentialBlob, cred->CredentialBlobSize);
    ::CredFree(cred);
    return data;
#else
    const QMutexLocker locker {&g_lock};
    const auto it = g_memory.constFind(key);
    if (it == g_memory.cend())
        return std::nullopt;
    return it.value();
#endif
}

bool Hybrid::SecureStore::remove(const QString &key)
{
#ifdef Q_OS_WIN
    const std::wstring target = targetName(key).toStdWString();
    return (::CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0) == TRUE) || (::GetLastError() == ERROR_NOT_FOUND);
#else
    const QMutexLocker locker {&g_lock};
    g_memory.remove(key);
    return true;
#endif
}

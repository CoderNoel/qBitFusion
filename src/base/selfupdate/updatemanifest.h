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

#include <compare>

#include <QtTypes>
#include <QByteArray>
#include <QString>
#include <QStringList>

#include "base/3rdparty/expected.hpp"

// qBitFusion's own updates (qBittorrent's update checker stays disabled in this fork).
//
// A release publishes two small files next to its package: update.json, which names the version and
// the package's URL, size and SHA-256, and update.json.sig, an Ed25519 signature over the exact bytes
// of update.json. The program trusts a package only if the signature verifies against the public key
// built into it, the package's size and hash match, and the version is newer than its own.

namespace SelfUpdate
{
    // <upstream major>.<minor>.<bugfix>.<qBitFusion revision>, e.g. 5.3.0.2. Revision 0 is a
    // development build, which never updates itself.
    struct Version
    {
        int major = 0;
        int minor = 0;
        int bugfix = 0;
        int revision = 0;

        static Version fromString(const QString &text);   // invalid (all zero) if malformed
        bool isValid() const;
        QString toString() const;

        friend auto operator<=>(const Version &, const Version &) = default;
    };

    // The version this program was built as.
    Version currentVersion();
    // "5.3.0.2", or "5.3.0 (development build)" for revision 0.
    QString displayVersion();

    struct Package
    {
        QString url;
        qint64 size = 0;
        QByteArray sha256;   // raw 32 bytes
    };

    struct Manifest
    {
        Version version;
        QString upstreamVersion;
        QString notesUrl;
        Package package;
    };

    // Ed25519 over `message`. `signature` is base64 (whitespace ignored); `publicKey` is the raw 32 bytes.
    bool verifySignature(const QByteArray &message, const QByteArray &signature, const QByteArray &publicKey);

    // Parses update.json for one platform ("windows-x64"). The package URL must start with one of
    // `allowedUrlPrefixes`. Call only after verifySignature() succeeded on the same bytes.
    nonstd::expected<Manifest, QString> parseManifest(const QByteArray &json, const QString &platform
            , const QStringList &allowedUrlPrefixes);

    // Quotes one argument for a Windows command line so that CommandLineToArgvW returns it unchanged.
    QString quoteWindowsArgument(const QString &argument);
}

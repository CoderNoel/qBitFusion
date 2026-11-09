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

#include <QHostAddress>
#include <QString>
#include <QStringList>
#include <QUrl>

// SSRF and secret-leak controls for every provider/CDN URL (spec 07 section C, spec 03 section 7).

namespace Hybrid
{
    struct UrlPolicy
    {
        // DNS suffixes the provider is authorized to send data/API traffic to, e.g. "real-debrid.com".
        // Empty means "any public host" (still HTTPS + public IP only).
        QStringList allowedHostSuffixes;

        // Test builds only: permit http:// and loopback for the local fixture server.
        // Must never be enabled from user configuration.
        bool allowLoopbackForTesting = false;
    };

    enum class UrlCheck
    {
        Allowed,
        InvalidUrl,
        SchemeNotAllowed,
        CredentialsInUrl,
        HostNotAllowed,
        PrivateAddress,
        PortNotAllowed
    };

    UrlCheck checkUrl(const QUrl &url, const UrlPolicy &policy);

    // Checks a resolved address before connecting (defence against DNS names pointing inward).
    bool isPublicAddress(const QHostAddress &address);

    // A redirect is acceptable only if the target passes checkUrl and does not downgrade the scheme.
    UrlCheck checkRedirect(const QUrl &from, const QUrl &to, const UrlPolicy &policy);

    // Whether an Authorization header may be sent to this URL (API host only, never CDN or redirects).
    bool mayCarryCredentials(const QUrl &url, const QStringList &apiHosts);

    // "https://host/…" — strips path, query (signed tokens) and fragment. Safe for logs and UI.
    QString redactUrl(const QUrl &url);

    QString describe(UrlCheck check);
}

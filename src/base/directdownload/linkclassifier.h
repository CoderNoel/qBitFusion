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

#include <functional>

#include <QByteArray>
#include <QByteArrayView>
#include <QString>
#include <QUrl>

// Deciding what a pasted link is (a torrent, a magnet, or a plain file), naming the file it
// serves, and the redirect rules for direct downloads. Pure functions: no I/O.

namespace DirectDownload
{
    // What the user pasted, judged before any network access.
    enum class InputKind
    {
        Empty,
        Magnet,
        InfoHash,
        TorrentFileUrl,  // http(s) URL whose path ends in ".torrent": handled by the torrent path unchanged
        WebUrl,          // any other http(s) URL: a torrent or a file, only the response can tell
        Other            // a local path, a file:// URL or anything else: the torrent path decides
    };

    InputKind classifyInput(const QString &text);

    struct ResponseHead
    {
        int httpStatus = 0;
        QByteArray contentType;
        QByteArray contentDisposition;
        qint64 contentLength = -1;
    };

    enum class ContentKind
    {
        Torrent,
        File,
        WebPage
    };

    // Decides from the response headers and its first bytes; the content, not the extension, wins.
    ContentKind classifyContent(const ResponseHead &head, QByteArrayView firstBytes);

    // A bencoded dictionary that starts with a string key ("d8:announce…", "d4:info…").
    bool looksLikeTorrent(QByteArrayView firstBytes);
    bool looksLikeHtml(QByteArrayView firstBytes);

    // The MIME type without parameters, lower case ("Video/MP4; x=y" -> "video/mp4").
    QByteArray mimeTypeOf(const QByteArray &contentType);

    // ------------------------------------------------------------------------- file names

    // RFC 6266 / RFC 5987: prefers filename* (UTF-8 or ISO-8859-1), then filename. Empty if none.
    QString fileNameFromContentDisposition(const QByteArray &header);

    // The last path segment, decoded, only if it looks like a file name (has an extension). A bare
    // last segment is often a download token, which must not become a visible, persisted name.
    QString fileNameFromUrl(const QUrl &url);

    // Safe on Windows: no reserved characters or device names, no trailing dots or spaces, bounded length.
    QString sanitizeFileName(const QString &name);

    // Content-Disposition, then the final URL, then "download" with an extension for the MIME type.
    QString chooseFileName(const QByteArray &contentDisposition, const QUrl &finalUrl, const QByteArray &contentType);

    // "name.ext", "name (1).ext", "name (2).ext", … the first one for which isTaken is false.
    QString uniqueFileName(const QString &name, const std::function<bool (const QString &)> &isTaken);

    // ------------------------------------------------------------------------- link policy

    enum class LinkCheck
    {
        Allowed,
        InvalidUrl,
        SchemeNotAllowed,
        SchemeDowngrade,
        RedirectToLocalNetwork
    };

    // A pasted link: http or https to any host. The user chose it, so a local address is allowed.
    LinkCheck checkLink(const QUrl &url);

    // A redirect may not downgrade https to http, and may not lead from a public host into the
    // local network (a remote server must not be able to point the app at the user's LAN).
    LinkCheck checkRedirect(const QUrl &from, const QUrl &to);

    // Loopback, private, link-local, single-label and .local/.internal/.localhost names.
    bool isLocalNetworkHost(const QString &host);

    QString describe(LinkCheck check);
}

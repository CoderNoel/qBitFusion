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

#include "linkclassifier.h"

#include <algorithm>

#include <QCoreApplication>
#include <QDateTime>
#include <QHostAddress>
#include <QList>
#include <QMimeDatabase>
#include <QRegularExpression>

#include "base/hybrid/urlguard.h"

using namespace Qt::Literals::StringLiterals;

namespace
{
    constexpr qsizetype MAX_NAME_LENGTH = 180;
    constexpr qsizetype MAX_KEPT_SUFFIX = 16;

    bool isHex(const QString &str)
    {
        return std::ranges::all_of(str, [](const QChar c)
        {
            const QChar l = c.toLower();
            return ((l >= u'0') && (l <= u'9')) || ((l >= u'a') && (l <= u'f'));
        });
    }

    bool isBase32(const QString &str)
    {
        return std::ranges::all_of(str, [](const QChar c)
        {
            const QChar l = c.toLower();
            return ((l >= u'2') && (l <= u'7')) || ((l >= u'a') && (l <= u'z'));
        });
    }

    QByteArrayView skipBomAndSpace(QByteArrayView bytes)
    {
        if (bytes.startsWith("\xEF\xBB\xBF"))
            bytes = bytes.sliced(3);
        while (!bytes.isEmpty() && QChar::isSpace(static_cast<uchar>(bytes.front())))
            bytes = bytes.sliced(1);
        return bytes;
    }

    struct Parameter
    {
        QByteArray name;   // lower case
        QByteArray value;  // unquoted
    };

    // Parameters of a header such as Content-Disposition, after its first token. Tolerates the
    // common malformations: missing quotes, stray spaces and empty parameters.
    QList<Parameter> parseParameters(const QByteArray &header)
    {
        QList<Parameter> params;
        qsizetype i = header.indexOf(';');
        if (i < 0)
            return params;
        ++i;

        const qsizetype n = header.size();
        while (i < n)
        {
            while ((i < n) && ((header[i] == ' ') || (header[i] == '\t') || (header[i] == ';')))
                ++i;
            const qsizetype nameStart = i;
            while ((i < n) && (header[i] != '=') && (header[i] != ';'))
                ++i;
            const QByteArray name = header.mid(nameStart, i - nameStart).trimmed().toLower();
            if ((i >= n) || (header[i] != '='))
                continue;
            ++i;  // '='
            while ((i < n) && ((header[i] == ' ') || (header[i] == '\t')))
                ++i;

            QByteArray value;
            if ((i < n) && (header[i] == '"'))
            {
                ++i;
                while ((i < n) && (header[i] != '"'))
                {
                    if ((header[i] == '\\') && ((i + 1) < n))
                        ++i;
                    value += header[i];
                    ++i;
                }
                ++i;  // closing quote
                while ((i < n) && (header[i] != ';'))
                    ++i;
            }
            else
            {
                const qsizetype valueStart = i;
                while ((i < n) && (header[i] != ';'))
                    ++i;
                value = header.mid(valueStart, i - valueStart).trimmed();
            }

            if (!name.isEmpty())
                params.append({name, value});
        }
        return params;
    }

    QString decodeExtValue(const QByteArray &value)
    {
        // charset'language'percent-encoded
        const qsizetype first = value.indexOf('\'');
        const qsizetype second = (first < 0) ? -1 : value.indexOf('\'', first + 1);
        if (second < 0)
            return {};
        const QByteArray charset = value.left(first).trimmed().toLower();
        const QByteArray encoded = QByteArray::fromPercentEncoding(value.mid(second + 1));
        if (charset == "utf-8")
            return QString::fromUtf8(encoded);
        if ((charset == "iso-8859-1") || (charset == "us-ascii"))
            return QString::fromLatin1(encoded);
        return {};
    }

    QString decodePlainValue(const QByteArray &value)
    {
        // Many servers send raw UTF-8 here although the RFC says ISO-8859-1.
        const QString utf8 = QString::fromUtf8(value);
        if (!utf8.contains(QChar::ReplacementCharacter))
            return utf8;
        return QString::fromLatin1(value);
    }

    QString lastPathComponent(const QString &name)
    {
        const qsizetype slash = std::max(name.lastIndexOf(u'/'), name.lastIndexOf(u'\\'));
        return (slash < 0) ? name : name.mid(slash + 1);
    }

    bool isReservedDeviceName(const QString &name)
    {
        static const QRegularExpression re {u"^(CON|PRN|AUX|NUL|COM[0-9]|LPT[0-9])$"_s, QRegularExpression::CaseInsensitiveOption};
        const QString base = name.section(u'.', 0, 0).trimmed();
        return re.match(base).hasMatch();
    }

    std::pair<QString, QString> splitSuffix(const QString &name)
    {
        const qsizetype dot = name.lastIndexOf(u'.');
        if ((dot <= 0) || ((name.size() - dot - 1) > MAX_KEPT_SUFFIX))
            return {name, {}};
        return {name.left(dot), name.mid(dot)};
    }
}

DirectDownload::InputKind DirectDownload::classifyInput(const QString &text)
{
    const QString str = text.trimmed();
    if (str.isEmpty())
        return InputKind::Empty;
    if (str.startsWith(u"magnet:", Qt::CaseInsensitive))
        return InputKind::Magnet;
    if (((str.size() == 40) || (str.size() == 64)) && isHex(str))
        return InputKind::InfoHash;
    if ((str.size() == 32) && isBase32(str))
        return InputKind::InfoHash;

    if (!str.startsWith(u"http://", Qt::CaseInsensitive) && !str.startsWith(u"https://", Qt::CaseInsensitive))
        return InputKind::Other;

    const QUrl url {str};
    if (!url.isValid() || url.host().isEmpty())
        return InputKind::Other;
    if (url.path().endsWith(u".torrent"_s, Qt::CaseInsensitive))
        return InputKind::TorrentFileUrl;
    return InputKind::WebUrl;
}

QByteArray DirectDownload::mimeTypeOf(const QByteArray &contentType)
{
    const qsizetype semicolon = contentType.indexOf(';');
    return ((semicolon < 0) ? contentType : contentType.left(semicolon)).trimmed().toLower();
}

bool DirectDownload::looksLikeTorrent(const QByteArrayView firstBytes)
{
    // 'd', then a string key: 1-6 digits without a leading zero, and a colon.
    if ((firstBytes.size() < 4) || (firstBytes[0] != 'd') || (firstBytes[1] == '0'))
        return false;
    qsizetype i = 1;
    while ((i < firstBytes.size()) && (i <= 6) && (firstBytes[i] >= '0') && (firstBytes[i] <= '9'))
        ++i;
    return (i > 1) && (i < firstBytes.size()) && (firstBytes[i] == ':');
}

bool DirectDownload::looksLikeHtml(const QByteArrayView firstBytes)
{
    const QByteArray start = skipBomAndSpace(firstBytes).left(16).toByteArray().toLower();
    return start.startsWith("<!doctype html") || start.startsWith("<html") || start.startsWith("<head")
            || start.startsWith("<body");
}

DirectDownload::ContentKind DirectDownload::classifyContent(const ResponseHead &head, const QByteArrayView firstBytes)
{
    if (looksLikeTorrent(firstBytes))
        return ContentKind::Torrent;

    const QByteArray mime = mimeTypeOf(head.contentType);
    const bool isAttachment = head.contentDisposition.trimmed().toLower().startsWith("attachment");
    const bool isHtml = looksLikeHtml(firstBytes) || (mime == "text/html") || (mime == "application/xhtml+xml");

    if (isHtml)
        return isAttachment ? ContentKind::File : ContentKind::WebPage;
    if (mime == "application/x-bittorrent")
        return ContentKind::Torrent;  // the torrent parser reports it if the body is not one
    return ContentKind::File;
}

QString DirectDownload::fileNameFromContentDisposition(const QByteArray &header)
{
    QString plain;
    QString extended;
    for (const Parameter &param : parseParameters(header))
    {
        if ((param.name == "filename*") && extended.isEmpty())
            extended = decodeExtValue(param.value);
        else if ((param.name == "filename") && plain.isEmpty())
            plain = decodePlainValue(param.value);
    }
    return lastPathComponent(extended.isEmpty() ? plain : extended).trimmed();
}

QString DirectDownload::fileNameFromUrl(const QUrl &url)
{
    const QString name = url.fileName(QUrl::FullyDecoded).trimmed();
    static const QRegularExpression hasExtension {u"^.+\\.[A-Za-z0-9]{1,10}$"_s};
    if (!hasExtension.match(name).hasMatch())
        return {};
    return name;
}

QString DirectDownload::sanitizeFileName(const QString &name)
{
    QString result;
    const QString last = lastPathComponent(name);
    result.reserve(last.size());
    for (const QChar c : last)
    {
        const char16_t u = c.unicode();
        const bool reserved = (u < 0x20) || (u == 0x7F) || (u == u'<') || (u == u'>') || (u == u':') || (u == u'"')
                || (u == u'/') || (u == u'\\') || (u == u'|') || (u == u'?') || (u == u'*');
        result += reserved ? QChar(u'_') : c;
    }

    result = result.trimmed();
    while (result.endsWith(u'.') || result.endsWith(u' '))
        result.chop(1);
    if (result.isEmpty())
        return u"download"_s;

    if (isReservedDeviceName(result))
        result.prepend(u'_');

    if (result.size() > MAX_NAME_LENGTH)
    {
        const auto [base, suffix] = splitSuffix(result);
        QString shortened = base.left(MAX_NAME_LENGTH - suffix.size());
        // Never cut a surrogate pair in half.
        if (!shortened.isEmpty() && shortened.back().isHighSurrogate())
            shortened.chop(1);
        result = shortened.trimmed() + suffix;
    }
    return result;
}

QString DirectDownload::chooseFileName(const QByteArray &contentDisposition, const QUrl &finalUrl, const QByteArray &contentType)
{
    if (const QString fromHeader = fileNameFromContentDisposition(contentDisposition); !fromHeader.isEmpty())
        return sanitizeFileName(fromHeader);
    if (const QString fromUrl = fileNameFromUrl(finalUrl); !fromUrl.isEmpty())
        return sanitizeFileName(fromUrl);

    QString name = u"download"_s;
    const QByteArray mime = mimeTypeOf(contentType);
    if (!mime.isEmpty() && (mime != "application/octet-stream"))
    {
        const QMimeType type = QMimeDatabase().mimeTypeForName(QString::fromLatin1(mime));
        if (type.isValid() && !type.preferredSuffix().isEmpty())
            name += u'.' + type.preferredSuffix();
    }
    return name;
}

QString DirectDownload::uniqueFileName(const QString &name, const std::function<bool (const QString &)> &isTaken)
{
    if (!isTaken(name))
        return name;
    const auto [base, suffix] = splitSuffix(name);
    for (int i = 1; i < 10'000; ++i)
    {
        const QString candidate = u"%1 (%2)%3"_s.arg(base, QString::number(i), suffix);
        if (!isTaken(candidate))
            return candidate;
    }
    return u"%1 (%2)%3"_s.arg(base, QString::number(QDateTime::currentMSecsSinceEpoch()), suffix);
}

bool DirectDownload::isLocalNetworkHost(const QString &host)
{
    const QString h = host.toLower();
    if (h.isEmpty())
        return true;

    const QHostAddress literal {h};
    if (!literal.isNull())
        return !Hybrid::isPublicAddress(literal);

    if ((h == u"localhost") || !h.contains(u'.'))
        return true;
    for (const QString &suffix : {u".localhost"_s, u".local"_s, u".internal"_s, u".lan"_s, u".home.arpa"_s})
    {
        if (h.endsWith(suffix))
            return true;
    }
    return false;
}

DirectDownload::LinkCheck DirectDownload::checkLink(const QUrl &url)
{
    if (!url.isValid() || url.isRelative() || url.host().isEmpty())
        return LinkCheck::InvalidUrl;
    const QString scheme = url.scheme().toLower();
    if ((scheme != u"http") && (scheme != u"https"))
        return LinkCheck::SchemeNotAllowed;
    return LinkCheck::Allowed;
}

DirectDownload::LinkCheck DirectDownload::checkRedirect(const QUrl &from, const QUrl &to)
{
    const QUrl target = from.resolved(to);
    if (const LinkCheck check = checkLink(target); check != LinkCheck::Allowed)
        return check;
    if ((from.scheme().compare(u"https"_s, Qt::CaseInsensitive) == 0)
            && (target.scheme().compare(u"https"_s, Qt::CaseInsensitive) != 0))
    {
        return LinkCheck::SchemeDowngrade;
    }
    if (!isLocalNetworkHost(from.host()) && isLocalNetworkHost(target.host()))
        return LinkCheck::RedirectToLocalNetwork;
    return LinkCheck::Allowed;
}

QString DirectDownload::describe(const LinkCheck check)
{
    switch (check)
    {
    case LinkCheck::Allowed:
        return QCoreApplication::translate("DirectDownload", "Allowed");
    case LinkCheck::InvalidUrl:
        return QCoreApplication::translate("DirectDownload", "Not a valid link");
    case LinkCheck::SchemeNotAllowed:
        return QCoreApplication::translate("DirectDownload", "Only http and https links can be downloaded");
    case LinkCheck::SchemeDowngrade:
        return QCoreApplication::translate("DirectDownload", "The server redirected from https to unencrypted http");
    case LinkCheck::RedirectToLocalNetwork:
        return QCoreApplication::translate("DirectDownload", "The server redirected to an address in your local network");
    }
    return {};
}

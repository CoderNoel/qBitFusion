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

#include "updatemanifest.h"

#include <memory>

#include <openssl/evp.h>

#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUrl>

#include "base/global.h"
#include "base/version.h"

#ifndef QBF_REVISION
#define QBF_REVISION 0
#endif

using namespace Qt::Literals::StringLiterals;

SelfUpdate::Version SelfUpdate::Version::fromString(const QString &text)
{
    static const QRegularExpression pattern {u"^(\\d{1,4})\\.(\\d{1,4})\\.(\\d{1,4})\\.(\\d{1,6})$"_s};
    const QRegularExpressionMatch match = pattern.match(text.trimmed());
    if (!match.hasMatch())
        return {};
    return {match.captured(1).toInt(), match.captured(2).toInt(), match.captured(3).toInt(), match.captured(4).toInt()};
}

bool SelfUpdate::Version::isValid() const
{
    return (*this != Version {});
}

QString SelfUpdate::Version::toString() const
{
    return u"%1.%2.%3.%4"_s.arg(major).arg(minor).arg(bugfix).arg(revision);
}

SelfUpdate::Version SelfUpdate::currentVersion()
{
    return {QBT_VERSION_MAJOR, QBT_VERSION_MINOR, QBT_VERSION_BUGFIX, QBF_REVISION};
}

QString SelfUpdate::displayVersion()
{
    const Version current = currentVersion();
    if (current.revision > 0)
        return current.toString();
    return u"%1.%2.%3 (development build)"_s.arg(current.major).arg(current.minor).arg(current.bugfix);
}

bool SelfUpdate::verifySignature(const QByteArray &message, const QByteArray &signature, const QByteArray &publicKey)
{
    constexpr qsizetype ED25519_KEY_SIZE = 32;
    constexpr qsizetype ED25519_SIGNATURE_SIZE = 64;

    if (publicKey.size() != ED25519_KEY_SIZE)
        return false;

    const QByteArray rawSignature = QByteArray::fromBase64(signature.simplified().replace(' ', QByteArray()));
    if (rawSignature.size() != ED25519_SIGNATURE_SIZE)
        return false;

    const std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key {EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr
            , reinterpret_cast<const unsigned char *>(publicKey.constData()), static_cast<size_t>(publicKey.size())), &EVP_PKEY_free};
    if (!key)
        return false;

    const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context {EVP_MD_CTX_new(), &EVP_MD_CTX_free};
    if (!context || (EVP_DigestVerifyInit(context.get(), nullptr, nullptr, nullptr, key.get()) != 1))
        return false;

    return EVP_DigestVerify(context.get()
            , reinterpret_cast<const unsigned char *>(rawSignature.constData()), static_cast<size_t>(rawSignature.size())
            , reinterpret_cast<const unsigned char *>(message.constData()), static_cast<size_t>(message.size())) == 1;
}

nonstd::expected<SelfUpdate::Manifest, QString> SelfUpdate::parseManifest(const QByteArray &json, const QString &platform
        , const QStringList &allowedUrlPrefixes)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if ((parseError.error != QJsonParseError::NoError) || !document.isObject())
        return nonstd::make_unexpected(u"update.json is not a JSON object"_s);

    const QJsonObject root = document.object();
    if (root.value(u"format"_s).toInt() != 1)
        return nonstd::make_unexpected(u"unsupported update.json format"_s);

    Manifest manifest;
    manifest.version = Version::fromString(root.value(u"version"_s).toString());
    if (!manifest.version.isValid() || (manifest.version.revision < 1))
        return nonstd::make_unexpected(u"invalid version"_s);
    manifest.upstreamVersion = root.value(u"upstream"_s).toString();
    manifest.notesUrl = root.value(u"notes"_s).toString();

    const QJsonObject entry = root.value(platform).toObject();
    if (entry.isEmpty())
        return nonstd::make_unexpected(u"no package for %1"_s.arg(platform));

    manifest.package.url = entry.value(u"url"_s).toString();
    manifest.package.size = entry.value(u"size"_s).toInteger();
    manifest.package.sha256 = QByteArray::fromHex(entry.value(u"sha256"_s).toString().toLatin1());

    const QUrl url {manifest.package.url, QUrl::StrictMode};
    const bool allowed = std::any_of(allowedUrlPrefixes.cbegin(), allowedUrlPrefixes.cend(), [&manifest](const QString &prefix)
    {
        return !prefix.isEmpty() && manifest.package.url.startsWith(prefix);
    });
    if (!url.isValid() || !allowed || manifest.package.url.contains(u".."_s))
        return nonstd::make_unexpected(u"package URL is not allowed"_s);
    if ((manifest.package.size <= 0) || (manifest.package.size > (1LL << 30)))
        return nonstd::make_unexpected(u"invalid package size"_s);
    if (manifest.package.sha256.size() != 32)
        return nonstd::make_unexpected(u"invalid package hash"_s);

    return manifest;
}

QString SelfUpdate::quoteWindowsArgument(const QString &argument)
{
    if (!argument.isEmpty() && !argument.contains(QRegularExpression(u"[\\s\"]"_s)))
        return argument;

    // Backslashes are literal unless they precede a quote; then each must be doubled.
    QString result = u"\""_s;
    qsizetype backslashes = 0;
    for (const QChar ch : argument)
    {
        if (ch == u'\\')
        {
            ++backslashes;
            continue;
        }
        if (ch == u'"')
        {
            result += QString(backslashes * 2 + 1, u'\\');
            result += ch;
        }
        else
        {
            result += QString(backslashes, u'\\');
            result += ch;
        }
        backslashes = 0;
    }
    result += QString(backslashes * 2, u'\\');
    result += u'"';
    return result;
}

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

#include "hybridsidecar.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>

#include "base/utils/fs.h"

using namespace Qt::Literals::StringLiterals;

namespace
{
    const QString KEY_SCHEMA = u"schema_version"_s;
    const QString KEY_POLICY = u"policy"_s;
    const QString KEY_CONSENT = u"p2p_fallback_consent"_s;
    const QString KEY_PROVIDER = u"provider_id"_s;
    const QString KEY_RESOURCE = u"provider_resource_id"_s;
    const QString KEY_REASON = u"last_reason"_s;
    const QString KEY_TRANSITION = u"last_transition_utc"_s;
    const QString KEY_SWITCHES = u"switch_count"_s;

    constexpr qint64 MAX_SIDECAR_SIZE = 16 * 1024;

    bool isTorrentIDSafe(const QString &id)
    {
        static const QRegularExpression re {u"^[0-9a-f]{40}$|^[0-9a-f]{64}$"_s};
        return re.match(id).hasMatch();
    }
}

bool Hybrid::isSafeIdentifier(const QString &value)
{
    static const QRegularExpression re {u"^[A-Za-z0-9_-]{1,64}$"_s};
    return re.match(value).hasMatch();
}

QByteArray Hybrid::serializeSidecar(const SidecarData &data)
{
    QJsonObject obj;
    obj[KEY_SCHEMA] = SIDECAR_SCHEMA_VERSION;
    obj[KEY_POLICY] = wireName(data.policy);
    obj[KEY_CONSENT] = data.p2pFallbackConsent;
    if (isSafeIdentifier(data.providerId))
        obj[KEY_PROVIDER] = data.providerId;
    if (isSafeIdentifier(data.providerResourceId))
        obj[KEY_RESOURCE] = data.providerResourceId;
    obj[KEY_REASON] = wireName(data.lastReason);
    if (data.lastTransitionUtc.isValid())
        obj[KEY_TRANSITION] = data.lastTransitionUtc.toUTC().toString(Qt::ISODateWithMs);
    obj[KEY_SWITCHES] = static_cast<qint64>(std::min<quint64>(data.switchCount, (1ULL << 52)));
    return QJsonDocument(obj).toJson(QJsonDocument::Indented);
}

nonstd::expected<Hybrid::SidecarData, QString> Hybrid::parseSidecar(const QByteArray &json)
{
    if (json.size() > MAX_SIDECAR_SIZE)
        return nonstd::make_unexpected(u"sidecar too large"_s);

    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &error);
    if ((error.error != QJsonParseError::NoError) || !doc.isObject())
        return nonstd::make_unexpected(u"sidecar is not a JSON object"_s);

    const QJsonObject obj = doc.object();
    const int schema = obj.value(KEY_SCHEMA).toInt(-1);
    if (schema < 1)
        return nonstd::make_unexpected(u"missing schema version"_s);
    // Newer schema: fields are additive by contract, so read what we understand.

    // Reject anything that smells like a secret or URL having leaked into the file.
    for (auto it = obj.constBegin(); it != obj.constEnd(); ++it)
    {
        if (!it.value().isString())
            continue;
        const QString value = it.value().toString();
        if (value.contains(u"://"_s) || value.startsWith(u"magnet:"_s, Qt::CaseInsensitive)
                || it.key().contains(u"token"_s, Qt::CaseInsensitive) || it.key().contains(u"url"_s, Qt::CaseInsensitive))
        {
            return nonstd::make_unexpected(u"sidecar contains forbidden data"_s);
        }
    }

    SidecarData data;
    data.policy = policyFromWireName(obj.value(KEY_POLICY).toString(), TransportPolicy::TorrentOnly);
    data.p2pFallbackConsent = obj.value(KEY_CONSENT).toBool(false);

    const QString provider = obj.value(KEY_PROVIDER).toString();
    if (!provider.isEmpty() && !isSafeIdentifier(provider))
        return nonstd::make_unexpected(u"invalid provider id"_s);
    data.providerId = provider;

    const QString resource = obj.value(KEY_RESOURCE).toString();
    if (!resource.isEmpty() && !isSafeIdentifier(resource))
        return nonstd::make_unexpected(u"invalid provider resource id"_s);
    data.providerResourceId = resource;

    data.lastReason = reasonFromWireName(obj.value(KEY_REASON).toString());
    const QString transition = obj.value(KEY_TRANSITION).toString();
    if (!transition.isEmpty())
        data.lastTransitionUtc = QDateTime::fromString(transition, Qt::ISODateWithMs).toUTC();
    data.switchCount = static_cast<quint64>(std::max<qint64>(0, obj.value(KEY_SWITCHES).toInteger(0)));

    // ADR-004 integrity: consent is meaningless (and must not be honoured) under debrid_only.
    if (data.policy == TransportPolicy::DebridOnly)
        data.p2pFallbackConsent = false;

    return data;
}

Hybrid::SidecarStore::SidecarStore(Path directory)
    : m_directory {std::move(directory)}
{
}

Path Hybrid::SidecarStore::directory() const
{
    return m_directory;
}

Path Hybrid::SidecarStore::pathFor(const QString &torrentID) const
{
    return m_directory / Path(torrentID + u".hybrid.json"_s);
}

std::optional<Hybrid::SidecarData> Hybrid::SidecarStore::load(const QString &torrentID) const
{
    if (!isTorrentIDSafe(torrentID))
        return std::nullopt;

    QFile file {pathFor(torrentID).data()};
    if (!file.open(QIODevice::ReadOnly))
        return std::nullopt;
    if (file.size() > MAX_SIDECAR_SIZE)
        return std::nullopt;

    const auto parsed = parseSidecar(file.readAll());
    if (!parsed)
        return std::nullopt;
    return parsed.value();
}

nonstd::expected<void, QString> Hybrid::SidecarStore::save(const QString &torrentID, const SidecarData &data) const
{
    if (!isTorrentIDSafe(torrentID))
        return nonstd::make_unexpected(u"invalid torrent id"_s);

    if (!Utils::Fs::mkpath(m_directory))
        return nonstd::make_unexpected(u"cannot create sidecar directory"_s);

    QSaveFile file {pathFor(torrentID).data()};
    if (!file.open(QIODevice::WriteOnly))
        return nonstd::make_unexpected(file.errorString());
    const QByteArray bytes = serializeSidecar(data);
    if ((file.write(bytes) != bytes.size()) || !file.commit())
        return nonstd::make_unexpected(file.errorString());
    return {};
}

bool Hybrid::SidecarStore::remove(const QString &torrentID) const
{
    if (!isTorrentIDSafe(torrentID))
        return false;
    const QString path = pathFor(torrentID).data();
    return !QFile::exists(path) || QFile::remove(path);
}

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

#include "urlguard.h"

#include <algorithm>

#include <QCoreApplication>

using namespace Qt::Literals::StringLiterals;

namespace
{
    bool hostMatchesSuffix(const QString &host, const QString &suffix)
    {
        const QString h = host.toLower();
        const QString s = suffix.toLower();
        if (s.isEmpty())
            return false;
        return (h == s) || h.endsWith(u'.' + s);
    }

    bool isLoopbackHost(const QString &host)
    {
        if (host.compare(u"localhost"_s, Qt::CaseInsensitive) == 0)
            return true;
        const QHostAddress address {host};
        return !address.isNull() && address.isLoopback();
    }
}

bool Hybrid::isPublicAddress(const QHostAddress &address)
{
    if (address.isNull())
        return false;

    QHostAddress addr = address;
    // Unwrap IPv4-mapped IPv6 (::ffff:a.b.c.d) so private IPv4 cannot hide behind it.
    bool isV4Mapped = false;
    const quint32 v4 = addr.toIPv4Address(&isV4Mapped);
    if (isV4Mapped)
        addr = QHostAddress {v4};

    if (addr.isLoopback() || addr.isLinkLocal() || addr.isSiteLocal() || addr.isUniqueLocalUnicast()
            || addr.isMulticast() || addr.isBroadcast() || addr.isPrivateUse())
    {
        return false;
    }

    if (addr.protocol() == QAbstractSocket::IPv4Protocol)
    {
        const quint32 ip = addr.toIPv4Address();
        const auto inRange = [ip](const quint32 net, const int prefix)
        {
            const quint32 mask = (prefix == 0) ? 0 : (0xFFFFFFFFu << (32 - prefix));
            return (ip & mask) == (net & mask);
        };
        if (inRange(0x00000000u, 8)        // 0.0.0.0/8 "this network"
                || inRange(0x64400000u, 10)  // 100.64.0.0/10 CGNAT
                || inRange(0xA9FE0000u, 16)  // 169.254.0.0/16 link-local / cloud metadata
                || inRange(0xC0000000u, 24)  // 192.0.0.0/24 IETF protocol assignments
                || inRange(0xC0000200u, 24)  // 192.0.2.0/24 TEST-NET-1
                || inRange(0xC6120000u, 15)  // 198.18.0.0/15 benchmarking
                || inRange(0xC6336400u, 24)  // 198.51.100.0/24 TEST-NET-2
                || inRange(0xCB007100u, 24)  // 203.0.113.0/24 TEST-NET-3
                || inRange(0xF0000000u, 4))  // 240.0.0.0/4 reserved
        {
            return false;
        }
    }
    else if (addr.protocol() == QAbstractSocket::IPv6Protocol)
    {
        const Q_IPV6ADDR ip6 = addr.toIPv6Address();
        bool allZero = true;
        for (int i = 0; i < 16; ++i)
            allZero = allZero && (ip6[i] == 0);
        if (allZero)
            return false; // ::
        if ((ip6[0] == 0x20) && (ip6[1] == 0x01) && (ip6[2] == 0x0d) && (ip6[3] == 0xb8))
            return false; // 2001:db8::/32 documentation
        if ((ip6[0] == 0x00) && (ip6[1] == 0x64) && (ip6[2] == 0xff) && (ip6[3] == 0x9b))
            return false; // 64:ff9b::/96 NAT64 can reach private IPv4
    }

    return true;
}

Hybrid::UrlCheck Hybrid::checkUrl(const QUrl &url, const UrlPolicy &policy)
{
    if (!url.isValid() || url.isRelative() || url.host().isEmpty())
        return UrlCheck::InvalidUrl;

    if (!url.userInfo().isEmpty())
        return UrlCheck::CredentialsInUrl;

    const QString scheme = url.scheme().toLower();
    const QString host = url.host();
    const bool loopback = isLoopbackHost(host);

    if (policy.allowLoopbackForTesting && loopback)
    {
        if ((scheme != u"http") && (scheme != u"https"))
            return UrlCheck::SchemeNotAllowed;
        return UrlCheck::Allowed;
    }

    if (scheme != u"https")
        return UrlCheck::SchemeNotAllowed;

    if (loopback)
        return UrlCheck::PrivateAddress;

    const int port = url.port(443);
    if ((port != 443) && (port != 8443))
        return UrlCheck::PortNotAllowed;

    const QHostAddress literal {host};
    if (!literal.isNull())
    {
        // IP-literal hosts are never on a provider allowlist and must at least be public.
        if (!isPublicAddress(literal))
            return UrlCheck::PrivateAddress;
        if (!policy.allowedHostSuffixes.isEmpty())
            return UrlCheck::HostNotAllowed;
        return UrlCheck::Allowed;
    }

    // Single-label or ".local"/".internal" names resolve inside the LAN.
    if (!host.contains(u'.') || host.endsWith(u".local"_s, Qt::CaseInsensitive)
            || host.endsWith(u".internal"_s, Qt::CaseInsensitive) || host.endsWith(u".localhost"_s, Qt::CaseInsensitive))
    {
        return UrlCheck::PrivateAddress;
    }

    if (!policy.allowedHostSuffixes.isEmpty())
    {
        const bool allowed = std::any_of(policy.allowedHostSuffixes.cbegin(), policy.allowedHostSuffixes.cend()
                , [&host](const QString &suffix) { return hostMatchesSuffix(host, suffix); });
        if (!allowed)
            return UrlCheck::HostNotAllowed;
    }

    return UrlCheck::Allowed;
}

Hybrid::UrlCheck Hybrid::checkRedirect(const QUrl &from, const QUrl &to, const UrlPolicy &policy)
{
    const QUrl target = from.resolved(to);
    if ((from.scheme().compare(u"https"_s, Qt::CaseInsensitive) == 0)
            && (target.scheme().compare(u"https"_s, Qt::CaseInsensitive) != 0))
    {
        return UrlCheck::SchemeNotAllowed;
    }
    return checkUrl(target, policy);
}

bool Hybrid::mayCarryCredentials(const QUrl &url, const QStringList &apiHosts)
{
    if (url.scheme().compare(u"https"_s, Qt::CaseInsensitive) != 0)
        return false;
    const QString host = url.host().toLower();
    return std::any_of(apiHosts.cbegin(), apiHosts.cend(), [&host](const QString &apiHost)
    {
        return host == apiHost.toLower();
    });
}

QString Hybrid::redactUrl(const QUrl &url)
{
    if (!url.isValid() || url.host().isEmpty())
        return u"<invalid-url>"_s;
    return url.scheme().toLower() + u"://"_s + url.host().toLower() + u"/…"_s;
}

QString Hybrid::describe(const UrlCheck check)
{
    switch (check)
    {
    case UrlCheck::Allowed:
        return QCoreApplication::translate("Hybrid", "Allowed");
    case UrlCheck::InvalidUrl:
        return QCoreApplication::translate("Hybrid", "Invalid URL");
    case UrlCheck::SchemeNotAllowed:
        return QCoreApplication::translate("Hybrid", "Only HTTPS is allowed");
    case UrlCheck::CredentialsInUrl:
        return QCoreApplication::translate("Hybrid", "Credentials embedded in URL");
    case UrlCheck::HostNotAllowed:
        return QCoreApplication::translate("Hybrid", "Host is not authorized for this provider");
    case UrlCheck::PrivateAddress:
        return QCoreApplication::translate("Hybrid", "Private or local network destination");
    case UrlCheck::PortNotAllowed:
        return QCoreApplication::translate("Hybrid", "Port not allowed");
    }
    return {};
}

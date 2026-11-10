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
#include <optional>

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QUrl>

#include "hybridtypes.h"
#include "rangevalidation.h"
#include "urlguard.h"

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

// Specialized HTTP Range transfer adapter (spec 05 "HTTP client (reuse Qt)").
// qBittorrent's Net::DownloadManager is not reused for the data plane because at release-5.3.0 it
// cannot set request headers (Range), buffers the full body, and transparently gunzips — all of
// which violate the range semantics required here. See qbitfusion/REPO_RECON.md.

namespace Hybrid
{
    class TokenBucket;

    struct RangeTimeouts
    {
        int inactivityMs = 15'000;
        int totalMs = 120'000;
    };

    struct RangeRequest
    {
        QUrl url;                // signed CDN link: never logged
        ByteRange range;
        qint64 expectedTotal = -1;
        RangeTimeouts timeouts;
    };

    struct RangeError
    {
        FailureCategory category = FailureCategory::Temporary;
        RangeVerdict verdict = RangeVerdict::HttpError;
        int httpStatus = 0;
        std::optional<qint64> retryAfterSeconds;
        QString detail;          // redacted, safe for logs
    };

    class RangeFetch final : public QObject
    {
        Q_OBJECT
        Q_DISABLE_COPY_MOVE(RangeFetch)

    public:
        using Clock = std::function<qint64 ()>;

        RangeFetch(QNetworkAccessManager *manager, RangeRequest request, UrlPolicy policy
                , TokenBucket *bucket, Clock clock, QObject *parent = nullptr);
        ~RangeFetch() override;

        void start();
        void abort();
        bool isFinished() const;
        qint64 bytesReceived() const;

    signals:
        void succeeded(const QByteArray &data);
        void failed(const Hybrid::RangeError &error);
        // Bytes accepted from the network (for HTTP rate display). Emitted per read.
        void progressed(qint64 newBytes);

    private:
        void onMetaDataChanged();
        void onReadyRead();
        void onFinished();
        void onRedirected(const QUrl &url);
        void drain();
        void complete();
        void fail(RangeError error);
        void cleanup();

        QNetworkAccessManager *m_manager = nullptr;
        RangeRequest m_request;
        UrlPolicy m_policy;
        TokenBucket *m_bucket = nullptr;
        Clock m_clock;

        QPointer<QNetworkReply> m_reply;
        QTimer *m_deadline = nullptr;
        QTimer *m_throttle = nullptr;
        QByteArray m_buffer;
        QUrl m_currentUrl;
        bool m_headersValidated = false;
        bool m_replyDone = false;
        bool m_finished = false;
        // Set when the status line already decided the outcome and only the error page is awaited.
        std::optional<RangeError> m_rejected;
        QTimer *m_errorPageTimer = nullptr;
    };
}

Q_DECLARE_METATYPE(Hybrid::RangeError)

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
#include <QFile>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>

#include "base/hybrid/hybridtypes.h"
#include "linkclassifier.h"

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

// HTTP for direct downloads. Net::DownloadManager is not reused for the body for the same reasons as
// the hybrid range fetcher (no Range header, whole body in memory, transparent decompression), see
// qbitfusion/docs/INTEGRATION-NOTES.md. Redirects are followed by hand so each hop is checked and a
// redirect to a magnet link can be recognized, as Net::DownloadHandlerImpl does.
//
// Links are secrets (signed URLs, API keys in addon URLs): only scheme and host ever reach a log,
// an error message or the UI.

namespace Hybrid
{
    class TokenBucket;
}

namespace DirectDownload
{
    struct Validator
    {
        QByteArray etag;
        QByteArray lastModified;

        bool isEmpty() const { return etag.isEmpty() && lastModified.isEmpty(); }
        // The value for If-Range: a strong ETag, else Last-Modified, else empty.
        QByteArray ifRange() const;
    };

    struct Failure
    {
        Hybrid::FailureCategory category = Hybrid::FailureCategory::Temporary;
        int httpStatus = 0;
        std::optional<qint64> retryAfterSeconds;
        bool restartFromZero = false;  // the file on the server changed: the bytes on disk are useless
        QString detail;                // redacted, safe for logs and the UI
    };

    // Follows redirects one hop at a time and hands the final response to the subclass.
    class HttpRequest : public QObject
    {
        Q_OBJECT
        Q_DISABLE_COPY_MOVE(HttpRequest)

    public:
        ~HttpRequest() override;

        // The URL after redirects. A secret: never log it.
        QUrl currentUrl() const;

    protected:
        HttpRequest(QNetworkAccessManager *manager, const QUrl &url, int inactivityMs, QObject *parent);

        bool sendFirstRequest();  // false (and failure reported) if the link itself is refused
        void closeReply();
        bool isClosed() const;
        QNetworkReply *reply() const;
        Failure failureFromReply() const;

        virtual QList<std::pair<QByteArray, QByteArray>> extraHeaders() const;
        virtual void handleHead() = 0;           // final response headers
        virtual void handleReadyRead() = 0;
        virtual void handleFinished() = 0;       // final response complete (check reply()->error())
        virtual void handleMagnetRedirect(const QString &magnetUri);
        virtual void handleFailure(const Failure &failure) = 0;

    private:
        void sendRequest();
        void onMetaDataChanged();
        void onReadyRead();
        void onFinished();
        bool isRedirectHop() const;

        QNetworkAccessManager *m_manager = nullptr;
        QUrl m_currentUrl;
        int m_inactivityMs = 30'000;
        int m_hops = 0;
        QPointer<QNetworkReply> m_reply;
        bool m_headDelivered = false;
        bool m_closed = false;
    };

    // ------------------------------------------------------------------------------- probe

    struct ProbeResult
    {
        ContentKind kind = ContentKind::File;
        QUrl finalUrl;           // a secret: memory only
        QString magnetUri;       // set when the server redirected to a magnet link
        QByteArray torrentData;  // the .torrent; empty means "let the torrent path fetch it"
        QString fileName;        // sanitized
        qint64 totalSize = -1;   // -1 if unknown
        bool rangeSupported = false;
        Validator validator;
        QByteArray contentType;
    };

    // Requests the link once and decides whether it serves a torrent or a file. For a file only the
    // headers and the first bytes are read; for a torrent the whole body (up to the size limit).
    class LinkProbe final : public HttpRequest
    {
        Q_OBJECT
        Q_DISABLE_COPY_MOVE(LinkProbe)

    public:
        LinkProbe(QNetworkAccessManager *manager, const QUrl &url, qint64 torrentSizeLimit, QObject *parent = nullptr);

        void start();
        void abort();

    signals:
        void finished(const DirectDownload::ProbeResult &result);
        void failed(const DirectDownload::Failure &failure);

    private:
        QList<std::pair<QByteArray, QByteArray>> extraHeaders() const override;
        void handleHead() override;
        void handleReadyRead() override;
        void handleFinished() override;
        void handleMagnetRedirect(const QString &magnetUri) override;
        void handleFailure(const Failure &failure) override;

        void decide(bool replyComplete);
        void succeed();

        qint64 m_torrentSizeLimit = 0;
        QTimer *m_deadline = nullptr;
        ResponseHead m_head;
        ProbeResult m_result;
        QByteArray m_body;
        std::optional<Failure> m_rejected;
        bool m_collectingTorrent = false;
        bool m_done = false;
    };

    // ------------------------------------------------------------------------------- transfer

    struct TransferRequest
    {
        QUrl url;                  // a secret: never logged
        QString filePath;          // the partial file; bytes [0, offset) are already in it
        qint64 offset = 0;
        qint64 expectedTotal = -1; // -1 if unknown
        Validator validator;       // from the first response, to detect a changed file
        int inactivityMs = 30'000;
    };

    struct TransferHead
    {
        qint64 total = -1;
        bool rangeSupported = false;
        bool restartedFromZero = false;  // the server ignored the range: the file is written anew
        Validator validator;
        QByteArray contentType;
    };

    // One GET from an offset to the end of the file, streamed to disk.
    class Transfer final : public HttpRequest
    {
        Q_OBJECT
        Q_DISABLE_COPY_MOVE(Transfer)

    public:
        using Clock = std::function<qint64 ()>;

        Transfer(QNetworkAccessManager *manager, TransferRequest request, Hybrid::TokenBucket *bucket
                , Clock clock, QObject *parent = nullptr);
        ~Transfer() override;

        void start();
        // Stops without a signal; the bytes written so far stay in the file.
        void abort();
        qint64 bytesOnDisk() const;

    signals:
        void responseStarted(const DirectDownload::TransferHead &head);
        void progressed(qint64 newBytes);
        void succeeded();
        void failed(const DirectDownload::Failure &failure);

    private:
        QList<std::pair<QByteArray, QByteArray>> extraHeaders() const override;
        void handleHead() override;
        void handleReadyRead() override;
        void handleFinished() override;
        void handleFailure(const Failure &failure) override;

        void drain();
        void finish();
        void fail(Failure failure);
        void stopTimers();

        TransferRequest m_request;
        Hybrid::TokenBucket *m_bucket = nullptr;
        Clock m_clock;
        QFile m_file;
        QTimer *m_throttle = nullptr;
        qint64 m_offset = 0;
        qint64 m_written = 0;
        qint64 m_total = -1;
        bool m_headOk = false;
        bool m_ignoreBody = false;  // a 416 that confirms the file is already complete
        bool m_replyDone = false;
        bool m_done = false;
        std::optional<Failure> m_rejected;
    };
}

Q_DECLARE_METATYPE(DirectDownload::Failure)
Q_DECLARE_METATYPE(DirectDownload::ProbeResult)
Q_DECLARE_METATYPE(DirectDownload::TransferHead)

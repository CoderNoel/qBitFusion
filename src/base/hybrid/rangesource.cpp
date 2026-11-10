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

#include "rangesource.h"

#include <algorithm>

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

#include "base/version.h"
#include "tokenbucket.h"

using namespace Qt::Literals::StringLiterals;

namespace
{
    constexpr qint64 READ_BUFFER_SIZE = 256 * 1024;
    constexpr int MAX_REDIRECTS = 5;
    constexpr qint64 MAX_ERROR_PAGE = 16 * 1024;
    constexpr int ERROR_PAGE_WAIT_MS = 3000;

    QByteArray header(const QNetworkReply *reply, const QByteArray &name)
    {
        return reply->rawHeader(name);
    }
}

Hybrid::RangeFetch::RangeFetch(QNetworkAccessManager *manager, RangeRequest request, UrlPolicy policy
        , TokenBucket *bucket, Clock clock, QObject *parent)
    : QObject(parent)
    , m_manager {manager}
    , m_request {std::move(request)}
    , m_policy {std::move(policy)}
    , m_bucket {bucket}
    , m_clock {std::move(clock)}
{
}

Hybrid::RangeFetch::~RangeFetch()
{
    cleanup();
}

bool Hybrid::RangeFetch::isFinished() const
{
    return m_finished;
}

qint64 Hybrid::RangeFetch::bytesReceived() const
{
    return m_buffer.size();
}

void Hybrid::RangeFetch::start()
{
    const qint64 length = m_request.range.length();
    if ((m_request.range.first < 0) || (length <= 0) || (m_request.expectedTotal <= 0)
            || (m_request.range.last >= m_request.expectedTotal))
    {
        fail({.category = FailureCategory::BadResponse, .verdict = RangeVerdict::WrongRange, .detail = u"invalid range request"_s});
        return;
    }

    const UrlCheck check = checkUrl(m_request.url, m_policy);
    if (check != UrlCheck::Allowed)
    {
        fail({.category = FailureCategory::BadResponse, .detail = describe(check) + u": "_s + redactUrl(m_request.url)});
        return;
    }

    m_currentUrl = m_request.url;
    m_buffer.reserve(length);

    QNetworkRequest req {m_request.url};
    req.setRawHeader("User-Agent", "qBitFusion/" QBT_VERSION);
    req.setRawHeader("Range", makeRangeHeader(m_request.range));
    // Disables Qt's transparent decompression; any other Content-Encoding is rejected.
    req.setRawHeader("Accept-Encoding", "identity");
    req.setRawHeader("Cache-Control", "no-transform");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::UserVerifiedRedirectPolicy);
    req.setMaximumRedirectsAllowed(MAX_REDIRECTS);
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    req.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    req.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    req.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    // Inactivity timeout: Qt aborts if no bytes move for this long.
    req.setTransferTimeout(m_request.timeouts.inactivityMs);

    m_reply = m_manager->get(req);
    m_reply->setReadBufferSize(READ_BUFFER_SIZE);  // backpressure: Qt stops reading the socket

    connect(m_reply, &QNetworkReply::metaDataChanged, this, &RangeFetch::onMetaDataChanged);
    connect(m_reply, &QNetworkReply::readyRead, this, &RangeFetch::onReadyRead);
    connect(m_reply, &QNetworkReply::finished, this, &RangeFetch::onFinished);
    connect(m_reply, &QNetworkReply::redirected, this, &RangeFetch::onRedirected);

    m_deadline = new QTimer(this);
    m_deadline->setSingleShot(true);
    connect(m_deadline, &QTimer::timeout, this, [this]
    {
        fail({.category = FailureCategory::Temporary, .detail = u"range request deadline exceeded"_s});
    });
    m_deadline->start(m_request.timeouts.totalMs);

    m_throttle = new QTimer(this);
    m_throttle->setSingleShot(true);
    connect(m_throttle, &QTimer::timeout, this, &RangeFetch::onReadyRead);
}

void Hybrid::RangeFetch::abort()
{
    if (m_finished)
        return;
    m_finished = true;
    cleanup();
}

void Hybrid::RangeFetch::cleanup()
{
    if (m_deadline)
        m_deadline->stop();
    if (m_throttle)
        m_throttle->stop();
    if (m_errorPageTimer)
        m_errorPageTimer->stop();
    if (m_reply)
    {
        m_reply->disconnect(this);
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
}

void Hybrid::RangeFetch::fail(RangeError error)
{
    if (m_finished)
        return;
    m_finished = true;
    m_buffer.clear();
    cleanup();
    emit failed(error);
}

void Hybrid::RangeFetch::onRedirected(const QUrl &url)
{
    const QUrl target = m_currentUrl.resolved(url);
    const UrlCheck check = checkRedirect(m_currentUrl, target, m_policy);
    if (check != UrlCheck::Allowed)
    {
        fail({.category = FailureCategory::BadResponse, .detail = u"redirect refused ("_s + describe(check) + u")"_s});
        return;
    }
    m_currentUrl = target;
    emit m_reply->redirectAllowed();
}

void Hybrid::RangeFetch::onMetaDataChanged()
{
    if (m_finished || !m_reply || m_headersValidated || m_rejected)
        return;

    const int status = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if ((status >= 300) && (status < 400))
        return; // redirect hop; validated in onRedirected

    RangeResponseHead head;
    head.httpStatus = status;
    head.contentRange = header(m_reply, "Content-Range");
    head.contentEncoding = header(m_reply, "Content-Encoding");
    head.contentType = header(m_reply, "Content-Type");
    const QVariant contentLength = m_reply->header(QNetworkRequest::ContentLengthHeader);
    head.contentLength = contentLength.isValid() ? contentLength.toLongLong() : -1;

    const RangeVerdict verdict = validateRangeResponse(head, m_request.range, m_request.expectedTotal);
    if (verdict == RangeVerdict::Accept)
    {
        m_headersValidated = true;
        return;
    }

    RangeError error;
    error.verdict = verdict;
    error.httpStatus = status;
    error.category = (verdict == RangeVerdict::HttpError)
            ? classifyHttpStatus(status, false) : classifyVerdict(verdict);
    error.retryAfterSeconds = parseRetryAfterSeconds(header(m_reply, "Retry-After"));
    // Sizes and offsets only: the range headers carry no secret, and they are what explains a rejection.
    error.detail = u"HTTP %1 from %2 (Content-Length %3, Content-Range '%4', Content-Encoding '%5')"_s
            .arg(QString::number(status), redactUrl(m_currentUrl), QString::number(head.contentLength)
                , QString::fromLatin1(head.contentRange.left(64)), QString::fromLatin1(head.contentEncoding.left(24)));

    // A small error page usually says why the server refused. The outcome is already decided by
    // the status line; wait a moment for the page only so that the reason can be reported.
    if ((verdict == RangeVerdict::HttpError) && (status >= 400) && (head.contentLength > 0) && (head.contentLength <= MAX_ERROR_PAGE))
    {
        m_rejected = error;
        m_errorPageTimer = new QTimer(this);
        m_errorPageTimer->setSingleShot(true);
        connect(m_errorPageTimer, &QTimer::timeout, this, [this] { fail(*m_rejected); });
        m_errorPageTimer->start(ERROR_PAGE_WAIT_MS);
        return;
    }
    fail(error);
}

void Hybrid::RangeFetch::drain()
{
    if (!m_reply)
        return;

    const qint64 expected = m_request.range.length();
    while (m_reply && (m_reply->bytesAvailable() > 0))
    {
        qint64 allowed = m_reply->bytesAvailable();
        if (m_bucket)
        {
            // Every byte passes the shared limiter, including bytes Qt already buffered before the
            // reply finished — otherwise small bodies would silently bypass the user's speed limit.
            allowed = m_bucket->take(allowed, m_clock());
            if (allowed <= 0)
            {
                m_throttle->start(static_cast<int>(std::clamp<qint64>(m_bucket->msUntilAvailable(m_clock()), 1, 1000)));
                return;
            }
        }

        if ((m_buffer.size() + allowed) > expected)
        {
            fail({.category = FailureCategory::BadResponse, .verdict = RangeVerdict::LengthMismatch
                    , .detail = u"response body longer than requested range"_s});
            return;
        }

        const QByteArray chunk = m_reply->read(allowed);
        m_buffer.append(chunk);
        emit progressed(chunk.size());
    }

    if (m_replyDone)
        complete();
}

void Hybrid::RangeFetch::complete()
{
    if (m_finished)
        return;

    if (m_buffer.size() != m_request.range.length())
    {
        fail({.category = FailureCategory::BadResponse, .verdict = RangeVerdict::LengthMismatch
                , .detail = u"truncated range response"_s});
        return;
    }

    m_finished = true;
    const QByteArray data = std::move(m_buffer);
    m_buffer = {};
    cleanup();
    emit succeeded(data);
}

void Hybrid::RangeFetch::onReadyRead()
{
    if (m_finished || !m_reply || m_rejected)
        return; // a rejected response's page is read once, when the reply has finished
    if (!m_headersValidated)
    {
        onMetaDataChanged();
        if (m_finished || !m_headersValidated)
            return;
    }
    drain();
}

void Hybrid::RangeFetch::onFinished()
{
    if (m_finished || !m_reply)
        return;

    if (m_rejected)
    {
        RangeError error = *m_rejected;
        const QString said = summarizeErrorPage(m_reply->read(MAX_ERROR_PAGE));
        if (!said.isEmpty())
            error.detail += u". Server said: \"%1\""_s.arg(said);
        fail(error);
        return;
    }

    if (m_reply->error() != QNetworkReply::NoError)
    {
        const int status = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        RangeError error;
        error.httpStatus = status;
        error.category = classifyHttpStatus(status, false);
        error.retryAfterSeconds = parseRetryAfterSeconds(header(m_reply, "Retry-After"));
        error.detail = u"network error %1 (%2)"_s.arg(QString::number(static_cast<int>(m_reply->error())), redactUrl(m_currentUrl));
        fail(error);
        return;
    }

    if (!m_headersValidated)
    {
        onMetaDataChanged();
        if (m_finished || !m_headersValidated)
            return;
    }

    // Remaining buffered bytes are drained through the limiter; completion follows the last byte.
    m_replyDone = true;
    drain();
}

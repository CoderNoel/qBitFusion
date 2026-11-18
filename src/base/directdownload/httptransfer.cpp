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

#include "httptransfer.h"

#include <algorithm>

#include <QMetaEnum>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

#include "base/hybrid/rangevalidation.h"
#include "base/hybrid/tokenbucket.h"
#include "base/hybrid/urlguard.h"
#include "base/version.h"

using namespace Qt::Literals::StringLiterals;

namespace
{
    constexpr int MAX_REDIRECTS = 10;
    constexpr qint64 READ_BUFFER_SIZE = 256 * 1024;
    constexpr qint64 MAX_ERROR_PAGE = 16 * 1024;
    constexpr qsizetype SNIFF_BYTES = 512;
    constexpr int PROBE_DEADLINE_MS = 90'000;

    int statusOf(const QNetworkReply *reply)
    {
        return reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    }

    qint64 contentLengthOf(const QNetworkReply *reply)
    {
        const QVariant value = reply->header(QNetworkRequest::ContentLengthHeader);
        return value.isValid() ? value.toLongLong() : -1;
    }

    bool isIdentityEncoding(const QNetworkReply *reply)
    {
        const QByteArray encoding = reply->rawHeader("Content-Encoding").trimmed().toLower();
        return encoding.isEmpty() || (encoding == "identity");
    }

    DirectDownload::Validator validatorOf(const QNetworkReply *reply)
    {
        return {.etag = reply->rawHeader("ETag").trimmed(), .lastModified = reply->rawHeader("Last-Modified").trimmed()};
    }

    DirectDownload::Failure httpFailure(const QNetworkReply *reply, const QUrl &url)
    {
        const int status = statusOf(reply);
        DirectDownload::Failure failure;
        failure.httpStatus = status;
        failure.category = Hybrid::classifyHttpStatus(status, false);
        failure.retryAfterSeconds = Hybrid::parseRetryAfterSeconds(reply->rawHeader("Retry-After"));
        failure.detail = u"HTTP %1 from %2"_s.arg(QString::number(status), Hybrid::redactUrl(url));
        return failure;
    }

    // Reads the small error page a server sent with a refusal and adds a safe summary of it.
    void addErrorPage(DirectDownload::Failure &failure, QNetworkReply *reply)
    {
        const QString said = Hybrid::summarizeErrorPage(reply->read(MAX_ERROR_PAGE));
        if (!said.isEmpty())
            failure.detail += u". Server said: \"%1\""_s.arg(said);
    }

    // Small error pages are worth waiting for: they usually say why the server refused.
    bool shouldWaitForErrorPage(const QNetworkReply *reply)
    {
        const qint64 length = contentLengthOf(reply);
        return (length > 0) && (length <= MAX_ERROR_PAGE);
    }
}

QByteArray DirectDownload::Validator::ifRange() const
{
    if (!etag.isEmpty() && !etag.startsWith("W/"))
        return etag;
    return lastModified;
}

// ------------------------------------------------------------------------------- HttpRequest

DirectDownload::HttpRequest::HttpRequest(QNetworkAccessManager *manager, const QUrl &url, const int inactivityMs, QObject *parent)
    : QObject(parent)
    , m_manager {manager}
    , m_currentUrl {url}
    , m_inactivityMs {inactivityMs}
{
}

DirectDownload::HttpRequest::~HttpRequest()
{
    closeReply();
}

QUrl DirectDownload::HttpRequest::currentUrl() const
{
    return m_currentUrl;
}

QNetworkReply *DirectDownload::HttpRequest::reply() const
{
    return m_reply;
}

bool DirectDownload::HttpRequest::isClosed() const
{
    return m_closed;
}

QList<std::pair<QByteArray, QByteArray>> DirectDownload::HttpRequest::extraHeaders() const
{
    return {};
}

void DirectDownload::HttpRequest::handleMagnetRedirect([[maybe_unused]] const QString &magnetUri)
{
    handleFailure({.category = Hybrid::FailureCategory::BadResponse, .detail = u"the server redirected to a magnet link"_s});
}

bool DirectDownload::HttpRequest::sendFirstRequest()
{
    if (const LinkCheck check = checkLink(m_currentUrl); check != LinkCheck::Allowed)
    {
        m_closed = true;
        handleFailure({.category = Hybrid::FailureCategory::BadResponse
                , .detail = describe(check) + u": "_s + Hybrid::redactUrl(m_currentUrl)});
        return false;
    }
    sendRequest();
    return true;
}

void DirectDownload::HttpRequest::sendRequest()
{
    QNetworkRequest request {m_currentUrl};
    request.setRawHeader("User-Agent", "qBitFusion/" QBT_VERSION);
    // Disables Qt's transparent decompression: offsets must count the bytes of the file itself.
    request.setRawHeader("Accept-Encoding", "identity");
    for (const auto &[name, value] : extraHeaders())
        request.setRawHeader(name, value);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    request.setTransferTimeout(m_inactivityMs);

    m_headDelivered = false;
    m_reply = m_manager->get(request);
    m_reply->setReadBufferSize(READ_BUFFER_SIZE);  // backpressure: Qt stops reading the socket
    connect(m_reply, &QNetworkReply::metaDataChanged, this, &HttpRequest::onMetaDataChanged);
    connect(m_reply, &QNetworkReply::readyRead, this, &HttpRequest::onReadyRead);
    connect(m_reply, &QNetworkReply::finished, this, &HttpRequest::onFinished);
}

void DirectDownload::HttpRequest::closeReply()
{
    m_closed = true;
    if (m_reply)
    {
        m_reply->disconnect(this);
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
}

bool DirectDownload::HttpRequest::isRedirectHop() const
{
    if (!m_reply)
        return false;
    const int status = statusOf(m_reply);
    return (status >= 300) && (status < 400)
            && m_reply->attribute(QNetworkRequest::RedirectionTargetAttribute).isValid();
}

DirectDownload::Failure DirectDownload::HttpRequest::failureFromReply() const
{
    Failure failure;
    if (!m_reply)
        return failure;

    const int status = statusOf(m_reply);
    if (status >= 400)
        return httpFailure(m_reply, m_currentUrl);

    // Qt's errorString() can contain the full URL, so only the error's name is reported.
    const char *name = QMetaEnum::fromType<QNetworkReply::NetworkError>().valueToKey(m_reply->error());
    failure.httpStatus = status;
    failure.category = Hybrid::FailureCategory::Temporary;
    failure.detail = u"%1 (%2)"_s.arg((name ? QString::fromLatin1(name) : QString::number(m_reply->error()))
            , Hybrid::redactUrl(m_currentUrl));
    return failure;
}

void DirectDownload::HttpRequest::onMetaDataChanged()
{
    if (m_closed || !m_reply || m_headDelivered || isRedirectHop())
        return;
    m_headDelivered = true;
    handleHead();
}

void DirectDownload::HttpRequest::onReadyRead()
{
    if (m_closed || !m_reply)
        return;
    if (isRedirectHop())
    {
        m_reply->readAll();
        return;
    }
    if (!m_headDelivered)
    {
        onMetaDataChanged();
        if (m_closed)
            return;
    }
    handleReadyRead();
}

void DirectDownload::HttpRequest::onFinished()
{
    if (m_closed || !m_reply)
        return;

    if (isRedirectHop())
    {
        const QUrl target = m_currentUrl.resolved(m_reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl());
        m_reply->disconnect(this);
        m_reply->deleteLater();
        m_reply = nullptr;

        if (target.scheme().compare(u"magnet"_s, Qt::CaseInsensitive) == 0)
        {
            m_closed = true;
            handleMagnetRedirect(target.toString());
            return;
        }
        if (++m_hops > MAX_REDIRECTS)
        {
            m_closed = true;
            handleFailure({.category = Hybrid::FailureCategory::BadResponse, .detail = u"too many redirects"_s});
            return;
        }
        if (const LinkCheck check = checkRedirect(m_currentUrl, target); check != LinkCheck::Allowed)
        {
            m_closed = true;
            handleFailure({.category = Hybrid::FailureCategory::BadResponse
                    , .detail = describe(check) + u" ("_s + Hybrid::redactUrl(target) + u')'});
            return;
        }
        m_currentUrl = target;
        sendRequest();
        return;
    }

    if (!m_headDelivered && (m_reply->error() == QNetworkReply::NoError))
    {
        onMetaDataChanged();
        if (m_closed)
            return;
    }
    handleFinished();
}

// ------------------------------------------------------------------------------- LinkProbe

DirectDownload::LinkProbe::LinkProbe(QNetworkAccessManager *manager, const QUrl &url, const qint64 torrentSizeLimit, QObject *parent)
    : HttpRequest(manager, url, 30'000, parent)
    , m_torrentSizeLimit {torrentSizeLimit}
{
}

void DirectDownload::LinkProbe::start()
{
    m_deadline = new QTimer(this);
    m_deadline->setSingleShot(true);
    connect(m_deadline, &QTimer::timeout, this, [this]
    {
        handleFailure({.category = Hybrid::FailureCategory::Temporary
                , .detail = u"no answer in time from %1"_s.arg(Hybrid::redactUrl(currentUrl()))});
    });
    m_deadline->start(PROBE_DEADLINE_MS);
    sendFirstRequest();
}

void DirectDownload::LinkProbe::abort()
{
    m_done = true;
    if (m_deadline)
        m_deadline->stop();
    closeReply();
}

QList<std::pair<QByteArray, QByteArray>> DirectDownload::LinkProbe::extraHeaders() const
{
    // Asking for a range from the start shows whether the server can resume.
    return {{"Range", "bytes=0-"}};
}

void DirectDownload::LinkProbe::handleHead()
{
    QNetworkReply *r = reply();
    const int status = statusOf(r);
    m_head.httpStatus = status;
    m_head.contentType = r->rawHeader("Content-Type");
    m_head.contentDisposition = r->rawHeader("Content-Disposition");
    m_head.contentLength = contentLengthOf(r);

    if ((status < 200) || (status >= 300))
    {
        Failure failure = httpFailure(r, currentUrl());
        if (shouldWaitForErrorPage(r))
            m_rejected = failure;
        else
            handleFailure(failure);
        return;
    }

    m_result.contentType = m_head.contentType;
    m_result.validator = validatorOf(r);
    if (status == 206)
    {
        const auto range = Hybrid::parseContentRange(r->rawHeader("Content-Range"));
        if (!range || (range->first != 0))
        {
            handleFailure({.category = Hybrid::FailureCategory::BadResponse, .httpStatus = status
                    , .detail = u"invalid Content-Range from %1"_s.arg(Hybrid::redactUrl(currentUrl()))});
            return;
        }
        m_result.totalSize = range->total;
        m_result.rangeSupported = true;
    }
    else
    {
        m_result.totalSize = m_head.contentLength;
        m_result.rangeSupported = r->rawHeader("Accept-Ranges").toLower().contains("bytes");
    }

    if (!isIdentityEncoding(r))
    {
        // A compressed body cannot be resumed by offset. A torrent is still fine: the torrent path
        // fetches it again and decompresses it.
        const bool torrentLike = (mimeTypeOf(m_head.contentType) == "application/x-bittorrent")
                || currentUrl().path().endsWith(u".torrent"_s, Qt::CaseInsensitive);
        if (torrentLike)
        {
            m_result.kind = ContentKind::Torrent;
            m_result.finalUrl = currentUrl();
            succeed();
            return;
        }
        handleFailure({.category = Hybrid::FailureCategory::BadResponse, .httpStatus = status
                , .detail = u"the server sent compressed data, which cannot be resumed (%1)"_s.arg(Hybrid::redactUrl(currentUrl()))});
    }
}

void DirectDownload::LinkProbe::handleReadyRead()
{
    if (m_done || m_rejected)
        return;  // a refusal's page is read once the reply has finished

    QNetworkReply *r = reply();
    if (m_collectingTorrent)
    {
        m_body += r->readAll();
        if (m_body.size() > m_torrentSizeLimit)
            handleFailure({.category = Hybrid::FailureCategory::BadResponse, .detail = u"the torrent file is larger than the size limit"_s});
        return;
    }

    m_body += r->read(SNIFF_BYTES - m_body.size());
    if (m_body.size() >= SNIFF_BYTES)
        decide(false);
}

void DirectDownload::LinkProbe::handleFinished()
{
    if (m_done)
        return;

    QNetworkReply *r = reply();
    if (m_rejected)
    {
        Failure failure = *m_rejected;
        addErrorPage(failure, r);
        handleFailure(failure);
        return;
    }
    if (r->error() != QNetworkReply::NoError)
    {
        handleFailure(failureFromReply());
        return;
    }

    if (m_collectingTorrent)
    {
        m_body += r->readAll();
        if (m_body.size() > m_torrentSizeLimit)
        {
            handleFailure({.category = Hybrid::FailureCategory::BadResponse, .detail = u"the torrent file is larger than the size limit"_s});
            return;
        }
        m_result.torrentData = m_body;
        succeed();
        return;
    }

    m_body += r->read(SNIFF_BYTES - m_body.size());
    decide(true);
}

void DirectDownload::LinkProbe::decide(const bool replyComplete)
{
    m_result.finalUrl = currentUrl();
    m_result.kind = classifyContent(m_head, m_body);

    switch (m_result.kind)
    {
    case ContentKind::WebPage:
        handleFailure({.category = Hybrid::FailureCategory::BadResponse, .httpStatus = m_head.httpStatus
                , .detail = u"the link opens a web page, not a file or a torrent (%1)"_s.arg(Hybrid::redactUrl(currentUrl()))});
        return;

    case ContentKind::Torrent:
        if (replyComplete)
        {
            m_result.torrentData = m_body;
            succeed();
            return;
        }
        if (m_result.totalSize > m_torrentSizeLimit)
        {
            handleFailure({.category = Hybrid::FailureCategory::BadResponse, .detail = u"the torrent file is larger than the size limit"_s});
            return;
        }
        m_collectingTorrent = true;
        m_body += reply()->readAll();
        return;

    case ContentKind::File:
        if (replyComplete && m_body.isEmpty() && (m_result.totalSize <= 0))
        {
            handleFailure({.category = Hybrid::FailureCategory::BadResponse, .httpStatus = m_head.httpStatus
                    , .detail = u"the server sent an empty file (%1)"_s.arg(Hybrid::redactUrl(currentUrl()))});
            return;
        }
        m_result.fileName = chooseFileName(m_head.contentDisposition, currentUrl(), m_head.contentType);
        succeed();
        return;
    }
}

void DirectDownload::LinkProbe::succeed()
{
    if (m_done)
        return;
    m_done = true;
    if (m_deadline)
        m_deadline->stop();
    closeReply();
    emit finished(m_result);
}

void DirectDownload::LinkProbe::handleMagnetRedirect(const QString &magnetUri)
{
    m_result.kind = ContentKind::Torrent;
    m_result.magnetUri = magnetUri;
    succeed();
}

void DirectDownload::LinkProbe::handleFailure(const Failure &failure)
{
    if (m_done)
        return;
    m_done = true;
    if (m_deadline)
        m_deadline->stop();
    closeReply();
    emit failed(failure);
}

// ------------------------------------------------------------------------------- Transfer

DirectDownload::Transfer::Transfer(QNetworkAccessManager *manager, TransferRequest request, Hybrid::TokenBucket *bucket
        , Clock clock, QObject *parent)
    : HttpRequest(manager, request.url, request.inactivityMs, parent)
    , m_request {std::move(request)}
    , m_bucket {bucket}
    , m_clock {std::move(clock)}
    , m_offset {m_request.offset}
{
}

DirectDownload::Transfer::~Transfer()
{
    stopTimers();
    closeReply();
    if (m_file.isOpen())
        m_file.close();
}

void DirectDownload::Transfer::start()
{
    m_throttle = new QTimer(this);
    m_throttle->setSingleShot(true);
    connect(m_throttle, &QTimer::timeout, this, &Transfer::drain);
    sendFirstRequest();
}

void DirectDownload::Transfer::abort()
{
    if (m_done)
        return;
    m_done = true;
    stopTimers();
    closeReply();
    if (m_file.isOpen())
    {
        m_file.flush();
        m_file.close();
    }
}

qint64 DirectDownload::Transfer::bytesOnDisk() const
{
    return m_offset + m_written;
}

void DirectDownload::Transfer::stopTimers()
{
    if (m_throttle)
        m_throttle->stop();
}

QList<std::pair<QByteArray, QByteArray>> DirectDownload::Transfer::extraHeaders() const
{
    QList<std::pair<QByteArray, QByteArray>> headers {{"Range", "bytes=" + QByteArray::number(m_offset) + '-'}};
    if (m_offset > 0)
    {
        // A changed file is then sent whole (200) instead of mixing two versions.
        if (const QByteArray ifRange = m_request.validator.ifRange(); !ifRange.isEmpty())
            headers.append({"If-Range", ifRange});
    }
    return headers;
}

void DirectDownload::Transfer::handleHead()
{
    QNetworkReply *r = reply();
    const int status = statusOf(r);
    const QString host = Hybrid::redactUrl(currentUrl());

    TransferHead head;
    head.validator = validatorOf(r);
    head.contentType = r->rawHeader("Content-Type");

    if ((status == 416) && (m_offset > 0))
    {
        if (m_offset == m_request.expectedTotal)
        {
            // Everything is on disk already; the body is an error page to ignore.
            m_ignoreBody = true;
            m_total = m_offset;
            head.total = m_total;
            head.rangeSupported = true;
        }
        else
        {
            fail({.category = Hybrid::FailureCategory::BadResponse, .httpStatus = status, .restartFromZero = true
                    , .detail = u"the server cannot resume from the saved position (%1)"_s.arg(host)});
            return;
        }
    }
    else if ((status < 200) || (status >= 300))
    {
        Failure failure = httpFailure(r, currentUrl());
        if (shouldWaitForErrorPage(r))
            m_rejected = failure;
        else
            fail(failure);
        return;
    }
    else if (!isIdentityEncoding(r))
    {
        fail({.category = Hybrid::FailureCategory::BadResponse, .httpStatus = status
                , .detail = u"the server sent compressed data, which cannot be resumed (%1)"_s.arg(host)});
        return;
    }
    else if (status == 206)
    {
        const auto range = Hybrid::parseContentRange(r->rawHeader("Content-Range"));
        if (!range || (range->first != m_offset))
        {
            fail({.category = Hybrid::FailureCategory::BadResponse, .httpStatus = status
                    , .detail = u"the server sent a different part of the file than requested (%1)"_s.arg(host)});
            return;
        }
        const bool sizeChanged = (m_request.expectedTotal > 0) && (range->total > 0) && (range->total != m_request.expectedTotal);
        const bool etagChanged = (m_offset > 0) && !m_request.validator.etag.isEmpty() && !head.validator.etag.isEmpty()
                && (head.validator.etag != m_request.validator.etag);
        if (sizeChanged || etagChanged)
        {
            fail({.category = Hybrid::FailureCategory::BadResponse, .httpStatus = status, .restartFromZero = true
                    , .detail = u"the file on the server changed (%1)"_s.arg(host)});
            return;
        }
        m_total = range->total;
        head.total = m_total;
        head.rangeSupported = true;
    }
    else
    {
        // 200: the whole file. If it was a resume, the server ignored the range or the file changed.
        head.restartedFromZero = (m_offset > 0);
        m_offset = 0;
        m_total = contentLengthOf(r);
        head.total = m_total;
        head.rangeSupported = false;
    }

    m_file.setFileName(m_request.filePath);
    if (!m_file.open(QIODevice::ReadWrite))
    {
        fail({.category = Hybrid::FailureCategory::Disk, .detail = u"cannot open the file: %1"_s.arg(m_file.errorString())});
        return;
    }
    if (m_file.size() < m_offset)
    {
        fail({.category = Hybrid::FailureCategory::Disk, .restartFromZero = true
                , .detail = u"the partial file is shorter than expected"_s});
        return;
    }
    if (!m_file.resize(m_offset) || !m_file.seek(m_offset))
    {
        fail({.category = Hybrid::FailureCategory::Disk, .detail = u"cannot prepare the file: %1"_s.arg(m_file.errorString())});
        return;
    }

    m_headOk = true;
    emit responseStarted(head);
}

void DirectDownload::Transfer::handleReadyRead()
{
    if (m_done || !m_headOk)
        return;
    drain();
}

void DirectDownload::Transfer::drain()
{
    if (m_done)
        return;

    while (reply() && (reply()->bytesAvailable() > 0))
    {
        if (m_ignoreBody)
        {
            reply()->readAll();
            break;
        }

        qint64 allowed = reply()->bytesAvailable();
        if (m_bucket)
        {
            allowed = m_bucket->take(allowed, m_clock());
            if (allowed <= 0)
            {
                m_throttle->start(static_cast<int>(std::clamp<qint64>(m_bucket->msUntilAvailable(m_clock()), 1, 1000)));
                return;
            }
        }

        const QByteArray chunk = reply()->read(allowed);
        if ((m_total >= 0) && ((bytesOnDisk() + chunk.size()) > m_total))
        {
            fail({.category = Hybrid::FailureCategory::BadResponse
                    , .detail = u"the server sent more data than the file size (%1)"_s.arg(Hybrid::redactUrl(currentUrl()))});
            return;
        }
        if (m_file.write(chunk) != chunk.size())
        {
            fail({.category = Hybrid::FailureCategory::Disk, .detail = u"cannot write the file: %1"_s.arg(m_file.errorString())});
            return;
        }
        m_written += chunk.size();
        emit progressed(chunk.size());
        if (m_done)
            return;
    }

    if (m_replyDone)
        finish();
}

void DirectDownload::Transfer::handleFinished()
{
    if (m_done)
        return;

    QNetworkReply *r = reply();
    if (m_rejected)
    {
        Failure failure = *m_rejected;
        addErrorPage(failure, r);
        fail(failure);
        return;
    }

    if (!m_ignoreBody && (r->error() != QNetworkReply::NoError))
    {
        // Keep what arrived: the next attempt resumes after it.
        if (m_headOk)
        {
            const qint64 unthrottled = std::min<qint64>(r->bytesAvailable(), READ_BUFFER_SIZE);
            const QByteArray rest = r->read(unthrottled);
            if ((m_total < 0) || ((bytesOnDisk() + rest.size()) <= m_total))
            {
                if (m_file.write(rest) == rest.size())
                    m_written += rest.size();
            }
        }
        fail(failureFromReply());
        return;
    }
    if (!m_headOk)
    {
        fail({.category = Hybrid::FailureCategory::Temporary, .detail = u"no response from %1"_s.arg(Hybrid::redactUrl(currentUrl()))});
        return;
    }

    m_replyDone = true;
    drain();
}

void DirectDownload::Transfer::finish()
{
    if (m_done)
        return;

    if (!m_file.flush())
    {
        fail({.category = Hybrid::FailureCategory::Disk, .detail = u"cannot write the file: %1"_s.arg(m_file.errorString())});
        return;
    }
    if ((m_total >= 0) && (bytesOnDisk() != m_total))
    {
        fail({.category = Hybrid::FailureCategory::Temporary
                , .detail = u"the connection closed before the end of the file (%1)"_s.arg(Hybrid::redactUrl(currentUrl()))});
        return;
    }

    m_done = true;
    stopTimers();
    closeReply();
    m_file.close();
    emit succeeded();
}

void DirectDownload::Transfer::fail(Failure failure)
{
    if (m_done)
        return;
    m_done = true;
    stopTimers();
    closeReply();
    if (m_file.isOpen())
    {
        m_file.flush();
        m_file.close();
    }
    emit failed(failure);
}

void DirectDownload::Transfer::handleFailure(const Failure &failure)
{
    fail(failure);
}

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

#include "hybridwidgets.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QTimer>
#include <QVBoxLayout>

#include "base/hybrid/providerrealdebrid.h"
#include "base/utils/misc.h"
#include "gui/uithememanager.h"

using namespace Qt::Literals::StringLiterals;

namespace
{
    QLabel *mutedLabel(const QString &text, QWidget *parent)
    {
        auto *label = new QLabel(text, parent);
        label->setWordWrap(true);
        label->setTextFormat(Qt::PlainText);
        QPalette pal = label->palette();
        pal.setColor(QPalette::WindowText, pal.color(QPalette::PlaceholderText));
        label->setPalette(pal);
        return label;
    }
}

QString HybridGui::p2pPrivacyNotice()
{
    return QCoreApplication::translate("HybridGui"
            , "If debrid is unavailable, this torrent may connect to BitTorrent peers. Peers and trackers may see your IP address. "
              "You can choose \"Debrid only\" to prevent local P2P activity.");
}

bool HybridGui::confirmP2PFallback(QWidget *parent)
{
    QMessageBox box {parent};
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(QCoreApplication::translate("HybridGui", "Allow BitTorrent fallback?"));
    box.setText(QCoreApplication::translate("HybridGui", "Allow this torrent to use BitTorrent peers when debrid is unavailable?"));
    box.setInformativeText(p2pPrivacyNotice());
    QPushButton *allow = box.addButton(QCoreApplication::translate("HybridGui", "Allow fallback"), QMessageBox::AcceptRole);
    box.addButton(QCoreApplication::translate("HybridGui", "Keep debrid only"), QMessageBox::RejectRole);
    box.setDefaultButton(qobject_cast<QPushButton *>(box.buttons().constLast()));
    box.exec();
    return box.clickedButton() == allow;
}

QIcon HybridGui::iconFor(const Hybrid::TransportPolicy policy)
{
    switch (policy)
    {
    case Hybrid::TransportPolicy::TorrentOnly:
        return UIThemeManager::instance()->getIcon(u"peers"_s);
    case Hybrid::TransportPolicy::DebridOnly:
        return UIThemeManager::instance()->getIcon(u"security-high"_s);
    case Hybrid::TransportPolicy::DebridPreferred:
        return UIThemeManager::instance()->getIcon(u"folder-remote"_s);
    }
    return {};
}

QIcon HybridGui::iconFor(const Hybrid::HybridStatus &status)
{
    using S = Hybrid::TransportState;
    switch (status.state)
    {
    case S::HttpActive:
        return UIThemeManager::instance()->getIcon(u"folder-remote"_s);
    case S::P2PActive:
    case S::RecoveryProbing:
        return UIThemeManager::instance()->getIcon(u"peers"_s);
    case S::HttpDraining:
    case S::HttpReady:
    case S::ProbingProvider:
        return UIThemeManager::instance()->getIcon(u"view-refresh"_s);
    case S::WaitingForUser:
    case S::Error:
        return UIThemeManager::instance()->getIcon(u"dialog-warning"_s);
    case S::WaitingForProvider:
    case S::AwaitingMetadata:
        return UIThemeManager::instance()->getIcon(u"stalledDL"_s);
    case S::Completed:
        return UIThemeManager::instance()->getIcon(u"checked-completed"_s);
    case S::Paused:
        return UIThemeManager::instance()->getIcon(u"stopped"_s);
    case S::Checking:
        return UIThemeManager::instance()->getIcon(u"force-recheck"_s);
    case S::Disabled:
        break;
    }
    return UIThemeManager::instance()->getIcon(u"peers"_s);
}

QString HybridGui::sourceText(const Hybrid::HybridStatus &status)
{
    using S = Hybrid::TransportState;
    const QString provider = status.providerName.isEmpty() ? QCoreApplication::translate("HybridGui", "Debrid") : status.providerName;
    switch (status.state)
    {
    case S::HttpActive:
        return QCoreApplication::translate("HybridGui", "HTTPS · %1").arg(provider);
    case S::P2PActive:
        // Name the cause, so the user can tell why the provider is not being used.
        switch (status.lastReason)
        {
        case Hybrid::Reason::Uncached:
            return QCoreApplication::translate("HybridGui", "BitTorrent · not cached on %1").arg(provider);
        case Hybrid::Reason::ProviderFetching:
            return QCoreApplication::translate("HybridGui", "BitTorrent · %1 still fetching").arg(provider);
        case Hybrid::Reason::UnmatchedTorrentFiles:
            return QCoreApplication::translate("HybridGui", "BitTorrent · files differ on %1").arg(provider);
        case Hybrid::Reason::ProviderLacksPieces:
            return QCoreApplication::translate("HybridGui", "BitTorrent · rest not on %1").arg(provider);
        case Hybrid::Reason::ProviderOutage:
        case Hybrid::Reason::WaitingForRetry:
            return QCoreApplication::translate("HybridGui", "BitTorrent · %1 unavailable").arg(provider);
        case Hybrid::Reason::NoByteRangeSupport:
            return QCoreApplication::translate("HybridGui", "BitTorrent · %1 download refused").arg(provider);
        default:
            break;
        }
        return QCoreApplication::translate("HybridGui", "BitTorrent (fallback)");
    case S::RecoveryProbing:
        return QCoreApplication::translate("HybridGui", "BitTorrent · checking %1").arg(provider);
    case S::HttpDraining:
    case S::HttpReady:
        return QCoreApplication::translate("HybridGui", "Switching...");
    case S::WaitingForUser:
        if ((status.lastReason == Hybrid::Reason::ProviderUnauthorized) || (status.providerStatus == Hybrid::ProviderStatus::AuthenticationRequired))
            return QCoreApplication::translate("HybridGui", "Sign in to %1").arg(provider);
        return (status.lastReason != Hybrid::Reason::None) ? Hybrid::displayText(status.lastReason)
                : QCoreApplication::translate("HybridGui", "Action required");
    case S::WaitingForProvider:
        return QCoreApplication::translate("HybridGui", "Waiting for %1").arg(provider);
    case S::Completed:
        return (status.policy == Hybrid::TransportPolicy::DebridOnly)
                ? QCoreApplication::translate("HybridGui", "Completed · not seeding")
                : QCoreApplication::translate("HybridGui", "Completed");
    default:
        return Hybrid::displayName(status.state);
    }
}

QString HybridGui::sourceToolTip(const Hybrid::HybridStatus &status)
{
    QStringList lines;
    lines << u"<b>%1</b>"_s.arg(Hybrid::displayName(status.policy).toHtmlEscaped());
    lines << QCoreApplication::translate("HybridGui", "State: %1").arg(Hybrid::displayName(status.state).toHtmlEscaped());
    if (status.providerStatus != Hybrid::ProviderStatus::Unknown)
        lines << QCoreApplication::translate("HybridGui", "Provider: %1").arg(Hybrid::displayName(status.providerStatus).toHtmlEscaped());
    if (status.lastReason != Hybrid::Reason::None)
        lines << QCoreApplication::translate("HybridGui", "Last change: %1").arg(Hybrid::displayText(status.lastReason).toHtmlEscaped());
    if (!status.failureDetail.isEmpty() && (status.state != Hybrid::TransportState::HttpActive))
        lines << status.failureDetail.toHtmlEscaped();
    if (!status.matchFailure.isEmpty())
    {
        lines << status.matchFailure.toHtmlEscaped();
    }
    else if (status.providerFileCount > 0)
    {
        lines << QCoreApplication::translate("HybridGui", "Provider has %1 of %2 files").arg(status.boundFileCount).arg(status.torrentFileCount);
        if (status.peerOnlyPieces > 0)
        {
            lines << QCoreApplication::translate("HybridGui", "%1 pieces need BitTorrent peers: they include data the provider does not have")
                    .arg(status.peerOnlyPieces);
        }
    }
    lines << QCoreApplication::translate("HybridGui", "HTTPS ↓ %1 · %2 pieces verified")
            .arg(Utils::Misc::friendlyUnit(status.httpRateBps, true), QString::number(status.httpVerifiedPieces));
    if (status.httpHashFailures > 0)
        lines << QCoreApplication::translate("HybridGui", "Rejected by hash check: %1 pieces").arg(status.httpHashFailures);
    if (status.switchCount > 0)
        lines << QCoreApplication::translate("HybridGui", "Source switches: %1").arg(status.switchCount);
    lines << (Hybrid::swarmAllowed(status.state, {status.policy, status.p2pFallbackApproved, true})
            ? QCoreApplication::translate("HybridGui", "BitTorrent peers may see your IP address.")
            : QCoreApplication::translate("HybridGui", "No BitTorrent peer, tracker or DHT activity."));
    return lines.join(u"<br>"_s);
}

// ------------------------------------------------------------------------------- SourcePicker

HybridGui::SourcePicker::SourcePicker(QWidget *parent)
    : QWidget(parent)
    , m_group {new QButtonGroup(this)}
    , m_torrentOnly {new QRadioButton(Hybrid::displayName(Hybrid::TransportPolicy::TorrentOnly), this)}
    , m_debridOnly {new QRadioButton(Hybrid::displayName(Hybrid::TransportPolicy::DebridOnly), this)}
    , m_debridPreferred {new QRadioButton(Hybrid::displayName(Hybrid::TransportPolicy::DebridPreferred), this)}
    , m_consent {new QCheckBox(tr("Allow BitTorrent fallback for this torrent (peers may see my IP address)"), this)}
    , m_details {mutedLabel({}, this)}
{
    m_torrentOnly->setIcon(iconFor(Hybrid::TransportPolicy::TorrentOnly));
    m_debridOnly->setIcon(iconFor(Hybrid::TransportPolicy::DebridOnly));
    m_debridPreferred->setIcon(iconFor(Hybrid::TransportPolicy::DebridPreferred));

    m_group->addButton(m_torrentOnly, static_cast<int>(Hybrid::TransportPolicy::TorrentOnly));
    m_group->addButton(m_debridOnly, static_cast<int>(Hybrid::TransportPolicy::DebridOnly));
    m_group->addButton(m_debridPreferred, static_cast<int>(Hybrid::TransportPolicy::DebridPreferred));

    auto *radios = new QHBoxLayout;
    radios->setSpacing(16);
    radios->addWidget(m_debridOnly);
    radios->addWidget(m_debridPreferred);
    radios->addWidget(m_torrentOnly);
    radios->addStretch();

    auto *consentRow = new QHBoxLayout;
    consentRow->setContentsMargins(24, 0, 0, 0);
    consentRow->addWidget(m_consent);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    layout->addLayout(radios);
    layout->addLayout(consentRow);
    layout->addWidget(m_details);

    connect(m_group, &QButtonGroup::idToggled, this, [this](const int, const bool checked)
    {
        if (!checked)
            return;
        updateDetails();
        emit selectionChanged();
    });
    connect(m_consent, &QCheckBox::toggled, this, [this](const bool checked)
    {
        if (checked && !confirmP2PFallback(this))
        {
            const QSignalBlocker blocker {m_consent};
            m_consent->setChecked(false);
            return;
        }
        updateDetails();
        emit selectionChanged();
    });

    m_debridOnly->setChecked(true);
    updateDetails();
}

void HybridGui::SourcePicker::setSelection(const Hybrid::TransportPolicy policy, const bool p2pConsent)
{
    const QSignalBlocker groupBlocker {m_group};
    const QSignalBlocker consentBlocker {m_consent};
    if (QAbstractButton *button = m_group->button(static_cast<int>(policy)))
        button->setChecked(true);
    m_consent->setChecked(p2pConsent && (policy == Hybrid::TransportPolicy::DebridPreferred));
    updateDetails();
}

Hybrid::TransportPolicy HybridGui::SourcePicker::policy() const
{
    return static_cast<Hybrid::TransportPolicy>(m_group->checkedId());
}

bool HybridGui::SourcePicker::p2pConsent() const
{
    return (policy() == Hybrid::TransportPolicy::DebridPreferred) && m_consent->isChecked();
}

void HybridGui::SourcePicker::setMagnetWithoutMetadata(const bool magnet)
{
    m_magnet = magnet;
    updateDetails();
}

void HybridGui::SourcePicker::updateDetails()
{
    const Hybrid::TransportPolicy p = policy();
    m_consent->setVisible(p == Hybrid::TransportPolicy::DebridPreferred);

    QString text;
    switch (p)
    {
    case Hybrid::TransportPolicy::TorrentOnly:
        text = tr("Standard BitTorrent behavior. Downloads only from BitTorrent peers.");
        break;
    case Hybrid::TransportPolicy::DebridOnly:
        text = tr("Downloads only over HTTPS from your debrid service. No peer, tracker, DHT or PEX connections are made for this torrent, and it will not seed.");
        if (m_magnet)
            text += u"\n"_s + tr("This magnet has no metadata yet. It will wait, because metadata cannot be obtained without contacting peers.");
        break;
    case Hybrid::TransportPolicy::DebridPreferred:
        text = m_consent->isChecked()
                ? tr("Uses HTTPS while your debrid service has an exact copy, and switches to BitTorrent peers if it becomes unavailable. Already verified pieces are never downloaded again.")
                : tr("Uses HTTPS while available. Without fallback permission the torrent waits instead of contacting peers.");
        break;
    }
    text += u"\n"_s + tr("Every piece is verified against the original torrent's hashes.");
    m_details->setText(text);
}

// ------------------------------------------------------------------------------- DeviceSignInDialog

HybridGui::DeviceSignInDialog::DeviceSignInDialog(Hybrid::RealDebridProvider *provider, QWidget *parent)
    : QDialog(parent)
    , m_provider {provider}
    , m_step {new QLabel(this)}
    , m_code {new QLabel(this)}
    , m_status {mutedLabel(tr("Requesting a sign-in code..."), this)}
    , m_progress {new QProgressBar(this)}
    , m_copy {new QPushButton(UIThemeManager::instance()->getIcon(u"edit-copy"_s), tr("Copy code"), this)}
    , m_open {new QPushButton(UIThemeManager::instance()->getIcon(u"application-url"_s), tr("Open sign-in page"), this)}
    , m_countdown {new QTimer(this)}
{
    setWindowTitle(tr("Sign in to Real-Debrid"));
    setWindowIcon(UIThemeManager::instance()->getIcon(u"wallet-open"_s));
    setMinimumWidth(460);

    auto *title = new QLabel(tr("Connect your Real-Debrid account"), this);
    QFont titleFont = title->font();
    titleFont.setPointSizeF(titleFont.pointSizeF() * 1.35);
    titleFont.setBold(true);
    title->setFont(titleFont);

    m_step->setWordWrap(true);
    m_step->setText(tr("qBitFusion never sees your password. Approve this device on Real-Debrid's website, then return here."));

    QFont codeFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    codeFont.setPointSizeF(codeFont.pointSizeF() * 2.4);
    codeFont.setBold(true);
    codeFont.setLetterSpacing(QFont::AbsoluteSpacing, 4);
    m_code->setFont(codeFont);
    m_code->setAlignment(Qt::AlignCenter);
    m_code->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_code->setMinimumHeight(64);
    m_code->setFrameShape(QFrame::StyledPanel);
    m_code->setText(u"········"_s);

    m_progress->setRange(0, 0);
    m_progress->setTextVisible(false);
    m_progress->setMaximumHeight(6);

    m_copy->setEnabled(false);
    m_open->setEnabled(false);
    m_open->setDefault(true);

    auto *buttons = new QHBoxLayout;
    buttons->addWidget(m_copy);
    buttons->addWidget(m_open);
    buttons->addStretch();

    auto *box = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(12);
    layout->addWidget(title);
    layout->addWidget(m_step);
    layout->addWidget(m_code);
    layout->addLayout(buttons);
    layout->addWidget(m_progress);
    layout->addWidget(m_status);
    layout->addStretch();
    layout->addWidget(box);

    connect(m_copy, &QPushButton::clicked, this, [this]
    {
        QApplication::clipboard()->setText(m_code->text());
        m_copy->setText(tr("Copied"));
        QTimer::singleShot(1500, this, [this] { m_copy->setText(tr("Copy code")); });
    });
    connect(m_open, &QPushButton::clicked, this, [this] { QDesktopServices::openUrl(m_url); });
    connect(m_provider, &Hybrid::RealDebridProvider::deviceCodeReady, this, &DeviceSignInDialog::onCode);
    connect(m_provider, &Hybrid::RealDebridProvider::authorizationFinished, this, &DeviceSignInDialog::onFinished);
    connect(m_countdown, &QTimer::timeout, this, &DeviceSignInDialog::tick);

    m_provider->startDeviceAuthorization();
}

HybridGui::DeviceSignInDialog::~DeviceSignInDialog()
{
    if (!m_done)
        m_provider->cancelDeviceAuthorization();
}

void HybridGui::DeviceSignInDialog::onCode(const QString &userCode, const QUrl &verificationUrl, const int expiresInSeconds)
{
    m_url = verificationUrl;
    m_code->setText(userCode);
    m_step->setText(tr("1. Open %1\n2. Enter the code below and approve qBitFusion.").arg(verificationUrl.toDisplayString()));
    m_copy->setEnabled(true);
    m_open->setEnabled(true);
    m_remaining = expiresInSeconds;
    m_countdown->start(1000);
    tick();
}

void HybridGui::DeviceSignInDialog::tick()
{
    m_remaining = std::max(0, m_remaining - 1);
    m_status->setText(tr("Waiting for approval… code expires in %1:%2")
            .arg(m_remaining / 60).arg(m_remaining % 60, 2, 10, QLatin1Char('0')));
}

void HybridGui::DeviceSignInDialog::onFinished(const bool ok, const QString &message)
{
    m_done = true;
    m_countdown->stop();
    m_progress->setRange(0, 1);
    m_progress->setValue(1);
    if (ok)
    {
        m_status->setText(tr("Signed in. You can close this window."));
        m_code->setText(u"✓"_s);
        QTimer::singleShot(900, this, &QDialog::accept);
    }
    else
    {
        m_status->setText(message.isEmpty() ? tr("Sign-in failed.") : message);
    }
}

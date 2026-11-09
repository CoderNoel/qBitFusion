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

#include "hybridoptionspage.h"

#include <QCheckBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QVBoxLayout>

#include "base/hybrid/hybridmanager.h"
#include "base/hybrid/providerrealdebrid.h"
#include "base/hybrid/securestore.h"
#include "gui/uithememanager.h"
#include "hybridwidgets.h"

using namespace Qt::Literals::StringLiterals;

namespace
{
    QLabel *muted(const QString &text, QWidget *parent)
    {
        auto *label = new QLabel(text, parent);
        label->setWordWrap(true);
        QPalette pal = label->palette();
        pal.setColor(QPalette::WindowText, pal.color(QPalette::PlaceholderText));
        label->setPalette(pal);
        return label;
    }
}

HybridGui::OptionsPage::OptionsPage(QWidget *parent)
    : QWidget(parent)
{
    auto *content = new QWidget;
    auto *layout = new QVBoxLayout(content);
    layout->setSpacing(14);

    // ---- header
    auto *title = new QLabel(tr("Hybrid sources"), content);
    QFont titleFont = title->font();
    titleFont.setPointSizeF(titleFont.pointSizeF() * 1.3);
    titleFont.setBold(true);
    title->setFont(titleFont);
    layout->addWidget(title);
    layout->addWidget(muted(tr("Download torrents over HTTPS from a debrid service you subscribe to. qBittorrent stays in charge: "
            "every piece is verified against the original torrent, there is still one download, one set of files and one "
            "info-hash, and verified pieces are kept if the source changes."), content));

    m_enabled = new QCheckBox(tr("Enable debrid sources"), content);
    QFont enabledFont = m_enabled->font();
    enabledFont.setBold(true);
    m_enabled->setFont(enabledFont);
    layout->addWidget(m_enabled);

    // ---- account
    m_accountBox = new QGroupBox(tr("Real-Debrid account"), content);
    auto *accountLayout = new QVBoxLayout(m_accountBox);
    auto *statusRow = new QHBoxLayout;
    m_accountDot = new QLabel(m_accountBox);
    m_accountDot->setFixedWidth(18);
    m_accountText = new QLabel(m_accountBox);
    m_accountText->setWordWrap(true);
    statusRow->addWidget(m_accountDot);
    statusRow->addWidget(m_accountText, 1);
    accountLayout->addLayout(statusRow);

    auto *accountButtons = new QHBoxLayout;
    m_signIn = new QPushButton(UIThemeManager::instance()->getIcon(u"network-connect"_s), tr("Sign in…"), m_accountBox);
    m_token = new QPushButton(tr("Use private API token…"), m_accountBox);
    m_token->setFlat(true);
    m_signOut = new QPushButton(UIThemeManager::instance()->getIcon(u"system-log-out"_s), tr("Sign out"), m_accountBox);
    accountButtons->addWidget(m_signIn);
    accountButtons->addWidget(m_signOut);
    accountButtons->addStretch();
    accountButtons->addWidget(m_token);
    accountLayout->addLayout(accountButtons);
    accountLayout->addWidget(muted(Hybrid::SecureStore::isPersistent()
            ? tr("Credentials are kept in the Windows Credential Manager and never written to settings, logs or exports.")
            : tr("This platform has no supported credential store yet: you will need to sign in again after restarting."), m_accountBox));
    layout->addWidget(m_accountBox);

    // ---- defaults
    m_defaultsBox = new QGroupBox(tr("Default source for new torrents"), content);
    auto *defaultsLayout = new QVBoxLayout(m_defaultsBox);
    m_picker = new SourcePicker(m_defaultsBox);
    defaultsLayout->addWidget(m_picker);
    defaultsLayout->addWidget(muted(tr("You can change the source for each torrent when adding it, or later from its context menu."), m_defaultsBox));
    layout->addWidget(m_defaultsBox);

    // ---- provider account changes
    auto *remoteBox = new QGroupBox(tr("Provider account changes"), content);
    auto *remoteLayout = new QVBoxLayout(remoteBox);
    m_remoteCreate = new QCheckBox(tr("Add torrents to my Real-Debrid account when they are not already there"), remoteBox);
    remoteLayout->addWidget(m_remoteCreate);
    remoteLayout->addWidget(muted(tr("When off, qBitFusion only uses torrents that already exist in your account and never changes it. "
            "When on, it may create items in your account and start remote downloads."), remoteBox));
    layout->addWidget(remoteBox);

    // ---- limits
    m_limitsBox = new QGroupBox(tr("Limits"), content);
    auto *form = new QFormLayout(m_limitsBox);
    m_perTorrent = new QSpinBox(m_limitsBox);
    m_perTorrent->setRange(1, 32);
    m_global = new QSpinBox(m_limitsBox);
    m_global->setRange(1, 64);
    m_memory = new QSpinBox(m_limitsBox);
    m_memory->setRange(8, 1024);
    m_memory->setSuffix(tr(" MiB"));
    form->addRow(tr("Parallel HTTPS pieces per torrent:"), m_perTorrent);
    form->addRow(tr("Parallel HTTPS pieces in total:"), m_global);
    form->addRow(tr("Maximum memory for pieces in flight:"), m_memory);
    form->addRow(muted(tr("HTTPS downloads count towards your global download speed limit."), m_limitsBox));
    layout->addWidget(m_limitsBox);
    layout->addStretch();

    auto *scroll = new QScrollArea(this);
    scroll->setWidget(content);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(scroll);

    // ---- behaviour
    connect(m_enabled, &QCheckBox::toggled, this, [this] { updateEnabledState(); emit changed(); });
    connect(m_picker, &SourcePicker::selectionChanged, this, &OptionsPage::changed);
    connect(m_remoteCreate, &QCheckBox::toggled, this, &OptionsPage::changed);
    for (QSpinBox *spin : {m_perTorrent, m_global, m_memory})
        connect(spin, &QSpinBox::valueChanged, this, &OptionsPage::changed);

    auto *manager = Hybrid::HybridManager::instance();
    Hybrid::RealDebridProvider *rd = manager->realDebrid();
    connect(m_signIn, &QPushButton::clicked, this, [this, rd]
    {
        DeviceSignInDialog dialog {rd, this};
        dialog.exec();
        refreshAccount();
    });
    connect(m_token, &QPushButton::clicked, this, [this, rd]
    {
        bool ok = false;
        const QString token = QInputDialog::getText(this, tr("Private API token")
                , tr("Paste your Real-Debrid private API token.\nUse this only on your own computer and never share it.")
                , QLineEdit::Password, {}, &ok);
        if (ok && !token.trimmed().isEmpty())
            rd->usePrivateToken(token);
        refreshAccount();
    });
    connect(m_signOut, &QPushButton::clicked, this, [this, rd]
    {
        if (QMessageBox::question(this, tr("Sign out"), tr("Remove your Real-Debrid credentials from this computer?")) == QMessageBox::Yes)
            rd->signOut();
        refreshAccount();
    });
    connect(manager, &Hybrid::HybridManager::providerAccountChanged, this, &OptionsPage::refreshAccount);

    load();
}

void HybridGui::OptionsPage::load()
{
    const auto *manager = Hybrid::HybridManager::instance();
    const QSignalBlocker b1 {m_enabled};
    const QSignalBlocker b2 {m_picker};
    const QSignalBlocker b3 {m_remoteCreate};
    const QSignalBlocker b4 {m_perTorrent};
    const QSignalBlocker b5 {m_global};
    const QSignalBlocker b6 {m_memory};
    m_enabled->setChecked(manager->isEnabled());
    m_picker->setSelection(manager->defaultPolicy(), manager->defaultP2PConsent());
    m_remoteCreate->setChecked(manager->allowRemoteCreate());
    m_perTorrent->setValue(manager->maxPiecesPerTorrent());
    m_global->setValue(manager->maxGlobalPieces());
    m_memory->setValue(manager->maxBufferedMiB());
    refreshAccount();
    updateEnabledState();
}

void HybridGui::OptionsPage::save() const
{
    auto *manager = Hybrid::HybridManager::instance();
    manager->setDefaultPolicy(m_picker->policy());
    manager->setDefaultP2PConsent(m_picker->p2pConsent());
    manager->setAllowRemoteCreate(m_remoteCreate->isChecked());
    manager->setMaxPiecesPerTorrent(m_perTorrent->value());
    manager->setMaxGlobalPieces(m_global->value());
    manager->setMaxBufferedMiB(m_memory->value());
    manager->setEnabled(m_enabled->isChecked());
}

void HybridGui::OptionsPage::updateEnabledState()
{
    const bool on = m_enabled->isChecked();
    m_defaultsBox->setEnabled(on);
    m_limitsBox->setEnabled(on);
}

void HybridGui::OptionsPage::refreshAccount()
{
    const Hybrid::RealDebridProvider *rd = Hybrid::HybridManager::instance()->realDebrid();
    const Hybrid::AccountState state = rd->accountState();
    QString dot;
    QString text;
    switch (state)
    {
    case Hybrid::AccountState::Connected:
        dot = u"<span style='color:#2e9e4f;font-size:16px'>●</span>"_s;
        text = rd->accountName().isEmpty() ? tr("Connected")
                : tr("Connected as <b>%1</b>").arg(rd->accountName().toHtmlEscaped());
        if (rd->premiumDaysLeft() > 0)
            text += u" · "_s + tr("Premium: %n day(s) left", nullptr, rd->premiumDaysLeft());
        if (rd->isUsingPrivateToken())
            text += u" · "_s + tr("private token");
        break;
    case Hybrid::AccountState::AuthenticationRequired:
        dot = u"<span style='color:#d08a00;font-size:16px'>●</span>"_s;
        text = tr("Sign-in expired. Please sign in again.");
        break;
    case Hybrid::AccountState::Error:
        dot = u"<span style='color:#c0392b;font-size:16px'>●</span>"_s;
        text = tr("Account error");
        break;
    case Hybrid::AccountState::NotConfigured:
        dot = u"<span style='color:#8a8a8a;font-size:16px'>○</span>"_s;
        text = tr("Not signed in");
        break;
    }
    m_accountDot->setText(dot);
    m_accountText->setText(text);
    m_signOut->setVisible(state != Hybrid::AccountState::NotConfigured);
    m_signIn->setText((state == Hybrid::AccountState::Connected) ? tr("Switch account…") : tr("Sign in…"));
}

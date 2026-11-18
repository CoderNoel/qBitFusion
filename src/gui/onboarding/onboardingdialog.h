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

#include <QDialog>
#include <QList>
#include <QUrl>

class QButtonGroup;
class QLabel;
class QPushButton;
class QStackedWidget;
class QTimer;
class QVBoxLayout;

namespace Hybrid
{
    class RealDebridProvider;
}

namespace Onboarding
{
    class ConnectArt;
    class PageDots;

    // The tour is shown once per version. Raise it when the tour gains something every existing
    // user should see; leave it for ordinary updates, so that people who saw it are not asked again.
    inline constexpr int TOUR_VERSION = 1;

    bool shouldShow();
    void markShown();
}

// The welcome tour: what qBitFusion adds to qBittorrent, connecting a debrid account with a live
// status, and where everything is. It can be skipped at any point and opened again from Help.
class OnboardingDialog final : public QDialog
{
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(OnboardingDialog)

public:
    explicit OnboardingDialog(QWidget *parent = nullptr);
    ~OnboardingDialog() override;

    void done(int result) override;

private:
    enum ConnectState
    {
        CONNECT_IDLE,
        CONNECT_WAITING,
        CONNECT_LINKED,
        CONNECT_PROBLEM
    };

    QWidget *createWelcomePage();
    QWidget *createHybridPage();
    QWidget *createConnectPage();
    QWidget *createLinksPage();
    QWidget *createDonePage();

    void goTo(int index);
    void updateButtons();

    void startSignIn();
    void cancelSignIn();
    void onDeviceCode(const QString &userCode, const QUrl &verificationUrl, int expiresInSeconds);
    void onSignInFinished(bool ok, const QString &message);
    void tick();
    void refreshAccount(bool justLinked);
    void showConnectState(ConnectState state, bool animate);
    void refreshDonePage();

    Hybrid::RealDebridProvider *m_provider = nullptr;
    QStackedWidget *m_pages = nullptr;
    Onboarding::PageDots *m_dots = nullptr;
    QPushButton *m_skip = nullptr;
    QPushButton *m_back = nullptr;
    QPushButton *m_next = nullptr;
    int m_connectPage = -1;

    // Connect page
    Onboarding::ConnectArt *m_connectArt = nullptr;
    QStackedWidget *m_connectStates = nullptr;
    QLabel *m_codeHint = nullptr;
    QLabel *m_code = nullptr;
    QLabel *m_waitStatus = nullptr;
    QPushButton *m_openPage = nullptr;
    QPushButton *m_copyCode = nullptr;
    QLabel *m_accountLabel = nullptr;
    QLabel *m_sourcesLabel = nullptr;
    QPushButton *m_turnOn = nullptr;
    QWidget *m_defaultChoice = nullptr;
    QButtonGroup *m_defaultGroup = nullptr;
    QLabel *m_problemLabel = nullptr;
    QTimer *m_countdown = nullptr;
    QUrl m_verificationUrl;
    int m_remaining = 0;
    bool m_signingIn = false;
    bool m_checking = false;   // waiting for the account to answer after linking

    // Done page
    QVBoxLayout *m_doneList = nullptr;
};

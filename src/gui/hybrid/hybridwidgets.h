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
#include <QWidget>

#include "base/hybrid/hybridcoordinator.h"
#include "base/hybrid/hybridtypes.h"

class QButtonGroup;
class QCheckBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QRadioButton;
class QTimer;

namespace Hybrid
{
    class RealDebridProvider;
}

namespace HybridGui
{
    // The privacy notice from qbitfusion/docs/SPECIFICATION.md section 07, verbatim in meaning.
    QString p2pPrivacyNotice();

    // Explicit, informed consent before any torrent may fall back to the swarm (ADR-004).
    bool confirmP2PFallback(QWidget *parent);

    QIcon iconFor(Hybrid::TransportPolicy policy);
    QIcon iconFor(const Hybrid::HybridStatus &status);
    QString sourceText(const Hybrid::HybridStatus &status);
    QString sourceToolTip(const Hybrid::HybridStatus &status);

    // Three-way source selector with live explanation and a distinct P2P consent checkbox.
    class SourcePicker final : public QWidget
    {
        Q_OBJECT
        Q_DISABLE_COPY_MOVE(SourcePicker)

    public:
        explicit SourcePicker(QWidget *parent = nullptr);

        void setSelection(Hybrid::TransportPolicy policy, bool p2pConsent);
        Hybrid::TransportPolicy policy() const;
        bool p2pConsent() const;
        // Shown under "Debrid only" when the input is a bare magnet.
        void setMagnetWithoutMetadata(bool magnet);

    signals:
        void selectionChanged();

    private:
        void updateDetails();

        QButtonGroup *m_group = nullptr;
        QRadioButton *m_torrentOnly = nullptr;
        QRadioButton *m_debridOnly = nullptr;
        QRadioButton *m_debridPreferred = nullptr;
        QCheckBox *m_consent = nullptr;
        QLabel *m_details = nullptr;
        bool m_magnet = false;
    };

    // OAuth device-code sign-in (Real-Debrid's documented flow for open-source apps).
    class DeviceSignInDialog final : public QDialog
    {
        Q_OBJECT
        Q_DISABLE_COPY_MOVE(DeviceSignInDialog)

    public:
        explicit DeviceSignInDialog(Hybrid::RealDebridProvider *provider, QWidget *parent = nullptr);
        ~DeviceSignInDialog() override;

    private:
        void onCode(const QString &userCode, const QUrl &verificationUrl, int expiresInSeconds);
        void onFinished(bool ok, const QString &message);
        void tick();

        Hybrid::RealDebridProvider *m_provider = nullptr;
        QLabel *m_step = nullptr;
        QLabel *m_code = nullptr;
        QLabel *m_status = nullptr;
        QProgressBar *m_progress = nullptr;
        QPushButton *m_copy = nullptr;
        QPushButton *m_open = nullptr;
        QTimer *m_countdown = nullptr;
        QUrl m_url;
        int m_remaining = 0;
        bool m_done = false;
    };
}

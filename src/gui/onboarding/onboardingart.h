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

#include <QWidget>

class QVariantAnimation;

// Illustrations for the welcome tour. Drawn, not bitmaps, so they follow the light or dark scheme
// and the accent color. Animations run only while the illustration is visible.
namespace Onboarding
{
    QColor accentColor();
    QColor mutedColor(const QWidget *widget);
    QColor hairlineColor(const QWidget *widget);
    QColor toneColor(int tone);  // ModernAppearance::Tone, with palette fallbacks for the classic look

    // A monochrome icon from the app's set in any color.
    QPixmap glyph(const QString &iconId, const QColor &color, int size, qreal devicePixelRatio);

    // A looping animation of 0..1 that runs only while the widget is visible.
    class AnimatedArt : public QWidget
    {
        Q_OBJECT
        Q_DISABLE_COPY_MOVE(AnimatedArt)

    public:
        AnimatedArt(int durationMs, bool loop, QWidget *parent = nullptr);

        void restart();

    protected:
        qreal phase() const;
        void showEvent(QShowEvent *event) override;
        void hideEvent(QHideEvent *event) override;

    private:
        QVariantAnimation *m_animation = nullptr;
        qreal m_phase = 0;
    };

    // Two sources (the debrid service and the swarm) filling one torrent's pieces.
    class HybridArt final : public AnimatedArt
    {
        Q_OBJECT
        Q_DISABLE_COPY_MOVE(HybridArt)

    public:
        explicit HybridArt(QWidget *parent = nullptr);
        QSize sizeHint() const override;

    protected:
        void paintEvent(QPaintEvent *event) override;
    };

    // qBitFusion and the debrid service, with the link between them in its current state.
    class ConnectArt final : public AnimatedArt
    {
        Q_OBJECT
        Q_DISABLE_COPY_MOVE(ConnectArt)

    public:
        enum class State
        {
            NotLinked,
            Waiting,
            Linked,
            Problem
        };

        explicit ConnectArt(QWidget *parent = nullptr);
        QSize sizeHint() const override;

        void setState(State state, bool animate);
        void setProviderName(const QString &name);

    protected:
        void paintEvent(QPaintEvent *event) override;

    private:
        State m_state = State::NotLinked;
        QString m_providerName;
        QVariantAnimation *m_check = nullptr;
        qreal m_checkProgress = 1;
    };

    // A copied link becoming a download on the Links page.
    class LinksArt final : public AnimatedArt
    {
        Q_OBJECT
        Q_DISABLE_COPY_MOVE(LinksArt)

    public:
        explicit LinksArt(QWidget *parent = nullptr);
        QSize sizeHint() const override;

    protected:
        void paintEvent(QPaintEvent *event) override;
    };

    // A large check that draws itself once when shown.
    class DoneArt final : public AnimatedArt
    {
        Q_OBJECT
        Q_DISABLE_COPY_MOVE(DoneArt)

    public:
        explicit DoneArt(QWidget *parent = nullptr);
        QSize sizeHint() const override;

    protected:
        void paintEvent(QPaintEvent *event) override;
    };

    // Page position: one dot per page, the current one in the accent color. Dots can be clicked.
    class PageDots final : public QWidget
    {
        Q_OBJECT
        Q_DISABLE_COPY_MOVE(PageDots)

    public:
        PageDots(int count, QWidget *parent = nullptr);

        void setCurrent(int index);
        QSize sizeHint() const override;

    signals:
        void clicked(int index);

    protected:
        void paintEvent(QPaintEvent *event) override;
        void mouseReleaseEvent(QMouseEvent *event) override;

    private:
        QRect dotRect(int index) const;

        int m_count = 0;
        int m_current = 0;
    };
}

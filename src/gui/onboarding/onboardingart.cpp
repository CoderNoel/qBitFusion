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

#include "onboardingart.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include <QApplication>
#include <QFile>
#include <QFontDatabase>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSvgRenderer>
#include <QVariantAnimation>

#include "gui/modern/modernappearance.h"
#include "gui/uithememanager.h"

using namespace Qt::Literals::StringLiterals;

namespace
{
    constexpr int PIECES = 96;
    constexpr int COLUMNS = 16;
    constexpr int SERVICE_FIRST = 52;   // pieces 0..51 come from the service, then it drops out
    constexpr int SWARM_UNTIL = 70;     // the swarm fills 52..69, then the service is back

    qreal clamp01(const qreal value)
    {
        return std::clamp<qreal>(value, 0, 1);
    }

    qreal span(const qreal phase, const qreal from, const qreal to)
    {
        return clamp01((phase - from) / (to - from));
    }

    qreal easeOut(const qreal t)
    {
        return 1 - std::pow(1 - t, 3);
    }

    QFont smallFont(const QWidget *widget)
    {
        QFont font = widget->font();
        font.setPointSizeF(font.pointSizeF() * 0.9);
        return font;
    }

    // A check mark inside a circle of `radius`, drawn up to `progress` (0..1) of its length.
    void drawCheck(QPainter &p, const QPointF &center, const qreal radius, const qreal progress, const QColor &color, const qreal width)
    {
        if (progress <= 0)
            return;
        QPainterPath path;
        const QPointF a = center + QPointF(-radius * 0.42, radius * 0.02);
        const QPointF b = center + QPointF(-radius * 0.12, radius * 0.32);
        const QPointF c = center + QPointF(radius * 0.45, -radius * 0.30);
        const qreal first = QLineF(a, b).length();
        const qreal second = QLineF(b, c).length();
        const qreal drawn = progress * (first + second);
        path.moveTo(a);
        if (drawn <= first)
        {
            path.lineTo(a + ((b - a) * (drawn / first)));
        }
        else
        {
            path.lineTo(b);
            path.lineTo(b + ((c - b) * ((drawn - first) / second)));
        }
        p.setPen(QPen(color, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
    }

    void drawNode(QPainter &p, const QPointF &center, const qreal radius, const QString &iconId, const QColor &ring
            , const QColor &iconColor, const qreal dpr, const bool dashed = false)
    {
        QPen pen {ring, 1.5};
        if (dashed)
            pen.setDashPattern({3, 3});
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(center, radius, radius);
        const int size = static_cast<int>(radius * 0.95);
        p.drawPixmap(QPointF(center.x() - (size / 2.0), center.y() - (size / 2.0)), Onboarding::glyph(iconId, iconColor, size, dpr));
    }
}

QColor Onboarding::toneColor(const int tone)
{
    if (ModernAppearance::isActive())
        return ModernAppearance::color(static_cast<ModernAppearance::Tone>(tone));

    switch (static_cast<ModernAppearance::Tone>(tone))
    {
    case ModernAppearance::Tone::Accent:
        return QApplication::palette().color(QPalette::Highlight);
    case ModernAppearance::Tone::Info:
        return QColor(0x3B, 0x82, 0xF6);
    case ModernAppearance::Tone::Success:
        return QColor(0x22, 0xA0, 0x55);
    case ModernAppearance::Tone::Danger:
        return QColor(0xDC, 0x3C, 0x3C);
    case ModernAppearance::Tone::Muted:
        return QApplication::palette().color(QPalette::PlaceholderText);
    default:
        return QApplication::palette().color(QPalette::WindowText);
    }
}

QColor Onboarding::accentColor()
{
    return toneColor(static_cast<int>(ModernAppearance::Tone::Accent));
}

QColor Onboarding::mutedColor(const QWidget *widget)
{
    return widget->palette().color(QPalette::PlaceholderText);
}

QColor Onboarding::hairlineColor(const QWidget *widget)
{
    QColor color = widget->palette().color(QPalette::WindowText);
    color.setAlphaF(0.16);
    return color;
}

QPixmap Onboarding::glyph(const QString &iconId, const QColor &color, const int size, const qreal devicePixelRatio)
{
    QFile file {u":/qbitfusion/icons/"_s + iconId + u".svg"_s};
    QByteArray svg;
    if (file.open(QIODevice::ReadOnly))
        svg = file.readAll();
    if (svg.isEmpty())
        return UIThemeManager::instance()->getIcon(iconId).pixmap(QSize(size, size), devicePixelRatio);

    svg.replace("currentColor", color.name(QColor::HexRgb).toLatin1());
    QSvgRenderer renderer {svg};
    QPixmap pixmap {QSize(size, size) * devicePixelRatio};
    pixmap.fill(Qt::transparent);
    pixmap.setDevicePixelRatio(devicePixelRatio);
    QPainter painter {&pixmap};
    painter.setOpacity(color.alphaF());
    renderer.render(&painter, QRectF(0, 0, size, size));
    return pixmap;
}

// ------------------------------------------------------------------------------- AnimatedArt

Onboarding::AnimatedArt::AnimatedArt(const int durationMs, const bool loop, QWidget *parent)
    : QWidget(parent)
    , m_animation {new QVariantAnimation(this)}
{
    m_animation->setStartValue(0.0);
    m_animation->setEndValue(1.0);
    m_animation->setDuration(durationMs);
    m_animation->setLoopCount(loop ? -1 : 1);
    connect(m_animation, &QVariantAnimation::valueChanged, this, [this](const QVariant &value)
    {
        m_phase = value.toReal();
        update();
    });
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

qreal Onboarding::AnimatedArt::phase() const
{
    return m_phase;
}

void Onboarding::AnimatedArt::restart()
{
    m_animation->stop();
    m_phase = 0;
    if (isVisible())
        m_animation->start();
    update();
}

void Onboarding::AnimatedArt::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    restart();
}

void Onboarding::AnimatedArt::hideEvent(QHideEvent *event)
{
    m_animation->stop();
    QWidget::hideEvent(event);
}

// ------------------------------------------------------------------------------- HybridArt

Onboarding::HybridArt::HybridArt(QWidget *parent)
    : AnimatedArt(9000, true, parent)
{
    setFixedHeight(sizeHint().height());
}

QSize Onboarding::HybridArt::sizeHint() const
{
    return {560, 196};
}

void Onboarding::HybridArt::paintEvent([[maybe_unused]] QPaintEvent *event)
{
    QPainter p {this};
    p.setRenderHint(QPainter::Antialiasing);
    const qreal dpr = devicePixelRatioF();
    const qreal t = phase();

    const QColor accent = accentColor();
    const QColor info = toneColor(static_cast<int>(ModernAppearance::Tone::Info));
    const QColor success = toneColor(static_cast<int>(ModernAppearance::Tone::Success));
    const QColor muted = mutedColor(this);
    const QColor hairline = hairlineColor(this);
    const QColor text = palette().color(QPalette::WindowText);

    // Timeline: the service fills pieces, drops out, the swarm takes over, the service comes back.
    qreal filled = 0;
    if (t < 0.42)
        filled = SERVICE_FIRST * span(t, 0.06, 0.42);
    else if (t < 0.48)
        filled = SERVICE_FIRST;
    else if (t < 0.64)
        filled = SERVICE_FIRST + ((SWARM_UNTIL - SERVICE_FIRST) * span(t, 0.48, 0.64));
    else
        filled = SWARM_UNTIL + ((PIECES - SWARM_UNTIL) * span(t, 0.64, 0.84));
    const int count = static_cast<int>(std::floor(filled));
    const bool serviceDown = (t >= 0.42) && (t < 0.64);
    const bool serviceActive = ((t >= 0.06) && (t < 0.42)) || ((t >= 0.64) && (t < 0.84));
    const bool swarmActive = (t >= 0.48) && (t < 0.64);
    const bool done = (count >= PIECES);

    const qreal x0 = (width() / 2.0) - 260;
    const QPointF service {x0 + 40, 50};
    const QPointF swarm {x0 + 40, 132};
    const qreal radius = 24;

    const qreal cell = 16;
    const qreal gap = 4;
    const qreal gridLeft = x0 + 204;
    const qreal gridTop = 26;
    const int rows = PIECES / COLUMNS;
    const qreal gridHeight = (rows * (cell + gap)) - gap;
    const QPointF gridEntry {gridLeft - 10, gridTop + (gridHeight / 2)};

    // Connections from each source to the file.
    const auto drawLink = [&](const QPointF &from, const bool active, const QColor &color)
    {
        QPainterPath path;
        const QPointF start = from + QPointF(radius + 6, 0);
        path.moveTo(start);
        path.cubicTo(start + QPointF(60, 0), gridEntry - QPointF(60, 0), gridEntry);
        QPen pen {active ? color : hairline, active ? 2.0 : 1.25};
        pen.setDashPattern(active ? QList<qreal> {4, 3} : QList<qreal> {2, 3});
        pen.setDashOffset(active ? -(t * 9000 / 45) : 0);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
    };
    drawLink(service, serviceActive, accent);
    drawLink(swarm, swarmActive, info);

    drawNode(p, service, radius, u"network-server"_s, (serviceActive ? accent : hairline)
            , (serviceDown ? muted : text), dpr, serviceDown);
    drawNode(p, swarm, radius, u"peers"_s, (swarmActive ? info : hairline), text, dpr);

    p.setFont(smallFont(this));
    p.setPen(serviceDown ? muted : text);
    p.drawText(QRectF(service.x() - 60, service.y() + radius + 4, 120, 18), Qt::AlignHCenter | Qt::AlignTop
            , serviceDown ? tr("Service unavailable") : tr("Your debrid service"));
    p.setPen(text);
    p.drawText(QRectF(swarm.x() - 60, swarm.y() + radius + 4, 120, 18), Qt::AlignHCenter | Qt::AlignTop, tr("Torrent swarm"));

    // The file, one square per piece.
    QColor empty = text;
    empty.setAlphaF(0.07);
    for (int i = 0; i < PIECES; ++i)
    {
        const QRectF r {gridLeft + ((i % COLUMNS) * (cell + gap)), gridTop + ((i / COLUMNS) * (cell + gap)), cell, cell};
        p.setPen(Qt::NoPen);
        if (i < count)
            p.setBrush(((i >= SERVICE_FIRST) && (i < SWARM_UNTIL)) ? info : accent);
        else
            p.setBrush(empty);
        p.drawRoundedRect(r, 3, 3);
    }

    // Caption and legend under the file.
    const qreal captionY = gridTop + gridHeight + 14;
    const QRectF captionRect {gridLeft, captionY, 170, 18};
    if (done)
    {
        p.drawPixmap(QPointF(gridLeft, captionY + 1), glyph(u"checked-completed"_s, success, 16, dpr));
        p.setPen(success);
        p.drawText(captionRect.adjusted(22, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, tr("All pieces checked"));
    }
    else
    {
        p.setPen(muted);
        p.drawText(captionRect, Qt::AlignLeft | Qt::AlignVCenter, tr("%1 of %2 pieces checked").arg(count).arg(PIECES));
    }

    const qreal legendRight = gridLeft + (COLUMNS * (cell + gap)) - gap;
    const QString fromPeers = tr("From peers");
    const QString fromService = tr("From your service");
    const QFontMetricsF metrics {p.font()};
    qreal x = legendRight - metrics.horizontalAdvance(fromPeers);
    p.setPen(muted);
    p.drawText(QPointF(x, captionY + 13), fromPeers);
    p.setPen(Qt::NoPen);
    p.setBrush(info);
    p.drawRoundedRect(QRectF(x - 14, captionY + 4, 9, 9), 2, 2);
    x -= 14 + 16 + metrics.horizontalAdvance(fromService);
    p.setPen(muted);
    p.drawText(QPointF(x, captionY + 13), fromService);
    p.setPen(Qt::NoPen);
    p.setBrush(accent);
    p.drawRoundedRect(QRectF(x - 14, captionY + 4, 9, 9), 2, 2);
}

// ------------------------------------------------------------------------------- ConnectArt

Onboarding::ConnectArt::ConnectArt(QWidget *parent)
    : AnimatedArt(1400, true, parent)
    , m_providerName {u"Real-Debrid"_s}
    , m_check {new QVariantAnimation(this)}
{
    setFixedHeight(sizeHint().height());
    m_check->setStartValue(0.0);
    m_check->setEndValue(1.0);
    m_check->setDuration(520);
    m_check->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_check, &QVariantAnimation::valueChanged, this, [this](const QVariant &value)
    {
        m_checkProgress = value.toReal();
        update();
    });
}

QSize Onboarding::ConnectArt::sizeHint() const
{
    return {560, 128};
}

void Onboarding::ConnectArt::setState(const State state, const bool animate)
{
    m_state = state;
    if ((state == State::Linked) && animate)
    {
        m_checkProgress = 0;
        m_check->start();
    }
    else
    {
        m_checkProgress = 1;
    }
    update();
}

void Onboarding::ConnectArt::setProviderName(const QString &name)
{
    m_providerName = name;
    update();
}

void Onboarding::ConnectArt::paintEvent([[maybe_unused]] QPaintEvent *event)
{
    QPainter p {this};
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    const qreal dpr = devicePixelRatioF();

    const QColor accent = accentColor();
    const QColor success = toneColor(static_cast<int>(ModernAppearance::Tone::Success));
    const QColor danger = toneColor(static_cast<int>(ModernAppearance::Tone::Danger));
    const QColor hairline = hairlineColor(this);
    const QColor text = palette().color(QPalette::WindowText);
    const QColor inverse = palette().color(QPalette::Base);

    const qreal cx = width() / 2.0;
    const qreal y = 46;
    const QPointF app {cx - 130, y};
    const QPointF provider {cx + 130, y};
    const qreal radius = 30;

    // The link between the two.
    const QPointF from = app + QPointF(radius + 10, 0);
    const QPointF to = provider - QPointF(radius + 10, 0);
    QPen pen;
    pen.setCapStyle(Qt::RoundCap);
    switch (m_state)
    {
    case State::NotLinked:
        pen = QPen(hairline, 1.5);
        pen.setDashPattern({2, 4});
        break;
    case State::Waiting:
        pen = QPen(accent, 2);
        pen.setDashPattern({4, 4});
        pen.setDashOffset(-phase() * 16);
        break;
    case State::Linked:
        pen = QPen(accent, 2);
        break;
    case State::Problem:
        pen = QPen(danger, 1.5);
        pen.setDashPattern({2, 4});
        break;
    }
    p.setPen(pen);
    if (m_state == State::Linked)
    {
        // The line grows from both ends towards the middle as the check appears.
        const QPointF mid = (from + to) / 2;
        const qreal grow = std::min<qreal>(1, m_checkProgress * 1.6);
        p.drawLine(from, from + ((mid - from) * grow));
        p.drawLine(to, to + ((mid - to) * grow));
    }
    else
    {
        p.drawLine(from, to);
    }

    // qBitFusion's mark on the left.
    const QPixmap mark = QIcon(u":/qbitfusion/brand/qbitfusion.png"_s).pixmap(QSize(52, 52), dpr);
    p.drawPixmap(QPointF(app.x() - 26, app.y() - 26), mark);

    // The provider on the right.
    drawNode(p, provider, radius, u"network-server"_s, ((m_state == State::Linked) ? accent : hairline), text, dpr);

    p.setFont(smallFont(this));
    p.setPen(text);
    p.drawText(QRectF(app.x() - 70, y + radius + 8, 140, 18), Qt::AlignHCenter | Qt::AlignTop, u"qBitFusion"_s);
    p.drawText(QRectF(provider.x() - 70, y + radius + 8, 140, 18), Qt::AlignHCenter | Qt::AlignTop, m_providerName);

    // The state in the middle.
    const QPointF mid = (from + to) / 2;
    switch (m_state)
    {
    case State::NotLinked:
        {
            QColor fill = palette().color(QPalette::Window);
            p.setPen(QPen(hairline, 1.5));
            p.setBrush(fill);
            p.drawEllipse(mid, 9, 9);
        }
        break;
    case State::Waiting:
        {
            // Three dots that take turns, like a request in progress.
            for (int i = 0; i < 3; ++i)
            {
                const qreal wave = 0.5 + (0.5 * std::sin((phase() * 2 * std::numbers::pi) - (i * 0.9)));
                QColor dot = accent;
                dot.setAlphaF(0.35 + (0.65 * wave));
                p.setPen(Qt::NoPen);
                p.setBrush(palette().color(QPalette::Window));
                p.drawEllipse(mid + QPointF((i - 1) * 14, 0), 6, 6);
                p.setBrush(dot);
                p.drawEllipse(mid + QPointF((i - 1) * 14, 0), 4, 4);
            }
        }
        break;
    case State::Linked:
        {
            const qreal scale = 0.7 + (0.3 * easeOut(clamp01(m_checkProgress * 1.4)));
            const qreal r = 15 * scale;
            p.setPen(Qt::NoPen);
            p.setBrush(success);
            p.drawEllipse(mid, r, r);
            drawCheck(p, mid, r, clamp01((m_checkProgress - 0.25) / 0.75), inverse, 2.4);
        }
        break;
    case State::Problem:
        {
            p.setPen(Qt::NoPen);
            p.setBrush(danger);
            p.drawEllipse(mid, 12, 12);
            p.setPen(QPen(inverse, 2.2, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(mid + QPointF(0, -5), mid + QPointF(0, 1.5));
            p.drawPoint(mid + QPointF(0, 5.5));
        }
        break;
    }
}

// ------------------------------------------------------------------------------- LinksArt

Onboarding::LinksArt::LinksArt(QWidget *parent)
    : AnimatedArt(6000, true, parent)
{
    setFixedHeight(sizeHint().height());
}

QSize Onboarding::LinksArt::sizeHint() const
{
    return {560, 136};
}

void Onboarding::LinksArt::paintEvent([[maybe_unused]] QPaintEvent *event)
{
    QPainter p {this};
    p.setRenderHint(QPainter::Antialiasing);
    const qreal dpr = devicePixelRatioF();
    const qreal t = phase();

    const QColor accent = accentColor();
    const QColor success = toneColor(static_cast<int>(ModernAppearance::Tone::Success));
    const QColor muted = mutedColor(this);
    const QColor hairline = hairlineColor(this);
    const QColor text = palette().color(QPalette::WindowText);

    const qreal x0 = (width() / 2.0) - 260;
    const QString name = u"Big.Movie.2024.mkv"_s;

    // The copied link.
    const QRectF chip {x0, 50, 212, 36};
    const qreal pasted = easeOut(span(t, 0.0, 0.12));
    QColor chipBorder = hairline;
    if (t < 0.18)
        chipBorder = accent;
    p.setPen(QPen(chipBorder, 1.25));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(chip.translated(0, (1 - pasted) * 6), 6, 6);
    p.drawPixmap(QPointF(chip.left() + 10, chip.top() + 10 + ((1 - pasted) * 6)), glyph(u"insert-link"_s, text, 16, dpr));
    const QFont linkFont = smallFont(this);
    p.setFont(linkFont);
    p.setPen(muted);
    const QRectF linkText = chip.adjusted(34, 0, -10, 0).translated(0, (1 - pasted) * 6);
    p.drawText(linkText, Qt::AlignVCenter | Qt::AlignLeft
            , QFontMetricsF(linkFont).elidedText(u"https://…/"_s + name, Qt::ElideMiddle, linkText.width()));

    // The arrow: the link is checked and becomes a download.
    const qreal arrow = easeOut(span(t, 0.08, 0.2));
    const QPointF a1 {chip.right() + 12, chip.center().y()};
    const QPointF a2 {chip.right() + 12 + (44 * arrow), chip.center().y()};
    if (arrow > 0)
    {
        p.setPen(QPen(accent, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawLine(a1, a2);
        if (arrow > 0.6)
        {
            p.drawLine(a2, a2 + QPointF(-6, -5));
            p.drawLine(a2, a2 + QPointF(-6, 5));
        }
    }

    // The row on the Links page.
    const QRectF panel {x0 + 280, 26, 240, 84};
    const qreal shown = easeOut(span(t, 0.16, 0.28));
    p.setOpacity(shown);
    p.setPen(QPen(hairline, 1.25));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(panel, 6, 6);

    const qreal progress = span(t, 0.28, 0.82);
    const bool complete = (progress >= 1);
    p.drawPixmap(QPointF(panel.left() + 14, panel.top() + 14)
            , glyph(complete ? u"checked-completed"_s : u"download"_s, (complete ? success : accent), 18, dpr));
    QFont bold = font();
    bold.setWeight(QFont::DemiBold);
    p.setFont(bold);
    p.setPen(text);
    p.drawText(QRectF(panel.left() + 40, panel.top() + 12, panel.width() - 54, 22), Qt::AlignVCenter | Qt::AlignLeft, name);

    const QRectF track {panel.left() + 14, panel.top() + 46, panel.width() - 28, 6};
    QColor trackColor = text;
    trackColor.setAlphaF(0.12);
    p.setPen(Qt::NoPen);
    p.setBrush(trackColor);
    p.drawRoundedRect(track, 3, 3);
    if (progress > 0)
    {
        p.setBrush(complete ? success : accent);
        p.drawRoundedRect(QRectF(track.left(), track.top(), std::max<qreal>(6, track.width() * progress), track.height()), 3, 3);
    }

    p.setFont(smallFont(this));
    p.setPen(complete ? success : muted);
    const QString status = complete ? tr("Completed")
            : tr("Downloading · %1%").arg(static_cast<int>(progress * 100));
    p.drawText(QRectF(track.left(), track.bottom() + 6, track.width(), 18), Qt::AlignLeft | Qt::AlignVCenter, status);
    p.setPen(muted);
    p.drawText(QRectF(track.left(), track.bottom() + 6, track.width(), 18), Qt::AlignRight | Qt::AlignVCenter, tr("Links"));
    p.setOpacity(1);
}

// ------------------------------------------------------------------------------- DoneArt

Onboarding::DoneArt::DoneArt(QWidget *parent)
    : AnimatedArt(900, false, parent)
{
    setFixedHeight(sizeHint().height());
}

QSize Onboarding::DoneArt::sizeHint() const
{
    return {560, 96};
}

void Onboarding::DoneArt::paintEvent([[maybe_unused]] QPaintEvent *event)
{
    QPainter p {this};
    p.setRenderHint(QPainter::Antialiasing);
    const qreal t = phase();
    const QPointF center {width() / 2.0, 46};
    const qreal r = 34 * (0.8 + (0.2 * easeOut(span(t, 0, 0.45))));
    QColor ring = toneColor(static_cast<int>(ModernAppearance::Tone::Success));
    p.setPen(Qt::NoPen);
    p.setBrush(ring);
    p.drawEllipse(center, r, r);
    drawCheck(p, center, r, easeOut(span(t, 0.3, 1)), palette().color(QPalette::Base), 4);
}

// ------------------------------------------------------------------------------- PageDots

Onboarding::PageDots::PageDots(const int count, QWidget *parent)
    : QWidget(parent)
    , m_count {count}
{
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

void Onboarding::PageDots::setCurrent(const int index)
{
    m_current = index;
    update();
}

QSize Onboarding::PageDots::sizeHint() const
{
    return {(m_count * 16) + 4, 20};
}

QRect Onboarding::PageDots::dotRect(const int index) const
{
    return {2 + (index * 16), 0, 16, height()};
}

void Onboarding::PageDots::paintEvent([[maybe_unused]] QPaintEvent *event)
{
    QPainter p {this};
    p.setRenderHint(QPainter::Antialiasing);
    for (int i = 0; i < m_count; ++i)
    {
        const QPointF c = QRectF(dotRect(i)).center();
        p.setPen(Qt::NoPen);
        if (i == m_current)
        {
            p.setBrush(accentColor());
            p.drawEllipse(c, 4, 4);
        }
        else
        {
            QColor dot = palette().color(QPalette::WindowText);
            dot.setAlphaF(0.25);
            p.setBrush(dot);
            p.drawEllipse(c, 3, 3);
        }
    }
}

void Onboarding::PageDots::mouseReleaseEvent(QMouseEvent *event)
{
    for (int i = 0; i < m_count; ++i)
    {
        if (dotRect(i).contains(event->position().toPoint()))
        {
            emit clicked(i);
            return;
        }
    }
}

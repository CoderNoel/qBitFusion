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

#include "modernappearance.h"

#include <algorithm>

#include <QApplication>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QHash>
#include <QEvent>
#include <QIconEngine>
#include <QImage>
#include <QMenu>
#include <QPainter>
#include <QPixmapCache>
#include <QProxyStyle>
#include <QSvgRenderer>

#include "base/settingvalue.h"

using namespace Qt::Literals::StringLiterals;

// Resources live in the static qbt_gui library and must be initialised explicitly.
static void initModernResources()
{
    Q_INIT_RESOURCE(modern);
}

namespace
{
    using ModernAppearance::Tone;

    const QString KEY_MODERN = u"Appearance/ModernUI"_s;
    bool g_active = false;
    bool g_dark = false;

    struct Scheme
    {
        QColor window, base, alternate, button, text, placeholder, mid, midlight, dark, light, shadow;
        QColor highlight, highlightedText, link, tooltipBase, tooltipText, disabledText;
        QColor icon, info, success, warning, danger, special;
    };

    // True-neutral surfaces and one accent ("ember" orange) that is used sparingly: progress, focus
    // and "on" states. Primary buttons are inverse-neutral, not accent-coloured.
    // Text colours meet WCAG AA against their surfaces.
    const Scheme DARK {
        .window = QColor(0x15, 0x15, 0x15), .base = QColor(0x1b, 0x1b, 0x1b), .alternate = QColor(0x1f, 0x1f, 0x1f)
        , .button = QColor(0x24, 0x24, 0x24), .text = QColor(0xec, 0xec, 0xec), .placeholder = QColor(0x8f, 0x8f, 0x8f)
        , .mid = QColor(0x2c, 0x2c, 0x2c), .midlight = QColor(0x25, 0x25, 0x25), .dark = QColor(0x47, 0x47, 0x47)
        , .light = QColor(0x33, 0x33, 0x33), .shadow = QColor(0x00, 0x00, 0x00)
        , .highlight = QColor(0xff, 0x6a, 0x3d), .highlightedText = QColor(0x1a, 0x0c, 0x06), .link = QColor(0xff, 0x8a, 0x65)
        , .tooltipBase = QColor(0x2a, 0x2a, 0x2a), .tooltipText = QColor(0xec, 0xec, 0xec), .disabledText = QColor(0x5e, 0x5e, 0x5e)
        , .icon = QColor(0xbd, 0xbd, 0xbd), .info = QColor(0x58, 0xa6, 0xff), .success = QColor(0x3f, 0xb9, 0x50)
        , .warning = QColor(0xd2, 0x99, 0x22), .danger = QColor(0xf8, 0x51, 0x49), .special = QColor(0x39, 0xc5, 0xcf)
    };

    const Scheme LIGHT {
        .window = QColor(0xf5, 0xf5, 0xf3), .base = QColor(0xff, 0xff, 0xff), .alternate = QColor(0xfa, 0xfa, 0xf9)
        , .button = QColor(0xff, 0xff, 0xff), .text = QColor(0x1a, 0x1a, 0x1a), .placeholder = QColor(0x70, 0x70, 0x6c)
        , .mid = QColor(0xe3, 0xe3, 0xdf), .midlight = QColor(0xec, 0xec, 0xe9), .dark = QColor(0xb8, 0xb8, 0xb3)
        , .light = QColor(0xff, 0xff, 0xff), .shadow = QColor(0xa3, 0xa3, 0x9e)
        , .highlight = QColor(0xe2, 0x4a, 0x17), .highlightedText = QColor(0xff, 0xff, 0xff), .link = QColor(0xc2, 0x3d, 0x10)
        , .tooltipBase = QColor(0x1f, 0x1f, 0x1f), .tooltipText = QColor(0xf5, 0xf5, 0xf3), .disabledText = QColor(0xa8, 0xa8, 0xa3)
        , .icon = QColor(0x45, 0x45, 0x42), .info = QColor(0x1f, 0x6f, 0xeb), .success = QColor(0x1a, 0x7f, 0x37)
        , .warning = QColor(0x9a, 0x67, 0x00), .danger = QColor(0xcf, 0x22, 0x2e), .special = QColor(0x1b, 0x7c, 0x83)
    };

    const QString KEY_ACCENT = u"Appearance/AccentColor"_s;
    QColor g_accentBase;   // invalid: use the default accent
    Scheme g_schemes[2];   // [light, dark], resolved with the accent
    bool g_schemesReady = false;

    Scheme resolveScheme(const bool dark)
    {
        Scheme s = dark ? DARK : LIGHT;
        const QColor base = g_accentBase.isValid() ? g_accentBase : ModernAppearance::defaultAccent();

        // Keep the chosen hue but hold its lightness in a range that stays legible on each surface.
        float hue = 0;
        float saturation = 0;
        float lightness = 0;
        base.getHslF(&hue, &saturation, &lightness);
        lightness = dark ? std::clamp(lightness, 0.60f, 0.74f) : std::clamp(lightness, 0.38f, 0.52f);
        s.highlight = QColor::fromHslF(hue, saturation, lightness);
        s.link = QColor::fromHslF(hue, saturation, (dark ? std::min(lightness + 0.08f, 0.82f) : std::max(lightness - 0.06f, 0.30f)));

        const int brightness = ((s.highlight.red() * 299) + (s.highlight.green() * 587) + (s.highlight.blue() * 114)) / 1000;
        s.highlightedText = (brightness > 150) ? QColor(0x10, 0x12, 0x16) : QColor(Qt::white);
        return s;
    }

    void rebuildSchemes()
    {
        g_schemes[0] = resolveScheme(false);
        g_schemes[1] = resolveScheme(true);
        g_schemesReady = true;
    }

    const Scheme &schemeFor(const bool dark)
    {
        if (!g_schemesReady)
        {
            g_accentBase = QColor::fromString(SettingValue<QString>(KEY_ACCENT).get());
            rebuildSchemes();
        }
        return g_schemes[dark ? 1 : 0];
    }

    const Scheme &scheme()
    {
        return schemeFor(g_dark);
    }

    QString rgba(const QColor &color, const qreal alpha)
    {
        return u"rgba(%1, %2, %3, %4)"_s.arg(color.red()).arg(color.green()).arg(color.blue()).arg(alpha);
    }

    // Semantic tint per theme icon id. Everything not listed uses the plain icon colour.
    Tone toneFor(const QString &iconId)
    {
        static const QHash<QString, Tone> tones {
            // Only state indicators carry colour. Icons of actions (menus, buttons) stay neutral.
            {u"downloading"_s, Tone::Info}, {u"filter-active"_s, Tone::Info},
            {u"upload"_s, Tone::Success}, {u"checked-completed"_s, Tone::Success}, {u"connected"_s, Tone::Success},
            {u"stalledUP"_s, Tone::Muted}, {u"stopped"_s, Tone::Muted}, {u"paused"_s, Tone::Muted},
            {u"filter-inactive"_s, Tone::Muted},
            {u"stalledDL"_s, Tone::Warning}, {u"queued"_s, Tone::Warning}, {u"filter-stalled"_s, Tone::Warning},
            {u"firewalled"_s, Tone::Warning}, {u"dialog-warning"_s, Tone::Warning}, {u"tracker-warning"_s, Tone::Warning},
            {u"slow"_s, Tone::Warning},
            {u"error"_s, Tone::Danger}, {u"tracker-error"_s, Tone::Danger}, {u"disconnected"_s, Tone::Danger}
        };
        return tones.value(iconId, Tone::Text);
    }

    // Renders a monochrome SVG in the colour of its tone for the scheme that is current when painted,
    // so icons already handed out follow light/dark switches without being recreated.
    class TintedSvgEngine final : public QIconEngine
    {
    public:
        TintedSvgEngine(const QString &id, const QByteArray &svg, const Tone tone, const QColor &fixedColor = {})
            : m_id {id}, m_svg {svg}, m_tone {tone}, m_fixedColor {fixedColor}
        {
        }

        QIconEngine *clone() const override
        {
            return new TintedSvgEngine(*this);
        }

        QString key() const override
        {
            return u"QbfTintedSvg"_s;
        }

        bool isNull() override
        {
            return m_svg.isEmpty();
        }

        QSize actualSize(const QSize &size, QIcon::Mode, QIcon::State) override
        {
            // The glyphs are square; a wider or taller request gets the largest square that fits.
            const int side = std::min(size.width(), size.height());
            return {side, side};
        }

        QList<QSize> availableSizes(QIcon::Mode, QIcon::State) override
        {
            return {{16, 16}, {20, 20}, {24, 24}, {32, 32}, {48, 48}};
        }

        void paint(QPainter *painter, const QRect &rect, const QIcon::Mode mode, const QIcon::State state) override
        {
            const qreal scale = painter->device() ? painter->device()->devicePixelRatioF() : qApp->devicePixelRatio();
            painter->drawPixmap(rect, scaledPixmap(rect.size(), mode, state, scale));
        }

        QPixmap pixmap(const QSize &size, const QIcon::Mode mode, const QIcon::State state) override
        {
            return scaledPixmap(size, mode, state, 1.0);
        }

        QPixmap scaledPixmap(const QSize &size, const QIcon::Mode mode, [[maybe_unused]] const QIcon::State state, const qreal scale) override
        {
            if (size.isEmpty())
                return {};

            const QColor tint = m_fixedColor.isValid() ? m_fixedColor : ModernAppearance::color(m_tone);
            const QSize devSize = (QSizeF(size) * scale).toSize();
            const QString cacheKey = u"qbf-icon:%1:%2x%3:%4:%5"_s.arg(m_id).arg(devSize.width()).arg(devSize.height())
                    .arg(static_cast<int>(mode)).arg(tint.rgba());

            QPixmap result;
            if (!QPixmapCache::find(cacheKey, &result))
            {
                QByteArray data = m_svg;
                data.replace("currentColor", tint.name(QColor::HexRgb).toLatin1());
                QSvgRenderer renderer {data};

                QImage image {devSize, QImage::Format_ARGB32_Premultiplied};
                image.fill(Qt::transparent);
                {
                    QPainter painter {&image};
                    painter.setRenderHint(QPainter::Antialiasing);
                    if (mode == QIcon::Disabled)
                        painter.setOpacity(0.38);
                    const qreal side = std::min(devSize.width(), devSize.height());
                    renderer.render(&painter, QRectF((devSize.width() - side) / 2, (devSize.height() - side) / 2, side, side));
                }
                result = QPixmap::fromImage(image);
                QPixmapCache::insert(cacheKey, result);
            }
            result.setDevicePixelRatio(scale);
            return result;
        }

    private:
        QString m_id;
        QByteArray m_svg;
        Tone m_tone;
        QColor m_fixedColor;
    };

    QByteArray readResource(const QString &path)
    {
        QFile file {path};
        if (!file.open(QIODevice::ReadOnly))
            return {};
        return file.readAll();
    }

    // Fusion underlines menu and button mnemonics all the time, which looks dated; they still work.
    class ModernStyle final : public QProxyStyle
    {
    public:
        using QProxyStyle::QProxyStyle;

        int styleHint(const StyleHint hint, const QStyleOption *option, const QWidget *widget, QStyleHintReturn *returnData) const override
        {
            if (hint == SH_UnderlineShortcut)
                return 0;
            return QProxyStyle::styleHint(hint, option, widget, returnData);
        }
    };

    // Rounded menus need a translucent, frameless popup window; otherwise the corners show black.
    class PopupPolisher final : public QObject
    {
    public:
        using QObject::QObject;

    private:
        bool eventFilter(QObject *watched, QEvent *event) override
        {
            if (event->type() == QEvent::Polish)
            {
                if (auto *menu = qobject_cast<QMenu *>(watched); menu && !menu->testAttribute(Qt::WA_TranslucentBackground))
                {
                    menu->setWindowFlags(menu->windowFlags() | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
                    menu->setAttribute(Qt::WA_TranslucentBackground);
                }
            }
            return QObject::eventFilter(watched, event);
        }
    };

    void applyFont()
    {
        // Segoe UI Variable (Windows 11) has proper optical sizes; fall back to the platform font.
        QFont font = QApplication::font();
        const QStringList families = QFontDatabase::families();
        for (const QString &family : {u"Segoe UI Variable Text"_s, u"Segoe UI"_s})
        {
            if (families.contains(family))
            {
                font.setFamily(family);
                break;
            }
        }
        font.setPointSizeF(std::max(font.pointSizeF(), 9.5));
        font.setHintingPreference(QFont::PreferNoHinting);
        QApplication::setFont(font);
    }
}

bool ModernAppearance::isEnabledSetting()
{
    return SettingValue<bool>(KEY_MODERN).get(true);
}

void ModernAppearance::setEnabledSetting(const bool enabled)
{
    SettingValue<bool> setting {KEY_MODERN};
    setting = enabled;
}

QStyle *ModernAppearance::createStyle(const QString &baseStyleName)
{
    return new ModernStyle(baseStyleName);
}

QColor ModernAppearance::defaultAccent()
{
    return {0xff, 0x5b, 0x2e};   // ember orange, the colour of the logo
}

QColor ModernAppearance::accentSetting()
{
    schemeFor(g_dark);   // makes sure the stored value is loaded
    return g_accentBase;
}

void ModernAppearance::setAccentSetting(const QColor &color)
{
    schemeFor(g_dark);
    if (color == g_accentBase)
        return;

    g_accentBase = color;
    SettingValue<QString> setting {KEY_ACCENT};
    setting = color.isValid() ? color.name(QColor::HexRgb) : QString();
    rebuildSchemes();
    if (g_active)
        apply(g_dark);
}

bool ModernAppearance::isActive()
{
    return g_active;
}

bool ModernAppearance::isDark()
{
    return g_dark;
}

qreal ModernAppearance::selectionAlpha()
{
    return g_dark ? 0.20 : 0.14;
}

QColor ModernAppearance::color(const Tone tone)
{
    const Scheme &s = scheme();
    switch (tone)
    {
    case Tone::Text:
        return s.icon;
    case Tone::Muted:
        return s.placeholder;
    case Tone::Accent:
        return s.highlight;
    case Tone::Info:
        return s.info;
    case Tone::Success:
        return s.success;
    case Tone::Warning:
        return s.warning;
    case Tone::Danger:
        return s.danger;
    case Tone::Special:
        return s.special;
    case Tone::Inverse:
        return g_dark ? QColor(0x15, 0x15, 0x15) : QColor(Qt::white);
    }
    return s.icon;
}

QColor ModernAppearance::themeColor(const QString &colorId)
{
    // Torrent states use the same few tones as their icons, so the Status column and the icon agree.
    static const QHash<QString, Tone> tones {
        {u"TransferList.Downloading"_s, Tone::Info}, {u"TransferList.ForcedDownloading"_s, Tone::Info},
        {u"TransferList.DownloadingMetadata"_s, Tone::Info}, {u"TransferList.ForcedDownloadingMetadata"_s, Tone::Info},
        {u"TransferList.StalledDownloading"_s, Tone::Warning},
        {u"TransferList.Uploading"_s, Tone::Success}, {u"TransferList.ForcedUploading"_s, Tone::Success},
        {u"TransferList.StalledUploading"_s, Tone::Muted},
        {u"TransferList.QueuedDownloading"_s, Tone::Muted}, {u"TransferList.QueuedUploading"_s, Tone::Muted},
        {u"TransferList.StoppedDownloading"_s, Tone::Muted}, {u"TransferList.StoppedUploading"_s, Tone::Muted},
        {u"TransferList.CheckingDownloading"_s, Tone::Special}, {u"TransferList.CheckingUploading"_s, Tone::Special},
        {u"TransferList.CheckingResumeData"_s, Tone::Special}, {u"TransferList.Moving"_s, Tone::Special},
        {u"TransferList.MissingFiles"_s, Tone::Danger}, {u"TransferList.Error"_s, Tone::Danger},
        {u"Log.Info"_s, Tone::Info}, {u"Log.Warning"_s, Tone::Warning}, {u"Log.Critical"_s, Tone::Danger},
        {u"Log.BannedPeer"_s, Tone::Danger}, {u"Log.TimeStamp"_s, Tone::Muted},
        {u"PiecesBar.Piece"_s, Tone::Accent}, {u"ProgressBar"_s, Tone::Accent}
    };

    if (const auto iter = tones.constFind(colorId); iter != tones.cend())
        return color(*iter);

    const Scheme &s = scheme();
    if (colorId == u"PiecesBar.Border")
        return s.mid;
    if (colorId == u"PiecesBar.MissingPiece")
        return s.midlight;
    if (colorId == u"PiecesBar.PartialPiece")
        return s.highlight.lighter(g_dark ? 70 : 150);
    return {};
}

QPalette ModernAppearance::palette(const bool dark)
{
    const Scheme &s = schemeFor(dark);
    QPalette p;
    for (const QPalette::ColorGroup group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled})
    {
        p.setColor(group, QPalette::Window, s.window);
        p.setColor(group, QPalette::WindowText, s.text);
        p.setColor(group, QPalette::Base, s.base);
        p.setColor(group, QPalette::AlternateBase, s.alternate);
        p.setColor(group, QPalette::Button, s.button);
        p.setColor(group, QPalette::ButtonText, s.text);
        p.setColor(group, QPalette::Text, s.text);
        p.setColor(group, QPalette::BrightText, Qt::white);
        p.setColor(group, QPalette::PlaceholderText, s.placeholder);
        p.setColor(group, QPalette::Light, s.light);
        p.setColor(group, QPalette::Midlight, s.midlight);
        p.setColor(group, QPalette::Mid, s.mid);
        p.setColor(group, QPalette::Dark, s.dark);
        p.setColor(group, QPalette::Shadow, s.shadow);
        p.setColor(group, QPalette::Highlight, s.highlight);
        p.setColor(group, QPalette::HighlightedText, s.highlightedText);
        p.setColor(group, QPalette::Link, s.link);
        p.setColor(group, QPalette::LinkVisited, s.link);
        p.setColor(group, QPalette::ToolTipBase, s.tooltipBase);
        p.setColor(group, QPalette::ToolTipText, s.tooltipText);
#if (QT_VERSION >= QT_VERSION_CHECK(6, 6, 0))
        p.setColor(group, QPalette::Accent, s.highlight);
#endif
    }
    for (const QPalette::ColorRole role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText, QPalette::HighlightedText})
        p.setColor(QPalette::Disabled, role, s.disabledText);
    // Keep selection readable when the window is inactive.
    p.setColor(QPalette::Inactive, QPalette::Highlight, s.highlight.darker(dark ? 125 : 110));
    return p;
}

void ModernAppearance::apply(const bool dark)
{
    initModernResources();
    if (!g_active)
    {
        applyFont();
        qApp->installEventFilter(new PopupPolisher(qApp));
    }
    g_active = true;
    g_dark = dark;
    qApp->setPalette(palette(dark));

    const QByteArray qss = readResource(u":/qbitfusion/modern.qss"_s);
    if (qss.isEmpty())
        return;

    // A few colours cannot be expressed as palette roles (translucent tints and semantic colours):
    // substitute per-scheme tokens. Everything else in the sheet follows the palette.
    const Scheme &s = scheme();
    QString sheet = QString::fromUtf8(qss);
    // Neutral overlays for hover/selection: the accent never tints surfaces.
    sheet.replace(u"@hover@"_s, dark ? u"rgba(255, 255, 255, 0.05)"_s : u"rgba(0, 0, 0, 0.04)"_s);
    sheet.replace(u"@hoverStrong@"_s, dark ? u"rgba(255, 255, 255, 0.085)"_s : u"rgba(0, 0, 0, 0.065)"_s);
    sheet.replace(u"@selection@"_s, rgba(s.highlight, selectionAlpha()));
    sheet.replace(u"@selectionStrong@"_s, rgba(s.highlight, dark ? 0.42 : 0.30));
    sheet.replace(u"@selectionInactive@"_s, dark ? u"rgba(255, 255, 255, 0.11)"_s : u"rgba(0, 0, 0, 0.085)"_s);
    sheet.replace(u"@selectionText@"_s, s.text.name());
    sheet.replace(u"@primaryBg@"_s, dark ? u"#ececec"_s : u"#1a1a1a"_s);
    sheet.replace(u"@primaryHover@"_s, dark ? u"#ffffff"_s : u"#333333"_s);
    sheet.replace(u"@primaryPressed@"_s, dark ? u"#cfcfcf"_s : u"#000000"_s);
    sheet.replace(u"@primaryText@"_s, dark ? u"#151515"_s : u"#ffffff"_s);
    sheet.replace(u"@primaryDivider@"_s, dark ? u"rgba(0, 0, 0, 0.18)"_s : u"rgba(255, 255, 255, 0.22)"_s);
    sheet.replace(u"@surfaceRaised@"_s, dark ? u"#222222"_s : u"#ffffff"_s);
    sheet.replace(u"@border@"_s, s.mid.name());
    sheet.replace(u"@borderStrong@"_s, s.dark.name());
    sheet.replace(u"@muted@"_s, s.placeholder.name());
    sheet.replace(u"@danger@"_s, s.danger.name());
    sheet.replace(u"@glyph@"_s, dark ? u"dark"_s : u"light"_s);
    qApp->setStyleSheet(sheet);
}

QIcon ModernAppearance::icon(const QString &iconId, const bool forDarkSurface)
{
    initModernResources();

    // Brand marks keep their own colours.
    if ((iconId == u"qbittorrent") || (iconId == u"qbittorrent-tray") || (iconId == u"qbittorrent_mac"))
        return QIcon(u":/qbitfusion/brand/qbitfusion.png"_s);

    if (iconId.startsWith(u"qbittorrent-tray"))
    {
        // Monochrome tray variants: white on a dark taskbar, near-black on a light one.
        const bool darkSurface = (iconId == u"qbittorrent-tray-dark") || ((iconId != u"qbittorrent-tray-light") && forDarkSurface);
        return QIcon(darkSurface ? u":/qbitfusion/brand/tray-light.png"_s : u":/qbitfusion/brand/tray-dark.png"_s);
    }

    // "<id>@inverse": the same glyph for primary (inverse-neutral) buttons; follows the scheme.
    if (const QString suffix = u"@inverse"_s; iconId.endsWith(suffix))
    {
        const QByteArray svg = readResource(u":/qbitfusion/icons/" + iconId.chopped(suffix.size()) + u".svg");
        if (svg.isEmpty())
            return {};
        return QIcon(new TintedSvgEngine(iconId, svg, Tone::Inverse));
    }

    const QByteArray svg = readResource(u":/qbitfusion/icons/" + iconId + u".svg");
    if (svg.isEmpty())
        return {};
    return QIcon(new TintedSvgEngine(iconId, svg, toneFor(iconId)));
}

void ModernAppearance::paintProgress(QPainter *painter, const QRect &cellRect, const int progress, const QString &text
        , const QColor &fill, const bool enabled, const QPalette &pal)
{
    // A slim track with the value beside it reads faster in a dense list than text inside a pill.
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    QFont font = painter->font();
    font.setFeature(QFont::Tag("tnum"), 1);
    painter->setFont(font);
    const QFontMetrics metrics {font};
    const int textWidth = metrics.horizontalAdvance(u"100.0%"_s) + 2;
    const QRect content = cellRect.adjusted(8, 0, -8, 0);
    const bool showBar = content.width() > (textWidth + 36);

    const QRect textRect = showBar ? QRect(content.right() - textWidth, content.top(), textWidth, content.height()) : content;
    painter->setPen(pal.color(enabled ? QPalette::Text : QPalette::PlaceholderText));
    painter->drawText(textRect, (Qt::AlignVCenter | Qt::AlignRight), text);

    if (showBar)
    {
        const qreal height = 6;
        const QRectF track {static_cast<qreal>(content.left()), content.center().y() - (height / 2) + 0.5
            , static_cast<qreal>(content.width() - textWidth - 10), height};
        // The track is drawn from the text colour so that it stays visible on a selected (tinted) row.
        QColor trackColor = pal.color(QPalette::Text);
        trackColor.setAlphaF(g_dark ? 0.16 : 0.12);
        painter->setPen(Qt::NoPen);
        painter->setBrush(trackColor);
        painter->drawRoundedRect(track, height / 2, height / 2);

        const qreal ratio = std::clamp(progress, 0, 100) / 100.0;
        if (ratio > 0)
        {
            // Not the palette's Highlight: inside an item view that role is the dim selection tint.
            QColor fillColor = fill.isValid() ? fill : color((progress >= 100) ? Tone::Success : Tone::Accent);
            if (!enabled)
                fillColor = color(Tone::Muted);
            painter->setBrush(fillColor);
            painter->drawRoundedRect(QRectF(track.left(), track.top(), std::max(track.width() * ratio, height), height)
                , height / 2, height / 2);
        }
    }
    painter->restore();
}

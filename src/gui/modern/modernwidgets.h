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

#include <QAbstractButton>
#include <QColor>
#include <QPointer>
#include <QStyledItemDelegate>
#include <QWidget>

class QAbstractItemModel;
class QAbstractItemView;
class QLabel;
class QPushButton;

namespace Modern
{
    // Splits "Downloading (3)" into {"Downloading", "3"}; the count is empty when there is none.
    std::pair<QString, QString> splitCount(const QString &text);

    // Header page switcher entry: icon, label and an optional count.
    class NavButton final : public QAbstractButton
    {
        Q_OBJECT
        Q_DISABLE_COPY_MOVE(NavButton)

    public:
        explicit NavButton(const QIcon &icon, const QString &text, QWidget *parent = nullptr);

        void setBadge(const QString &badge);
        QSize sizeHint() const override;

    protected:
        void paintEvent(QPaintEvent *event) override;
        void enterEvent(QEnterEvent *event) override;
        void leaveEvent(QEvent *event) override;

    private:
        QString m_badge;
        bool m_hovered = false;
    };

    // The product mark and name shown at the start of the header.
    QWidget *createBrandWidget(QWidget *parent);

    // Friendly placeholder over an item view while it has no rows: explains what to do next
    // and offers the primary actions, instead of an empty grid.
    class EmptyState final : public QWidget
    {
        Q_OBJECT
        Q_DISABLE_COPY_MOVE(EmptyState)

    public:
        EmptyState(QAbstractItemView *view, QAbstractItemModel *sourceModel);

        QPushButton *addPrimaryAction(QAction *action, const QString &text);
        QPushButton *addSecondaryAction(QAction *action, const QString &text);

    private:
        bool eventFilter(QObject *watched, QEvent *event) override;
        void refresh();

        QAbstractItemView *m_view = nullptr;
        QPointer<QAbstractItemModel> m_sourceModel;
        QLabel *m_title = nullptr;
        QLabel *m_body = nullptr;
        QWidget *m_buttons = nullptr;
    };

    // Sidebar filter rows: neutral hover/selection, icon, name and a right-aligned count parsed
    // from the "Name (n)" texts the filter models produce.
    class FilterRowDelegate final : public QStyledItemDelegate
    {
        Q_OBJECT
        Q_DISABLE_COPY_MOVE(FilterRowDelegate)

    public:
        using QStyledItemDelegate::QStyledItemDelegate;

        void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
        QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    };

    // Accent colour choice for Options: preset chips plus a custom colour.
    class AccentPicker final : public QWidget
    {
        Q_OBJECT
        Q_DISABLE_COPY_MOVE(AccentPicker)

    public:
        explicit AccentPicker(QWidget *parent = nullptr);

        QColor color() const;

    signals:
        void colorChanged();

    private:
        QColor m_color;
    };

    // Applies FilterRowDelegate and the sidebar look to every filter list inside a sidebar widget.
    void styleSidebarViews(QWidget *sidebar);

    // Sections are drawn without a box (title, hairline, content), so what a group box holds starts
    // at the title's left edge instead of being inset as if there were a frame around it.
    void flattenSections(QWidget *root);

    // Marks a label as secondary text (muted colour).
    void markAsHint(QLabel *label);
}

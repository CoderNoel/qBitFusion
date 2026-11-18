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

#include "directdownloadwidget.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QStyledItemDelegate>
#include <QToolBar>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

#include "base/directdownload/directdownloadmanager.h"
#include "base/directdownload/linkclassifier.h"
#include "base/global.h"
#include "base/utils/misc.h"
#include "gui/modern/modernappearance.h"
#include "gui/modern/modernwidgets.h"
#include "gui/progressbarpainter.h"
#include "gui/uithememanager.h"
#include "gui/utils.h"
#include "directdownloadmodel.h"

using namespace Qt::Literals::StringLiterals;
using DirectDownload::State;

namespace
{
    class ProgressDelegate final : public QStyledItemDelegate
    {
    public:
        using QStyledItemDelegate::QStyledItemDelegate;

        QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
        {
            QSize size = QStyledItemDelegate::sizeHint(option, index);
            if (ModernAppearance::isActive())
                size.setHeight(std::max(size.height(), 36));   // the same comfortable rows as Transfers
            return size;
        }

        void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
        {
            if (index.column() != DirectDownloadModel::COL_PROGRESS)
            {
                QStyledItemDelegate::paint(painter, option, index);
                return;
            }

            const auto state = static_cast<State>(index.data(DirectDownloadModel::StateRole).toInt());
            const int progress = static_cast<int>(index.data(DirectDownloadModel::SortRole).toReal() * 100);
            QStyleOptionViewItem opt {option};
            opt.state.setFlag(QStyle::State_Enabled, (state != State::Paused) && (state != State::Failed) && (state != State::NeedsLink));
            m_painter.paint(painter, opt, index.data().toString(), progress);
        }

    private:
        ProgressBarPainter m_painter;
    };

    bool canStart(const State state)
    {
        return (state == State::Paused) || (state == State::Failed);
    }

    bool canPause(const State state)
    {
        return (state == State::Queued) || (state == State::Downloading);
    }
}

DirectDownloadWidget::DirectDownloadWidget(DirectDownload::Manager *manager, QAction *addLinksAction, QWidget *parent)
    : QWidget(parent)
    , m_manager {manager}
    , m_storeHeaderState {u"DirectDownload/ListHeaderState"_s}
{
    setObjectName(u"qbfListPage"_s);
    setAttribute(Qt::WA_StyledBackground);

    auto *theme = UIThemeManager::instance();

    m_actionStart = new QAction(theme->getIcon(u"torrent-start"_s, u"media-playback-start"_s), tr("Start"), this);
    m_actionPause = new QAction(theme->getIcon(u"torrent-stop"_s, u"media-playback-pause"_s), tr("Pause"), this);
    m_actionRemove = new QAction(theme->getIcon(u"list-remove"_s), tr("Remove"), this);
    m_actionReplaceLink = new QAction(theme->getIcon(u"insert-link"_s), tr("Replace link…"), this);
    m_actionOpenFolder = new QAction(theme->getIcon(u"directory"_s), tr("Open folder"), this);
    m_actionStart->setToolTip(tr("Start or retry the selected downloads"));
    m_actionPause->setToolTip(tr("Pause the selected downloads"));
    m_actionRemove->setToolTip(tr("Remove the selected downloads from the list"));
    m_actionRemove->setShortcut(QKeySequence::Delete);
    m_actionRemove->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    m_actionReplaceLink->setToolTip(tr("Continue with a new link, for example when the old one expired"));
    m_actionOpenFolder->setToolTip(tr("Open the folder of the selected download"));
    addAction(m_actionRemove);

    connect(m_actionStart, &QAction::triggered, this, &DirectDownloadWidget::startSelected);
    connect(m_actionPause, &QAction::triggered, this, &DirectDownloadWidget::pauseSelected);
    connect(m_actionRemove, &QAction::triggered, this, &DirectDownloadWidget::removeSelected);
    connect(m_actionReplaceLink, &QAction::triggered, this, &DirectDownloadWidget::replaceLink);
    connect(m_actionOpenFolder, &QAction::triggered, this, &DirectDownloadWidget::openFolder);

    auto *actionBar = new QToolBar(this);
    actionBar->setProperty("qbfActionBar", true);
    actionBar->setIconSize({16, 16});
    actionBar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    for (QAction *action : {m_actionStart, m_actionPause, m_actionRemove})
    {
        actionBar->addAction(action);
        if (auto *button = qobject_cast<QToolButton *>(actionBar->widgetForAction(action)))
            button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    }
    actionBar->addSeparator();
    actionBar->addAction(m_actionReplaceLink);
    actionBar->addAction(m_actionOpenFolder);

    auto *spacer = new QWidget(actionBar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    actionBar->addWidget(spacer);
    m_summary = new QLabel(actionBar);
    m_summary->setObjectName(u"qbfSelectionSummary"_s);
    actionBar->addWidget(m_summary);

    m_model = new DirectDownloadModel(manager, this);
    m_proxy = new QSortFilterProxyModel(this);
    m_proxy->setSourceModel(m_model);
    m_proxy->setSortRole(DirectDownloadModel::SortRole);
    m_proxy->setSortCaseSensitivity(Qt::CaseInsensitive);
    m_proxy->setDynamicSortFilter(false);  // rows do not jump while their speed changes

    m_view = new QTreeView(this);
    m_view->setObjectName(u"qbfLinkList"_s);
    m_view->setModel(m_proxy);
    m_view->setItemDelegate(new ProgressDelegate(m_view));
    m_view->setUniformRowHeights(true);
    m_view->setRootIsDecorated(false);
    m_view->setAllColumnsShowFocus(true);
    m_view->setSortingEnabled(true);
    m_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_view->setItemsExpandable(false);
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    m_view->setAttribute(Qt::WA_MacShowFocusRect, false);
    m_view->header()->setStretchLastSection(false);
    m_view->header()->setTextElideMode(Qt::ElideRight);
    m_view->setColumnWidth(DirectDownloadModel::COL_NAME, 380);
    m_view->setColumnWidth(DirectDownloadModel::COL_PROGRESS, 150);
    m_view->setColumnWidth(DirectDownloadModel::COL_STATUS, 130);
    m_view->setColumnWidth(DirectDownloadModel::COL_SOURCE, 210);
    m_view->setColumnWidth(DirectDownloadModel::COL_ADDED, 190);
    if (const QByteArray state = m_storeHeaderState; !state.isEmpty())
        m_view->header()->restoreState(state);
    else
        m_view->sortByColumn(DirectDownloadModel::COL_ADDED, Qt::AscendingOrder);

    connect(m_view, &QWidget::customContextMenuRequested, this, &DirectDownloadWidget::showContextMenu);
    connect(m_view, &QAbstractItemView::doubleClicked, this, &DirectDownloadWidget::openSelected);
    connect(m_view->selectionModel(), &QItemSelectionModel::selectionChanged, this, &DirectDownloadWidget::updateActions);
    for (const auto signal : {&QAbstractItemModel::rowsInserted, &QAbstractItemModel::rowsRemoved})
    {
        connect(m_model, signal, this, [this]
        {
            updateActions();
            emit countChanged(count());
        });
    }
    // States change without a selection change (a download finishes, fails or waits for a link).
    connect(m_model, &QAbstractItemModel::dataChanged, this, &DirectDownloadWidget::updateActions);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(actionBar);
    layout->addWidget(m_view);

    auto *emptyState = new Modern::EmptyState(m_view, m_model);
    emptyState->setTexts(tr("No direct downloads")
            , tr("Paste a link to a file, such as a debrid download link or a stream link copied from Stremio. Torrent and magnet links go to Transfers."));
    if (addLinksAction)
        emptyState->addPrimaryAction(addLinksAction, tr("Paste links"))->setIcon(theme->getIcon(u"qbf-add@inverse"_s));

    updateActions();
}

DirectDownloadWidget::~DirectDownloadWidget()
{
    m_storeHeaderState = m_view->header()->saveState();
}

int DirectDownloadWidget::count() const
{
    return m_model->rowCount();
}

QStringList DirectDownloadWidget::selectedIds() const
{
    QStringList ids;
    for (const QModelIndex &index : m_view->selectionModel()->selectedRows())
        ids.append(index.data(DirectDownloadModel::IdRole).toString());
    return ids;
}

void DirectDownloadWidget::updateActions()
{
    bool anyStart = false;
    bool anyPause = false;
    bool anyIncomplete = false;
    const QStringList ids = selectedIds();
    for (const QString &id : ids)
    {
        const auto info = m_manager->info(id);
        if (!info)
            continue;
        anyStart = anyStart || canStart(info->state);
        anyPause = anyPause || canPause(info->state);
        anyIncomplete = anyIncomplete || (info->state != State::Completed);
    }
    m_actionStart->setEnabled(anyStart);
    m_actionPause->setEnabled(anyPause);
    m_actionRemove->setEnabled(!ids.isEmpty());
    m_actionReplaceLink->setEnabled((ids.size() == 1) && anyIncomplete);
    m_actionOpenFolder->setEnabled(ids.size() == 1);

    const int total = count();
    QString text;
    if (!ids.isEmpty())
        text = tr("%1 of %2 selected").arg(ids.size()).arg(total);
    else if (const qint64 speed = m_manager->totalSpeed(); speed > 0)
        text = tr("%1 downloading · %2").arg(m_manager->activeCount()).arg(Utils::Misc::friendlyUnit(speed, true));
    else if (total == 1)
        text = tr("1 download");
    else
        text = tr("%1 downloads").arg(total);
    if (m_summary->text() != text)
        m_summary->setText(text);
}

void DirectDownloadWidget::showContextMenu(const QPoint &pos)
{
    if (selectedIds().isEmpty())
        return;

    auto *menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->addAction(m_actionStart);
    menu->addAction(m_actionPause);
    menu->addAction(m_actionRemove);
    menu->addSeparator();
    menu->addAction(m_actionReplaceLink);
    menu->addAction(m_actionOpenFolder);
    menu->addAction(UIThemeManager::instance()->getIcon(u"edit-copy"_s), tr("Copy name"), this, &DirectDownloadWidget::copyNames);
    menu->popup(m_view->viewport()->mapToGlobal(pos));
}

void DirectDownloadWidget::startSelected()
{
    for (const QString &id : selectedIds())
        m_manager->resume(id);
}

void DirectDownloadWidget::pauseSelected()
{
    for (const QString &id : selectedIds())
        m_manager->pause(id);
}

void DirectDownloadWidget::removeSelected()
{
    const QStringList ids = selectedIds();
    if (ids.isEmpty())
        return;

    const auto first = m_manager->info(ids.first());
    QMessageBox box {QMessageBox::Question, tr("Remove downloads")
            , ((ids.size() == 1) && first)
                ? tr("Remove \"%1\" from the list?").arg(first->name)
                : tr("Remove %1 downloads from the list?").arg(ids.size())
            , (QMessageBox::Yes | QMessageBox::Cancel), this};
    box.button(QMessageBox::Yes)->setText(tr("Remove"));
    auto *deleteFiles = new QCheckBox(tr("Also delete the files from the disk"), &box);
    box.setCheckBox(deleteFiles);
    if (box.exec() != QMessageBox::Yes)
        return;

    for (const QString &id : ids)
        m_manager->remove(id, deleteFiles->isChecked());
}

void DirectDownloadWidget::replaceLink()
{
    const QStringList ids = selectedIds();
    if (ids.size() != 1)
        return;
    const auto info = m_manager->info(ids.first());
    if (!info)
        return;

    bool ok = false;
    const QString text = QInputDialog::getText(this, tr("Replace link")
            , tr("Paste a new link for \"%1\". The download continues from what is already saved, if the file is the same.").arg(info->name)
            , QLineEdit::Normal, {}, &ok).trimmed();
    if (!ok || text.isEmpty())
        return;

    const QUrl link {text};
    if (DirectDownload::checkLink(link) != DirectDownload::LinkCheck::Allowed)
    {
        QMessageBox::warning(this, tr("Replace link"), DirectDownload::describe(DirectDownload::checkLink(link)));
        return;
    }
    m_manager->replaceLink(info->id, link);
}

void DirectDownloadWidget::openSelected()
{
    const QStringList ids = selectedIds();
    if (ids.size() != 1)
        return;
    const auto info = m_manager->info(ids.first());
    if (!info)
        return;
    if (info->state == State::Completed)
        Utils::Gui::openPath(info->filePath());
    else
        openFolder();
}

void DirectDownloadWidget::openFolder()
{
    const QStringList ids = selectedIds();
    if (ids.size() != 1)
        return;
    const auto info = m_manager->info(ids.first());
    if (!info)
        return;
    if (info->filePath().exists())
        Utils::Gui::openFolderSelect(info->filePath());
    else
        Utils::Gui::openPath(info->savePath);
}

void DirectDownloadWidget::copyNames()
{
    QStringList names;
    for (const QString &id : selectedIds())
    {
        if (const auto info = m_manager->info(id))
            names.append(info->name);
    }
    QApplication::clipboard()->setText(names.join(u'\n'));
}

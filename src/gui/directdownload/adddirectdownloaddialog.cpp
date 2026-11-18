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

#include "adddirectdownloaddialog.h"

#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "base/bittorrent/session.h"
#include "base/global.h"
#include "base/directdownload/directdownloadmanager.h"
#include "base/directdownload/linkclassifier.h"
#include "base/hybrid/securestore.h"
#include "base/hybrid/urlguard.h"
#include "base/utils/misc.h"
#include "gui/fspathedit.h"
#include "gui/modern/modernwidgets.h"

using namespace Qt::Literals::StringLiterals;

namespace
{
    enum Column
    {
        COL_NAME,
        COL_SIZE,
        COL_SOURCE,
        COL_STATUS
    };
}

AddDirectDownloadDialog::AddDirectDownloadDialog(DirectDownload::Manager *manager, const QList<QUrl> &links, QWidget *parent)
    : QDialog(parent)
    , m_manager {manager}
    , m_storeSavePath {u"DirectDownload/SavePath"_s}
    , m_storeDialogSize {u"DirectDownload/AddDialogSize"_s}
{
    setWindowTitle(tr("Add links"));

    auto *layout = new QVBoxLayout(this);

    auto *intro = new QLabel(tr("Each link is checked once: files are downloaded here, torrents open in the add torrent dialog."), this);
    intro->setWordWrap(true);
    Modern::markAsHint(intro);
    layout->addWidget(intro);

    m_list = new QTreeWidget(this);
    m_list->setColumnCount(4);
    m_list->setHeaderLabels({tr("Name"), tr("Size"), tr("Source"), tr("Status")});
    m_list->setRootIsDecorated(false);
    m_list->setUniformRowHeights(true);
    m_list->setSelectionMode(QAbstractItemView::NoSelection);
    m_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_list->header()->setStretchLastSection(false);
    m_list->header()->setSectionResizeMode(COL_NAME, QHeaderView::Stretch);
    m_list->header()->setSectionResizeMode(COL_SIZE, QHeaderView::ResizeToContents);
    m_list->header()->setSectionResizeMode(COL_SOURCE, QHeaderView::ResizeToContents);
    m_list->header()->setSectionResizeMode(COL_STATUS, QHeaderView::Interactive);
    m_list->setColumnWidth(COL_STATUS, 300);
    layout->addWidget(m_list, 1);

    m_summary = new QLabel(this);
    Modern::markAsHint(m_summary);
    layout->addWidget(m_summary);

    auto *saveLabel = new QLabel(tr("Save files to:"), this);
    layout->addWidget(saveLabel);
    m_savePath = new FileSystemPathComboEdit(this);
    m_savePath->setMode(FileSystemPathEdit::Mode::DirectorySave);
    m_savePath->setDialogCaption(tr("Choose save path"));
    Path defaultPath = m_storeSavePath;
    if (defaultPath.isEmpty() && BitTorrent::Session::instance())
        defaultPath = BitTorrent::Session::instance()->savePath();
    m_savePath->setSelectedPath(defaultPath);
    layout->addWidget(m_savePath);

    if (Hybrid::SecureStore::isPersistent())
    {
        auto *privacy = new QLabel(tr("Until a file is complete, its link is kept in the Windows Credential Manager so the download can resume after a restart. Links are never written to settings files or logs."), this);
        privacy->setWordWrap(true);
        Modern::markAsHint(privacy);
        layout->addWidget(privacy);
    }

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_downloadButton = buttons->button(QDialogButtonBox::Ok);
    m_downloadButton->setText(tr("Download"));
    m_downloadButton->setObjectName(u"qbfPrimaryButton"_s);
    m_downloadButton->setEnabled(false);
    connect(buttons, &QDialogButtonBox::accepted, this, &AddDirectDownloadDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    // Names can be changed before the download starts.
    connect(m_list, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, const int column)
    {
        if ((column == COL_NAME) && (item->flags() & Qt::ItemIsEditable))
            m_list->editItem(item, COL_NAME);
    });

    for (const QUrl &link : links)
    {
        auto *item = new QTreeWidgetItem(m_list);
        item->setText(COL_NAME, tr("Checking…"));
        item->setText(COL_SOURCE, link.host().toLower());
        item->setText(COL_STATUS, tr("Checking link…"));
        m_entries.append({.link = link, .item = item});

        const int index = static_cast<int>(m_entries.size()) - 1;
        DirectDownload::LinkProbe *probe = m_manager->createProbe(link, this);
        connect(probe, &DirectDownload::LinkProbe::finished, this, [this, index, probe](const DirectDownload::ProbeResult &result)
        {
            onProbeFinished(index, result);
            probe->deleteLater();
        });
        connect(probe, &DirectDownload::LinkProbe::failed, this, [this, index, probe](const DirectDownload::Failure &failure)
        {
            onProbeFailed(index, failure);
            probe->deleteLater();
        });
        probe->start();
    }

    if (const QSize size = m_storeDialogSize; size.isValid())
        resize(size);
    else
        resize(880, 420);
    updateState();
}

AddDirectDownloadDialog::~AddDirectDownloadDialog()
{
    m_storeDialogSize = size();
}

void AddDirectDownloadDialog::onProbeFinished(const int index, const DirectDownload::ProbeResult &result)
{
    Entry &entry = m_entries[index];
    entry.done = true;
    QTreeWidgetItem *item = entry.item;

    if (result.kind == DirectDownload::ContentKind::Torrent)
    {
        item->setText(COL_NAME, tr("Torrent"));
        item->setText(COL_STATUS, tr("Opened in the add torrent dialog"));
        if (!result.magnetUri.isEmpty())
            emit magnetFound(result.magnetUri);
        else if (!result.torrentData.isEmpty())
            emit torrentDataFound(Hybrid::redactUrl(entry.link), result.torrentData);
        else
            emit torrentLinkFound(QString::fromUtf8(entry.link.toEncoded()));
    }
    else
    {
        entry.file = result;
        item->setText(COL_NAME, result.fileName);
        item->setFlags(item->flags() | Qt::ItemIsEditable);
        item->setToolTip(COL_NAME, tr("Double-click to rename"));
        item->setText(COL_SIZE, (result.totalSize >= 0) ? Utils::Misc::friendlyUnit(result.totalSize) : tr("Unknown"));
        if (result.finalUrl.isValid())
            item->setText(COL_SOURCE, result.finalUrl.host().toLower());
        item->setText(COL_STATUS, result.rangeSupported ? tr("Ready") : tr("Ready (cannot resume)"));
        if (!result.rangeSupported)
            item->setToolTip(COL_STATUS, tr("This server does not support resuming. A paused or interrupted download starts again from the beginning."));
    }
    updateState();
}

void AddDirectDownloadDialog::onProbeFailed(const int index, const DirectDownload::Failure &failure)
{
    Entry &entry = m_entries[index];
    entry.done = true;
    entry.failed = true;
    entry.item->setText(COL_NAME, tr("Not added"));
    entry.item->setText(COL_STATUS, failure.detail);
    entry.item->setToolTip(COL_STATUS, failure.detail);
    updateState();
}

void AddDirectDownloadDialog::updateState()
{
    const auto pending = std::ranges::count_if(m_entries, [](const Entry &e) { return !e.done; });
    const auto files = std::ranges::count_if(m_entries, [](const Entry &e) { return e.file.has_value(); });
    const auto failed = std::ranges::count_if(m_entries, [](const Entry &e) { return e.failed; });

    m_downloadButton->setEnabled(files > 0);

    QString text;
    if (pending > 0)
        text = tr("Checking %1 of %2 links…").arg(pending).arg(m_entries.size());
    else if (files == 0)
        text = (failed > 0) ? tr("No file to download.") : QString();
    else if (files == 1)
        text = tr("1 file ready.");
    else
        text = tr("%1 files ready.").arg(files);
    m_summary->setText(text);

    // Only torrents: they went to their own dialogs, nothing is left to do here.
    if ((pending == 0) && (files == 0) && (failed == 0))
        QMetaObject::invokeMethod(this, &QDialog::reject, Qt::QueuedConnection);
}

void AddDirectDownloadDialog::accept()
{
    const Path savePath = m_savePath->selectedPath();
    if (savePath.isEmpty() || !savePath.isAbsolute())
    {
        m_savePath->setFocus();
        return;
    }

    for (const Entry &entry : asConst(m_entries))
    {
        if (!entry.file)
            continue;
        const QString name = entry.item->text(COL_NAME).trimmed();
        m_manager->add(entry.link, *entry.file, savePath, (name.isEmpty() ? entry.file->fileName : name));
    }
    m_storeSavePath = savePath;
    QDialog::accept();
}

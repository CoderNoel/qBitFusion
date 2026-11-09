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

#include <QTest>

#include "base/hybrid/filebinding.h"

using namespace Hybrid;
using namespace Qt::Literals::StringLiterals;

namespace
{
    QList<TorrentFileEntry> multiFileTorrent()
    {
        // Native order, including a pad file as hybrid torrents have.
        return {
            {0, u"Show/a.mkv"_s, 1000, false},
            {1, u".pad/24"_s, 24, true},
            {2, u"Show/sub/b.srt"_s, 50, false},
            {3, u"Show/c.nfo"_s, 7, false}
        };
    }
}

class TestHybridFileBinding final : public QObject
{
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(TestHybridFileBinding)

public:
    TestHybridFileBinding() = default;

private slots:
    void normalize() const
    {
        QCOMPARE(normalizeRelativePath(u"/a/b.mkv"_s), u"a/b.mkv"_s);
        QCOMPARE(normalizeRelativePath(u"a\\b.mkv"_s), u"a/b.mkv"_s);
        QVERIFY(normalizeRelativePath(u"../etc/passwd"_s).isEmpty());
        QVERIFY(normalizeRelativePath(u"a/../../b"_s).isEmpty());
        QVERIFY(normalizeRelativePath(u"a/./b"_s).isEmpty());
        QVERIFY(normalizeRelativePath(u"a//b"_s).isEmpty());
        QVERIFY(normalizeRelativePath(u"C:/Windows/x"_s).isEmpty());
        QVERIFY(normalizeRelativePath(QString(u"a"_s + QChar(0) + u"b"_s)).isEmpty());
        QVERIFY(normalizeRelativePath(u""_s).isEmpty());
        // NFC: "é" composed vs decomposed compare equal (T08).
        QCOMPARE(normalizeRelativePath(u"caf\u0065\u0301.txt"_s), normalizeRelativePath(u"caf\u00e9.txt"_s));
    }

    void bindsWithRoot() const
    {
        const auto r = bindFiles(multiFileTorrent(), {
            {u"10"_s, u"/Show/a.mkv"_s, 1000},
            {u"11"_s, u"/Show/sub/b.srt"_s, 50},
            {u"12"_s, u"/Show/c.nfo"_s, 7}
        });
        QVERIFY(r.has_value());
        QVERIFY(r->providerPathsIncludeRoot);
        QCOMPARE(r->byNativeIndex.size(), 3);
        QCOMPARE(r->byNativeIndex.value(2).providerFileId, u"11"_s);
        QVERIFY(!r->byNativeIndex.contains(1)); // pad files are never bound
        QVERIFY(r->unboundNativeIndexes.isEmpty());
    }

    void bindsWithoutRoot() const
    {
        // Real-Debrid style: paths relative to the torrent root.
        const auto r = bindFiles(multiFileTorrent(), {
            {u"1"_s, u"/a.mkv"_s, 1000},
            {u"2"_s, u"/sub/b.srt"_s, 50},
            {u"3"_s, u"/c.nfo"_s, 7}
        });
        QVERIFY(r.has_value());
        QVERIFY(!r->providerPathsIncludeRoot);
        QCOMPARE(r->byNativeIndex.size(), 3);
    }

    void partialSelectionIsUnboundNotError() const
    {
        // T05: provider exposes only the selected file; neighbours are unbound and the planner
        // must route boundary pieces elsewhere.
        const auto r = bindFiles(multiFileTorrent(), {{u"1"_s, u"/a.mkv"_s, 1000}});
        QVERIFY(r.has_value());
        QCOMPARE(r->byNativeIndex.size(), 1);
        QCOMPARE(r->unboundNativeIndexes, (QList<int> {2, 3}));
    }

    void sizeMismatchFailsClosed() const
    {
        // T10: same path, different size = different release. Never guess.
        const auto r = bindFiles(multiFileTorrent(), {
            {u"1"_s, u"/a.mkv"_s, 999},
            {u"2"_s, u"/sub/b.srt"_s, 50},
        });
        QVERIFY(!r.has_value());
        QCOMPARE(r.error().error, BindingError::SizeMismatch);
        QCOMPARE(r.error().nativeIndex, 0);
    }

    void duplicateProviderPathsAmbiguous() const
    {
        // T07
        const auto r = bindFiles(multiFileTorrent(), {
            {u"1"_s, u"/a.mkv"_s, 1000},
            {u"2"_s, u"a.mkv"_s, 1000}
        });
        QVERIFY(!r.has_value());
        QCOMPARE(r.error().error, BindingError::DuplicateProviderPath);
    }

    void traversalRejected() const
    {
        const auto r = bindFiles(multiFileTorrent(), {{u"1"_s, u"../../a.mkv"_s, 1000}});
        QVERIFY(!r.has_value());
        QCOMPARE(r.error().error, BindingError::UnsafeProviderPath);
    }

    void noBasenameGuessing() const
    {
        // Same basename in a different folder must not bind (spec: never match by basename).
        const auto r = bindFiles(multiFileTorrent(), {{u"1"_s, u"/Other/a.mkv"_s, 1000}});
        QVERIFY(!r.has_value());
        QCOMPARE(r.error().error, BindingError::NoMatchingLayout);
    }

    void caseIsSignificant() const
    {
        const auto r = bindFiles(multiFileTorrent(), {{u"1"_s, u"/A.MKV"_s, 1000}});
        QVERIFY(!r.has_value());
    }

    void renamedLocalFileStillBindsByMetadata() const
    {
        // T06: local renames do not change metadata paths, which are what we bind on.
        QList<TorrentFileEntry> files = multiFileTorrent();
        const auto r = bindFiles(files, {{u"1"_s, u"/a.mkv"_s, 1000}});
        QVERIFY(r.has_value());
        QCOMPARE(r->byNativeIndex.value(0).providerFileId, u"1"_s);
    }

    void emptyProviderList() const
    {
        const auto r = bindFiles(multiFileTorrent(), {});
        QVERIFY(!r.has_value());
        QCOMPARE(r.error().error, BindingError::NoProviderFiles);
    }
};

QTEST_APPLESS_MAIN(TestHybridFileBinding)
#include "testhybridfilebinding.moc"

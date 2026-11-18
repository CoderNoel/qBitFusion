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

#include "base/hybrid/transportstatemachine.h"

using namespace Hybrid;
using S = TransportState;
using E = MachineEvent;

namespace
{
    MachineContext ctx(const TransportPolicy policy, const bool consent, const bool metadata = true, const bool combined = false)
    {
        return {policy, consent, metadata, combined};
    }
}

class TestHybridStateMachine final : public QObject
{
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(TestHybridStateMachine)

public:
    TestHybridStateMachine() = default;

private slots:
    void torrentOnlyStaysDisabled() const
    {
        TransportStateMachine m;
        const auto c = ctx(TransportPolicy::TorrentOnly, false);
        QCOMPARE(m.handle(E::Enable, c).to, S::Disabled);
        QVERIFY(swarmAllowed(m.state(), c)); // upstream behavior (I7)
    }

    void happyPathHttp() const
    {
        TransportStateMachine m;
        const auto c = ctx(TransportPolicy::DebridPreferred, true, false);
        QCOMPARE(m.handle(E::Enable, c).to, S::AwaitingMetadata);
        QCOMPARE(m.handle(E::MetadataReady, c).to, S::ProbingProvider);
        QCOMPARE(m.handle(E::ProviderReady, c).to, S::HttpActive);
        QVERIFY(httpDispatchAllowed(m.state()));
        QVERIFY(!swarmAllowed(m.state(), c)); // HTTP active => swarm held
        QCOMPARE(m.handle(E::Completed, c).to, S::Completed);
    }

    void scenario30To55() const
    {
        // F02/F03: outage at 30% with consent, recovery at 55%.
        TransportStateMachine m;
        const auto c = ctx(TransportPolicy::DebridPreferred, true);
        m.handle(E::Enable, c);
        m.handle(E::ProviderReady, c);
        QCOMPARE(m.handle(E::HttpOutage, c, Reason::ProviderOutage).to, S::HttpDraining);
        QVERIFY(!httpDispatchAllowed(m.state()));
        QVERIFY(!swarmAllowed(m.state(), c)); // still draining HTTP, P2P not yet
        QCOMPARE(m.handle(E::HttpJobsSettled, c).to, S::P2PActive);
        QVERIFY(swarmAllowed(m.state(), c));
        QCOMPARE(m.handle(E::RecoveryDue, c).to, S::RecoveryProbing);
        QCOMPARE(m.handle(E::RecoveryProbeFailed, c).to, S::P2PActive);
        m.handle(E::RecoveryDue, c);
        QCOMPARE(m.handle(E::RecoveryProbeOk, c).to, S::HttpReady);
        QVERIFY(!swarmAllowed(m.state(), c)); // draining peers
        QVERIFY(!httpDispatchAllowed(m.state()));
        QCOMPARE(m.handle(E::PeersDrained, c).to, S::HttpActive);
    }

    void preferredWithoutConsentAsksUser() const
    {
        TransportStateMachine m;
        const auto c = ctx(TransportPolicy::DebridPreferred, false);
        m.handle(E::Enable, c);
        const Transition t = m.handle(E::ProviderUnavailable, c);
        QCOMPARE(t.to, S::WaitingForUser);
        QCOMPARE(t.reason, Reason::PermissionRequiredForP2P);
        const auto granted = ctx(TransportPolicy::DebridPreferred, true);
        QCOMPARE(m.handle(E::ConsentChanged, granted).to, S::P2PActive);
        // Revoking consent stops swarm activity immediately.
        QCOMPARE(m.handle(E::ConsentChanged, c).to, S::ProbingProvider);
    }

    void debridOnlyWaits() const
    {
        TransportStateMachine m;
        const auto c = ctx(TransportPolicy::DebridOnly, true /* ignored */);
        m.handle(E::Enable, c);
        m.handle(E::ProviderReady, c);
        m.handle(E::HttpOutage, c);
        QCOMPARE(m.handle(E::HttpJobsSettled, c).to, S::WaitingForProvider);
        QCOMPARE(m.handle(E::RetryDue, c).to, S::ProbingProvider);
        QCOMPARE(m.handle(E::ProviderNeedsUser, c).to, S::WaitingForUser);
    }

    void providerExhaustedHandsTheRestToPeersOnlyIfPermitted() const
    {
        // The provider supplied all it can; the remaining pieces need bytes it does not expose.
        const auto ready = [](TransportStateMachine &m, const MachineContext &c)
        {
            m.handle(E::Enable, c);
            m.handle(E::ProviderReady, c);
            QCOMPARE(m.state(), S::HttpActive);
        };

        TransportStateMachine consented;
        const auto withConsent = ctx(TransportPolicy::DebridPreferred, true);
        ready(consented, withConsent);
        const Transition toPeers = consented.handle(E::HttpExhausted, withConsent, Reason::ProviderLacksPieces);
        QCOMPARE(toPeers.to, S::P2PActive);
        QCOMPARE(toPeers.reason, Reason::ProviderLacksPieces);

        TransportStateMachine unconsented;
        const auto withoutConsent = ctx(TransportPolicy::DebridPreferred, false);
        ready(unconsented, withoutConsent);
        const Transition ask = unconsented.handle(E::HttpExhausted, withoutConsent, Reason::ProviderLacksPieces);
        QCOMPARE(ask.to, S::WaitingForUser);
        QCOMPARE(ask.reason, Reason::PermissionRequiredForP2P);

        TransportStateMachine strict;
        const auto debridOnly = ctx(TransportPolicy::DebridOnly, true /* ignored */);
        ready(strict, debridOnly);
        const Transition stop = strict.handle(E::HttpExhausted, debridOnly, Reason::ProviderLacksPieces);
        QCOMPARE(stop.to, S::WaitingForUser);   // retrying cannot produce the missing bytes
        QCOMPARE(stop.reason, Reason::ProviderLacksPieces);
        QVERIFY(!swarmAllowed(strict.state(), debridOnly));

        // Only meaningful while HTTP is active.
        TransportStateMachine probing;
        probing.handle(E::Enable, withConsent);
        QCOMPARE(probing.handle(E::HttpExhausted, withConsent).to, S::ProbingProvider);
    }

    void switchingToDebridOnlyStopsP2P() const
    {
        TransportStateMachine m;
        auto c = ctx(TransportPolicy::DebridPreferred, true);
        m.handle(E::Enable, c);
        m.handle(E::ProviderUnavailable, c);
        QCOMPARE(m.state(), S::P2PActive);
        c.policy = TransportPolicy::DebridOnly;
        QCOMPARE(m.handle(E::PolicyChanged, c).to, S::ProbingProvider);
        c.policy = TransportPolicy::TorrentOnly;
        QCOMPARE(m.handle(E::PolicyChanged, c).to, S::Disabled);
    }

    void pauseRecheckRestart() const
    {
        TransportStateMachine m;
        const auto c = ctx(TransportPolicy::DebridPreferred, true);
        m.handle(E::Enable, c);
        m.handle(E::ProviderReady, c);
        QCOMPARE(m.handle(E::UserPause, c).to, S::Paused);
        QCOMPARE(m.handle(E::ProviderReady, c).to, S::Paused); // ignored while paused
        QCOMPARE(m.handle(E::UserResume, c).to, S::ProbingProvider);
        m.handle(E::ProviderReady, c);
        QCOMPARE(m.handle(E::RecheckStarted, c).to, S::Checking);
        QVERIFY(!httpDispatchAllowed(m.state())); // add_piece suspended during checking
        QCOMPARE(m.handle(E::RecheckFinished, c).to, S::ProbingProvider);
        m.resetForRestart(c);
        QCOMPARE(m.state(), S::ProbingProvider); // never restored as HttpActive (I6)
    }

    void diskErrorIsFatal() const
    {
        TransportStateMachine m;
        const auto c = ctx(TransportPolicy::DebridPreferred, true);
        m.handle(E::Enable, c);
        QCOMPARE(m.handle(E::FatalError, c, Reason::DiskError).to, S::Error);
    }

    void debridOnlyNeverReachesSwarm() const
    {
        // Release-blocker property (ADR-004, G4 logic layer): random event storms under debrid_only
        // (and under debrid_preferred without consent) never permit swarm activity.
        const QList<MachineContext> contexts =
        {
            ctx(TransportPolicy::DebridOnly, false, false),
            ctx(TransportPolicy::DebridOnly, true, true),
            ctx(TransportPolicy::DebridPreferred, false, true),
            ctx(TransportPolicy::DebridPreferred, false, false),
            // Combined mode asked for where P2P is not permitted: still no swarm (ADR-011).
            ctx(TransportPolicy::DebridOnly, true, true, true),
            ctx(TransportPolicy::DebridPreferred, false, true, true)
        };
        constexpr int eventCount = static_cast<int>(E::TailToPeers) + 1;
        quint32 seed = 4242;
        for (const MachineContext &c : contexts)
        {
            TransportStateMachine m;
            m.handle(E::Enable, c);
            for (int i = 0; i < 50000; ++i)
            {
                seed = (seed * 1664525u) + 1013904223u;
                const auto ev = static_cast<E>((seed >> 8) % eventCount);
                if (ev == E::Disable || ev == E::PolicyChanged)
                    continue; // policy is fixed for this property
                m.handle(ev, c);
                QVERIFY2(!swarmAllowed(m.state(), c), qPrintable(wireName(m.state())));
                QVERIFY(m.state() != S::P2PActive);
                QVERIFY(m.state() != S::RecoveryProbing);
                QVERIFY(m.state() != S::CombinedActive);
            }
        }
    }

    void combinedModeRunsBothSources() const
    {
        // ADR-011: with combined mode in effect the provider no longer pauses peers.
        TransportStateMachine m;
        const auto c = ctx(TransportPolicy::DebridPreferred, true, true, true);
        m.handle(E::Enable, c);
        QCOMPARE(m.state(), S::ProbingProvider);
        QCOMPARE(m.handle(E::ProviderReady, c).to, S::CombinedActive);
        QVERIFY(swarmAllowed(m.state(), c));
        QVERIFY(httpDispatchAllowed(m.state()));
        QCOMPARE(activeTransport(m.state()), ActiveTransport::Combined);
        QCOMPARE(wireName(m.state()), QStringLiteral("COMBINED_ACTIVE"));
        QCOMPARE(wireName(ActiveTransport::Combined), QStringLiteral("combined"));

        // Provider fails: peers are already running, so no drain step.
        QCOMPARE(m.handle(E::HttpOutage, c, Reason::ProviderOutage).to, S::P2PActive);
        // Recovery returns straight to both sources, without pausing peers.
        m.handle(E::RecoveryDue, c);
        QCOMPARE(m.handle(E::RecoveryProbeOk, c).to, S::CombinedActive);
        // Provider has nothing more: peers finish.
        QCOMPARE(m.handle(E::HttpExhausted, c, Reason::ProviderLacksPieces).to, S::P2PActive);
    }

    void combinedModeTailGoesToOneSource() const
    {
        const auto c = ctx(TransportPolicy::DebridPreferred, true, true, true);
        const auto combined = [&c]
        {
            TransportStateMachine m;
            m.handle(E::Enable, c);
            m.handle(E::ProviderReady, c);
            return m;
        };

        TransportStateMachine toProvider = combined();
        QCOMPARE(toProvider.handle(E::TailToProvider, c, Reason::TailToProvider).to, S::HttpReady);
        QVERIFY(!swarmAllowed(toProvider.state(), c));   // peers are held while they drain
        QCOMPARE(toProvider.handle(E::PeersDrained, c).to, S::HttpActive);

        TransportStateMachine toPeers = combined();
        const Transition t = toPeers.handle(E::TailToPeers, c, Reason::TailToPeers);
        QCOMPARE(t.to, S::P2PActive);
        QCOMPARE(t.reason, Reason::TailToPeers);
    }

    void combinedModeCanBeSwitchedWhileRunning() const
    {
        const auto on = ctx(TransportPolicy::DebridPreferred, true, true, true);
        const auto off = ctx(TransportPolicy::DebridPreferred, true, true, false);

        TransportStateMachine m;
        m.handle(E::Enable, off);
        QCOMPARE(m.handle(E::ProviderReady, off).to, S::HttpActive);   // serialized as before
        QCOMPARE(m.handle(E::CombinedChanged, on).to, S::CombinedActive);
        QCOMPARE(m.handle(E::CombinedChanged, off).to, S::HttpReady);   // peers drain first
        QCOMPARE(m.handle(E::PeersDrained, off).to, S::HttpActive);

        // Without combined mode nothing reaches CombinedActive, and recovery pauses peers first.
        TransportStateMachine serial;
        serial.handle(E::Enable, off);
        serial.handle(E::ProviderUnavailable, off);
        QCOMPARE(serial.state(), S::P2PActive);
        serial.handle(E::RecoveryDue, off);
        QCOMPARE(serial.handle(E::RecoveryProbeOk, off).to, S::HttpReady);
        QCOMPARE(serial.handle(E::TailToPeers, off).to, S::HttpReady);   // ignored outside combined mode
    }

    void combinedModeStopsWhenConsentIsWithdrawn() const
    {
        TransportStateMachine m;
        const auto on = ctx(TransportPolicy::DebridPreferred, true, true, true);
        m.handle(E::Enable, on);
        m.handle(E::ProviderReady, on);
        QCOMPARE(m.state(), S::CombinedActive);

        const auto revoked = ctx(TransportPolicy::DebridPreferred, false, true, false);
        QCOMPARE(m.handle(E::ConsentChanged, revoked).to, S::ProbingProvider);
        QVERIFY(!swarmAllowed(m.state(), revoked));

        TransportStateMachine strict;
        strict.handle(E::Enable, on);
        strict.handle(E::ProviderReady, on);
        const auto debridOnly = ctx(TransportPolicy::DebridOnly, false, true, false);
        QCOMPARE(strict.handle(E::PolicyChanged, debridOnly).to, S::ProbingProvider);
        QVERIFY(!swarmAllowed(strict.state(), debridOnly));
    }

    void stateNames() const
    {
        QCOMPARE(wireName(S::HttpActive), QStringLiteral("HTTP_ACTIVE"));
        QCOMPARE(wireName(S::P2PActive), QStringLiteral("P2P_ACTIVE"));
        QCOMPARE(wireName(S::WaitingForProvider), QStringLiteral("WAITING_FOR_PROVIDER"));
        QCOMPARE(wireName(S::AwaitingMetadata), QStringLiteral("AWAITING_METADATA"));
    }
};

QTEST_APPLESS_MAIN(TestHybridStateMachine)
#include "testhybridstatemachine.moc"

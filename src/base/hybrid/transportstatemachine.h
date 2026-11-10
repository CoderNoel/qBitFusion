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

#include "hybridtypes.h"

// Pure per-torrent transport state machine (spec 04). No I/O, timers or libtorrent calls: the
// coordinator feeds events and executes the side effects implied by the resulting state.

namespace Hybrid
{
    enum class MachineEvent
    {
        Enable,
        Disable,
        PolicyChanged,
        ConsentChanged,
        MetadataReady,
        ProviderReady,           // exact mapping + range preflight succeeded
        ProviderUnavailable,     // uncached / outage / rate limit: automatic retry possible
        ProviderNeedsUser,       // auth / forbidden / mismatch: no automatic retry
        HttpOutage,              // circuit opened while HTTP active
        HttpJobsSettled,         // in-flight HTTP cancelled, submitted pieces resolved
        HttpExhausted,           // provider supplied all it can; the rest needs bytes it does not expose
        RecoveryDue,             // backoff elapsed while P2P active
        RecoveryProbeOk,         // provider revalidated
        RecoveryProbeFailed,
        PeersDrained,            // outstanding peer pieces finished / torrent gracefully paused
        RetryDue,                // backoff elapsed while waiting
        UserPause,
        UserResume,
        RecheckStarted,
        RecheckFinished,
        Completed,
        FatalError
    };

    struct MachineContext
    {
        TransportPolicy policy = TransportPolicy::TorrentOnly;
        bool fallbackConsent = false;
        bool hasMetadata = false;
    };

    struct Transition
    {
        TransportState from = TransportState::Disabled;
        TransportState to = TransportState::Disabled;
        Reason reason = Reason::None;
        bool changed = false;
    };

    class TransportStateMachine
    {
    public:
        TransportState state() const;
        Transition handle(MachineEvent event, const MachineContext &context, Reason reason = Reason::None);

        // Restart: state is re-derived, never restored from disk (spec 04, invariant I6).
        void resetForRestart(const MachineContext &context);

    private:
        TransportState next(MachineEvent event, const MachineContext &context, Reason &reason) const;
        TransportState entryState(const MachineContext &context) const;
        TransportState fallbackState(const MachineContext &context, bool needsUser, Reason &reason) const;

        TransportState m_state = TransportState::Disabled;
    };

    // Whether libtorrent may run swarm networking (peers, trackers, DHT, PEX, LSD) for the torrent.
    // Only P2PActive and RecoveryProbing (and the Disabled/TorrentOnly upstream path) allow it.
    // HttpReady drains peers and therefore must already be graceful-pausing.
    bool swarmAllowed(TransportState state, const MachineContext &context);

    // Whether the HTTP piece producer may dispatch new work.
    bool httpDispatchAllowed(TransportState state);

    ActiveTransport activeTransport(TransportState state);
}

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

#include "transportstatemachine.h"

namespace
{
    bool isHybrid(const Hybrid::MachineContext &context)
    {
        return context.policy != Hybrid::TransportPolicy::TorrentOnly;
    }

    bool p2pPermitted(const Hybrid::MachineContext &context)
    {
        return Hybrid::policyAllowsP2P(context.policy, context.fallbackConsent);
    }
}

Hybrid::TransportState Hybrid::TransportStateMachine::state() const
{
    return m_state;
}

Hybrid::TransportState Hybrid::TransportStateMachine::entryState(const MachineContext &context) const
{
    if (!isHybrid(context))
        return TransportState::Disabled;
    return context.hasMetadata ? TransportState::ProbingProvider : TransportState::AwaitingMetadata;
}

Hybrid::TransportState Hybrid::TransportStateMachine::fallbackState(const MachineContext &context, const bool needsUser, Reason &reason) const
{
    if (p2pPermitted(context))
        return TransportState::P2PActive;

    if ((context.policy == TransportPolicy::DebridPreferred) && !context.fallbackConsent)
    {
        // Fallback would be possible, but the user has not consented to exposing their IP.
        reason = Reason::PermissionRequiredForP2P;
        return TransportState::WaitingForUser;
    }

    return needsUser ? TransportState::WaitingForUser : TransportState::WaitingForProvider;
}

Hybrid::TransportState Hybrid::TransportStateMachine::next(const MachineEvent event, const MachineContext &context, Reason &reason) const
{
    using S = TransportState;
    using E = MachineEvent;

    // Global events first.
    switch (event)
    {
    case E::Disable:
        return S::Disabled;
    case E::FatalError:
        return (m_state == S::Disabled) ? S::Disabled : S::Error;
    case E::PolicyChanged:
        if (!isHybrid(context))
            return S::Disabled;
        if ((m_state == S::Paused) || (m_state == S::Checking) || (m_state == S::Completed))
            return m_state;
        // Re-evaluate from scratch: stops P2P immediately if it is no longer permitted.
        if ((m_state == S::P2PActive || m_state == S::RecoveryProbing) && p2pPermitted(context))
            return m_state;
        return entryState(context);
    case E::ConsentChanged:
        if (!isHybrid(context))
            return m_state;
        if ((m_state == S::P2PActive || m_state == S::RecoveryProbing) && !p2pPermitted(context))
            return entryState(context);  // consent revoked: stop swarm activity now
        if ((m_state == S::WaitingForUser) && p2pPermitted(context))
            return S::P2PActive;
        return m_state;
    case E::Completed:
        return ((m_state == S::Disabled) || (m_state == S::Error)) ? m_state : S::Completed;
    case E::UserPause:
        return (m_state == S::Disabled) ? S::Disabled : S::Paused;
    case E::RecheckStarted:
        return (m_state == S::Disabled) ? S::Disabled : S::Checking;
    default:
        break;
    }

    switch (m_state)
    {
    case S::Disabled:
        if (event == E::Enable)
            return entryState(context);
        return S::Disabled;

    case S::AwaitingMetadata:
        if (event == E::MetadataReady)
            return S::ProbingProvider;
        if (event == E::ProviderNeedsUser)
            return S::WaitingForUser;   // e.g. metadata unavailable without P2P
        return m_state;

    case S::ProbingProvider:
        if (event == E::ProviderReady)
            return S::HttpActive;
        if (event == E::ProviderUnavailable)
            return fallbackState(context, false, reason);
        if (event == E::ProviderNeedsUser)
            return fallbackState(context, true, reason);
        return m_state;

    case S::HttpReady:
        // Provider recovered; waiting for peers to drain before HTTP takes over (spec 03 P2P -> HTTP).
        if (event == E::PeersDrained)
            return S::HttpActive;
        if ((event == E::HttpOutage) || (event == E::ProviderUnavailable) || (event == E::ProviderNeedsUser))
            return fallbackState(context, event == E::ProviderNeedsUser, reason);
        return m_state;

    case S::HttpActive:
        if ((event == E::HttpOutage) || (event == E::ProviderUnavailable) || (event == E::ProviderNeedsUser))
            return S::HttpDraining;
        return m_state;

    case S::HttpDraining:
        if (event == E::HttpJobsSettled)
            return fallbackState(context, false, reason);
        return m_state;

    case S::P2PActive:
        if (event == E::RecoveryDue)
            return S::RecoveryProbing;
        return m_state;

    case S::RecoveryProbing:
        if (event == E::RecoveryProbeOk)
            return S::HttpReady;
        if ((event == E::RecoveryProbeFailed) || (event == E::ProviderNeedsUser))
            return S::P2PActive;
        return m_state;

    case S::WaitingForProvider:
        if (event == E::RetryDue)
            return context.hasMetadata ? S::ProbingProvider : S::AwaitingMetadata;
        return m_state;

    case S::WaitingForUser:
        if ((event == E::RetryDue) || (event == E::UserResume))
            return entryState(context);
        return m_state;

    case S::Paused:
        if (event == E::UserResume)
            return entryState(context);
        return m_state;

    case S::Checking:
        if (event == E::RecheckFinished)
            return entryState(context);
        return m_state;

    case S::Completed:
    case S::Error:
        if (event == E::UserResume)
            return entryState(context);
        return m_state;
    }

    return m_state;
}

Hybrid::Transition Hybrid::TransportStateMachine::handle(const MachineEvent event, const MachineContext &context, Reason reason)
{
    const TransportState from = m_state;
    TransportState to = next(event, context, reason);

    // Defence in depth for ADR-004: whatever the table says, a policy that forbids P2P can never
    // land in a swarm state.
    if (!p2pPermitted(context) && ((to == TransportState::P2PActive) || (to == TransportState::RecoveryProbing)))
    {
        reason = Reason::PermissionRequiredForP2P;
        to = (context.policy == TransportPolicy::DebridPreferred) ? TransportState::WaitingForUser : TransportState::WaitingForProvider;
    }

    m_state = to;
    return Transition {from, to, reason, from != to};
}

void Hybrid::TransportStateMachine::resetForRestart(const MachineContext &context)
{
    m_state = entryState(context);
}

bool Hybrid::swarmAllowed(const TransportState state, const MachineContext &context)
{
    if (context.policy == TransportPolicy::TorrentOnly)
        return true;
    if (!policyAllowsP2P(context.policy, context.fallbackConsent))
        return false;
    return (state == TransportState::P2PActive) || (state == TransportState::RecoveryProbing);
}

bool Hybrid::httpDispatchAllowed(const TransportState state)
{
    return state == TransportState::HttpActive;
}

Hybrid::ActiveTransport Hybrid::activeTransport(const TransportState state)
{
    switch (state)
    {
    case TransportState::HttpActive:
        return ActiveTransport::Http;
    case TransportState::P2PActive:
    case TransportState::RecoveryProbing:
        return ActiveTransport::P2P;
    case TransportState::HttpDraining:
    case TransportState::HttpReady:
        return ActiveTransport::Switching;
    default:
        return ActiveTransport::None;
    }
}

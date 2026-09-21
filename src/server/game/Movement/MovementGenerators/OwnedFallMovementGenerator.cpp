/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "OwnedFallMovementGenerator.h"
#include "GameTime.h"
#include "MoveSpline.h"
#include "MoveSplineInit.h"
#include "Player.h"
#include <atomic>

namespace
{
    std::atomic<uint64> NextOwnedFallToken{0};
    constexpr uint32 FallFlags = MOVEMENTFLAG_FALLING | MOVEMENTFLAG_FALLING_FAR;
    constexpr uint32 ForeignFrames = MOVEMENTFLAG_ONTRANSPORT | MOVEMENTFLAG_SWIMMING | MOVEMENTFLAG_FLYING |
        MOVEMENTFLAG_CAN_FLY | MOVEMENTFLAG_DISABLE_GRAVITY | MOVEMENTFLAG_HOVER | MOVEMENTFLAG_WATERWALKING;

    G3D::Vector3 CurrentPosition(Unit const& unit)
    {
        return {unit.GetPositionX(), unit.GetPositionY(), unit.GetPositionZ()};
    }
}

bool Unit::HasOwnedFall() const
{
    return _ownedFall && _ownedFall->Status.Result == OwnedFallResult::Active;
}

bool Unit::OwnedFallContextMatches() const
{
    auto const& status = _ownedFall->Status;
    return IsInWorld() && FindMap() && GetMapId() == status.MapId && GetInstanceId() == status.InstanceId &&
        GetPhaseMask() == status.PhaseMask && !GetTransport() && !GetVehicle() && !GetTransGUID() &&
        !movespline->onTransport && !HasUnitMovementFlag(ForeignFrames);
}

bool Unit::OwnsFallSpline() const
{
    return HasOwnedFall() && movespline->GetId() == _ownedFall->Status.SplineId;
}

std::optional<OwnedFallStatus> Unit::GetOwnedFallStatus(OwnedFallToken token) const
{
    if (!token || !_ownedFall || _ownedFall->Status.Token != token)
        return std::nullopt;

    auto status = _ownedFall->Status;
    if (OwnsFallSpline() && movespline->Initialized())
    {
        status.Elapsed = movespline->timePassed();
        status.Duration = movespline->Duration();
        status.InterruptCount = movespline->GetInterruptCount();
        status.Position = CurrentPosition(*this);
    }
    return status;
}

OwnedFallToken Unit::PrepareOwnedFall(uint32 id, G3D::Vector3 const& destination)
{
    if (!_ownedFall)
        _ownedFall = std::make_unique<OwnedFallData>();
    auto& status = _ownedFall->Status;
    status = {};
    status.Token.Value = ++NextOwnedFallToken;
    status.MovementId = id;
    status.MapId = GetMapId();
    status.InstanceId = GetInstanceId();
    status.PhaseMask = GetPhaseMask();
    status.Start = status.Position = CurrentPosition(*this);
    status.Destination = destination;
    return status.Token;
}

bool Unit::CanLaunchOwnedFall(OwnedFallToken token) const
{
    return token && _ownedFall && _ownedFall->Status.Token == token &&
        _ownedFall->Status.Result == OwnedFallResult::Pending && OwnedFallContextMatches() &&
        IsPlayer() && IsAlive() && !ToPlayer()->IsBeingTeleported() &&
        !HasUnitFlag(UNIT_FLAG_DISABLE_MOVE) && !HasUnitState(UNIT_STATE_NOT_MOVE | UNIT_STATE_IN_FLIGHT) &&
        !HasUnitMovementFlag(FallFlags | MOVEMENTFLAG_ROOT) && !IsMovementPreventedByCasting() &&
        CurrentPosition(*this) == _ownedFall->Status.Start && movespline->Finalized();
}

void Unit::CommitOwnedFall(OwnedFallToken token, uint32 splineId, uint32 duration)
{
    ASSERT(_ownedFall && _ownedFall->Status.Token == token);
    auto& status = _ownedFall->Status;
    m_movementInfo.SetFallTime(0);
    ToPlayer()->SetFallInformation(GameTime::GetGameTime().count(), status.Start.z);
    status.SplineId = splineId;
    status.Duration = duration;
    status.InterruptCount = movespline->GetInterruptCount();
    status.Result = OwnedFallResult::Active;
}

void Unit::FailOwnedFall(OwnedFallToken token)
{
    if (_ownedFall && _ownedFall->Status.Token == token &&
        _ownedFall->Status.Result == OwnedFallResult::Pending)
        _ownedFall->Status.Result = OwnedFallResult::Failed;
}

uint32 Unit::RetireOwnedFall(OwnedFallResult result, bool stop, bool transfer)
{
    if (!HasOwnedFall())
        return 0;

    auto& status = _ownedFall->Status;
    bool ownSpline = OwnsFallSpline();
    bool knownState = ownSpline && OwnedFallContextMatches() &&
        HasUnitMovementFlag(MOVEMENTFLAG_FALLING) && !HasUnitMovementFlag(MOVEMENTFLAG_FALLING_FAR);
    status.Position = CurrentPosition(*this);
    if (ownSpline && movespline->Initialized())
    {
        status.Elapsed = movespline->timePassed();
        status.Duration = movespline->Duration();
    }
    status.InterruptCount = movespline->GetInterruptCount();
    status.Result = knownState ? result : OwnedFallResult::Revoked;
    if (!knownState)
    {
        if (stop && ownSpline && !movespline->Finalized())
        {
            Movement::MoveSplineInit init(this);
            init.Stop(true);
        }
        return 0;
    }
    if (transfer)
        return 0;

    uint32 released = m_movementInfo.GetMovementFlags() & MOVEMENTFLAG_FALLING;
    m_movementInfo.RemoveMovementFlag(MOVEMENTFLAG_FALLING);
    m_movementInfo.SetFallTime(0);
    if (IsPlayer())
        ToPlayer()->SetFallInformation(0, GetPositionZ());
    if (stop && !movespline->Finalized())
    {
        Movement::MoveSplineInit init(this);
        init.Stop(true);
    }
    return released;
}

void Unit::RevokeOwnedFall()
{
    RetireOwnedFall(OwnedFallResult::Revoked, true);
    if (_ownedFall)
    {
        ++_ownedFall->BookkeepingVersion;
        if (_ownedFall->Status.Result == OwnedFallResult::Pending)
            _ownedFall->Status.Result = OwnedFallResult::Revoked;
    }
}

uint32 Unit::PrepareFallSplineTransition(bool airborne)
{
    return RetireOwnedFall(OwnedFallResult::Replaced, false, airborne);
}

void Unit::OnFallInformationChanged()
{
    if (!_ownedFall)
        return;
    RetireOwnedFall(OwnedFallResult::Revoked, true);
    ++_ownedFall->BookkeepingVersion;
}

void Unit::AddUnitMovementFlag(uint32 flags)
{
    // An explicit same-bit assertion is a new writer; a whole-mask inheritance is not.
    if (flags & (FallFlags | ForeignFrames))
    {
        RevokeOwnedFall();
        if (_ownedFall)
            ++_ownedFall->BookkeepingVersion;
    }
    m_movementInfo.flags |= flags;
}

void Unit::RemoveUnitMovementFlag(uint32 flags)
{
    if (flags & FallFlags)
    {
        RevokeOwnedFall();
        if (_ownedFall)
            ++_ownedFall->BookkeepingVersion;
    }
    m_movementInfo.flags &= ~flags;
}

void Unit::SetUnitMovementFlags(uint32 flags)
{
    bool foreignChange = ((flags ^ m_movementInfo.flags) & (FallFlags | ForeignFrames)) != 0;
    if (HasOwnedFall() && ((flags & ForeignFrames) || !(flags & MOVEMENTFLAG_FALLING)))
        RevokeOwnedFall();
    if (foreignChange && _ownedFall)
        ++_ownedFall->BookkeepingVersion;
    m_movementInfo.flags = flags;
}

void Unit::CheckOwnedFallBeforeUpdate()
{
    if (!HasOwnedFall())
        return;
    if (!OwnedFallContextMatches() || !IsAlive() || !IsPlayer() || ToPlayer()->IsBeingTeleported() ||
        HasUnitFlag(UNIT_FLAG_DISABLE_MOVE) || HasUnitState(UNIT_STATE_NOT_MOVE | UNIT_STATE_IN_FLIGHT) ||
        HasUnitMovementFlag(MOVEMENTFLAG_ROOT) || IsMovementPreventedByCasting())
        RevokeOwnedFall();
    else if (!OwnsFallSpline())
        _ownedFall->Status.Result = OwnedFallResult::Revoked;
    else if (movespline->Finalized())
        RetireOwnedFall(OwnedFallResult::Cancelled, false);
}

void Unit::CompleteOwnedFall()
{
    if (!OwnsFallSpline())
        return;

    auto& status = _ownedFall->Status;
    if (!OwnedFallContextMatches() || !IsPlayer() || !IsAlive() ||
        !HasUnitMovementFlag(MOVEMENTFLAG_FALLING) || HasUnitMovementFlag(MOVEMENTFLAG_FALLING_FAR) ||
        !movespline->Initialized() || !movespline->Finalized() ||
        !movespline->isFalling() || movespline->timePassed() != movespline->Duration() ||
        movespline->GetInterruptCount() != status.InterruptCount + 1 ||
        (CurrentPosition(*this) - movespline->ComputePosition()).squaredLength() > 0.0001f)
    {
        RetireOwnedFall(OwnedFallResult::Revoked, false);
        return;
    }

    auto token = status.Token;
    uint32 splineId = status.SplineId;
    uint64 version = _ownedFall->BookkeepingVersion;
    status.Result = OwnedFallResult::Landed;
    status.Elapsed = movespline->timePassed();
    status.Duration = movespline->Duration();
    status.InterruptCount = movespline->GetInterruptCount();
    status.Position = CurrentPosition(*this);
    m_movementInfo.RemoveMovementFlag(MOVEMENTFLAG_FALLING);

    MovementInfo landing = m_movementInfo;
    landing.pos.Relocate(GetPosition());
    landing.fallTime = status.Elapsed;
    // This is the real terminal spline position, never a synthetic destination supplied at cancellation.
    status.LandingHandled = true;
    ToPlayer()->HandleFall(landing);

    // Landing effects can synchronously change motion or fall information. Never reset the new writer.
    if (_ownedFall->Status.Token == token && _ownedFall->Status.Result == OwnedFallResult::Landed &&
        movespline->GetId() == splineId && _ownedFall->BookkeepingVersion == version &&
        OwnedFallContextMatches() && IsAlive())
    {
        m_movementInfo.SetFallTime(0);
        ToPlayer()->SetFallInformation(0, GetPositionZ());
    }
}

void OwnedFallMovementGenerator::Initialize(Unit* unit)
{
    auto token = _token;
    MovementGenerator const* initializingGenerator = this;
    auto status = unit->GetOwnedFallStatus(token);
    if (!status || !unit->CanLaunchOwnedFall(token))
    {
        unit->FailOwnedFall(token);
        return;
    }
    Movement::MoveSplineInit init(unit);
    init.MoveTo(status->Destination.x, status->Destination.y, status->Destination.z);
    init.SetFall();
    if (init.LaunchOwnedFall(token) <= 0)
        unit->FailOwnedFall(token);
    else if (auto launched = unit->GetOwnedFallStatus(token))
        if (launched->Result == OwnedFallResult::Active &&
            unit->GetMotionMaster()->GetMotionSlot(MOTION_SLOT_CONTROLLED) == initializingGenerator &&
            unit->movespline->GetId() == launched->SplineId)
            _splineId = launched->SplineId;
}

bool OwnedFallMovementGenerator::Update(Unit* unit, uint32)
{
    unit->CheckOwnedFallBeforeUpdate();
    auto status = unit->GetOwnedFallStatus(_token);
    return status && status->Result == OwnedFallResult::Active;
}

void OwnedFallMovementGenerator::Finalize(Unit* unit)
{
    auto status = unit->GetOwnedFallStatus(_token);
    if (status && status->Result == OwnedFallResult::Active)
        unit->RetireOwnedFall(OwnedFallResult::Cancelled, true);
    else
        unit->FailOwnedFall(_token);
}

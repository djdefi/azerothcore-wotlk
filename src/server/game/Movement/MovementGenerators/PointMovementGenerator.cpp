/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "PointMovementGenerator.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "Log.h"
#include "MoveSpline.h"
#include "MoveSplineInit.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "World.h"
#include <limits>

namespace
{
    constexpr float PointPathTolerance = 0.01f;
    constexpr float PointPathToleranceSq = PointPathTolerance * PointPathTolerance;

    G3D::Vector3 PointPathPosition(Unit const& unit)
    {
        return {unit.GetPositionX(), unit.GetPositionY(), unit.GetPositionZ()};
    }

    bool SamePointPathPosition(G3D::Vector3 const& left, G3D::Vector3 const& right)
    {
        return (left - right).squaredLength() <= PointPathToleranceSq;
    }

    bool PointPathGhost(Unit const& unit)
    {
        return unit.IsPlayer() && unit.ToPlayer()->HasPlayerFlag(PLAYER_FLAGS_GHOST);
    }

    bool PointPathContext(Unit const& unit, uint32 mapId, uint32 instanceId)
    {
        if (!unit.IsInWorld() || !unit.FindMap())
            return false;

        bool ghost = PointPathGhost(unit);
        return (ghost ? unit.isDead() : unit.IsAlive()) &&
            unit.GetMapId() == mapId && unit.GetInstanceId() == instanceId &&
            !unit.HasUnitFlag(UNIT_FLAG_DISABLE_MOVE) &&
            !unit.HasUnitState(UNIT_STATE_IN_FLIGHT | UNIT_STATE_DIED) &&
            !unit.GetTransport() && !unit.GetVehicle() && !unit.GetTransGUID() && !unit.movespline->onTransport &&
            !unit.HasUnitMovementFlag(MovementFlags(MOVEMENTFLAG_ONTRANSPORT | MOVEMENTFLAG_SWIMMING |
                MOVEMENTFLAG_FLYING | MOVEMENTFLAG_CAN_FLY | MOVEMENTFLAG_DISABLE_GRAVITY |
                MOVEMENTFLAG_FALLING | MOVEMENTFLAG_FALLING_FAR | MOVEMENTFLAG_HOVER)) &&
            (ghost || !unit.HasUnitMovementFlag(MOVEMENTFLAG_WATERWALKING)) &&
            (!unit.IsPlayer() || !unit.ToPlayer()->IsBeingTeleported());
    }

    float PointPathSpeed(Unit const& unit, ForcedMovement forced, float speed, bool backwards)
    {
        uint32 flags = unit.m_movementInfo.GetMovementFlags();
        if (forced == FORCED_MOVEMENT_WALK)
            flags |= MOVEMENTFLAG_WALKING;
        else if (forced == FORCED_MOVEMENT_RUN)
            flags &= ~MOVEMENTFLAG_WALKING;

        if (backwards)
            flags |= MOVEMENTFLAG_BACKWARD;
        else
            flags &= ~MOVEMENTFLAG_BACKWARD;

        float velocity = speed > 0.0f ? speed : unit.GetSpeed(Movement::SelectSpeedType(flags));
        float runSpeed = unit.GetSpeed(MOVE_RUN);
        if (!std::isfinite(velocity) || !std::isfinite(runSpeed))
            return 0.0f;

        return std::min(velocity, std::max(28.0f, runSpeed * 4.0f));
    }

    bool PointPathGeometry(Movement::PointsArray const& path, float velocity, bool progress = false)
    {
        if (path.size() < 2 || path.size() > MAX_POINT_PATH_LENGTH || velocity <= 0.01f)
            return false;

        double length = 0.0;
        G3D::Vector3 middle = (path.front() + path.back()) * 0.5f;
        for (std::size_t i = 0; i < path.size(); ++i)
        {
            auto const& point = path[i];
            if (!Acore::IsValidMapCoord(point.x, point.y, point.z))
                return false;

            if (i)
            {
                float segment = (point - path[i - 1]).length();
                float minimum = progress && i == 1 ? 0.0f : PointPathTolerance;
                if (!std::isfinite(segment) || segment <= minimum)
                    return false;
                length += segment;
            }

            // The linear monster-move packet packs intermediate offsets in signed 11/11/10-bit quarter yards.
            if (i && i + 1 < path.size() &&
                (std::abs(point.x - middle.x) >= 256.0f || std::abs(point.y - middle.y) >= 256.0f ||
                    std::abs(point.z - middle.z) >= 128.0f))
                return false;
        }

        return length * 1000.0 / velocity < std::numeric_limits<int32>::max() - path.size();
    }

    bool OnPointPathSegment(G3D::Vector3 const& point, G3D::Vector3 const& start, G3D::Vector3 const& end)
    {
        G3D::Vector3 delta = end - start;
        float fraction = (point - start).dot(delta) / delta.squaredLength();
        return SamePointPathPosition(point, start + delta * std::clamp(fraction, 0.0f, 1.0f));
    }
}

bool Movement::CanMovePointPath(Unit const& unit, uint32 id, PointsArray const& path, uint32 mapId,
    uint32 instanceId, ForcedMovement forcedMovement, float speed, float orientation, bool backwards)
{
    if (id == EVENT_CHARGE || id == EVENT_CHARGE_PREPATH ||
        forcedMovement < FORCED_MOVEMENT_NONE || forcedMovement >= FORCED_MOVEMENT_MAX ||
        !std::isfinite(speed) || speed < 0.0f || (speed > 0.0f && speed <= 0.01f) ||
        !std::isfinite(orientation) || orientation < 0.0f ||
        !PointPathContext(unit, mapId, instanceId) ||
        !PointPathGeometry(path, PointPathSpeed(unit, forcedMovement, speed, backwards)))
        return false;

    G3D::Vector3 source = PointPathPosition(unit);
    if (!SamePointPathPosition(source, path.front()))
        return false;

    // Launch uses ComputePosition rather than the stored Unit position while another spline is running.
    return unit.movespline->Finalized() ||
        SamePointPathPosition(unit.movespline->ComputePosition(), path.front());
}

template<class T>
PointMovementGenerator<T>::PointMovementGenerator(uint32 pointId, Movement::PointsArray const& path,
    Unit const& unit, ForcedMovement forcedMovement, float velocity, float orientation, bool backwards)
    : PointMovementGenerator(pointId, path.back().x, path.back().y, path.back().z, forcedMovement, velocity,
        orientation, &path, false, false, std::nullopt, ObjectGuid::Empty, backwards)
{
    _checkedPath = CheckedPath{unit.GetMapId(), unit.GetInstanceId(), unit.GetPhaseMask(),
        unit.movespline->GetId(), unit.movespline->GetInterruptCount(),
        PointPathGhost(unit), unit.HasUnitMovementFlag(MOVEMENTFLAG_WATERWALKING)};
}

template<class T>
bool PointMovementGenerator<T>::OwnsCheckedSpline(T const* unit) const
{
    auto const& stop = unit->movespline->GetLastStop();
    return _checkedPath->Launched && (unit->movespline->GetId() == _checkedPath->SplineId ||
        (stop && stop->SplineId == _checkedPath->SplineId));
}

template<class T>
bool PointMovementGenerator<T>::FailCheckedPath(T* unit, char const* reason)
{
    _checkedPath->Failed = true;
    LOG_DEBUG("movement.motionmaster", "Retiring checked point path for {} (Id: {}): {}",
        unit->GetGUID().ToString(), id, reason);
    if (OwnsCheckedSpline(unit))
    {
        // A displaced source must not be relocated back to the old spline by StopMoving().
        if (!unit->movespline->Finalized())
        {
            Movement::MoveSplineInit init(unit);
            init.Stop(true);
        }
        unit->ClearUnitState(UNIT_STATE_ROAMING | UNIT_STATE_ROAMING_MOVE);
    }
    return false;
}

template<class T>
bool PointMovementGenerator<T>::UpdateCheckedPath(T* unit, uint32 diff)
{
    auto& checked = *_checkedPath;
    if (checked.Failed)
        return false;

    if (!PointPathContext(*unit, checked.MapId, checked.InstanceId) ||
        unit->GetPhaseMask() != checked.PhaseMask ||
        PointPathGhost(*unit) != checked.Ghost ||
        unit->HasUnitMovementFlag(MOVEMENTFLAG_WATERWALKING) != checked.WaterWalking ||
        PointPathSpeed(*unit, _forcedMovement, speed, _reverseOrientation) <= 0.01f)
        return FailCheckedPath(unit, "life state, map, phase, movement mode or speed changed");

    auto const& spline = *unit->movespline;
    G3D::Vector3 source = PointPathPosition(*unit);
    G3D::Vector3 progressSource = source;
    std::size_t next = 1;
    bool stopped = false;
    if (!checked.Launched)
    {
        auto const& stop = spline.GetLastStop();
        bool sameSpline = spline.GetId() == checked.SplineId || (stop && stop->SplineId == checked.SplineId);
        // A covering distract generator can finish by turning in place; this does not alter the source proof.
        bool orientationOnly = spline.Initialized() && spline._Spline().last() - spline._Spline().first() == 1 &&
            spline._Spline().getPoint(spline._Spline().first()) == source && spline.FinalDestination() == source;
        if ((!sameSpline && !orientationOnly) || spline.GetInterruptCount() != checked.InterruptCount ||
            !SamePointPathPosition(source, m_precomputedPath.front()) ||
            (!spline.Finalized() && !SamePointPathPosition(spline.ComputePosition(), m_precomputedPath.front())))
            return FailCheckedPath(unit, "source changed before launch");
    }
    else if (spline.GetId() == checked.SplineId && spline.Initialized())
    {
        bool arrived = spline.Finalized() && spline.timePassed() == spline.Duration();
        // Unit::UpdateSplineMovement calls DisableSpline once on natural arrival.
        if (spline.GetInterruptCount() != checked.InterruptCount &&
            !(arrived && spline.GetInterruptCount() == checked.InterruptCount + 1))
            return FailCheckedPath(unit, "spline interrupted");

        if (!SamePointPathPosition(source, spline.ComputePosition()))
            return FailCheckedPath(unit, "source left owned spline");

        if (arrived)
        {
            checked.Arrived = true;
            return false;
        }
        if (spline.Finalized())
            return FailCheckedPath(unit, "spline interrupted before arrival");

        source = spline.ComputePosition();
        progressSource = source;
        next = spline.currentPathIdx() + 1;
    }
    else
    {
        auto const& stop = spline.GetLastStop();
        if (!stop || stop->SplineId != checked.SplineId ||
            spline.GetInterruptCount() != checked.InterruptCount ||
            !SamePointPathPosition(source, stop->Position))
            return FailCheckedPath(unit, "owned stop provenance lost or source displaced");

        next = stop->PathIndex + 1;
        progressSource = stop->Position;
        stopped = true;
    }

    if (next >= m_precomputedPath.size() ||
        !OnPointPathSegment(source, m_precomputedPath[next - 1], m_precomputedPath[next]))
        return FailCheckedPath(unit, "source left ordered checked segment");

    if (unit->HasUnitState(UNIT_STATE_NOT_MOVE) || unit->HasUnitMovementFlag(MOVEMENTFLAG_ROOT) ||
        unit->IsMovementPreventedByCasting())
    {
        if (!spline.Finalized())
            unit->StopMoving();
        return true;
    }

    if (_pauseTime)
    {
        if (diff < static_cast<uint32>(*_pauseTime))
        {
            *_pauseTime -= diff;
            return true;
        }
        _pauseTime.reset();
        _hasBeenStalled = false;
        i_recalculateSpeed = true;
    }
    if (_stalled)
        return true;

    if (checked.Launched && !stopped && !i_recalculateSpeed)
        return true;

    // Only the recorded segment can consume its exact endpoint; proximity to another vertex is not progress.
    bool finishFacing = false;
    if (checked.Launched && source == m_precomputedPath[next] && progressSource == m_precomputedPath[next])
    {
        if (next + 1 == m_precomputedPath.size())
        {
            if (i_orientation <= 0.0f)
            {
                checked.Arrived = true;
                return false;
            }
            finishFacing = true;
        }
        else
            ++next;
    }

    Movement::PointsArray remaining{source};
    remaining.insert(remaining.end(), m_precomputedPath.begin() + next, m_precomputedPath.end());
    if (!finishFacing && !PointPathGeometry(remaining,
        PointPathSpeed(*unit, _forcedMovement, speed, _reverseOrientation),
        checked.Launched))
        return FailCheckedPath(unit, "remaining path cannot be launched safely");

    Movement::MoveSplineInit init(unit);
    init.MovebyPath(remaining, next - 1);
    if (speed > 0.0f)
        init.SetVelocity(speed);
    if (_forcedMovement != FORCED_MOVEMENT_NONE)
        init.SetWalk(_forcedMovement == FORCED_MOVEMENT_WALK);
    if (_reverseOrientation)
        init.SetOrientationInversed();
    if (i_orientation > 0.0f)
        init.SetFacing(i_orientation);

    if (init.Launch() <= 0)
        return FailCheckedPath(unit, "spline launch refused");

    checked.SplineId = unit->movespline->GetId();
    checked.InterruptCount = unit->movespline->GetInterruptCount();
    checked.Launched = true;
    i_recalculateSpeed = false;
    _hasBeenStalled = false;
    unit->AddUnitState(UNIT_STATE_ROAMING | UNIT_STATE_ROAMING_MOVE);
    return true;
}

//----- Point Movement Generator
template<class T>
void PointMovementGenerator<T>::DoInitialize(T* unit)
{
    if (_checkedPath)
    {
        UpdateCheckedPath(unit, 0);
        return;
    }

    _stalled = false;
    _hasBeenStalled = false;
    _pauseTime.reset();

    if (unit->HasUnitState(UNIT_STATE_NOT_MOVE) || unit->IsMovementPreventedByCasting())
    {
        // the next line is to ensure that a new spline is created in DoUpdate() once the unit is no longer rooted/stunned
        /// @todo: rename this flag to something more appropriate since it is set to true even without speed change now.
        i_recalculateSpeed = true;
        return;
    }

    if (!unit->IsStopped())
        unit->StopMoving();

    unit->AddUnitState(UNIT_STATE_ROAMING | UNIT_STATE_ROAMING_MOVE);
    if (id == EVENT_CHARGE || id == EVENT_CHARGE_PREPATH)
    {
        unit->AddUnitState(UNIT_STATE_CHARGING);
    }

    i_recalculateSpeed = false;
    Movement::MoveSplineInit init(unit);

    // mod-playerbots
    if (_reverseOrientation)
        init.SetOrientationInversed();

    if (m_precomputedPath.size() > 2) // pussywizard: for charge
        init.MovebyPath(m_precomputedPath);
    else if (_generatePath)
    {
        PathGenerator path(unit);
        bool result = path.CalculatePath(i_x, i_y, i_z, _forceDestination);
        if (result && !(path.GetPathType() & PATHFIND_NOPATH) && path.GetPath().size() > 2)
        {
            m_precomputedPath = path.GetPath();
            init.MovebyPath(m_precomputedPath);
        }
        else
        {
            // Xinef: fix strange client visual bug, moving on z coordinate only switches orientation by 180 degrees (visual only)
            if (G3D::fuzzyEq(unit->GetPositionX(), i_x) && G3D::fuzzyEq(unit->GetPositionY(), i_y))
            {
                i_x += 0.2f * cos(unit->GetOrientation());
                i_y += 0.2f * std::sin(unit->GetOrientation());
            }

            init.MoveTo(i_x, i_y, i_z, true);
        }
    }
    else
    {
        // Xinef: fix strange client visual bug, moving on z coordinate only switches orientation by 180 degrees (visual only)
        if (G3D::fuzzyEq(unit->GetPositionX(), i_x) && G3D::fuzzyEq(unit->GetPositionY(), i_y))
        {
            i_x += 0.2f * cos(unit->GetOrientation());
            i_y += 0.2f * std::sin(unit->GetOrientation());
        }

        init.MoveTo(i_x, i_y, i_z, true);
    }
    if (speed > 0.0f)
        init.SetVelocity(speed);

    if (_forcedMovement == FORCED_MOVEMENT_WALK)
        init.SetWalk(true);
    else if (_forcedMovement == FORCED_MOVEMENT_RUN)
        init.SetWalk(false);

    if (i_orientation > 0.0f)
    {
        init.SetFacing(i_orientation);
    }

    if (_animTier)
        init.SetAnimation(*_animTier);

    init.Launch();
}

template<class T>
bool PointMovementGenerator<T>::DoUpdate(T* unit, uint32 diff)
{
    if (!unit)
        return false;

    if (_checkedPath)
        return UpdateCheckedPath(unit, diff);

    if (unit->IsMovementPreventedByCasting())
    {
        unit->StopMoving();
        return true;
    }

    if (unit->HasUnitState(UNIT_STATE_NOT_MOVE))
    {
        if (!unit->HasUnitState(UNIT_STATE_CHARGING))
            unit->StopMoving();
        return true;
    }

    unit->AddUnitState(UNIT_STATE_ROAMING_MOVE);

    if (_pauseTime.has_value())
    {
        if (diff >= static_cast<uint32>(_pauseTime.value()))
            _pauseTime.reset();
        else
        {
            _pauseTime = static_cast<int32>(_pauseTime.value() - diff);
            return true;
        }

        _hasBeenStalled = false;
        _stalled = false;
        i_recalculateSpeed = true;
    }

    // Relaunch path when speed changed or when resuming from a stall.
    // Keep an indefinitely paused movement stalled until Resume() clears _stalled.
    if (id != EVENT_CHARGE_PREPATH && !_stalled && (i_recalculateSpeed || _hasBeenStalled))
    {
        i_recalculateSpeed = false;
        Movement::MoveSplineInit init(unit);
        auto rebasePrecomputedPath = [this, unit](std::optional<uint32> offset = std::nullopt)
        {
            Movement::PointsArray rebasedPath;
            G3D::Vector3 currentPos(unit->GetPositionX(), unit->GetPositionY(), unit->GetPositionZ());

            if (m_precomputedPath.empty())
                return rebasedPath;

            if (offset.has_value())
            {
                if (offset.value() >= m_precomputedPath.size())
                {
                    rebasedPath.push_back(currentPos);
                    rebasedPath.push_back(m_precomputedPath.back());
                    return rebasedPath;
                }

                rebasedPath.insert(rebasedPath.end(), m_precomputedPath.begin() + offset.value(), m_precomputedPath.end());
            }
            else
            {
                uint32 closestPointIndex = 0;
                float closestPointDist = std::numeric_limits<float>::max();
                for (uint32 pointIndex = 1; pointIndex < m_precomputedPath.size(); ++pointIndex)
                {
                    float const sqDist = (currentPos - m_precomputedPath[pointIndex]).squaredLength();
                    if (sqDist < closestPointDist)
                    {
                        closestPointDist = sqDist;
                        closestPointIndex = pointIndex;
                    }
                }

                rebasedPath.insert(rebasedPath.end(), m_precomputedPath.begin() + closestPointIndex, m_precomputedPath.end());
            }

            // MovebyPath requires the first point to be the mover's current position.
            rebasedPath.insert(rebasedPath.begin(), currentPos);
            return rebasedPath;
        };

        if (m_precomputedPath.size())
        {
            if (!unit->movespline->Finalized())
            {
                uint32 offset = std::min(uint32(unit->movespline->_currentSplineIdx()), uint32(m_precomputedPath.size()));
                m_precomputedPath = rebasePrecomputedPath(offset);

                if (m_precomputedPath.size() > 2)
                    init.MovebyPath(m_precomputedPath);
                else if (m_precomputedPath.size() == 2)
                    init.MoveTo(m_precomputedPath[1].x, m_precomputedPath[1].y, m_precomputedPath[1].z, true);
            }
            else
            {
                // Unit was stopped (finalized) due to Pause/StopMoving; trim path from current position.
                m_precomputedPath = rebasePrecomputedPath();

                if (m_precomputedPath.size() > 2)
                    init.MovebyPath(m_precomputedPath);
                else if (m_precomputedPath.size() == 2)
                    init.MoveTo(m_precomputedPath[1].x, m_precomputedPath[1].y, m_precomputedPath[1].z, true);
                else
                    init.MoveTo(i_x, i_y, i_z, true);
            }
        }
        else
            init.MoveTo(i_x, i_y, i_z, true);

        if (speed > 0.0f) // Default value for point motion type is 0.0, if 0.0 spline will use GetSpeed on unit
            init.SetVelocity(speed);

        if (_forcedMovement == FORCED_MOVEMENT_WALK)
            init.SetWalk(true);
        else if (_forcedMovement == FORCED_MOVEMENT_RUN)
            init.SetWalk(false);

        if (_animTier)
            init.SetAnimation(*_animTier);

        if (i_orientation > 0.0f)
            init.SetFacing(i_orientation);

        init.Launch();
    }

    // If finalized but we were stalled (paused) keep generator active so Finalize() won't be called
    if (unit->movespline->Finalized())
    {
        if (_hasBeenStalled)
            return true;

        return false;
    }

    return true;
}

template<class T>
void PointMovementGenerator<T>::DoFinalize(T* unit)
{
    if (_checkedPath)
    {
        if (OwnsCheckedSpline(unit))
        {
            if (!unit->movespline->Finalized())
            {
                if (PointPathContext(*unit, _checkedPath->MapId, _checkedPath->InstanceId) &&
                    unit->GetPhaseMask() == _checkedPath->PhaseMask &&
                    PointPathGhost(*unit) == _checkedPath->Ghost &&
                    unit->HasUnitMovementFlag(MOVEMENTFLAG_WATERWALKING) == _checkedPath->WaterWalking &&
                    unit->movespline->GetInterruptCount() == _checkedPath->InterruptCount &&
                    SamePointPathPosition(PointPathPosition(*unit), unit->movespline->ComputePosition()))
                    unit->StopMoving();
                else
                {
                    Movement::MoveSplineInit init(unit);
                    init.Stop(true);
                }
            }
            unit->ClearUnitState(UNIT_STATE_ROAMING | UNIT_STATE_ROAMING_MOVE);
            if (_checkedPath->Arrived && !_checkedPath->Failed)
                MovementInform(unit);
        }
        return;
    }

    unit->ClearUnitState(UNIT_STATE_ROAMING | UNIT_STATE_ROAMING_MOVE);
    if (id == EVENT_CHARGE || id == EVENT_CHARGE_PREPATH)
    {
        unit->ClearUnitState(UNIT_STATE_CHARGING);

        if (_chargeTargetGUID && _chargeTargetGUID == unit->GetTarget())
        {
            if (Unit* target = ObjectAccessor::GetUnit(*unit, _chargeTargetGUID))
            {
                unit->Attack(target, true);
            }
        }
    }

    // Only inform AI if this is a real arrival, not a finalize caused by a Pause/Stop
    if (unit->movespline->Finalized() && !_hasBeenStalled)
        MovementInform(unit);
}

template<class T>
void PointMovementGenerator<T>::Pause(uint32 timer)
{
    _stalled = timer ? false : true;
    _hasBeenStalled = true;
    if (timer)
        _pauseTime = static_cast<int32>(timer);
    else
        _pauseTime.reset();
}

template<class T>
void PointMovementGenerator<T>::Resume(uint32 overrideTimer)
{
    _hasBeenStalled = false;
    _stalled = false;
    if (overrideTimer)
        _pauseTime = static_cast<int32>(overrideTimer);
    else
        _pauseTime.reset();

    i_recalculateSpeed = true;
}

template<class T>
void PointMovementGenerator<T>::DoReset(T* unit)
{
    if (_checkedPath)
    {
        i_recalculateSpeed = true;
        return;
    }

    if (!unit->IsStopped())
        unit->StopMoving();

    unit->AddUnitState(UNIT_STATE_ROAMING | UNIT_STATE_ROAMING_MOVE);
    if (id == EVENT_CHARGE || id == EVENT_CHARGE_PREPATH)
    {
        unit->AddUnitState(UNIT_STATE_CHARGING);
    }
}

template<class T>
void PointMovementGenerator<T>::MovementInform(T* /*unit*/)
{
}

template <> void PointMovementGenerator<Creature>::MovementInform(Creature* unit)
{
    if (unit->AI())
        unit->AI()->MovementInform(POINT_MOTION_TYPE, id);

    if (Unit* summoner = unit->GetCharmerOrOwner())
    {
        if (UnitAI* AI = summoner->GetAI())
            AI->SummonMovementInform(unit, POINT_MOTION_TYPE, id);
    }
    else
    {
        if (TempSummon* tempSummon = unit->ToTempSummon())
            if (Unit* summoner = tempSummon->GetSummonerUnit())
                if (UnitAI* AI = summoner->GetAI())
                    AI->SummonMovementInform(unit, POINT_MOTION_TYPE, id);
    }
}

template PointMovementGenerator<Player>::PointMovementGenerator(uint32, Movement::PointsArray const&,
    Unit const&, ForcedMovement, float, float, bool);
template PointMovementGenerator<Creature>::PointMovementGenerator(uint32, Movement::PointsArray const&,
    Unit const&, ForcedMovement, float, float, bool);
template void PointMovementGenerator<Player>::DoInitialize(Player*);
template void PointMovementGenerator<Creature>::DoInitialize(Creature*);
template void PointMovementGenerator<Player>::DoFinalize(Player*);
template void PointMovementGenerator<Creature>::DoFinalize(Creature*);
template void PointMovementGenerator<Player>::DoReset(Player*);
template void PointMovementGenerator<Creature>::DoReset(Creature*);
template bool PointMovementGenerator<Player>::DoUpdate(Player*, uint32);
template bool PointMovementGenerator<Creature>::DoUpdate(Creature*, uint32);

template void PointMovementGenerator<Player>::Pause(uint32);
template void PointMovementGenerator<Creature>::Pause(uint32);
template void PointMovementGenerator<Player>::Resume(uint32);
template void PointMovementGenerator<Creature>::Resume(uint32);

void AssistanceMovementGenerator::Finalize(Unit* unit)
{
    unit->ToCreature()->SetNoCallAssistance(false);
    unit->ToCreature()->CallAssistance();
    if (unit->IsAlive())
        unit->GetMotionMaster()->MoveSeekAssistanceDistract(sWorld->getIntConfig(CONFIG_CREATURE_FAMILY_ASSISTANCE_DELAY));
}

bool EffectMovementGenerator::Update(Unit* unit, uint32)
{
    return !unit->movespline->Finalized();
}

void EffectMovementGenerator::Initialize(Unit*)
{
    i_spline.Launch();
}

void EffectMovementGenerator::Finalize(Unit* unit)
{
    if (!unit->IsCreature())
        return;

    if (unit->IsCreature() && unit->HasUnitMovementFlag(MOVEMENTFLAG_FALLING) && unit->movespline->isFalling()) // pussywizard
        unit->RemoveUnitMovementFlag(MOVEMENTFLAG_FALLING);

    // Need restore previous movement since we have no proper states system
    //if (unit->IsAlive() && !unit->HasUnitState(UNIT_STATE_CONFUSED | UNIT_STATE_FLEEING))
    //{
    //    if (Unit* victim = unit->GetVictim())
    //        unit->GetMotionMaster()->MoveChase(victim);
    //    else
    //        unit->GetMotionMaster()->Initialize();
    //}

    if (unit->ToCreature()->AI())
        unit->ToCreature()->AI()->MovementInform(EFFECT_MOTION_TYPE, m_Id);
}

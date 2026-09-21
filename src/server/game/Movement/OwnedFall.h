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

#ifndef ACORE_OWNED_FALL_H
#define ACORE_OWNED_FALL_H

#include "Define.h"
#include <G3D/Vector3.h>

struct OwnedFallToken
{
    uint64 Value = 0;
    explicit operator bool() const { return Value != 0; }
    bool operator==(OwnedFallToken const&) const = default;
};

enum class OwnedFallResult : uint8
{
    Pending,
    Active,
    Landed,
    Cancelled,
    Replaced,
    Revoked,
    Failed
};

// A value snapshot, not a pointer to a movement generator. Tokens belong to one Unit lifetime.
struct OwnedFallStatus
{
    OwnedFallToken Token;
    OwnedFallResult Result = OwnedFallResult::Pending;
    uint32 MovementId = 0;
    uint32 SplineId = 0;
    uint64 InterruptCount = 0;
    uint32 Elapsed = 0;
    uint32 Duration = 0;
    uint32 MapId = 0;
    uint32 InstanceId = 0;
    uint32 PhaseMask = 0;
    G3D::Vector3 Start;
    G3D::Vector3 Destination;
    G3D::Vector3 Position;
    bool LandingHandled = false;
};

// Allocated only for opt-in users; the latest terminal record remains available until the next accepted fall.
struct OwnedFallData
{
    OwnedFallStatus Status;
    uint64 BookkeepingVersion = 0;
};

#endif

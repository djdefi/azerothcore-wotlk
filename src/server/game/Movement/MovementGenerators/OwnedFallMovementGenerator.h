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

#ifndef ACORE_OWNED_FALL_MOVEMENT_GENERATOR_H
#define ACORE_OWNED_FALL_MOVEMENT_GENERATOR_H

#include "MovementGenerator.h"
#include "OwnedFall.h"

class OwnedFallMovementGenerator final : public MovementGenerator
{
public:
    explicit OwnedFallMovementGenerator(OwnedFallToken token) : _token(token) { }
    void Initialize(Unit* unit) override;
    void Finalize(Unit* unit) override;
    void Reset(Unit*) override { }
    bool Update(Unit* unit, uint32) override;
    MovementGeneratorType GetMovementGeneratorType() override { return EFFECT_MOTION_TYPE; }
    uint32 GetSplineId() const override { return _splineId; }
    OwnedFallToken GetToken() const { return _token; }

private:
    OwnedFallToken _token;
    uint32 _splineId = 0;
};

#endif

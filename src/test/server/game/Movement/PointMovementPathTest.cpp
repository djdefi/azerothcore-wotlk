/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "CreatureAI.h"
#include "DetourNavMeshBuilder.h"
#include "MapCollisionData.h"
#include "MovementGenerator.h"
#include "Player.h"
#include "PointMovementGenerator.h"
#include "ScriptDefines/PlayerScript.h"
#include "ScriptMgr.h"
#include "TestCreature.h"
#include "TestMap.h"
#include "WorldMock.h"
#include "WorldSession.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>

using namespace testing;

namespace
{
    class PointPathWorld : public WorldMock
    {
    public:
        MOCK_METHOD(SQLQueryHolderCallback&, AddQueryHolderCallback, (SQLQueryHolderCallback&&), (override));
    };

    class PointPathMap : public TestMap
    {
    public:
        void SetTestTerrain(std::shared_ptr<GridTerrainData> terrain)
        {
            auto coord = Acore::ComputeGridCoord(10.0f, 10.0f);
            _mapGridManager.GetGrid(coord.x_coord, coord.y_coord)->SetTerrainData(std::move(terrain));
        }

        void RegisterCreature(Creature* creature)
        {
            Cell cell(creature->GetPositionX(), creature->GetPositionY());
            EnsureGridCreated(GridCoord(cell.GridX(), cell.GridY()));
            AddToGrid<Creature>(creature, cell);
        }

        void RegisterPlayer(Player* player)
        {
            Cell cell(player->GetPositionX(), player->GetPositionY());
            EnsureGridCreated(GridCoord(cell.GridX(), cell.GridY()));
            AddToGrid<Player>(player, cell);
        }
    };

    class PointPathCreature : public TestCreature
    {
    public:
        bool IsMovementPreventedByCasting() const override { return Casting; }
        float GetCollisionHeight() const override { return 2.0f; }
        float GetCollisionWidth() const override { return 1.0f; }
        void ChangeMapId(uint32 mapId) { SetLocationMapId(mapId); }
        void ChangeInstanceId(uint32 instanceId) { SetLocationInstanceId(instanceId); }
        bool Casting = false;
    };

    class PointPathAI : public CreatureAI
    {
    public:
        explicit PointPathAI(Creature* creature) : CreatureAI(creature) { }
        void UpdateAI(uint32) override { }
        void MovementInform(uint32 type, uint32 id) override { Arrivals.emplace_back(type, id); }
        std::vector<std::pair<uint32, uint32>> Arrivals;
    };

    class PointPathPlayer : public Player
    {
    public:
        using Player::Player;

        void UpdateObjectVisibility(bool = true, bool = false) override { }
        bool IsMovementPreventedByCasting() const override { return Casting; }
        float GetCollisionHeight() const override { return 2.0f; }
        float GetCollisionWidth() const override { return 1.0f; }

        void InitializeForMovement(Map* map)
        {
            Object::_Create(101, 0, HighGuid::Player);
            SetMap(map);
            SetPhaseMask(1, false);
            Relocate(10, 10, 10);
            SetUnitFlag(UNIT_FLAG_PLAYER_CONTROLLED);
            SetMaxHealth(100);
            Object::AddToWorld();
        }

        void SetLifeState(DeathState state, bool ghost)
        {
            m_deathState = state;
            if (ghost)
                SetPlayerFlag(PLAYER_FLAGS_GHOST);
            else
                RemovePlayerFlag(PLAYER_FLAGS_GHOST);
        }

        void ReleaseGhost()
        {
            SetLifeState(DeathState::Corpse, true);
            SetHealth(1);
            SetWaterWalking(true);
            // BuildPlayerRepop requests waterwalking; the client ACK carries this movement flag.
            AddUnitMovementFlag(MOVEMENTFLAG_WATERWALKING);
        }

        void LeaveWorld() { Object::RemoveFromWorld(); }
        bool Casting = false;
    };

    enum class PointPathRestart
    {
        Pause,
        Cast,
        Root,
        Speed
    };

    class PointMovementPathTest : public Test
    {
    protected:
        void SetUp() override
        {
            _previousWorld = std::move(sWorld);
            auto world = std::make_unique<NiceMock<PointPathWorld>>();
            ON_CALL(*world, getIntConfig(_)).WillByDefault(Return(0));
            ON_CALL(*world, getFloatConfig(_)).WillByDefault(Return(1.0f));
            ON_CALL(*world, getBoolConfig(_)).WillByDefault(Return(false));
            static std::string const empty;
            ON_CALL(*world, GetDataPath()).WillByDefault(ReturnRef(empty));
            sWorld = std::move(world);
            TestMap::EnsureDBC();
            if (!sMovementGeneratorRegistry->GetRegistryItem(IDLE_MOTION_TYPE))
                (new IdleMovementFactory())->RegisterSelf();

            _map = std::make_unique<PointPathMap>();
            _unit = std::make_unique<PointPathCreature>();
            _unit->SetupForCombatTest(_map.get(), 100, 100);
            _unit->Relocate(10.0f, 10.0f, 10.0f);
            _map->RegisterCreature(_unit.get());
            _ai = new PointPathAI(_unit.get());
            _unit->SetAI(_ai);
            Motion()->MoveIdle();
        }

        void TearDown() override
        {
            if (_player)
            {
                _player->GetSession()->SetPlayer(nullptr);
                _player->LeaveWorld();
                _player->RemoveFromGrid();
                _player.reset();
            }
            _unit->CleanupCombatState();
            _unit->RemoveFromGrid();
            _unit.reset();
            _map.reset();
            sWorld = std::move(_previousWorld);
        }

        MotionMaster* Motion() const { return _unit->GetMotionMaster(); }

        Movement::PointsArray Path() const
        {
            return {{10.0f, 10.0f, 10.0f}, {20.0f, 10.0f, 10.0f}, {20.0f, 20.0f, 10.0f}};
        }

        bool Dispatch(Movement::PointsArray const& path, bool backwards = false, float speed = 0.0f)
        {
            return Motion()->MovePointPath(42, path, _unit->GetMapId(), _unit->GetInstanceId(),
                FORCED_MOVEMENT_RUN, speed, 1.0f, backwards);
        }

        void Advance(uint32 milliseconds)
        {
            Advance(_unit.get(), milliseconds);
        }

        void Advance(Unit* unit, uint32 milliseconds)
        {
            auto& spline = *unit->movespline;
            spline.updateState(milliseconds);
            auto position = spline.ComputePosition();
            unit->Relocate(position.x, position.y, position.z, position.orientation);
            // The production Unit update disables the spline after its final position is reached.
            if (spline.Finalized())
                unit->DisableSpline();
        }

        Movement::PointsArray SplinePath() const
        {
            return SplinePath(_unit.get());
        }

        Movement::PointsArray SplinePath(Unit const* unit) const
        {
            auto const& spline = unit->movespline->_Spline();
            Movement::PointsArray result;
            for (int32 i = spline.first(); i <= spline.last(); ++i)
                result.push_back(spline.getPoint(i));
            return result;
        }

        void ExpectRetired()
        {
            Motion()->UpdateMotion(1);
            EXPECT_EQ(Motion()->GetMotionSlotType(MOTION_SLOT_ACTIVE), NULL_MOTION_TYPE);
            EXPECT_TRUE(_ai->Arrivals.empty());
        }

        void CreatePlayer()
        {
            ScriptRegistry<PlayerScript>::InitEnabledHooksIfNeeded(PLAYERHOOK_END);
            // Like the existing Player fixture, keep a socketless session alive: its destructor writes to the DB.
            static WorldSession* session = []
            {
                auto* value = new WorldSession(1, "movement-test", 0, nullptr, SEC_PLAYER,
                    EXPANSION_WRATH_OF_THE_LICH_KING, 0, LOCALE_enUS, 0, false, false, 0);
                value->InitRBACDataForTest();
                return value;
            }();
            _player = std::make_unique<PointPathPlayer>(session);
            _player->InitializeForMovement(_map.get());
            session->SetPlayer(_player.get());
            _map->RegisterPlayer(_player.get());
            _player->GetMotionMaster()->MoveIdle();
        }

        bool DispatchPlayer(Movement::PointsArray const& path)
        {
            return _player->GetMotionMaster()->MovePointPath(42, path, _player->GetMapId(),
                _player->GetInstanceId(), FORCED_MOVEMENT_RUN);
        }

        void Restart(PointPathRestart kind)
        {
            switch (kind)
            {
                case PointPathRestart::Pause:
                    _unit->PauseMovement(0, MOTION_SLOT_ACTIVE);
                    Motion()->UpdateMotion(1);
                    _unit->ResumeMovement(0, MOTION_SLOT_ACTIVE);
                    break;
                case PointPathRestart::Cast:
                    _unit->Casting = true;
                    Motion()->UpdateMotion(1);
                    _unit->Casting = false;
                    break;
                case PointPathRestart::Root:
                    _unit->SetControlled(true, UNIT_STATE_ROOT);
                    Motion()->UpdateMotion(1);
                    _unit->SetControlled(false, UNIT_STATE_ROOT);
                    break;
                case PointPathRestart::Speed:
                    _unit->SetSpeedRate(MOVE_RUN, 1.5f);
                    Motion()->propagateSpeedChange();
                    break;
            }
            EXPECT_TRUE(_ai->Arrivals.empty());
            Motion()->UpdateMotion(1);
        }

        void ResetPathSource(G3D::Vector3 const& source)
        {
            if (Motion()->GetMotionSlot(MOTION_SLOT_ACTIVE))
                Motion()->MovementExpired();
            _ai->Arrivals.clear();
            _unit->SetSpeedRate(MOVE_RUN, 1.0f);
            _unit->RemoveFromGrid();
            _unit->Relocate(source.x, source.y, source.z);
            _map->RegisterCreature(_unit.get());
        }

        std::unique_ptr<IWorld> _previousWorld;
        std::unique_ptr<PointPathMap> _map;
        std::unique_ptr<PointPathCreature> _unit;
        std::unique_ptr<PointPathPlayer> _player;
        PointPathAI* _ai = nullptr;
    };

    // cppcheck-suppress syntaxError
    TEST_F(PointMovementPathTest, CopiesExactPathAndNotifiesOnlyOnArrival)
    {
        auto path = Path();
        ASSERT_TRUE(Dispatch(path));
        EXPECT_EQ(Motion()->GetCurrentMovementGeneratorType(), POINT_MOTION_TYPE);
        EXPECT_EQ(Motion()->GetCurrentSplineId(), _unit->movespline->GetId());
        EXPECT_NE(Motion()->GetCurrentSplineId(), 0u);
        EXPECT_EQ(SplinePath(), path);
        path.back().x = 30.0f;
        EXPECT_EQ(SplinePath(), Path());
        EXPECT_TRUE(_ai->Arrivals.empty());
        Advance(_unit->movespline->Duration());
        Motion()->UpdateMotion(1);
        ASSERT_EQ(_ai->Arrivals.size(), 1u);
        EXPECT_EQ(_ai->Arrivals.front(), std::make_pair(uint32(POINT_MOTION_TYPE), 42u));
        Motion()->UpdateMotion(1);
        EXPECT_EQ(_ai->Arrivals.size(), 1u);
    }

    TEST_F(PointMovementPathTest, TwoPointsUseTheExactSuppliedSegment)
    {
        Movement::PointsArray path{{10.0f, 10.0f, 10.0f}, {10.0f, 10.0f, 20.0f}};
        ASSERT_TRUE(Dispatch(path));
        EXPECT_EQ(SplinePath(), path);
        EXPECT_EQ(_unit->movespline->FinalDestination(), path.back());
    }

    TEST_F(PointMovementPathTest, RejectionDoesNotReplaceExistingMotion)
    {
        ASSERT_TRUE(Dispatch(Path()));
        auto* owner = Motion()->top();
        uint32 splineId = _unit->movespline->GetId();
        float nan = std::numeric_limits<float>::quiet_NaN();
        float inf = std::numeric_limits<float>::infinity();
        std::vector<Movement::PointsArray> invalid{
            {}, {{10, 10, 10}}, {{10, 10, 10}, {10, 10, 10}},
            {{10, 10, 10}, {10, 10, 10}, {20, 10, 10}},
            {{10, 10, 10}, {nan, 10, 10}}, {{10, 10, 10}, {inf, 10, 10}},
            {{10, 10, 10}, {50000, 10, 10}}, {{10.02f, 10, 10}, {20, 10, 10}},
            {{10, 10, 10}, {1000, 10, 10}, {20, 10, 10}}
        };
        for (auto const& path : invalid)
        {
            EXPECT_FALSE(Dispatch(path));
            EXPECT_EQ(Motion()->top(), owner);
            EXPECT_EQ(_unit->movespline->GetId(), splineId);
        }
        EXPECT_FALSE(Motion()->MovePointPath(42, Path(), 1, 0));
        EXPECT_FALSE(Motion()->MovePointPath(42, Path(), 0, 1));
        EXPECT_FALSE(Motion()->MovePointPath(EVENT_CHARGE, Path(), 0, 0));
        EXPECT_FALSE(Motion()->MovePointPath(42, Path(), 0, 0, FORCED_MOVEMENT_MAX));
        EXPECT_FALSE(Motion()->MovePointPath(42, Path(), 0, 0, FORCED_MOVEMENT_RUN, nan));
        EXPECT_FALSE(Motion()->MovePointPath(42, Path(), 0, 0, FORCED_MOVEMENT_RUN, -1.0f));
        EXPECT_FALSE(Motion()->MovePointPath(42, Path(), 0, 0, FORCED_MOVEMENT_RUN, 0.001f));
        EXPECT_FALSE(Motion()->MovePointPath(42, Path(), 0, 0, FORCED_MOVEMENT_RUN, 0.0f, inf));
        EXPECT_EQ(Motion()->top(), owner);
        EXPECT_EQ(_unit->movespline->GetId(), splineId);
    }

    TEST_F(PointMovementPathTest, RejectsDisabledMovementAndNonGroundFrames)
    {
        auto* owner = Motion()->top();
        _unit->SetUnitFlag(UNIT_FLAG_DISABLE_MOVE);
        EXPECT_FALSE(Dispatch(Path()));
        _unit->RemoveUnitFlag(UNIT_FLAG_DISABLE_MOVE);
        for (auto flag : {MOVEMENTFLAG_ONTRANSPORT, MOVEMENTFLAG_SWIMMING, MOVEMENTFLAG_FLYING,
            MOVEMENTFLAG_CAN_FLY, MOVEMENTFLAG_DISABLE_GRAVITY, MOVEMENTFLAG_FALLING,
            MOVEMENTFLAG_FALLING_FAR, MOVEMENTFLAG_HOVER, MOVEMENTFLAG_WATERWALKING})
        {
            _unit->AddUnitMovementFlag(flag);
            EXPECT_FALSE(Dispatch(Path()));
            _unit->RemoveUnitMovementFlag(flag);
        }
        EXPECT_EQ(Motion()->top(), owner);
        EXPECT_EQ(_unit->movespline->GetId(), 0u);
    }

    TEST_F(PointMovementPathTest, RootedInitializationDefersAndThenLaunches)
    {
        _unit->SetControlled(true, UNIT_STATE_ROOT);
        ASSERT_TRUE(Dispatch(Path()));
        EXPECT_EQ(Motion()->GetCurrentSplineId(), 0u);
        Motion()->UpdateMotion(100);
        EXPECT_EQ(Motion()->GetCurrentSplineId(), 0u);
        _unit->SetControlled(false, UNIT_STATE_ROOT);
        Motion()->UpdateMotion(1);
        EXPECT_EQ(SplinePath(), Path());
        EXPECT_NE(Motion()->GetCurrentSplineId(), 0u);
    }

    TEST_F(PointMovementPathTest, CastingDefersInitializationAndResumesFromAnActualStop)
    {
        _unit->Casting = true;
        ASSERT_TRUE(Dispatch(Path()));
        EXPECT_EQ(Motion()->GetCurrentSplineId(), 0u);
        _unit->Casting = false;
        Motion()->UpdateMotion(1);
        Advance(500);
        auto source = _unit->movespline->ComputePosition();
        _unit->Casting = true;
        Motion()->UpdateMotion(1);
        ASSERT_TRUE(_unit->movespline->GetLastStop());
        EXPECT_EQ(_unit->movespline->GetLastStop()->Position, G3D::Vector3(source));
        _unit->Casting = false;
        Motion()->UpdateMotion(1);
        auto expected = Path();
        expected.front() = source;
        EXPECT_EQ(SplinePath(), expected);
    }

    TEST_F(PointMovementPathTest, RootAndStunResumeUsingExplicitStopProvenance)
    {
        for (UnitState state : {UNIT_STATE_ROOT, UNIT_STATE_STUNNED})
        {
            if (Motion()->GetMotionSlot(MOTION_SLOT_ACTIVE))
                Motion()->MovementExpired();
            _unit->Relocate(10, 10, 10);
            ASSERT_TRUE(Dispatch(Path()));
            Advance(500);
            auto source = _unit->movespline->ComputePosition();
            uint32 id = _unit->movespline->GetId();
            _unit->SetControlled(true, state);
            ASSERT_TRUE(_unit->movespline->GetLastStop());
            EXPECT_EQ(_unit->movespline->GetLastStop()->SplineId, id);
            Motion()->UpdateMotion(1);
            EXPECT_TRUE(_unit->movespline->Finalized());
            _unit->SetControlled(false, state);
            Motion()->UpdateMotion(1);
            auto expected = Path();
            expected.front() = source;
            EXPECT_EQ(SplinePath(), expected);
            EXPECT_TRUE(_ai->Arrivals.empty());
        }
    }

    TEST_F(PointMovementPathTest, PauseResumePreservesHairpinOrderIncludingRepeatedStops)
    {
        Movement::PointsArray path{{10, 10, 10}, {20, 10, 10}, {20, 10.02f, 10}, {10, 10.02f, 10},
            {10, 10, 10}, {30, 10, 10}};
        ASSERT_TRUE(Dispatch(path));
        Advance(500);
        auto source = _unit->movespline->ComputePosition();
        _unit->PauseMovement(0, MOTION_SLOT_ACTIVE);
        ASSERT_TRUE(_unit->movespline->GetLastStop());
        auto stop = *_unit->movespline->GetLastStop();
        _unit->StopMoving();
        ASSERT_TRUE(_unit->movespline->GetLastStop());
        EXPECT_EQ(_unit->movespline->GetLastStop()->SplineId, stop.SplineId);
        Motion()->UpdateMotion(10000);
        EXPECT_TRUE(_unit->movespline->Finalized());
        _unit->ResumeMovement(0, MOTION_SLOT_ACTIVE);
        Motion()->UpdateMotion(1);
        path.front() = source;
        EXPECT_EQ(SplinePath(), path);
        EXPECT_FALSE(_unit->movespline->GetLastStop());
    }

    TEST_F(PointMovementPathTest, ClosedLoopResumesByProgressNotRepeatedCoordinates)
    {
        Movement::PointsArray path{{10, 10, 10}, {20, 10, 10}, {20, 20, 10}, {10, 10, 10}, {20, 10, 10}};
        ASSERT_TRUE(Dispatch(path, false, 10.0f));
        Advance(3500);
        ASSERT_EQ(_unit->movespline->currentPathIdx(), 3);
        auto source = _unit->movespline->ComputePosition();
        _unit->PauseMovement(0, MOTION_SLOT_ACTIVE);
        _unit->ResumeMovement(0, MOTION_SLOT_ACTIVE);
        Motion()->UpdateMotion(1);
        EXPECT_EQ(SplinePath(), (Movement::PointsArray{source, path.back()}));
        EXPECT_EQ(_unit->movespline->currentPathIdx(), 3);
    }

    TEST_F(PointMovementPathTest, SpeedChangeMidLegKeepsTheNextCornerAndBackwardsSettings)
    {
        ASSERT_TRUE(Dispatch(Path(), true));
        Advance(500);
        auto source = _unit->movespline->ComputePosition();
        _unit->SetSpeedRate(MOVE_RUN_BACK, 1.5f);
        Motion()->propagateSpeedChange();
        Motion()->UpdateMotion(1);
        auto expected = Path();
        expected.front() = source;
        EXPECT_EQ(SplinePath(), expected);
        EXPECT_FLOAT_EQ(_unit->movespline->Velocity(), _unit->GetSpeed(MOVE_RUN_BACK));
        EXPECT_TRUE(_unit->HasUnitMovementFlag(MOVEMENTFLAG_BACKWARD));
        _unit->PauseMovement(0, MOTION_SLOT_ACTIVE);
        _unit->ResumeMovement(0, MOTION_SLOT_ACTIVE);
        Motion()->UpdateMotion(1);
        EXPECT_TRUE(_unit->HasUnitMovementFlag(MOVEMENTFLAG_BACKWARD));
        Advance(_unit->movespline->Duration());
        EXPECT_FLOAT_EQ(_unit->GetOrientation(), 1.0f);
        Motion()->UpdateMotion(1);
        EXPECT_EQ(_ai->Arrivals.size(), 1u);
    }

    TEST_F(PointMovementPathTest, HigherSlotDefersInitializationWithoutDiscardingIt)
    {
        Motion()->MoveDistract(10);
        uint32 id = _unit->movespline->GetId();
        ASSERT_TRUE(Dispatch(Path()));
        EXPECT_EQ(_unit->movespline->GetId(), id);
        EXPECT_EQ(Motion()->GetMotionSlotType(MOTION_SLOT_ACTIVE), POINT_MOTION_TYPE);
        EXPECT_EQ(Motion()->GetCurrentMovementGeneratorType(), DISTRACT_MOTION_TYPE);
        Motion()->UpdateMotion(11);
        EXPECT_EQ(Motion()->GetCurrentMovementGeneratorType(), POINT_MOTION_TYPE);
        EXPECT_EQ(SplinePath(), Path());
    }

    TEST_F(PointMovementPathTest, DisplacedDeferredSourceRetiresWithoutTouchingForeignSpline)
    {
        Motion()->MoveDistract(10);
        ASSERT_TRUE(Dispatch(Path()));
        _unit->Relocate(11, 10, 10);
        Motion()->UpdateMotion(11);
        uint32 foreignId = _unit->movespline->GetId();
        ExpectRetired();
        EXPECT_EQ(_unit->movespline->GetId(), foreignId);
        EXPECT_FLOAT_EQ(_unit->GetPositionX(), 11.0f);
    }

    TEST_F(PointMovementPathTest, DeferredMapPhaseModeAndSpeedAreRechecked)
    {
        for (unsigned change = 0; change < 6; ++change)
        {
            _unit->Casting = true;
            ASSERT_TRUE(Dispatch(Path()));
            if (change == 0)
                _unit->SetPhaseMask(2, false);
            else if (change == 1)
                _unit->AddUnitMovementFlag(MOVEMENTFLAG_SWIMMING);
            else if (change == 2)
                _unit->SetUnitFlag(UNIT_FLAG_DISABLE_MOVE);
            else if (change == 3)
                _unit->SetSpeedRate(MOVE_RUN, 0.0f);
            else if (change == 4)
                _unit->ChangeMapId(1);
            else
                _unit->ChangeInstanceId(1);
            _unit->Casting = false;
            ExpectRetired();
            _unit->SetPhaseMask(1, false);
            _unit->RemoveUnitMovementFlag(MOVEMENTFLAG_SWIMMING);
            _unit->RemoveUnitFlag(UNIT_FLAG_DISABLE_MOVE);
            _unit->SetSpeedRate(MOVE_RUN, 1.0f);
            _unit->ChangeMapId(0);
            _unit->ChangeInstanceId(0);
        }
    }

    TEST_F(PointMovementPathTest, SamePositionTeleportInvalidatesDeferredProof)
    {
        _unit->Casting = true;
        ASSERT_TRUE(Dispatch(Path()));
        _unit->NearTeleportTo(10, 10, 10, 0);
        _unit->Casting = false;
        ExpectRetired();
        EXPECT_EQ(Motion()->GetCurrentSplineId(), 0u);
    }

    TEST_F(PointMovementPathTest, SamePositionTeleportAfterStopInvalidatesProgress)
    {
        ASSERT_TRUE(Dispatch(Path()));
        Advance(500);
        _unit->PauseMovement(0, MOTION_SLOT_ACTIVE);
        _unit->NearTeleportTo(_unit->GetPositionX(), _unit->GetPositionY(), _unit->GetPositionZ(), 0);
        _unit->ResumeMovement(0, MOTION_SLOT_ACTIVE);
        ExpectRetired();
    }

    TEST_F(PointMovementPathTest, OffCorridorDisplacementDoesNotSnapBack)
    {
        ASSERT_TRUE(Dispatch(Path()));
        Advance(500);
        _unit->Relocate(12, 15, 10);
        ExpectRetired();
        EXPECT_FLOAT_EQ(_unit->GetPositionX(), 12.0f);
        EXPECT_FLOAT_EQ(_unit->GetPositionY(), 15.0f);
        EXPECT_TRUE(_unit->movespline->Finalized());
        EXPECT_FALSE(_unit->movespline->Initialized());
    }

    TEST_F(PointMovementPathTest, StoppedSourceCannotSkipToALaterLoopSegment)
    {
        auto path = Path();
        path.push_back({10, 10, 10});
        ASSERT_TRUE(Dispatch(path));
        Advance(500);
        _unit->PauseMovement(0, MOTION_SLOT_ACTIVE);
        _unit->Relocate(20, 15, 10);
        _unit->ResumeMovement(0, MOTION_SLOT_ACTIVE);
        ExpectRetired();
        EXPECT_FLOAT_EQ(_unit->GetPositionY(), 15.0f);
    }

    TEST_F(PointMovementPathTest, ModeChangeAfterInterruptionRetires)
    {
        ASSERT_TRUE(Dispatch(Path()));
        Advance(500);
        _unit->PauseMovement(0, MOTION_SLOT_ACTIVE);
        _unit->AddUnitMovementFlag(MOVEMENTFLAG_ONTRANSPORT);
        _unit->ResumeMovement(0, MOTION_SLOT_ACTIVE);
        ExpectRetired();
        _unit->RemoveUnitMovementFlag(MOVEMENTFLAG_ONTRANSPORT);
    }

    TEST_F(PointMovementPathTest, ForcedStopCannotLaunchAnUncheckedFrameConnector)
    {
        ASSERT_TRUE(Dispatch(Path()));
        Advance(500);
        _unit->AddUnitMovementFlag(MOVEMENTFLAG_ONTRANSPORT);
        _unit->m_movementInfo.transport.guid = ObjectGuid::Create<HighGuid::Transport>(1, 1);
        _unit->m_movementInfo.transport.pos.Relocate(100, 200, 300);
        ExpectRetired();
        EXPECT_TRUE(_unit->movespline->Finalized());
        EXPECT_FALSE(_unit->movespline->Initialized());
        EXPECT_FALSE(_unit->movespline->GetLastStop());
        _unit->RemoveUnitMovementFlag(MOVEMENTFLAG_ONTRANSPORT);
        _unit->m_movementInfo.transport.guid.Clear();
    }

    TEST_F(PointMovementPathTest, MapPhaseAndSpeedChangesAfterStopRetire)
    {
        for (unsigned change = 0; change < 4; ++change)
        {
            _unit->Relocate(10, 10, 10);
            ASSERT_TRUE(Dispatch(Path()));
            Advance(500);
            _unit->PauseMovement(0, MOTION_SLOT_ACTIVE);
            if (change == 0)
                _unit->ChangeMapId(1);
            else if (change == 1)
                _unit->ChangeInstanceId(1);
            else if (change == 2)
                _unit->SetPhaseMask(2, false);
            else
                _unit->SetSpeedRate(MOVE_RUN, 0.0f);
            _unit->ResumeMovement(0, MOTION_SLOT_ACTIVE);
            ExpectRetired();
            _unit->ChangeMapId(0);
            _unit->ChangeInstanceId(0);
            _unit->SetPhaseMask(1, false);
            _unit->SetSpeedRate(MOVE_RUN, 1.0f);
        }
    }

    TEST_F(PointMovementPathTest, TimedPauseDoesNotRestartBeforeItsDeadline)
    {
        ASSERT_TRUE(Dispatch(Path()));
        Advance(500);
        auto source = _unit->movespline->ComputePosition();
        _unit->PauseMovement(100, MOTION_SLOT_ACTIVE);
        Motion()->UpdateMotion(99);
        EXPECT_TRUE(_unit->movespline->Finalized());
        Motion()->UpdateMotion(1);
        EXPECT_FALSE(_unit->movespline->Finalized());
        auto expected = Path();
        expected.front() = source;
        EXPECT_EQ(SplinePath(), expected);
    }

    TEST_F(PointMovementPathTest, DefaultStopPreservesLegacyProgressAndRepeatedStopIsANoop)
    {
        ASSERT_TRUE(Dispatch(Path()));
        _unit->movespline->updateState(500);
        auto expected = _unit->movespline->ComputePosition();
        uint32 ownedId = _unit->movespline->GetId();
        uint64 interruptCount = _unit->movespline->GetInterruptCount();
        Movement::MoveSplineInit stop(_unit.get());
        stop.Stop();
        EXPECT_TRUE(_unit->movespline->Finalized());
        EXPECT_FALSE(_unit->movespline->Initialized());
        ASSERT_TRUE(_unit->movespline->GetLastStop());
        EXPECT_EQ(_unit->movespline->GetLastStop()->SplineId, ownedId);
        EXPECT_EQ(_unit->movespline->GetLastStop()->Position, G3D::Vector3(expected));
        EXPECT_EQ(_unit->movespline->GetLastStop()->PathIndex, 0);
        EXPECT_EQ(_unit->movespline->GetInterruptCount(), interruptCount);
        EXPECT_FLOAT_EQ(_unit->GetPositionX(), 10.0f);
        uint32 stoppedId = _unit->movespline->GetId();
        Movement::MoveSplineInit repeated(_unit.get());
        repeated.Stop();
        EXPECT_EQ(_unit->movespline->GetId(), stoppedId);
        EXPECT_EQ(_unit->movespline->GetLastStop()->SplineId, ownedId);
    }

    TEST_F(PointMovementPathTest, ForcedStopInvalidatesProvenanceWithoutRelocationOrLaunch)
    {
        ASSERT_TRUE(Dispatch(Path()));
        Advance(500);
        _unit->Relocate(12, 15, 10);
        uint64 interruptCount = _unit->movespline->GetInterruptCount();
        Movement::MoveSplineInit stop(_unit.get());
        stop.Stop(true);
        EXPECT_TRUE(_unit->movespline->Finalized());
        EXPECT_FALSE(_unit->movespline->Initialized());
        EXPECT_FALSE(_unit->movespline->GetLastStop());
        EXPECT_EQ(_unit->movespline->GetInterruptCount(), interruptCount + 1);
        EXPECT_FLOAT_EQ(_unit->GetPositionX(), 12.0f);
        EXPECT_FLOAT_EQ(_unit->GetPositionY(), 15.0f);
        EXPECT_FALSE(_unit->HasUnitMovementFlag(MOVEMENTFLAG_SPLINE_ENABLED));
    }

    TEST_F(PointMovementPathTest, InvalidatingAnAlreadyStoppedPathDoesNotCreateAnotherSpline)
    {
        ASSERT_TRUE(Dispatch(Path()));
        _unit->PauseMovement(0, MOTION_SLOT_ACTIVE);
        uint32 stoppedId = _unit->movespline->GetId();
        _unit->Relocate(12, 15, 10);
        ExpectRetired();
        EXPECT_EQ(_unit->movespline->GetId(), stoppedId);
        EXPECT_TRUE(_unit->movespline->Finalized());
        EXPECT_FALSE(_unit->movespline->Initialized());
    }

    TEST_F(PointMovementPathTest, SpeedChangeOnFinalLegStillDispatchesTwoPoints)
    {
        ASSERT_TRUE(Dispatch(Path(), false, 10.0f));
        Advance(1500);
        auto source = _unit->movespline->ComputePosition();
        Motion()->propagateSpeedChange();
        Motion()->UpdateMotion(1);
        EXPECT_EQ(SplinePath(), (Movement::PointsArray{source, Path().back()}));
    }

    TEST_F(PointMovementPathTest, ClosedPathIsNotMistakenForANoop)
    {
        auto path = Path();
        path.push_back(path.front());
        ASSERT_TRUE(Dispatch(path));
        EXPECT_EQ(SplinePath(), path);
        Advance(_unit->movespline->Duration());
        Motion()->UpdateMotion(1);
        EXPECT_EQ(_ai->Arrivals.size(), 1u);
    }

    TEST_F(PointMovementPathTest, ForeignSplineIsNotStoppedOrReportedAsArrival)
    {
        ASSERT_TRUE(Dispatch(Path()));
        Movement::MoveSplineInit foreign(_unit.get());
        foreign.MovebyPath({{10, 10, 10}, {15, 15, 10}});
        ASSERT_GT(foreign.Launch(), 0);
        uint32 id = _unit->movespline->GetId();
        _unit->AddUnitState(UNIT_STATE_ROAMING | UNIT_STATE_ROAMING_MOVE);
        // Avoid Idle::Reset, which is independent of the strict generator's ownership checks.
        auto* generator = Motion()->top();
        EXPECT_FALSE(generator->Update(_unit.get(), 1));
        generator->Finalize(_unit.get());
        EXPECT_EQ(_unit->movespline->GetId(), id);
        EXPECT_FALSE(_unit->movespline->Finalized());
        EXPECT_TRUE(_unit->HasUnitState(UNIT_STATE_ROAMING_MOVE));
        EXPECT_TRUE(_ai->Arrivals.empty());
    }

    TEST_F(PointMovementPathTest, FinalizingCoveredPointLeavesForeignGeneratorAndSplineUntouched)
    {
        ASSERT_TRUE(Dispatch(Path()));
        Motion()->MovePoint(99, 15, 15, 10, FORCED_MOVEMENT_RUN, 0, 0, false, false, MOTION_SLOT_CONTROLLED);
        auto* foreign = Motion()->top();
        uint32 splineId = _unit->movespline->GetId();
        Motion()->MovementExpiredOnSlot(MOTION_SLOT_ACTIVE, false);
        EXPECT_EQ(Motion()->top(), foreign);
        EXPECT_EQ(_unit->movespline->GetId(), splineId);
        EXPECT_FALSE(_unit->movespline->Finalized());
        EXPECT_TRUE(_unit->HasUnitState(UNIT_STATE_ROAMING_MOVE));
        EXPECT_TRUE(_ai->Arrivals.empty());
    }

    TEST_F(PointMovementPathTest, ReplacingAnAcceptedPathAndCancellingDoNotReportArrival)
    {
        ASSERT_TRUE(Dispatch(Path()));
        auto replacement = Path();
        replacement.back() = {25, 25, 10};
        ASSERT_TRUE(Dispatch(replacement));
        EXPECT_EQ(SplinePath(), replacement);
        Motion()->MovementExpired();
        EXPECT_TRUE(_unit->movespline->Finalized());
        EXPECT_TRUE(_ai->Arrivals.empty());
    }

    TEST_F(PointMovementPathTest, SourceToleranceIsNumericalNotANormalizedHeightSnap)
    {
        auto path = Path();
        path.front().z += 0.005f;
        ASSERT_TRUE(Dispatch(path));
        EXPECT_EQ(SplinePath(), Path());
        Motion()->MovementExpired();
        path.front().z = 10.02f;
        EXPECT_FALSE(Dispatch(path));
        path.front().z = 10.5f;
        EXPECT_FALSE(Dispatch(path));
        // An explicitly checked actual-source connector is part of the proof, not inserted by the core.
        path.insert(path.begin(), {10, 10, 10});
        ASSERT_TRUE(Dispatch(path));
        EXPECT_EQ(SplinePath(), path);
    }

    TEST_F(PointMovementPathTest, ProductionNormalizationRequiresAnExplicitlyCheckedSourceConnector)
    {
        struct TerrainFixture
        {
            std::filesystem::path File = std::filesystem::temp_directory_path() /
                ("ac-point-path-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                    ".map");
            ~TerrainFixture() { std::filesystem::remove(File); }
        } fixture;
        map_fileheader header{};
        header.mapMagic = MapMagic.asUInt;
        header.versionMagic = MapVersionMagic;
        header.heightMapOffset = sizeof(header);
        header.heightMapSize = sizeof(map_heightHeader);
        map_heightHeader height{MapHeightMagic.asUInt, MAP_HEIGHT_NO_HEIGHT, 10.0f, 10.0f};
        {
            std::ofstream file(fixture.File, std::ios::binary);
            file.write(reinterpret_cast<char const*>(&header), sizeof(header));
            file.write(reinterpret_cast<char const*>(&height), sizeof(height));
            ASSERT_TRUE(file.good());
        }
        auto terrain = std::make_shared<GridTerrainData>();
        ASSERT_EQ(terrain->Load(fixture.File.string()), TerrainMapDataReadResult::Success);
        _map->SetTestTerrain(std::move(terrain));
        PathGenerator ground(_unit.get());
        ASSERT_TRUE(ground.CalculatePath(20, 10, 10));
        ASSERT_EQ(ground.GetPath().front(), G3D::Vector3(10, 10, 10));
        ASSERT_TRUE(Dispatch(ground.GetPath()));
        EXPECT_EQ(SplinePath(), ground.GetPath());
        Motion()->MovementExpired();

        _unit->Relocate(10, 10, 10.5f);
        PathGenerator normalized(_unit.get());
        ASSERT_TRUE(normalized.CalculatePath(20, 10, 10));
        EXPECT_EQ(normalized.GetPath().front(), G3D::Vector3(10, 10, 10));
        EXPECT_FALSE(Dispatch(normalized.GetPath()));
        // The fixture exercises source normalization, not navmesh/collision certification by the caller.
        auto checked = normalized.GetPath();
        checked.insert(checked.begin(), {10, 10, 10.5f});
        ASSERT_TRUE(Dispatch(checked));
        EXPECT_EQ(SplinePath(), checked);
    }

    TEST_F(PointMovementPathTest, ReleasedPlayerGhostUsesGroundCorpseAndHealerApproachPaths)
    {
        CreatePlayer();
        _player->ReleaseGhost();
        ASSERT_TRUE(_player->IsPlayer());
        ASSERT_TRUE(_player->isDead());
        ASSERT_TRUE(_player->HasPlayerFlag(PLAYER_FLAGS_GHOST));
        ASSERT_TRUE(_player->HasUnitMovementFlag(MOVEMENTFLAG_WATERWALKING));
        ASSERT_FALSE(_player->HasUnitState(UNIT_STATE_DIED));
        for (auto const& path : {Path(), Movement::PointsArray{{10, 10, 10}, {15, 15, 10}}})
        {
            _player->Relocate(10, 10, 10);
            ASSERT_TRUE(DispatchPlayer(path));
            EXPECT_EQ(_player->GetMotionMaster()->GetCurrentMovementGeneratorType(), POINT_MOTION_TYPE);
            EXPECT_EQ(_player->GetMotionMaster()->GetCurrentSplineId(), _player->movespline->GetId());
            EXPECT_FALSE(_player->movespline->Finalized());
            EXPECT_EQ(SplinePath(_player.get()), path);
            Advance(_player.get(), _player->movespline->Duration());
            _player->GetMotionMaster()->UpdateMotion(1);
            EXPECT_EQ(_player->GetMotionMaster()->GetMotionSlotType(MOTION_SLOT_ACTIVE), NULL_MOTION_TYPE);
        }
    }

    TEST_F(PointMovementPathTest, ReleasedPlayerGhostPreservesRootsCastingAndOrderedResume)
    {
        CreatePlayer();
        _player->ReleaseGhost();
        _player->SetControlled(true, UNIT_STATE_ROOT);
        ASSERT_TRUE(DispatchPlayer(Path()));
        EXPECT_EQ(_player->GetMotionMaster()->GetCurrentSplineId(), 0u);
        _player->GetMotionMaster()->UpdateMotion(1);
        EXPECT_EQ(_player->GetMotionMaster()->GetCurrentSplineId(), 0u);
        _player->SetControlled(false, UNIT_STATE_ROOT);
        _player->GetMotionMaster()->UpdateMotion(1);
        EXPECT_EQ(SplinePath(_player.get()), Path());
        Advance(_player.get(), 500);
        auto expected = Path();
        expected.front() = _player->movespline->ComputePosition();
        _player->Casting = true;
        _player->GetMotionMaster()->UpdateMotion(1);
        ASSERT_TRUE(_player->movespline->GetLastStop());
        EXPECT_TRUE(_player->movespline->Finalized());
        _player->Casting = false;
        _player->GetMotionMaster()->UpdateMotion(1);
        EXPECT_EQ(SplinePath(_player.get()), expected);
        EXPECT_TRUE(_player->HasUnitMovementFlag(MOVEMENTFLAG_WATERWALKING));
    }

    TEST_F(PointMovementPathTest, PlayerDeadUnreleasedAndFeignDeathStatesAreNotGhostPermission)
    {
        CreatePlayer();
        auto* owner = _player->GetMotionMaster()->top();
        for (DeathState state : {DeathState::Corpse, DeathState::Dead, DeathState::JustDied})
        {
            _player->SetLifeState(state, false);
            EXPECT_FALSE(DispatchPlayer(Path()));
        }
        _player->SetLifeState(DeathState::JustDied, true);
        EXPECT_FALSE(DispatchPlayer(Path()));
        _player->SetLifeState(DeathState::Alive, true);
        EXPECT_FALSE(DispatchPlayer(Path()));
        for (bool ghost : {false, true})
        {
            _player->SetLifeState(ghost ? DeathState::Corpse : DeathState::Alive, ghost);
            _player->AddUnitState(UNIT_STATE_DIED);
            EXPECT_FALSE(DispatchPlayer(Path()));
            _player->ClearUnitState(UNIT_STATE_DIED);
        }
        _player->SetLifeState(DeathState::Alive, false);
        _player->AddUnitMovementFlag(MOVEMENTFLAG_WATERWALKING);
        EXPECT_FALSE(DispatchPlayer(Path()));
        EXPECT_EQ(_player->GetMotionMaster()->top(), owner);
        EXPECT_EQ(_player->movespline->GetId(), 0u);
    }

    TEST_F(PointMovementPathTest, ReleasedPlayerGhostDoesNotBypassOtherFrameRestrictions)
    {
        CreatePlayer();
        _player->ReleaseGhost();
        for (auto flag : {MOVEMENTFLAG_SWIMMING, MOVEMENTFLAG_ONTRANSPORT, MOVEMENTFLAG_FLYING,
            MOVEMENTFLAG_CAN_FLY, MOVEMENTFLAG_DISABLE_GRAVITY, MOVEMENTFLAG_FALLING, MOVEMENTFLAG_HOVER})
        {
            _player->AddUnitMovementFlag(flag);
            EXPECT_FALSE(DispatchPlayer(Path()));
            _player->RemoveUnitMovementFlag(flag);
        }
        _player->SetUnitFlag(UNIT_FLAG_DISABLE_MOVE);
        EXPECT_FALSE(DispatchPlayer(Path()));
        _player->RemoveUnitFlag(UNIT_FLAG_DISABLE_MOVE);
        EXPECT_EQ(_player->GetMotionMaster()->GetCurrentMovementGeneratorType(), IDLE_MOTION_TYPE);
    }

    TEST_F(PointMovementPathTest, PlayerResurrectionAndWaterwalkingChangesInvalidateDeferredOrStoppedProof)
    {
        CreatePlayer();
        for (bool stopped : {false, true})
        {
            for (bool resurrect : {false, true})
            {
                SCOPED_TRACE(testing::Message() << "stopped=" << stopped << " resurrect=" << resurrect);
                _player->Relocate(10, 10, 10);
                _player->ReleaseGhost();
                _player->Casting = !stopped;
                ASSERT_TRUE(DispatchPlayer(Path()));
                if (stopped)
                {
                    Advance(_player.get(), 500);
                    _player->PauseMovement(0, MOTION_SLOT_ACTIVE);
                }
                if (resurrect)
                    _player->SetLifeState(DeathState::Alive, false);
                _player->SetWaterWalking(false);
                _player->RemoveUnitMovementFlag(MOVEMENTFLAG_WATERWALKING);
                _player->Casting = false;
                _player->ResumeMovement(0, MOTION_SLOT_ACTIVE);
                _player->GetMotionMaster()->UpdateMotion(1);
                EXPECT_EQ(_player->GetMotionMaster()->GetMotionSlotType(MOTION_SLOT_ACTIVE), NULL_MOTION_TYPE);
                EXPECT_TRUE(_player->movespline->Finalized());
            }
        }
    }

    TEST_F(PointMovementPathTest, PlayerLifeTransitionsInvalidateRunningAndLivingDeferredProof)
    {
        CreatePlayer();
        _player->ReleaseGhost();
        ASSERT_TRUE(DispatchPlayer(Path()));
        Advance(_player.get(), 500);
        _player->SetLifeState(DeathState::Alive, false);
        _player->RemoveUnitMovementFlag(MOVEMENTFLAG_WATERWALKING);
        _player->GetMotionMaster()->UpdateMotion(1);
        EXPECT_EQ(_player->GetMotionMaster()->GetMotionSlotType(MOTION_SLOT_ACTIVE), NULL_MOTION_TYPE);
        EXPECT_TRUE(_player->movespline->Finalized());

        _player->Relocate(10, 10, 10);
        _player->Casting = true;
        ASSERT_TRUE(DispatchPlayer(Path()));
        _player->ReleaseGhost();
        _player->Casting = false;
        _player->GetMotionMaster()->UpdateMotion(1);
        EXPECT_EQ(_player->GetMotionMaster()->GetMotionSlotType(MOTION_SLOT_ACTIVE), NULL_MOTION_TYPE);
    }

    TEST_F(PointMovementPathTest, InitialTinySegmentsRemainRejected)
    {
        EXPECT_FALSE(Dispatch({{10, 10, 10}, {10.005f, 10, 10}}));
        EXPECT_FALSE(Dispatch({{10, 10, 10}, {10.005f, 10, 10}, {20, 10, 10}}));
        EXPECT_FALSE(Dispatch({{10, 10, 10}, {20, 10, 10}, {20.005f, 10, 10}}));
        EXPECT_EQ(Motion()->GetCurrentMovementGeneratorType(), IDLE_MOTION_TYPE);
    }

    TEST_F(PointMovementPathTest, TinyLeadingResidualBeforeCornerOrHairpinPreservesEveryRemainingVertex)
    {
        Movement::PointsArray hairpin{{10, 10, 10}, {20, 10, 10}, {20, 10.02f, 10}, {10, 10.02f, 10},
            {10, 10, 10}, {30, 10, 10}};
        for (auto const& path : {Path(), hairpin})
        {
            for (auto kind : {PointPathRestart::Pause, PointPathRestart::Cast,
                PointPathRestart::Root, PointPathRestart::Speed})
            {
                SCOPED_TRACE(testing::Message() << "vertices=" << path.size() << " restart=" << int(kind));
                ResetPathSource(path.front());
                ASSERT_TRUE(Dispatch(path));
                Advance(1428);
                auto source = _unit->movespline->ComputePosition();
                ASSERT_GT((source - path[1]).length(), 0.0f);
                ASSERT_LT((source - path[1]).length(), 0.01f);
                Restart(kind);
                ASSERT_EQ(Motion()->GetCurrentMovementGeneratorType(), POINT_MOTION_TYPE);
                EXPECT_FALSE(_unit->movespline->Finalized());
                auto expected = path;
                expected.front() = source;
                EXPECT_EQ(SplinePath(), expected);
                EXPECT_TRUE(_ai->Arrivals.empty());
            }
        }
    }

    TEST_F(PointMovementPathTest, TinyLeadingResidualBeforeFinalEndpointDoesNotAnnounceEarlyArrival)
    {
        Movement::PointsArray path{{10, 10, 10}, {20, 10, 10}};
        for (auto kind : {PointPathRestart::Pause, PointPathRestart::Cast,
            PointPathRestart::Root, PointPathRestart::Speed})
        {
            SCOPED_TRACE(int(kind));
            ResetPathSource(path.front());
            ASSERT_TRUE(Dispatch(path));
            Advance(1428);
            auto source = _unit->movespline->ComputePosition();
            ASSERT_GT((source - path.back()).length(), 0.0f);
            ASSERT_LT((source - path.back()).length(), 0.01f);
            Restart(kind);
            ASSERT_EQ(Motion()->GetCurrentMovementGeneratorType(), POINT_MOTION_TYPE);
            EXPECT_EQ(SplinePath(), (Movement::PointsArray{source, path.back()}));
            EXPECT_TRUE(_ai->Arrivals.empty());
            Advance(_unit->movespline->Duration());
            Motion()->UpdateMotion(1);
            ASSERT_EQ(_ai->Arrivals.size(), 1u);
            EXPECT_EQ(_ai->Arrivals.front().second, 42u);
        }
    }

    TEST_F(PointMovementPathTest, ExactlyReachedInteriorVertexResumesByRecordedIndex)
    {
        for (auto kind : {PointPathRestart::Pause, PointPathRestart::Cast,
            PointPathRestart::Root, PointPathRestart::Speed})
        {
            SCOPED_TRACE(int(kind));
            ResetPathSource(Path().front());
            ASSERT_TRUE(Dispatch(Path()));
            Advance(1429);
            ASSERT_EQ(_unit->movespline->currentPathIdx(), 1);
            ASSERT_EQ(_unit->movespline->ComputePosition(), Path()[1]);
            Restart(kind);
            EXPECT_EQ(SplinePath(), (Movement::PointsArray{Path()[1], Path().back()}));
            EXPECT_EQ(_unit->movespline->currentPathIdx(), 1);
            EXPECT_TRUE(_ai->Arrivals.empty());
        }
    }

    TEST_F(PointMovementPathTest, FloatRoundedExactResidualUsesOnlyItsRecordedCornerOrFinalEndpoint)
    {
        for (bool final : {false, true})
        {
            Movement::PointsArray path{{10000, 10, 10}, {10010, 10, 10}};
            if (!final)
            {
                path.push_back({10010, 10.02f, 10});
                path.push_back({10000, 10.02f, 10});
            }
            for (auto kind : {PointPathRestart::Pause, PointPathRestart::Cast,
                PointPathRestart::Root, PointPathRestart::Speed})
            {
                SCOPED_TRACE(testing::Message() << "final=" << final << " restart=" << int(kind));
                ResetPathSource(path.front());
                ASSERT_TRUE(Dispatch(path, false, 0.1f));
                auto const& spline = _unit->movespline->_Spline();
                Advance(spline.length(spline.first() + 1) - 1);
                ASSERT_EQ(_unit->movespline->currentPathIdx(), 0);
                ASSERT_FALSE(_unit->movespline->Finalized());
                ASSERT_EQ(_unit->movespline->ComputePosition(), path[1]);
                Restart(kind);
                if (final)
                {
                    EXPECT_EQ(Motion()->GetCurrentMovementGeneratorType(), POINT_MOTION_TYPE);
                    EXPECT_EQ(SplinePath(), (Movement::PointsArray{path.back(), path.back()}));
                    EXPECT_FALSE(_unit->HasUnitMovementFlag(MOVEMENTFLAG_MASK_MOVING));
                    EXPECT_TRUE(_ai->Arrivals.empty());
                    Advance(_unit->movespline->Duration());
                    Motion()->UpdateMotion(1);
                    EXPECT_EQ(Motion()->GetCurrentMovementGeneratorType(), IDLE_MOTION_TYPE);
                    ASSERT_EQ(_ai->Arrivals.size(), 1u);
                    EXPECT_EQ(_ai->Arrivals.front().second, 42u);
                    EXPECT_TRUE(_unit->movespline->Finalized());
                    EXPECT_FLOAT_EQ(_unit->GetOrientation(), 1.0f);
                }
                else
                {
                    EXPECT_EQ(Motion()->GetCurrentMovementGeneratorType(), POINT_MOTION_TYPE);
                    EXPECT_EQ(SplinePath(), (Movement::PointsArray{path[1], path[2], path[3]}));
                    EXPECT_EQ(_unit->movespline->currentPathIdx(), 1);
                    EXPECT_TRUE(_ai->Arrivals.empty());
                }
            }
        }
    }

    TEST_F(PointMovementPathTest, ExactFinalVertexWithoutRequestedFacingDoesNotLaunchAgain)
    {
        Movement::PointsArray path{{10000, 10, 10}, {10010, 10, 10}};
        ResetPathSource(path.front());
        ASSERT_TRUE(Motion()->MovePointPath(42, path, 0, 0, FORCED_MOVEMENT_RUN, 0.1f));
        Advance(_unit->movespline->Duration() - 1);
        ASSERT_EQ(_unit->movespline->ComputePosition(), path.back());
        _unit->PauseMovement(0, MOTION_SLOT_ACTIVE);
        uint32 stoppedId = _unit->movespline->GetId();
        float orientation = _unit->GetOrientation();
        _unit->ResumeMovement(0, MOTION_SLOT_ACTIVE);
        Motion()->UpdateMotion(1);
        EXPECT_EQ(Motion()->GetCurrentMovementGeneratorType(), IDLE_MOTION_TYPE);
        EXPECT_EQ(_unit->movespline->GetId(), stoppedId);
        EXPECT_FLOAT_EQ(_unit->GetOrientation(), orientation);
        EXPECT_EQ(_ai->Arrivals.size(), 1u);
        Motion()->UpdateMotion(1);
        EXPECT_EQ(_ai->Arrivals.size(), 1u);
        EXPECT_EQ(_unit->movespline->GetId(), stoppedId);
    }

    TEST_F(PointMovementPathTest, ExactFinalFacingDefersAcrossCastRootAndStunAndCompletesOnce)
    {
        Movement::PointsArray path{{10000, 10, 10}, {10010, 10, 10}};
        for (unsigned block = 0; block < 3; ++block)
        {
            SCOPED_TRACE(block);
            ResetPathSource(path.front());
            ASSERT_TRUE(Dispatch(path, false, 0.1f));
            Advance(_unit->movespline->Duration() - 1);
            Restart(PointPathRestart::Pause);
            ASSERT_EQ(SplinePath(), (Movement::PointsArray{path.back(), path.back()}));
            if (block == 0)
                _unit->Casting = true;
            else
                _unit->SetControlled(true, block == 1 ? UNIT_STATE_ROOT : UNIT_STATE_STUNNED);
            Motion()->UpdateMotion(1);
            uint32 stoppedId = _unit->movespline->GetId();
            ASSERT_TRUE(_unit->movespline->Finalized());
            Motion()->UpdateMotion(100);
            EXPECT_EQ(_unit->movespline->GetId(), stoppedId);
            EXPECT_TRUE(_ai->Arrivals.empty());
            EXPECT_FLOAT_EQ(_unit->GetPositionX(), path.back().x);
            EXPECT_FLOAT_EQ(_unit->GetPositionY(), path.back().y);
            if (block == 0)
                _unit->Casting = false;
            else
                _unit->SetControlled(false, block == 1 ? UNIT_STATE_ROOT : UNIT_STATE_STUNNED);
            Motion()->UpdateMotion(1);
            ASSERT_EQ(SplinePath(), (Movement::PointsArray{path.back(), path.back()}));
            EXPECT_TRUE(_ai->Arrivals.empty());
            EXPECT_FALSE(_unit->HasUnitMovementFlag(MOVEMENTFLAG_MASK_MOVING));
            Advance(_unit->movespline->Duration());
            Motion()->UpdateMotion(1);
            EXPECT_FLOAT_EQ(_unit->GetOrientation(), 1.0f);
            ASSERT_EQ(_ai->Arrivals.size(), 1u);
            uint32 completedId = _unit->movespline->GetId();
            Motion()->UpdateMotion(1);
            EXPECT_EQ(_unit->movespline->GetId(), completedId);
            EXPECT_EQ(_ai->Arrivals.size(), 1u);
        }
    }

    TEST_F(PointMovementPathTest, ExactFinalFacingCancellationAndReplacementPreserveOwnership)
    {
        Movement::PointsArray path{{10000, 10, 10}, {10010, 10, 10}};
        Movement::PointsArray replacement{path.back(), {10015, 15, 10}};
        for (unsigned action = 0; action < 3; ++action)
        {
            SCOPED_TRACE(action);
            ResetPathSource(path.front());
            ASSERT_TRUE(Dispatch(path, false, 0.1f));
            Advance(_unit->movespline->Duration() - 1);
            Restart(PointPathRestart::Pause);
            ASSERT_EQ(SplinePath(), (Movement::PointsArray{path.back(), path.back()}));
            if (action == 0)
            {
                Motion()->MovementExpired();
                EXPECT_TRUE(_unit->movespline->Finalized());
                EXPECT_EQ(Motion()->GetCurrentMovementGeneratorType(), IDLE_MOTION_TYPE);
            }
            else if (action == 1)
            {
                ASSERT_TRUE(Motion()->MovePointPath(43, replacement, 0, 0));
                EXPECT_EQ(SplinePath(), replacement);
                EXPECT_TRUE(_ai->Arrivals.empty());
                Advance(_unit->movespline->Duration());
                Motion()->UpdateMotion(1);
                ASSERT_EQ(_ai->Arrivals.size(), 1u);
                EXPECT_EQ(_ai->Arrivals.front().second, 43u);
                continue;
            }
            else
            {
                Movement::MoveSplineInit foreign(_unit.get());
                foreign.MovebyPath(replacement);
                ASSERT_GT(foreign.Launch(), 0);
                uint32 foreignId = _unit->movespline->GetId();
                Motion()->MovementExpiredOnSlot(MOTION_SLOT_ACTIVE, false);
                EXPECT_EQ(_unit->movespline->GetId(), foreignId);
                EXPECT_FALSE(_unit->movespline->Finalized());
                EXPECT_EQ(SplinePath(), replacement);
                EXPECT_TRUE(_unit->HasUnitState(UNIT_STATE_ROAMING_MOVE));
            }
            EXPECT_TRUE(_ai->Arrivals.empty());
            EXPECT_FLOAT_EQ(_unit->GetPositionX(), path.back().x);
            EXPECT_FLOAT_EQ(_unit->GetPositionY(), path.back().y);
        }
    }

    TEST_F(PointMovementPathTest, SmallDisplacementToAVertexDoesNotReplaceRecordedProgress)
    {
        ASSERT_TRUE(Dispatch(Path()));
        Advance(1428);
        _unit->PauseMovement(0, MOTION_SLOT_ACTIVE);
        ASSERT_NE(_unit->movespline->GetLastStop()->Position, Path()[1]);
        _unit->Relocate(20, 10, 10);
        _unit->ResumeMovement(0, MOTION_SLOT_ACTIVE);
        ExpectRetired();
    }

    TEST_F(PointMovementPathTest, LegacyPointRetainsItsExistingEndpointBehavior)
    {
        Motion()->MovePoint(7, 20, 10, 10, FORCED_MOVEMENT_RUN, 0, 0, false);
        EXPECT_EQ(Motion()->GetCurrentMovementGeneratorType(), POINT_MOTION_TYPE);
        EXPECT_EQ(Motion()->GetCurrentSplineId(), 0u);
        EXPECT_EQ(_unit->movespline->FinalDestination(), G3D::Vector3(20, 10, 10));
        Advance(_unit->movespline->Duration());
        Motion()->UpdateMotion(1);
        ASSERT_EQ(_ai->Arrivals.size(), 1u);
        EXPECT_EQ(_ai->Arrivals.front().second, 7u);
    }

    class PathGeneratorCorridorTest : public PointMovementPathTest
    {
    protected:
        void SetUp() override
        {
            PointMovementPathTest::SetUp();
            _terrainFile = std::filesystem::temp_directory_path() /
                ("ac-corridor-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".map");
            map_fileheader header{};
            header.mapMagic = MapMagic.asUInt;
            header.versionMagic = MapVersionMagic;
            header.heightMapOffset = sizeof(header);
            header.heightMapSize = sizeof(map_heightHeader);
            map_heightHeader height{MapHeightMagic.asUInt, MAP_HEIGHT_NO_HEIGHT, 10.0f, 10.0f};
            {
                std::ofstream file(_terrainFile, std::ios::binary);
                file.write(reinterpret_cast<char const*>(&header), sizeof(header));
                file.write(reinterpret_cast<char const*>(&height), sizeof(height));
                ASSERT_TRUE(file.good());
            }
            auto terrain = std::make_shared<GridTerrainData>();
            ASSERT_EQ(terrain->Load(_terrainFile.string()), TerrainMapDataReadResult::Success);
            _map->SetTestTerrain(std::move(terrain));
        }

        void TearDown() override
        {
            PointMovementPathTest::TearDown();
            std::filesystem::remove(_terrainFile);
        }

        void BuildIslands(bool connectedPrefix = false)
        {
            // Detour YZX coordinates: ground X=[0,64] and X=[80,128], separated by an unwalkable gap.
            std::vector<unsigned short> vertices{
                0, 10, 0, 0, 10, 64, 64, 10, 64, 64, 10, 0,
                0, 10, 80, 0, 10, 128, 64, 10, 128, 64, 10, 80
            };
            std::vector<unsigned short> polygons{
                0, 1, 2, 3, 0xffff, 0xffff, 0xffff, 0xffff,
                4, 5, 6, 7, 0xffff, 0xffff, 0xffff, 0xffff
            };
            if (connectedPrefix)
            {
                vertices = {
                    0, 10, 0, 0, 10, 32, 64, 10, 32, 64, 10, 0, 0, 10, 64, 64, 10, 64,
                    0, 10, 80, 0, 10, 128, 64, 10, 128, 64, 10, 80
                };
                polygons = {
                    0, 1, 2, 3, 0xffff, 1, 0xffff, 0xffff,
                    1, 4, 5, 2, 0xffff, 0xffff, 0xffff, 0,
                    6, 7, 8, 9, 0xffff, 0xffff, 0xffff, 0xffff
                };
            }
            std::vector<unsigned short> flags(polygons.size() / 8, NAV_GROUND);
            std::vector<unsigned char> areas(flags.size(), 0);
            dtNavMeshCreateParams params{};
            params.verts = vertices.data();
            params.vertCount = vertices.size() / 3;
            params.polys = polygons.data();
            params.polyCount = flags.size();
            params.polyFlags = flags.data();
            params.polyAreas = areas.data();
            params.nvp = 4;
            params.bmax[0] = 64.0f;
            params.bmax[1] = 20.0f;
            params.bmax[2] = 128.0f;
            params.walkableHeight = 2.0f;
            params.walkableRadius = 0.5f;
            params.walkableClimb = 1.0f;
            params.cs = params.ch = 1.0f;
            params.buildBvTree = true;
            unsigned char* data = nullptr;
            int size = 0;
            ASSERT_TRUE(dtCreateNavMeshData(&params, &data, &size));
            std::unique_ptr<unsigned char, decltype(&dtFree)> tile(data, dtFree);
            std::shared_ptr<dtNavMesh> mesh(dtAllocNavMesh(), MMAP::NavMeshDeleter{});
            ASSERT_NE(mesh, nullptr);
            dtNavMeshParams meshParams{};
            meshParams.tileWidth = meshParams.tileHeight = 128.0f;
            meshParams.maxTiles = 1;
            meshParams.maxPolys = 4;
            ASSERT_TRUE(dtStatusSucceed(mesh->init(&meshParams)));
            ASSERT_TRUE(dtStatusSucceed(mesh->addTile(data, size, DT_TILE_FREE_DATA, 0, nullptr)));
            tile.release();

            struct TestMMapData : MMapData
            {
                explicit TestMMapData(std::shared_ptr<dtNavMesh> mesh) { _navMesh = std::move(mesh); }
            };
            _map->GetMapCollisionData().GetMMapData() = TestMMapData(std::move(mesh));
        }

        void ExpectPartialPrefix(PathGenerator const& path, int expectedPolygons)
        {
            EXPECT_EQ(path.GetPathType(), PATHFIND_INCOMPLETE);
            ASSERT_GT(path.GetPath().size(), 2u);
            EXPECT_EQ(path.GetPath().front(), G3D::Vector3(10, 10, 10));
            EXPECT_EQ(path.GetPath().back(), G3D::Vector3(64, 10, 10));
            EXPECT_EQ(path.GetActualEndPosition(), path.GetPath().back());
            EXPECT_EQ(path.GetEndPosition(), G3D::Vector3(90, 10, 10));
            for (auto const& point : path.GetPath())
            {
                EXPECT_GE(point.x, 10.0f);
                EXPECT_LE(point.x, 64.0f);
                EXPECT_FLOAT_EQ(point.y, 10.0f);
                EXPECT_FLOAT_EQ(point.z, 10.0f);
            }

            auto& mmap = _map->GetMapCollisionData().GetMMapData();
            dtPolyRef base = mmap.GetNavMesh()->getPolyRefBase(mmap.GetNavMesh()->getTileAt(0, 0, 0));
            float start[3]{10, 10, 10};
            float end[3]{10, 10, 90};
            dtPolyRef refs[4]{};
            int count = 0;
            dtQueryFilter filter;
            ASSERT_TRUE(dtStatusSucceed(mmap.GetNavMeshQuery()->findPath(base, base + expectedPolygons,
                start, end, &filter, refs, &count, 4)));
            ASSERT_EQ(count, expectedPolygons);
            EXPECT_EQ(refs[count - 1], base + expectedPolygons - 1);
        }

        std::filesystem::path _terrainFile;
    };

    TEST_F(PathGeneratorCorridorTest, DisconnectedOnePolygonSmoothPathEndsAtReachableBoundary)
    {
        BuildIslands();
        PathGenerator path(_unit.get());
        ASSERT_TRUE(path.CalculatePath(90, 10, 10, false));
        ExpectPartialPrefix(path, 1);
        ASSERT_TRUE(Dispatch(path.GetPath()));
        EXPECT_EQ(SplinePath(), path.GetPath());
        EXPECT_EQ(_unit->movespline->FinalDestination(), G3D::Vector3(64, 10, 10));
    }

    TEST_F(PathGeneratorCorridorTest, StartAtPartialBoundaryDoesNotAppendUnreachableGoal)
    {
        BuildIslands();
        ResetPathSource({64, 10, 10});
        for (bool straight : {false, true})
        {
            for (bool onePointLimit : {false, true})
            {
                PathGenerator path(_unit.get());
                path.SetUseStraightPath(straight);
                if (onePointLimit)
                    path.SetPathLengthLimit(SMOOTH_PATH_STEP_SIZE);
                ASSERT_TRUE(path.CalculatePath(90, 10, 10, false));
                EXPECT_EQ(path.GetPathType(), PATHFIND_INCOMPLETE);
                ASSERT_EQ(path.GetPath(), (Movement::PointsArray{{64, 10, 10}}));
                EXPECT_EQ(path.GetActualEndPosition(), path.GetPath().back());
                EXPECT_EQ(path.GetEndPosition(), G3D::Vector3(90, 10, 10));
                EXPECT_FALSE(Dispatch(path.GetPath()));
            }
        }
    }

    TEST_F(PathGeneratorCorridorTest, ConnectedMultiPolygonPartialPrefixKeepsExistingClamp)
    {
        BuildIslands(true);
        PathGenerator path(_unit.get());
        ASSERT_TRUE(path.CalculatePath(90, 10, 10, false));
        ExpectPartialPrefix(path, 2);
    }

    TEST_F(PathGeneratorCorridorTest, CompleteShortOnePolygonPathKeepsItsExactDestination)
    {
        BuildIslands();
        for (bool straight : {false, true})
        {
            PathGenerator path(_unit.get());
            path.SetUseStraightPath(straight);
            ASSERT_TRUE(path.CalculatePath(10.1f, 10, 10, false));
            EXPECT_EQ(path.GetPathType(), PATHFIND_NORMAL);
            EXPECT_EQ(path.GetPath(), (Movement::PointsArray{{10, 10, 10}, {10.1f, 10, 10}}));
            EXPECT_EQ(path.GetActualEndPosition(), path.GetPath().back());
        }
    }

    TEST_F(PathGeneratorCorridorTest, ExplicitForcedDestinationRetainsItsNotUsingPathFlag)
    {
        BuildIslands();
        for (float sourceX : {10.0f, 64.0f})
        {
            ResetPathSource({sourceX, 10, 10});
            PathGenerator path(_unit.get());
            ASSERT_TRUE(path.CalculatePath(90, 10, 10, true));
            EXPECT_EQ(path.GetPathType(), PATHFIND_NORMAL | PATHFIND_NOT_USING_PATH);
            ASSERT_GE(path.GetPath().size(), 2u);
            EXPECT_EQ(path.GetPath().back(), G3D::Vector3(90, 10, 10));
            EXPECT_EQ(path.GetActualEndPosition(), path.GetPath().back());
        }
    }

    TEST_F(PathGeneratorCorridorTest, InvalidPublicQueryDoesNotReplacePreviousCompletePath)
    {
        BuildIslands();
        PathGenerator path(_unit.get());
        ASSERT_TRUE(path.CalculatePath(20, 10, 10, false));
        auto expected = path.GetPath();
        ASSERT_EQ(path.GetPathType(), PATHFIND_NORMAL);
        EXPECT_FALSE(path.CalculatePath(std::numeric_limits<float>::quiet_NaN(), 10, 10, false));
        EXPECT_EQ(path.GetPath(), expected);
        EXPECT_EQ(path.GetPathType(), PATHFIND_NORMAL);
        EXPECT_EQ(path.GetActualEndPosition(), expected.back());
    }
}

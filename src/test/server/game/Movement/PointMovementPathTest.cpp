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
#include "ScriptDefines/AchievementScript.h"
#include "ScriptDefines/PlayerScript.h"
#include "ScriptMgr.h"
#include "TestCreature.h"
#include "TestMap.h"
#include "WorldMock.h"
#include "WorldSession.h"
#include "WorldPacket.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <type_traits>

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
        void SendMessageToSet(WorldPacket const* data, bool self) const override
        {
            if (data->GetOpcode() == SMSG_ENVIRONMENTAL_DAMAGE_LOG && OnEnvironmentalDamage)
            {
                auto callback = std::exchange(OnEnvironmentalDamage, {});
                callback();
            }
            if (data->GetOpcode() == SMSG_MONSTER_MOVE && OnMonsterMove)
            {
                auto callback = std::exchange(OnMonsterMove, {});
                callback();
            }
            Player::SendMessageToSet(data, self);
        }

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
        mutable std::function<void()> OnEnvironmentalDamage;
        mutable std::function<void()> OnMonsterMove;
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

        void BuildIslands(bool connectedPrefix = false, bool steep = false)
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
                if (steep)
                {
                    vertices[4 * 3 + 1] = 100;
                    vertices[5 * 3 + 1] = 100;
                }
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
            params.bmax[1] = steep ? 110.0f : 20.0f;
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

    class PathQueryDiagnosticsTest : public PathGeneratorCorridorTest
    {
    protected:
        using Check = PathQueryDiagnostics::Check;
        using Lookup = PathQueryDiagnostics::Lookup;
        using Shortcut = PathQueryDiagnostics::Shortcut;
        using CorridorMode = PathQueryDiagnostics::CorridorMode;
        using PointMode = PathQueryDiagnostics::PointMode;

        PathQueryDiagnostics Compare(Unit* unit, G3D::Vector3 const& goal, bool force = false,
            bool straight = false, bool raycast = false, float limit = 296.0f, bool slope = false)
        {
            PathGenerator ordinary(unit);
            PathGenerator captured(unit);
            for (auto* path : {&ordinary, &captured})
            {
                path->SetUseStraightPath(straight);
                path->SetUseRaycast(raycast);
                path->SetPathLengthLimit(limit);
                path->SetSlopeCheck(slope);
            }
            PathQueryDiagnostics diagnostics;
            bool original = ordinary.CalculatePath(goal.x, goal.y, goal.z, force);
            bool observed = captured.CalculatePath(goal.x, goal.y, goal.z, force, diagnostics);
            EXPECT_EQ(observed, original);
            ExpectSamePath(ordinary, captured);
            EXPECT_EQ(diagnostics.ResultUpdated, observed);
            EXPECT_EQ(diagnostics.ResultType, captured.GetPathType());
            EXPECT_EQ(diagnostics.ActualEnd, captured.GetActualEndPosition());
            EXPECT_EQ(diagnostics.ResultPointCount, captured.GetPath().size());
            EXPECT_EQ(diagnostics.End, goal);
            EXPECT_EQ(diagnostics.MapId, unit->GetMapId());
            EXPECT_EQ(diagnostics.InstanceId, unit->GetInstanceId());
            EXPECT_EQ(diagnostics.PhaseMask, unit->GetPhaseMask());
            EXPECT_FALSE(diagnostics.ExplicitStart);
            EXPECT_EQ(diagnostics.UseStraightPath, straight);
            EXPECT_EQ(diagnostics.UseRaycast, raycast);
            EXPECT_EQ(diagnostics.ForceDestination, force);
            EXPECT_EQ(diagnostics.SlopeCheck, slope);
            return diagnostics;
        }

        void ExpectSamePath(PathGenerator const& original, PathGenerator const& captured)
        {
            EXPECT_EQ(captured.GetPathType(), original.GetPathType());
            EXPECT_EQ(captured.GetPath(), original.GetPath());
            EXPECT_EQ(captured.GetStartPosition(), original.GetStartPosition());
            EXPECT_EQ(captured.GetEndPosition(), original.GetEndPosition());
            EXPECT_EQ(captured.GetActualEndPosition(), original.GetActualEndPosition());
        }
    };

    static_assert(std::is_trivially_copyable_v<PathQueryDiagnostics>);
    static_assert(std::is_standard_layout_v<PathQueryDiagnostics>);
    static_assert(sizeof(PathQueryDiagnostics) <= 512);

    TEST_F(PathQueryDiagnosticsTest, MissingMeshPreservesUnevaluatedPrerequisites)
    {
        auto diagnostics = Compare(_unit.get(), {20, 10, 10});
        EXPECT_EQ(diagnostics.ShortcutReason, Shortcut::Prerequisite);
        EXPECT_EQ(diagnostics.HasNavMesh, Check::No);
        EXPECT_EQ(diagnostics.HasNavMeshQuery, Check::NotEvaluated);
        EXPECT_EQ(diagnostics.IgnorePathfinding, Check::NotEvaluated);
        EXPECT_EQ(diagnostics.StartTile.Present, Check::NotEvaluated);
        EXPECT_EQ(diagnostics.EndTile.Present, Check::NotEvaluated);
        EXPECT_EQ(diagnostics.StartPolygon.Source, Lookup::NotEvaluated);
        EXPECT_EQ(diagnostics.CorridorStage.Mode, CorridorMode::NotEvaluated);
        EXPECT_EQ(diagnostics.PointStage.Mode, PointMode::NotEvaluated);
        EXPECT_FALSE(diagnostics.FilterUpdated);
        EXPECT_EQ(diagnostics.ResultType, PATHFIND_NORMAL | PATHFIND_NOT_USING_PATH);
    }

    TEST_F(PathQueryDiagnosticsTest, IgnoredPathfindingDoesNotEvaluateTilesOrPolygons)
    {
        BuildIslands();
        _unit->AddUnitState(UNIT_STATE_IGNORE_PATHFINDING);
        auto diagnostics = Compare(_unit.get(), {20, 10, 10});
        EXPECT_EQ(diagnostics.ShortcutReason, Shortcut::Prerequisite);
        EXPECT_EQ(diagnostics.HasNavMesh, Check::Yes);
        EXPECT_EQ(diagnostics.HasNavMeshQuery, Check::Yes);
        EXPECT_EQ(diagnostics.IgnorePathfinding, Check::Yes);
        EXPECT_EQ(diagnostics.StartTile.Present, Check::NotEvaluated);
        EXPECT_EQ(diagnostics.EndTile.Present, Check::NotEvaluated);
        EXPECT_EQ(diagnostics.EndPolygon.Source, Lookup::NotEvaluated);
        EXPECT_FALSE(diagnostics.FilterUpdated);
    }

    TEST_F(PathQueryDiagnosticsTest, MissingStartAndEndTilesRetainOriginalShortCircuitOrder)
    {
        BuildIslands();
        _unit->Relocate(-1, 10, 10);
        auto startMissing = Compare(_unit.get(), {20, 10, 10});
        EXPECT_EQ(startMissing.StartTile.Present, Check::No);
        EXPECT_LT(startMissing.StartTile.Y, 0);
        EXPECT_EQ(startMissing.EndTile.Present, Check::NotEvaluated);
        EXPECT_EQ(startMissing.StartPolygon.Source, Lookup::NotEvaluated);
        EXPECT_FALSE(startMissing.FilterUpdated);
        _unit->Relocate(10, 10, 10);
        auto endMissing = Compare(_unit.get(), {150, 10, 10});
        EXPECT_EQ(endMissing.StartTile.Present, Check::Yes);
        EXPECT_EQ(endMissing.StartTile.X, 0);
        EXPECT_EQ(endMissing.StartTile.Y, 0);
        EXPECT_EQ(endMissing.EndTile.Present, Check::No);
        EXPECT_EQ(endMissing.EndTile.Y, 1);
        EXPECT_EQ(endMissing.ShortcutReason, Shortcut::Prerequisite);
        EXPECT_FALSE(endMissing.FilterUpdated);
    }

    TEST_F(PathQueryDiagnosticsTest, OriginalPlayerMissingPolygonShortcutKeepsLookupFacts)
    {
        BuildIslands();
        CreatePlayer();
        _player->Relocate(70, 10, 10);
        auto diagnostics = Compare(_player.get(), {90, 10, 10});
        EXPECT_EQ(diagnostics.ShortcutReason, Shortcut::MissingPolygon);
        EXPECT_EQ(diagnostics.ResultType, PATHFIND_NORMAL | PATHFIND_NOT_USING_PATH);
        EXPECT_EQ(diagnostics.Start, G3D::Vector3(70, 10, 10));
        EXPECT_EQ(diagnostics.StartPolygon.Source, Lookup::NotFound);
        EXPECT_EQ(diagnostics.StartPolygon.Ref, INVALID_POLYREF);
        EXPECT_EQ(diagnostics.StartPolygon.Distance, std::numeric_limits<float>::max());
        EXPECT_EQ(diagnostics.StartPolygon.SmallSearchSucceeded, Check::Yes);
        EXPECT_EQ(diagnostics.StartPolygon.TallSearchSucceeded, Check::Yes);
        EXPECT_EQ(diagnostics.EndPolygon.Source, Lookup::SmallExtent);
        EXPECT_NE(diagnostics.EndPolygon.Ref, INVALID_POLYREF);
        EXPECT_EQ(diagnostics.StartFar, Check::NotEvaluated);
        EXPECT_EQ(diagnostics.CorridorStage.Mode, CorridorMode::NotEvaluated);
        EXPECT_TRUE(diagnostics.FilterUpdated);
        EXPECT_EQ(diagnostics.UsedIncludeFlags, diagnostics.EntryIncludeFlags);
        EXPECT_EQ(diagnostics.UsedExcludeFlags, diagnostics.EntryExcludeFlags);
    }

    TEST_F(PathQueryDiagnosticsTest, CreatureMissingPolygonFailureIsNotMislabelledAsPlayerShortcut)
    {
        BuildIslands();
        auto diagnostics = Compare(_unit.get(), {70, 10, 10});
        EXPECT_EQ(diagnostics.ResultType, PATHFIND_NOPATH);
        EXPECT_EQ(diagnostics.ShortcutReason, Shortcut::None);
        EXPECT_EQ(diagnostics.StartPolygon.Source, Lookup::SmallExtent);
        EXPECT_EQ(diagnostics.EndPolygon.Source, Lookup::NotFound);
        EXPECT_EQ(diagnostics.StartFar, Check::NotEvaluated);
        EXPECT_EQ(diagnostics.EndFar, Check::NotEvaluated);
    }

    TEST_F(PathQueryDiagnosticsTest, FarPolygonFlightShortcutRetainsTallLookupAndRawSource)
    {
        BuildIslands();
        _unit->AddUnitMovementFlag(MOVEMENTFLAG_FLYING);
        _unit->Relocate(10, 10, 30);
        auto diagnostics = Compare(_unit.get(), {20, 10, 30});
        EXPECT_EQ(diagnostics.ShortcutReason, Shortcut::FarFromPolygon);
        EXPECT_EQ(diagnostics.Start, G3D::Vector3(10, 10, 30));
        EXPECT_EQ(diagnostics.StartPolygon.Source, Lookup::TallExtent);
        EXPECT_EQ(diagnostics.EndPolygon.Source, Lookup::TallExtent);
        EXPECT_FLOAT_EQ(diagnostics.StartPolygon.Distance, 20.0f);
        EXPECT_EQ(diagnostics.StartFar, Check::Yes);
        EXPECT_EQ(diagnostics.EndFar, Check::Yes);
        EXPECT_EQ(diagnostics.ResultType, PATHFIND_NORMAL | PATHFIND_NOT_USING_PATH | PATHFIND_FARFROMPOLY);
        EXPECT_EQ(diagnostics.CorridorStage.Mode, CorridorMode::NotEvaluated);
    }

    TEST_F(PathQueryDiagnosticsTest, ForcedEndpointDoesNotOverwriteOriginalPartialCorridorFacts)
    {
        BuildIslands();
        auto diagnostics = Compare(_unit.get(), {90, 10, 10}, true);
        EXPECT_EQ(diagnostics.ShortcutReason, Shortcut::ForcedDestination);
        EXPECT_EQ(diagnostics.CorridorStage.Mode, CorridorMode::FindPath);
        EXPECT_TRUE(diagnostics.CorridorStage.StatusAvailable);
        EXPECT_NE(diagnostics.CorridorStage.Status & DT_PARTIAL_RESULT, 0u);
        EXPECT_EQ(diagnostics.CorridorStage.PolyCount, 1u);
        EXPECT_EQ(diagnostics.CorridorStage.LastPoly, diagnostics.StartPolygon.Ref);
        EXPECT_NE(diagnostics.CorridorStage.LastPoly, diagnostics.EndPolygon.Ref);
        EXPECT_EQ(diagnostics.CorridorStage.ReachesEndPolygon, Check::No);
        EXPECT_EQ(diagnostics.ResultType, PATHFIND_NORMAL | PATHFIND_NOT_USING_PATH);
        EXPECT_EQ(diagnostics.ActualEnd, G3D::Vector3(90, 10, 10));
    }

    TEST_F(PathQueryDiagnosticsTest, PartialCorridorAndOnePointNoProgressRemainDistinctFromErrors)
    {
        BuildIslands(true);
        auto partial = Compare(_unit.get(), {90, 10, 10});
        EXPECT_EQ(partial.ResultType, PATHFIND_INCOMPLETE);
        EXPECT_EQ(partial.ShortcutReason, Shortcut::None);
        EXPECT_EQ(partial.CorridorStage.PolyCount, 2u);
        EXPECT_EQ(partial.CorridorStage.ReachesEndPolygon, Check::No);
        EXPECT_EQ(partial.PointStage.Mode, PointMode::Smooth);
        EXPECT_TRUE(partial.PointStage.StatusAvailable);
        EXPECT_TRUE(dtStatusSucceed(partial.PointStage.Status));
        EXPECT_GT(partial.PointStage.Count, 1u);
        EXPECT_EQ(partial.ActualEnd, G3D::Vector3(64, 10, 10));
        _unit->Relocate(64, 10, 10);
        auto stopped = Compare(_unit.get(), {90, 10, 10});
        EXPECT_EQ(stopped.ResultType, PATHFIND_INCOMPLETE);
        EXPECT_EQ(stopped.PointStage.Count, 1u);
        EXPECT_EQ(stopped.ResultPointCount, 1u);
        EXPECT_EQ(stopped.ActualEnd, G3D::Vector3(64, 10, 10));
    }

    TEST_F(PathQueryDiagnosticsTest, CompletePathsAndPointLimitExposeActualPointStage)
    {
        BuildIslands();
        for (bool straight : {false, true})
        {
            auto complete = Compare(_unit.get(), {10.1f, 10, 10}, false, straight, false, 296.0f, true);
            EXPECT_EQ(complete.CorridorStage.Mode, CorridorMode::SamePolygon);
            EXPECT_FALSE(complete.CorridorStage.StatusAvailable);
            EXPECT_EQ(complete.CorridorStage.ReachesEndPolygon, Check::Yes);
            EXPECT_EQ(complete.ResultType, PATHFIND_NORMAL);
            EXPECT_EQ(complete.PointStage.Mode, straight ? PointMode::Straight : PointMode::Smooth);
            EXPECT_EQ(complete.PointStage.Count, straight ? 2u : 1u);
            EXPECT_EQ(complete.PointStage.LimitReached, straight ? Check::No : Check::NotEvaluated);
            EXPECT_EQ(complete.ResultPointCount, 2u);
        }
        auto limited = Compare(_unit.get(), {60, 10, 10}, false, false, false, 8.0f);
        EXPECT_EQ(limited.PointLimit, 2u);
        EXPECT_EQ(limited.PointStage.Count, 2u);
        EXPECT_EQ(limited.PointStage.LimitReached, Check::Yes);
        EXPECT_NE(limited.ResultType & PATHFIND_SHORT, 0);
        EXPECT_EQ(limited.ShortcutReason, Shortcut::None);
    }

    TEST_F(PathQueryDiagnosticsTest, RaycastEvidenceIsFromOriginalClearOrBlockedQuery)
    {
        BuildIslands();
        auto clear = Compare(_unit.get(), {20, 10, 10}, false, false, true);
        EXPECT_EQ(clear.CorridorStage.Mode, CorridorMode::Raycast);
        EXPECT_TRUE(clear.CorridorStage.StatusAvailable);
        EXPECT_EQ(clear.CorridorStage.RaycastClear, Check::Yes);
        EXPECT_EQ(clear.CorridorStage.RaycastHit, std::numeric_limits<float>::max());
        EXPECT_EQ(clear.PointStage.Mode, PointMode::Raycast);
        EXPECT_FALSE(clear.PointStage.StatusAvailable);
        auto blocked = Compare(_unit.get(), {90, 10, 10}, false, false, true);
        EXPECT_EQ(blocked.CorridorStage.RaycastClear, Check::No);
        EXPECT_GT(blocked.CorridorStage.RaycastHit, 0.0f);
        EXPECT_LT(blocked.CorridorStage.RaycastHit, 1.0f);
        EXPECT_EQ(blocked.ResultType, PATHFIND_INCOMPLETE);
        EXPECT_EQ(blocked.PointStage.Count, 2u);
    }

    TEST_F(PathQueryDiagnosticsTest, SlopeLimitedPointsKeepTheirOriginalStatusAndReachablePrefix)
    {
        BuildIslands(true, true);
        auto diagnostics = Compare(_unit.get(), {60, 10, 88.75f}, false, false, false, 296.0f, true);
        EXPECT_TRUE(diagnostics.PointStage.StatusAvailable);
        EXPECT_NE(diagnostics.PointStage.Status & DT_SLOPE_TOO_STEEP, 0u);
        EXPECT_EQ(diagnostics.PointStage.SlopeLimited, Check::Yes);
        EXPECT_EQ(diagnostics.PointStage.LimitReached, Check::NotEvaluated);
        EXPECT_EQ(diagnostics.ResultType, PATHFIND_NORMAL | PATHFIND_INCOMPLETE);
        EXPECT_LT(diagnostics.ActualEnd.x, 60.0f);
        EXPECT_EQ(diagnostics.CorridorStage.ReachesEndPolygon, Check::Yes);
        EXPECT_EQ(diagnostics.ShortcutReason, Shortcut::None);
    }

    TEST_F(PathQueryDiagnosticsTest, SinkResetsAfterInvalidAttemptWithoutMisreportingRetainedPath)
    {
        BuildIslands();
        PathGenerator original(_unit.get());
        PathGenerator captured(_unit.get());
        PathQueryDiagnostics diagnostics;
        ASSERT_TRUE(original.CalculatePath(90, 10, 10, true));
        ASSERT_TRUE(captured.CalculatePath(90, 10, 10, true, diagnostics));
        ASSERT_EQ(diagnostics.ShortcutReason, Shortcut::ForcedDestination);
        auto previous = captured.GetPath();
        float invalid = std::numeric_limits<float>::quiet_NaN();
        EXPECT_FALSE(original.CalculatePath(invalid, 10, 10, false));
        EXPECT_FALSE(captured.CalculatePath(invalid, 10, 10, false, diagnostics));
        ExpectSamePath(original, captured);
        EXPECT_EQ(captured.GetPath(), previous);
        EXPECT_FALSE(diagnostics.ResultUpdated);
        EXPECT_EQ(diagnostics.ResultType, PATHFIND_BLANK);
        EXPECT_EQ(diagnostics.ResultPointCount, 0u);
        EXPECT_EQ(diagnostics.CoordinatesValid, Check::No);
        EXPECT_EQ(diagnostics.HasNavMesh, Check::NotEvaluated);
        EXPECT_EQ(diagnostics.StartPolygon.Source, Lookup::NotEvaluated);
        EXPECT_EQ(diagnostics.CorridorStage.Mode, CorridorMode::NotEvaluated);
        EXPECT_EQ(diagnostics.ShortcutReason, Shortcut::None);
        EXPECT_TRUE(std::isnan(diagnostics.End.x));
        EXPECT_FALSE(original.CalculatePath(invalid, 10, 10, 20, 10, 10, false));
        EXPECT_FALSE(captured.CalculatePath(invalid, 10, 10, 20, 10, 10, false, diagnostics));
        ExpectSamePath(original, captured);
        EXPECT_TRUE(diagnostics.ExplicitStart);
        EXPECT_TRUE(std::isnan(diagnostics.Start.x));
        EXPECT_EQ(diagnostics.End, G3D::Vector3(20, 10, 10));
        EXPECT_FALSE(diagnostics.ResultUpdated);
        EXPECT_EQ(diagnostics.HasNavMesh, Check::NotEvaluated);
    }

    TEST_F(PathQueryDiagnosticsTest, ReusedGeneratorRecordsCacheAndExtensionWithoutAnotherLookup)
    {
        BuildIslands(true);
        PathGenerator original(_unit.get());
        PathGenerator captured(_unit.get());
        PathQueryDiagnostics diagnostics;
        for (float goalX : {50.0f, 60.0f, 90.0f, 150.0f})
        {
            ASSERT_EQ(original.CalculatePath(goalX, 10, 10, false),
                captured.CalculatePath(goalX, 10, 10, false, diagnostics));
            ExpectSamePath(original, captured);
            if (goalX == 60.0f)
            {
                EXPECT_EQ(diagnostics.StartPolygon.Source, Lookup::CachedCorridor);
                EXPECT_EQ(diagnostics.EndPolygon.Source, Lookup::CachedCorridor);
                EXPECT_EQ(diagnostics.StartPolygon.SmallSearchSucceeded, Check::NotEvaluated);
                EXPECT_EQ(diagnostics.EndPolygon.TallSearchSucceeded, Check::NotEvaluated);
                EXPECT_EQ(diagnostics.CorridorStage.Mode, CorridorMode::Reused);
                EXPECT_FALSE(diagnostics.CorridorStage.StatusAvailable);
                EXPECT_EQ(diagnostics.CorridorStage.PolyCount, 2u);
            }
            if (goalX == 90.0f)
            {
                EXPECT_EQ(diagnostics.CorridorStage.Mode, CorridorMode::Extended);
                EXPECT_TRUE(diagnostics.CorridorStage.StatusAvailable);
                EXPECT_NE(diagnostics.CorridorStage.Status & DT_PARTIAL_RESULT, 0u);
                EXPECT_EQ(diagnostics.CorridorStage.ReachesEndPolygon, Check::No);
            }
            if (goalX == 150.0f)
            {
                EXPECT_EQ(diagnostics.ShortcutReason, Shortcut::Prerequisite);
                EXPECT_EQ(diagnostics.EndTile.Present, Check::No);
                EXPECT_EQ(diagnostics.StartPolygon.Source, Lookup::NotEvaluated);
                EXPECT_EQ(diagnostics.CorridorStage.Mode, CorridorMode::NotEvaluated);
                EXPECT_EQ(diagnostics.CorridorStage.PolyCount, 0u);
                EXPECT_EQ(diagnostics.CorridorStage.LastPoly, INVALID_POLYREF);
                EXPECT_FALSE(diagnostics.CorridorStage.StatusAvailable);
                EXPECT_FALSE(diagnostics.FilterUpdated);
            }
        }
    }

    TEST_F(PathQueryDiagnosticsTest, ExplicitStartSnapshotIsNotActorPositionOrNormalizedPathFront)
    {
        BuildIslands();
        PathGenerator original(_unit.get());
        PathGenerator captured(_unit.get());
        PathQueryDiagnostics diagnostics;
        EXPECT_EQ(original.CalculatePath(15, 10, 10.5f, 20, 10, 10.5f, false),
            captured.CalculatePath(15, 10, 10.5f, 20, 10, 10.5f, false, diagnostics));
        ExpectSamePath(original, captured);
        EXPECT_TRUE(diagnostics.ExplicitStart);
        EXPECT_EQ(diagnostics.Start, G3D::Vector3(15, 10, 10.5f));
        EXPECT_EQ(diagnostics.End, G3D::Vector3(20, 10, 10.5f));
        EXPECT_EQ(captured.GetPath().front(), G3D::Vector3(15, 10, 10));
        EXPECT_EQ(diagnostics.ActualEnd, G3D::Vector3(20, 10, 10));
        EXPECT_FLOAT_EQ(_unit->GetPositionX(), 10.0f);
        EXPECT_FLOAT_EQ(_unit->GetPositionZ(), 10.0f);
        captured.Clear();
        EXPECT_TRUE(captured.GetPath().empty());
        EXPECT_EQ(diagnostics.Start, G3D::Vector3(15, 10, 10.5f));
        EXPECT_TRUE(diagnostics.ResultUpdated);
        ASSERT_TRUE(captured.CalculatePath(40, 10, 10, false));
        EXPECT_EQ(diagnostics.End, G3D::Vector3(20, 10, 10.5f));
        EXPECT_EQ(diagnostics.ActualEnd, G3D::Vector3(20, 10, 10));
    }

    class OwnedFallMovementTest : public PathGeneratorCorridorTest
    {
    protected:
        void SetUp() override
        {
            PathGeneratorCorridorTest::SetUp();
            ScriptRegistry<AchievementScript>::InitEnabledHooksIfNeeded(ACHIEVEMENTHOOK_END);
            CreatePlayer();
            _player->SetLifeState(DeathState::Alive, false);
            _player->SetHealth(100);
            _player->Relocate(10, 10, 12);
            _player->SetFallInformation(0, 12);
        }

        void Begin()
        {
            auto token = _player->GetMotionMaster()->MoveFallOwned(81);
            ASSERT_TRUE(token);
            _token = *token;
            auto status = _player->GetOwnedFallStatus(_token);
            ASSERT_TRUE(status);
            ASSERT_EQ(status->Result, OwnedFallResult::Active);
            EXPECT_EQ(status->SplineId, _player->movespline->GetId());
            EXPECT_EQ(status->Start, G3D::Vector3(10, 10, 12));
            EXPECT_EQ(status->Destination, G3D::Vector3(10, 10, 10));
            EXPECT_TRUE(_player->HasUnitMovementFlag(MOVEMENTFLAG_FALLING));
            EXPECT_EQ(_player->m_movementInfo.fallTime, 0u);
        }

        void Tick(uint32 milliseconds)
        {
            _player->Unit::Update(milliseconds);
        }

        OwnedFallStatus Status()
        {
            auto status = _player->GetOwnedFallStatus(_token);
            EXPECT_TRUE(status);
            return status.value_or(OwnedFallStatus{});
        }

        void ExpectGroundReady()
        {
            EXPECT_FALSE(_player->HasUnitMovementFlag(MOVEMENTFLAG_FALLING | MOVEMENTFLAG_FALLING_FAR));
            // Player's legacy falling query compares Z with lastFallZ, independently of spline/flag ownership.
            EXPECT_EQ(_player->IsFalling(), _player->GetPositionZ() < Status().Position.z);
            EXPECT_FALSE(static_cast<Unit*>(_player.get())->IsFalling());
            EXPECT_EQ(_player->m_movementInfo.fallTime, 0u);
            EXPECT_EQ(_player->GetHealth(), 100u);
        }

        OwnedFallToken _token;
    };

    TEST_F(OwnedFallMovementTest, ActualNaturalLandingIsRecordedAndCleanedBeforeTheNextPoint)
    {
        Begin();
        uint32 duration = _player->movespline->Duration();
        Tick(duration);
        auto status = Status();
        EXPECT_EQ(status.Result, OwnedFallResult::Landed);
        EXPECT_TRUE(status.LandingHandled);
        EXPECT_EQ(status.Elapsed, status.Duration);
        EXPECT_EQ(status.InterruptCount, 1u);
        EXPECT_NEAR(status.Position.z, 10.0f, 0.01f);
        EXPECT_EQ(status.Position, G3D::Vector3(_player->GetPositionX(), _player->GetPositionY(),
            _player->GetPositionZ()));
        ExpectGroundReady();
        EXPECT_EQ(_player->GetMotionMaster()->GetCurrentMovementGeneratorType(), IDLE_MOTION_TYPE);
        Movement::PointsArray next{status.Position, {15, 10, 10}};
        EXPECT_TRUE(_player->GetMotionMaster()->MovePointPath(82, next, 0, 0));
        EXPECT_FALSE(_player->GetMotionMaster()->CancelOwnedFall(_token));
        EXPECT_EQ(Status().Result, OwnedFallResult::Landed);
    }

    TEST_F(OwnedFallMovementTest, QueuedActivePointCannotOverwriteNaturalLandingEvidence)
    {
        Begin();
        uint32 ownedSpline = _player->movespline->GetId();
        _player->GetMotionMaster()->MovePoint(82, 15, 10, 10, FORCED_MOVEMENT_RUN, 0, 0, false);
        ASSERT_EQ(_player->movespline->GetId(), ownedSpline);
        ASSERT_EQ(_player->GetMotionMaster()->GetCurrentMovementGeneratorType(), EFFECT_MOTION_TYPE);
        Tick(_player->movespline->Duration());
        auto landed = Status();
        EXPECT_EQ(landed.Result, OwnedFallResult::Landed);
        EXPECT_TRUE(landed.LandingHandled);
        EXPECT_EQ(landed.SplineId, ownedSpline);
        EXPECT_NE(_player->movespline->GetId(), ownedSpline);
        EXPECT_EQ(_player->GetMotionMaster()->GetCurrentMovementGeneratorType(), POINT_MOTION_TYPE);
        EXPECT_FALSE(_player->HasUnitMovementFlag(MOVEMENTFLAG_FALLING));
        Tick(_player->movespline->Duration());
        ExpectGroundReady();
        EXPECT_EQ(Status().SplineId, ownedSpline);
        EXPECT_TRUE(_player->GetMotionMaster()->MovePointPath(83, {{15, 10, 10}, {20, 10, 10}}, 0, 0));
    }

    TEST_F(OwnedFallMovementTest, ForeignGroundSplineAtomicallyRetiresOnlyTheOwnedState)
    {
        Begin();
        Tick(1);
        Movement::MoveSplineInit foreign(_player.get());
        foreign.MovebyPath({{10, 10, _player->GetPositionZ()}, {15, 10, 10}});
        ASSERT_GT(foreign.Launch(), 0);
        uint32 foreignId = _player->movespline->GetId();
        EXPECT_EQ(Status().Result, OwnedFallResult::Replaced);
        EXPECT_FALSE(Status().LandingHandled);
        EXPECT_FALSE(_player->HasUnitMovementFlag(MOVEMENTFLAG_FALLING));
        EXPECT_FALSE(_player->GetMotionMaster()->CancelOwnedFall(_token));
        EXPECT_EQ(_player->movespline->GetId(), foreignId);
        Tick(_player->movespline->Duration());
        ExpectGroundReady();
        EXPECT_TRUE(_player->GetMotionMaster()->MovePointPath(82, {{15, 10, 10}, {20, 10, 10}}, 0, 0));
    }

    TEST_F(OwnedFallMovementTest, ControlledPointReplacementRetiresWithoutArrivalOrDamage)
    {
        Begin();
        _player->GetMotionMaster()->MovePoint(82, 15, 10, 10, FORCED_MOVEMENT_RUN,
            0, 0, false, false, MOTION_SLOT_CONTROLLED);
        EXPECT_EQ(Status().Result, OwnedFallResult::Cancelled);
        EXPECT_FALSE(Status().LandingHandled);
        EXPECT_FALSE(_player->HasUnitMovementFlag(MOVEMENTFLAG_FALLING));
        EXPECT_EQ(_player->GetMotionMaster()->GetCurrentMovementGeneratorType(), POINT_MOTION_TYPE);
        Tick(_player->movespline->Duration());
        ExpectGroundReady();
    }

    TEST_F(OwnedFallMovementTest, ExplicitSameBitAssertionTransfersToTheForeignFallWriter)
    {
        Begin();
        _player->AddUnitMovementFlag(MOVEMENTFLAG_FALLING);
        _player->SetFallInformation(77, 40);
        EXPECT_EQ(Status().Result, OwnedFallResult::Revoked);
        EXPECT_FALSE(Status().LandingHandled);
        Movement::MoveSplineInit foreign(_player.get());
        foreign.MovebyPath({{10, 10, 12}, {15, 10, 10}});
        ASSERT_GT(foreign.Launch(), 0);
        EXPECT_TRUE(_player->HasUnitMovementFlag(MOVEMENTFLAG_FALLING));
        EXPECT_FALSE(_player->GetMotionMaster()->CancelOwnedFall(_token));
        EXPECT_TRUE(_player->HasUnitMovementFlag(MOVEMENTFLAG_FALLING));
        EXPECT_TRUE(_player->IsFalling());
        EXPECT_EQ(_player->GetHealth(), 100u);
    }

    TEST_F(OwnedFallMovementTest, WholeMaskCopyDoesNotPretendToBeANewFallWriter)
    {
        Begin();
        _player->SetUnitMovementFlags(_player->GetUnitMovementFlags() | MOVEMENTFLAG_WALKING);
        EXPECT_EQ(Status().Result, OwnedFallResult::Active);
        Movement::MoveSplineInit foreign(_player.get());
        foreign.MovebyPath({{10, 10, 12}, {15, 10, 10}});
        ASSERT_GT(foreign.Launch(), 0);
        EXPECT_EQ(Status().Result, OwnedFallResult::Replaced);
        EXPECT_FALSE(_player->HasUnitMovementFlag(MOVEMENTFLAG_FALLING));
        EXPECT_TRUE(_player->HasUnitMovementFlag(MOVEMENTFLAG_WALKING));
    }

    TEST_F(OwnedFallMovementTest, ForeignFallAndParabolicStateSurviveOldTokenCancellation)
    {
        for (bool parabolic : {false, true})
        {
            SCOPED_TRACE(parabolic);
            Begin();
            if (parabolic)
            {
                Movement::MoveSplineInit foreign(_player.get());
                foreign.MovebyPath({{10, 10, 12}, {15, 10, 12}});
                foreign.SetParabolic(2, 0);
                ASSERT_GT(foreign.Launch(), 0);
            }
            else
                _player->GetMotionMaster()->MoveFall(82);
            uint32 foreignId = _player->movespline->GetId();
            EXPECT_TRUE(_player->HasUnitMovementFlag(MOVEMENTFLAG_FALLING));
            EXPECT_FALSE(_player->GetMotionMaster()->CancelOwnedFall(_token));
            EXPECT_EQ(_player->movespline->GetId(), foreignId);
            EXPECT_TRUE(_player->HasUnitMovementFlag(MOVEMENTFLAG_FALLING));
            EXPECT_FALSE(Status().LandingHandled);
            _player->StopMoving();
            _player->GetMotionMaster()->Clear();
            _player->RemoveUnitMovementFlag(MOVEMENTFLAG_FALLING | MOVEMENTFLAG_FALLING_FAR);
            _player->Relocate(10, 10, 12);
            _player->SetFallInformation(0, 12);
        }
    }

    TEST_F(OwnedFallMovementTest, FailedForeignLaunchDoesNotRetireTheFallOrChangeBookkeeping)
    {
        Begin();
        auto before = Status();
        uint32 flags = _player->GetUnitMovementFlags();
        Movement::MoveSplineInit refused(_player.get());
        EXPECT_EQ(refused.Launch(), 0);
        EXPECT_EQ(Status().Result, OwnedFallResult::Active);
        EXPECT_EQ(Status().SplineId, before.SplineId);
        EXPECT_EQ(_player->GetUnitMovementFlags(), flags);
        EXPECT_EQ(Status().InterruptCount, before.InterruptCount);
        Movement::MoveSplineInit invalid(_player.get());
        invalid.MovebyPath({{10, 10, 12}, {15, 10, 10}});
        invalid.SetVelocity(0.0f);
        EXPECT_EQ(invalid.Launch(), 0);
        EXPECT_EQ(Status().Result, OwnedFallResult::Active);
        EXPECT_EQ(_player->movespline->GetId(), before.SplineId);
        EXPECT_EQ(_player->GetUnitMovementFlags(), flags);
    }

    TEST_F(OwnedFallMovementTest, CancelStopAndInterruptNeverPerformLanding)
    {
        for (unsigned operation = 0; operation < 3; ++operation)
        {
            SCOPED_TRACE(operation);
            Begin();
            Tick(1);
            float height = _player->GetPositionZ();
            if (operation == 0)
                EXPECT_TRUE(_player->GetMotionMaster()->CancelOwnedFall(_token));
            else if (operation == 1)
                _player->StopMoving();
            else
                _player->DisableSpline();
            Tick(1);
            EXPECT_EQ(Status().Result, OwnedFallResult::Cancelled);
            EXPECT_FALSE(Status().LandingHandled);
            EXPECT_FLOAT_EQ(_player->GetPositionZ(), height);
            ExpectGroundReady();
            EXPECT_FALSE(_player->GetMotionMaster()->CancelOwnedFall(_token));
            _player->Relocate(10, 10, 12);
            _player->SetFallInformation(0, 12);
        }
    }

    TEST_F(OwnedFallMovementTest, WrongUnitStaleAndRepeatedTokensCannotMutateMovement)
    {
        Begin();
        auto first = _token;
        EXPECT_FALSE(_unit->GetOwnedFallStatus(first));
        EXPECT_FALSE(_unit->GetMotionMaster()->CancelOwnedFall(first));
        EXPECT_TRUE(_player->GetMotionMaster()->CancelOwnedFall(first));
        EXPECT_FALSE(_player->GetMotionMaster()->CancelOwnedFall(first));
        Begin();
        EXPECT_NE(first, _token);
        EXPECT_FALSE(_player->GetOwnedFallStatus(first));
        EXPECT_FALSE(_player->GetMotionMaster()->CancelOwnedFall(first));
        EXPECT_EQ(Status().Result, OwnedFallResult::Active);
    }

    TEST_F(OwnedFallMovementTest, RejectedAdmissionDoesNotReplaceOrAllocateAClaim)
    {
        _player->SetControlled(true, UNIT_STATE_ROOT);
        EXPECT_FALSE(_player->GetMotionMaster()->MoveFallOwned());
        _player->SetControlled(false, UNIT_STATE_ROOT);
        _player->Casting = true;
        EXPECT_FALSE(_player->GetMotionMaster()->MoveFallOwned());
        _player->Casting = false;
        _player->AddUnitMovementFlag(MOVEMENTFLAG_FALLING);
        EXPECT_FALSE(_player->GetMotionMaster()->MoveFallOwned());
        _player->RemoveUnitMovementFlag(MOVEMENTFLAG_FALLING);
        _player->GetMotionMaster()->MoveDistract(100);
        auto* owner = _player->GetMotionMaster()->top();
        EXPECT_FALSE(_player->GetMotionMaster()->MoveFallOwned());
        EXPECT_EQ(_player->GetMotionMaster()->top(), owner);
        EXPECT_EQ(_player->movespline->GetId(), 0u);
    }

    TEST_F(OwnedFallMovementTest, SupportedBookkeepingAndAuthoritativeWritesRevokeBeforeNewState)
    {
        Begin();
        Tick(1);
        _player->SetFallInformation(37, 50);
        EXPECT_EQ(Status().Result, OwnedFallResult::Revoked);
        EXPECT_FALSE(_player->HasUnitMovementFlag(MOVEMENTFLAG_FALLING));
        EXPECT_TRUE(_player->IsFalling());
        EXPECT_FALSE(_player->GetMotionMaster()->CancelOwnedFall(_token));
        Tick(1);
        _player->Relocate(10, 10, 12);
        _player->SetFallInformation(0, 12);
        Begin();
        MovementInfo incoming = _player->m_movementInfo;
        incoming.flags = MOVEMENTFLAG_FALLING | MOVEMENTFLAG_FALLING_FAR;
        incoming.fallTime = 91;
        _player->RevokeOwnedFall();
        _player->m_movementInfo = incoming;
        EXPECT_EQ(Status().Result, OwnedFallResult::Revoked);
        EXPECT_FALSE(_player->GetMotionMaster()->CancelOwnedFall(_token));
        EXPECT_TRUE(_player->HasUnitMovementFlag(MOVEMENTFLAG_FALLING_FAR));
        EXPECT_EQ(_player->m_movementInfo.fallTime, 91u);
    }

    TEST_F(OwnedFallMovementTest, AuthoritativeNotActiveMoverPacketPreservesItsNewFallState)
    {
        Begin();
        MovementInfo incoming = _player->m_movementInfo;
        incoming.guid = _player->GetGUID();
        incoming.flags = MOVEMENTFLAG_FALLING | MOVEMENTFLAG_FALLING_FAR;
        incoming.fallTime = 123;
        incoming.pos.Relocate(10, 10, 11);
        WorldPacket packet(CMSG_MOVE_NOT_ACTIVE_MOVER);
        _player->GetSession()->WriteMovementInfo(&packet, &incoming);
        _player->GetSession()->HandleMoveNotActiveMover(packet);
        EXPECT_EQ(Status().Result, OwnedFallResult::Revoked);
        EXPECT_FALSE(Status().LandingHandled);
        EXPECT_TRUE(_player->movespline->Finalized());
        EXPECT_EQ(_player->m_movementInfo.flags, incoming.flags);
        EXPECT_EQ(_player->m_movementInfo.fallTime, 123u);
        EXPECT_FALSE(_player->GetMotionMaster()->CancelOwnedFall(_token));
        EXPECT_EQ(_player->m_movementInfo.flags, incoming.flags);
    }

    TEST_F(OwnedFallMovementTest, NaturalLandingDamageIsHandledExactlyOnce)
    {
        ON_CALL(*static_cast<PointPathWorld*>(sWorld.get()), getRate(_)).WillByDefault(Return(1.0f));
        _player->Relocate(10, 10, 30);
        _player->SetFallInformation(0, 30);
        auto token = _player->GetMotionMaster()->MoveFallOwned(81);
        ASSERT_TRUE(token);
        _token = *token;
        unsigned landingPackets = 0;
        _player->OnEnvironmentalDamage = [&]
        {
            EXPECT_FALSE(Status().LandingHandled);
            ++landingPackets;
        };
        Tick(_player->movespline->Duration());
        EXPECT_EQ(Status().Result, OwnedFallResult::Landed);
        EXPECT_TRUE(Status().LandingHandled);
        EXPECT_EQ(landingPackets, 1u);
        EXPECT_LT(_player->GetHealth(), 100u);
        uint32 health = _player->GetHealth();
        Tick(1);
        EXPECT_EQ(_player->GetHealth(), health);
        EXPECT_EQ(landingPackets, 1u);
        EXPECT_FALSE(_player->GetMotionMaster()->CancelOwnedFall(_token));
        EXPECT_FALSE(_player->HasUnitMovementFlag(MOVEMENTFLAG_FALLING));
    }

    TEST_F(OwnedFallMovementTest, LandingSideEffectsCannotOverwriteNewPhaseBookkeeping)
    {
        ON_CALL(*static_cast<PointPathWorld*>(sWorld.get()), getRate(_)).WillByDefault(Return(1.0f));
        _player->Relocate(10, 10, 30);
        _player->SetFallInformation(0, 30);
        auto token = _player->GetMotionMaster()->MoveFallOwned(81);
        ASSERT_TRUE(token);
        _token = *token;
        _player->OnEnvironmentalDamage = [&] { _player->SetPhaseMask(2, false); };
        Tick(_player->movespline->Duration());
        EXPECT_EQ(Status().Result, OwnedFallResult::Landed);
        EXPECT_TRUE(Status().LandingHandled);
        EXPECT_EQ(_player->GetPhaseMask(), 2u);
        // A context-only writer retires the outstanding old bookkeeping before changing phase.
        EXPECT_FALSE(_player->IsFalling());
        EXPECT_EQ(_player->m_movementInfo.fallTime, 0u);
        EXPECT_FALSE(_player->HasUnitMovementFlag(MOVEMENTFLAG_FALLING));
        EXPECT_FALSE(_player->GetMotionMaster()->CancelOwnedFall(_token));
    }

    TEST_F(OwnedFallMovementTest, GenuineForeignLandingCallbackBookkeepingMustSurvive)
    {
        ON_CALL(*static_cast<PointPathWorld*>(sWorld.get()), getRate(_)).WillByDefault(Return(1.0f));
        _player->Relocate(10, 10, 30);
        _player->SetFallInformation(0, 30);
        auto token = _player->GetMotionMaster()->MoveFallOwned(81);
        ASSERT_TRUE(token);
        _token = *token;
        _player->OnEnvironmentalDamage = [&]
        {
            EXPECT_FALSE(Status().LandingHandled);
            _player->AddUnitMovementFlag(MOVEMENTFLAG_FALLING);
            _player->SetFallInformation(555, 60);
        };
        Tick(_player->movespline->Duration());
        EXPECT_EQ(Status().Result, OwnedFallResult::Landed);
        EXPECT_TRUE(Status().LandingHandled);
        EXPECT_TRUE(_player->HasUnitMovementFlag(MOVEMENTFLAG_FALLING));
        EXPECT_TRUE(_player->IsFalling());
        EXPECT_FALSE(_player->GetMotionMaster()->CancelOwnedFall(_token));
        EXPECT_TRUE(_player->HasUnitMovementFlag(MOVEMENTFLAG_FALLING));
    }

    TEST_F(OwnedFallMovementTest, WholeMaskModeSwitchCannotReintroduceReleasedOwnedFalling)
    {
        Begin();
        _player->SetUnitMovementFlags(_player->GetUnitMovementFlags() | MOVEMENTFLAG_SWIMMING);
        EXPECT_EQ(Status().Result, OwnedFallResult::Revoked);
        EXPECT_FALSE(_player->HasUnitMovementFlag(MOVEMENTFLAG_FALLING));
        EXPECT_FALSE(_player->HasUnitMovementFlag(MOVEMENTFLAG_SPLINE_ENABLED));
        EXPECT_TRUE(_player->HasUnitMovementFlag(MOVEMENTFLAG_SWIMMING));
        _player->RemoveUnitMovementFlag(MOVEMENTFLAG_SWIMMING);
        Tick(1);
        EXPECT_TRUE(_player->GetMotionMaster()->MovePointPath(82, {{10, 10, 12}, {15, 10, 10}}, 0, 0));
    }

    TEST_F(OwnedFallMovementTest, ExplicitForeignFlagAssertionMustNotBeSanitizedAsAnInheritedMask)
    {
        Begin();
        _player->AddUnitMovementFlag(MOVEMENTFLAG_FALLING);
        _player->SetUnitMovementFlags(_player->GetUnitMovementFlags() | MOVEMENTFLAG_SWIMMING);
        EXPECT_EQ(Status().Result, OwnedFallResult::Revoked);
        EXPECT_TRUE(_player->HasUnitMovementFlag(MOVEMENTFLAG_FALLING));
        EXPECT_TRUE(_player->HasUnitMovementFlag(MOVEMENTFLAG_SWIMMING));
        EXPECT_FALSE(_player->GetMotionMaster()->CancelOwnedFall(_token));
        EXPECT_TRUE(_player->HasUnitMovementFlag(MOVEMENTFLAG_FALLING));
    }

    TEST_F(OwnedFallMovementTest, StockKnockbackCommandMustRevokeBeforeDelayedAcknowledgement)
    {
        Begin();
        uint32 duration = _player->movespline->Duration();
        _player->KnockbackFrom(9, 10, 5, 3);
        EXPECT_EQ(Status().Result, OwnedFallResult::Revoked);
        EXPECT_TRUE(_player->movespline->Finalized());
        Tick(duration + 1);
        EXPECT_EQ(Status().Result, OwnedFallResult::Revoked);
        EXPECT_FALSE(Status().LandingHandled);
        EXPECT_FLOAT_EQ(_player->GetPositionZ(), 12.0f);

        MovementInfo incoming = _player->m_movementInfo;
        incoming.guid = _player->GetGUID();
        incoming.flags = MOVEMENTFLAG_FALLING | MOVEMENTFLAG_FALLING_FAR;
        incoming.fallTime = 123;
        incoming.pos.Relocate(11, 10, 13);
        WorldPacket encoded(MSG_MOVE_HEARTBEAT);
        _player->GetSession()->WriteMovementInfo(&encoded, &incoming);
        WorldPacket packet(CMSG_MOVE_KNOCK_BACK_ACK);
        packet << incoming.guid.WriteAsPacked() << uint32(1);
        std::size_t guidSize = _player->GetPackGUID().size();
        packet.append(encoded.contents() + guidSize, encoded.size() - guidSize);
        _player->GetSession()->HandleMoveKnockBackAck(packet);
        EXPECT_FALSE(_player->GetMotionMaster()->CancelOwnedFall(_token));
        EXPECT_EQ(_player->m_movementInfo.flags, incoming.flags);
        EXPECT_EQ(_player->m_movementInfo.fallTime, 123u);
    }

    TEST_F(OwnedFallMovementTest, InitialPacketCallbacksCannotAdoptADeletedOwnerOrForeignSpline)
    {
        for (bool replace : {false, true})
        {
            SCOPED_TRACE(replace);
            _player->OnMonsterMove = [&]
            {
                if (replace)
                {
                    Movement::MoveSplineInit foreign(_player.get());
                    foreign.MovebyPath({{10, 10, 12}, {15, 10, 10}});
                    EXPECT_GT(foreign.Launch(), 0);
                }
                else
                    _player->GetMotionMaster()->Clear();
            };
            auto token = _player->GetMotionMaster()->MoveFallOwned(81);
            ASSERT_TRUE(token);
            _token = *token;
            EXPECT_EQ(Status().Result, replace ? OwnedFallResult::Replaced : OwnedFallResult::Cancelled);
            EXPECT_FALSE(Status().LandingHandled);
            EXPECT_FALSE(_player->HasUnitMovementFlag(MOVEMENTFLAG_FALLING));
            EXPECT_FALSE(_player->GetMotionMaster()->CancelOwnedFall(_token));
            if (replace)
            {
                EXPECT_FALSE(_player->movespline->Finalized());
                EXPECT_NE(Status().SplineId, _player->movespline->GetId());
                Tick(_player->movespline->Duration());
            }
            EXPECT_EQ(_player->GetHealth(), 100u);
            _player->Relocate(10, 10, 12);
            _player->SetFallInformation(0, 12);
        }
    }

    TEST_F(OwnedFallMovementTest, PhaseControlDeathAndModeChangesCannotBecomeAnArrival)
    {
        for (unsigned change = 0; change < 4; ++change)
        {
            SCOPED_TRACE(change);
            Begin();
            if (change == 0)
                _player->SetPhaseMask(2, false);
            else if (change == 1)
                _player->SetClientControl(_player.get(), true);
            else if (change == 2)
                _player->SetLifeState(DeathState::Corpse, false);
            else
                _player->AddUnitMovementFlag(MOVEMENTFLAG_SWIMMING);
            Tick(1);
            EXPECT_EQ(Status().Result, OwnedFallResult::Revoked);
            EXPECT_FALSE(Status().LandingHandled);
            EXPECT_FALSE(_player->HasUnitMovementFlag(MOVEMENTFLAG_FALLING));
            EXPECT_EQ(_player->GetHealth(), 100u);
            _player->SetLifeState(DeathState::Alive, false);
            _player->RemoveUnitMovementFlag(MOVEMENTFLAG_SWIMMING);
            _player->SetPhaseMask(1, false);
            _player->Relocate(10, 10, 12);
            _player->SetFallInformation(0, 12);
        }
    }

    TEST_F(OwnedFallMovementTest, UntaggedLegacyFallRetainsItsExistingFlagsAndCompletionBehavior)
    {
        _player->GetMotionMaster()->MoveFall(81);
        Tick(_player->movespline->Duration());
        EXPECT_TRUE(_player->HasUnitMovementFlag(MOVEMENTFLAG_FALLING));
        EXPECT_TRUE(_player->IsFalling());
        EXPECT_FALSE(_player->GetOwnedFallStatus({1}));
        EXPECT_EQ(_player->GetHealth(), 100u);
    }

    TEST_F(OwnedFallMovementTest, LandingDebtDoesNotOwnANewWholeMaskFallAssertion)
    {
        ON_CALL(*static_cast<PointPathWorld*>(sWorld.get()), getRate(_)).WillByDefault(Return(1.0f));
        _player->Relocate(10, 10, 30);
        _player->SetFallInformation(0, 30);
        auto token = _player->GetMotionMaster()->MoveFallOwned(81);
        ASSERT_TRUE(token);
        _token = *token;
        _player->OnEnvironmentalDamage = [&]
        {
            EXPECT_FALSE(_player->HasUnitMovementFlag(MOVEMENTFLAG_FALLING));
            _player->SetUnitMovementFlags(_player->GetUnitMovementFlags() | MOVEMENTFLAG_FALLING);
            EXPECT_TRUE(_player->HasUnitMovementFlag(MOVEMENTFLAG_FALLING));
            _player->SetFallInformation(555, 60);
        };
        Tick(_player->movespline->Duration());
        EXPECT_EQ(Status().Result, OwnedFallResult::Landed);
        EXPECT_TRUE(Status().LandingHandled);
        EXPECT_TRUE(_player->HasUnitMovementFlag(MOVEMENTFLAG_FALLING));
        EXPECT_TRUE(_player->IsFalling());
        EXPECT_FALSE(_player->GetMotionMaster()->CancelOwnedFall(_token));
        EXPECT_TRUE(_player->HasUnitMovementFlag(MOVEMENTFLAG_FALLING));
        MovementInfo probe = _player->m_movementInfo;
        probe.pos.Relocate(_player->GetPosition());
        probe.fallTime = 1;
        _player->UpdateFallInformationIfNeed(probe, MSG_MOVE_HEARTBEAT);
        EXPECT_FALSE(_player->IsFalling());
    }

    TEST_F(OwnedFallMovementTest, SplineOnlyFlyAndTransportAnimationsDoNotClaimUnitFallState)
    {
        for (unsigned mode = 0; mode < 3; ++mode)
        {
            SCOPED_TRACE(mode);
            Begin();
            Tick(100);
            float sourceZ = _player->GetPositionZ();
            ASSERT_LT(sourceZ, 12.0f);
            Movement::MoveSplineInit foreign(_player.get());
            foreign.MovebyPath({{10, 10, sourceZ}, {15, 10, 10}});
            if (mode == 0)
                foreign.SetFly();
            else if (mode == 1)
                foreign.SetTransportEnter();
            else
                foreign.SetTransportExit();
            ASSERT_GT(foreign.Launch(), 0);
            uint32 foreignId = _player->movespline->GetId();
            EXPECT_FALSE(_player->movespline->isFalling());
            EXPECT_FALSE(_player->movespline->onTransport);
            EXPECT_EQ(_player->movespline->isBoarding(), mode != 0);
            EXPECT_FALSE(_player->HasUnitMovementFlag(MOVEMENTFLAG_FALLING));
            EXPECT_FALSE(_player->IsFalling());
            EXPECT_EQ(Status().Result, OwnedFallResult::Replaced);
            EXPECT_FALSE(Status().LandingHandled);
            EXPECT_FALSE(_player->GetMotionMaster()->CancelOwnedFall(_token));
            EXPECT_EQ(_player->movespline->GetId(), foreignId);
            Tick(_player->movespline->Duration());
            EXPECT_EQ(_player->movespline->FinalDestination(), G3D::Vector3(15, 10, 10));
            EXPECT_EQ(_player->GetPositionX(), 15.0f);
            EXPECT_TRUE(_player->GetMotionMaster()->MovePointPath(82, {{15, 10, 10}, {20, 10, 10}}, 0, 0));
            _player->GetMotionMaster()->Clear();
            _player->StopMoving();
            _player->RemoveUnitMovementFlag(MOVEMENTFLAG_FALLING | MOVEMENTFLAG_FALLING_FAR);
            _player->Relocate(10, 10, 12);
            _player->SetFallInformation(0, 12);
        }
    }

    TEST_F(OwnedFallMovementTest, SupportedClaimWriterAndIncomingSplineMatrix)
    {
        ON_CALL(*static_cast<PointPathWorld*>(sWorld.get()), getRate(_)).WillByDefault(Return(1.0f));
        unsigned transitions = 0;
        // Claim: none, active fall, landing bookkeeping only. Writer: none, explicit Add, whole-mask OR.
        for (unsigned claim = 0; claim < 3; ++claim)
        {
            for (unsigned writer = 0; writer < 3; ++writer)
            {
                // Incoming: ground, falling, parabolic, flying, enter, exit, refused validation.
                for (unsigned mode = 0; mode < 7; ++mode)
                {
                    SCOPED_TRACE(testing::Message() << "claim=" << claim << " writer=" << writer << " mode=" << mode);
                    _player->GetMotionMaster()->Clear();
                    _player->StopMoving();
                    _player->RemoveUnitMovementFlag(MOVEMENTFLAG_FALLING | MOVEMENTFLAG_FALLING_FAR);
                    _player->Relocate(10, 10, claim == 2 ? 30.0f : 12.0f);
                    _player->SetFallInformation(0, _player->GetPositionZ());
                    _player->SetHealth(100);
                    auto transition = [&]
                    {
                        if (writer == 1)
                            _player->AddUnitMovementFlag(MOVEMENTFLAG_FALLING);
                        else if (writer == 2)
                            _player->SetUnitMovementFlags(_player->GetUnitMovementFlags() | MOVEMENTFLAG_FALLING);

                        uint32 beforeFlags = _player->GetUnitMovementFlags();
                        uint32 beforeSpline = _player->movespline->GetId();
                        bool beforeBookkeeping = _player->IsFalling();
                        Movement::MoveSplineInit incoming(_player.get());
                        incoming.MovebyPath({{10, 10, _player->GetPositionZ()}, {15, 10, 10}});
                        if (mode == 1)
                            incoming.SetFall();
                        else if (mode == 2)
                            incoming.SetParabolic(1, 0);
                        else if (mode == 3)
                            incoming.SetFly();
                        else if (mode == 4)
                            incoming.SetTransportEnter();
                        else if (mode == 5)
                            incoming.SetTransportExit();
                        else if (mode == 6)
                            incoming.SetVelocity(0);

                        if (mode == 6)
                        {
                            EXPECT_EQ(incoming.Launch(), 0);
                            EXPECT_EQ(_player->GetUnitMovementFlags(), beforeFlags);
                            EXPECT_EQ(_player->movespline->GetId(), beforeSpline);
                            EXPECT_EQ(_player->IsFalling(), beforeBookkeeping);
                        }
                        else
                        {
                            EXPECT_GT(incoming.Launch(), 0);
                            bool explicitForeign = writer == 1 || (writer == 2 && claim != 1);
                            bool inheritedAir = claim == 1 && writer != 1 && (mode == 1 || mode == 2);
                            EXPECT_EQ(_player->HasUnitMovementFlag(MOVEMENTFLAG_FALLING),
                                explicitForeign || inheritedAir);
                            EXPECT_EQ(_player->IsFalling(), inheritedAir);
                            EXPECT_EQ(_player->movespline->isFalling(), mode == 1);
                            EXPECT_EQ(_player->movespline->isBoarding(), mode == 4 || mode == 5);
                            EXPECT_EQ(_player->movespline->_Spline().mode(), mode == 3
                                ? Movement::SplineBase::ModeCatmullrom : Movement::SplineBase::ModeLinear);
                            EXPECT_FALSE(_player->movespline->onTransport);
                        }
                        ++transitions;
                    };

                    if (claim == 0)
                        transition();
                    else if (claim == 1)
                    {
                        Begin();
                        Tick(100);
                        ASSERT_LT(_player->GetPositionZ(), 12.0f);
                        transition();
                    }
                    else
                    {
                        auto token = _player->GetMotionMaster()->MoveFallOwned(81);
                        ASSERT_TRUE(token);
                        _token = *token;
                        _player->OnEnvironmentalDamage = [&]
                        {
                            EXPECT_EQ(Status().Result, OwnedFallResult::Landed);
                            EXPECT_FALSE(Status().LandingHandled);
                            EXPECT_FALSE(_player->HasUnitMovementFlag(MOVEMENTFLAG_FALLING));
                            transition();
                        };
                        Tick(_player->movespline->Duration());
                    }
                }
            }
        }
        EXPECT_EQ(transitions, 63u);
    }

    TEST_F(OwnedFallMovementTest, ActiveBookkeepingOnlyAndNoClaimSetterMatrix)
    {
        ON_CALL(*static_cast<PointPathWorld*>(sWorld.get()), getRate(_)).WillByDefault(Return(1.0f));
        unsigned transitions = 0;
        for (unsigned claim = 0; claim < 3; ++claim)
        {
            // Setter: whole WALKING, whole SWIMMING, whole FALLING, explicit FALLING, explicit bookkeeping.
            for (unsigned writer = 0; writer < 5; ++writer)
            {
                SCOPED_TRACE(testing::Message() << "claim=" << claim << " writer=" << writer);
                _player->GetMotionMaster()->Clear();
                _player->StopMoving();
                _player->RemoveUnitMovementFlag(MOVEMENTFLAG_FALLING | MOVEMENTFLAG_FALLING_FAR |
                    MOVEMENTFLAG_WALKING | MOVEMENTFLAG_SWIMMING);
                _player->Relocate(10, 10, claim == 2 ? 30.0f : 12.0f);
                _player->SetFallInformation(0, _player->GetPositionZ());
                _player->SetHealth(100);
                auto transition = [&]
                {
                    if (writer == 0)
                        _player->SetUnitMovementFlags(_player->GetUnitMovementFlags() | MOVEMENTFLAG_WALKING);
                    else if (writer == 1)
                        _player->SetUnitMovementFlags(_player->GetUnitMovementFlags() | MOVEMENTFLAG_SWIMMING);
                    else if (writer == 2)
                        _player->SetUnitMovementFlags(_player->GetUnitMovementFlags() | MOVEMENTFLAG_FALLING);
                    else if (writer == 3)
                        _player->AddUnitMovementFlag(MOVEMENTFLAG_FALLING);
                    else
                        _player->SetFallInformation(555, 60);

                    bool retainedOwned = claim == 1 && (writer == 0 || writer == 2);
                    bool foreign = writer == 3 || (writer == 2 && claim != 1);
                    EXPECT_EQ(_player->HasUnitMovementFlag(MOVEMENTFLAG_FALLING), retainedOwned || foreign);
                    if (writer == 4)
                        EXPECT_TRUE(_player->IsFalling());
                    if (claim == 1)
                        EXPECT_EQ(Status().Result, retainedOwned ? OwnedFallResult::Active : OwnedFallResult::Revoked);
                    if (writer == 0)
                        EXPECT_TRUE(_player->HasUnitMovementFlag(MOVEMENTFLAG_WALKING));
                    if (writer == 1)
                        EXPECT_TRUE(_player->HasUnitMovementFlag(MOVEMENTFLAG_SWIMMING));
                    ++transitions;
                };
                if (claim == 0)
                    transition();
                else if (claim == 1)
                {
                    Begin();
                    Tick(100);
                    transition();
                }
                else
                {
                    auto token = _player->GetMotionMaster()->MoveFallOwned(81);
                    ASSERT_TRUE(token);
                    _token = *token;
                    _player->OnEnvironmentalDamage = transition;
                    Tick(_player->movespline->Duration());
                    EXPECT_EQ(Status().Result, OwnedFallResult::Landed);
                    EXPECT_TRUE(Status().LandingHandled);
                    EXPECT_EQ(_player->IsFalling(), writer == 4);
                }
            }
        }
        EXPECT_EQ(transitions, 15u);
    }
}

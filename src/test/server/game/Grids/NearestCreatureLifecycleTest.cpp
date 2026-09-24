/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 */

#include "AbstractFollower.h"
#include "CellImpl.h"
#include "GridNotifiers.h"
#include "IdleMovementGenerator.h"
#include "MotionMaster.h"
#include "MovementGenerator.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptDefines/PlayerScript.h"
#include "ScriptMgr.h"
#include "TargetedMovementGenerator.h"
#include "TemporarySummon.h"
#include "TestCreature.h"
#include "TestMap.h"
#include "WorldMock.h"
#include "WorldSession.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include <cmath>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

using namespace testing;

namespace
{
    constexpr uint32 TargetEntry = 24373;
    constexpr uint32 OtherEntry = 900002;
    constexpr uint32 OwnerEntry = 23709;

    struct Lifetime
    {
        Creature* Current = nullptr;
        bool Deleted = false;
        std::function<void(Creature*)> BeforeDelete;
    };

    class LifecycleWorld : public WorldMock
    {
    public:
        MOCK_METHOD(SQLQueryHolderCallback&, AddQueryHolderCallback, (SQLQueryHolderCallback&&), (override));
    };

    class LifecycleMap : public TestMap
    {
    public:
        void Register(Creature* creature)
        {
            LoadGrid(creature->GetPositionX(), creature->GetPositionY());
            GetObjectsStore().Insert<Creature>(creature->GetGUID(), creature);
            AddToGrid<Creature>(creature, Cell(creature->GetPositionX(), creature->GetPositionY()));
        }
    };

    class LifecycleCreature : public TestCreature
    {
    public:
        explicit LifecycleCreature(std::shared_ptr<Lifetime> lifetime) : _lifetime(std::move(lifetime))
        {
            _lifetime->Current = this;
        }

        ~LifecycleCreature() override
        {
            if (_lifetime->BeforeDelete)
                _lifetime->BeforeDelete(this);
            _lifetime->Current = nullptr;
            _lifetime->Deleted = true;
        }

        // TestCreature's default override is a no-op; removal here must use the real lifecycle.
        void RemoveFromWorld() override { Creature::RemoveFromWorld(); }

        void DestroyForPlayer(Player*, bool = false) const override
        {
            if (OnDestroyForPlayer)
                OnDestroyForPlayer();
        }

        std::function<void()> OnDestroyForPlayer;

    private:
        std::shared_ptr<Lifetime> _lifetime;
    };

    class LifecycleSummon : public TempSummon
    {
    public:
        explicit LifecycleSummon(std::shared_ptr<Lifetime> lifetime)
            : TempSummon(nullptr, ObjectGuid::Empty), _lifetime(std::move(lifetime))
        {
            _lifetime->Current = this;
        }

        ~LifecycleSummon() override
        {
            if (_lifetime->BeforeDelete)
                _lifetime->BeforeDelete(this);
            _lifetime->Current = nullptr;
            _lifetime->Deleted = true;
        }

        void InitializeForTest(Map* map, CreatureTemplate const* prototype, ObjectGuid::LowType guid)
        {
            Object::_Create(guid, OwnerEntry, HighGuid::Unit);
            SetEntry(OwnerEntry);
            m_originalEntry = OwnerEntry;
            m_creatureInfo = prototype;
            SetMap(map);
            SetPhaseMask(1, false);
            Relocate(10.0f, 10.0f, 10.0f);
            SetUInt32Value(UNIT_FIELD_FACTIONTEMPLATE, 14);
            SetMaxHealth(100);
            SetHealth(100);
            m_deathState = DeathState::Alive;
            GetThreatMgr().Initialize();
            Object::AddToWorld();
        }

        void UpdateObjectVisibility(bool = true, bool = false) override { }

    private:
        std::shared_ptr<Lifetime> _lifetime;
    };

    class VisibilityObserver : public Player
    {
    public:
        explicit VisibilityObserver(WorldSession* session) : Player(session)
        {
            Object::_Create(990001, 0, HighGuid::Player);
        }
    };

    class NearestCreatureLifecycleTest : public Test
    {
    protected:
        void SetUp() override
        {
            _previousWorld = std::move(sWorld);
            auto world = std::make_unique<NiceMock<LifecycleWorld>>();
            ON_CALL(*world, getIntConfig(_)).WillByDefault(Return(0));
            ON_CALL(*world, getFloatConfig(_)).WillByDefault(Return(1.0f));
            ON_CALL(*world, getBoolConfig(_)).WillByDefault(Return(false));
            static std::string const empty;
            ON_CALL(*world, GetDataPath()).WillByDefault(ReturnRef(empty));
            sWorld = std::move(world);
            TestMap::EnsureDBC();
            if (!sMovementGeneratorRegistry->GetRegistryItem(IDLE_MOTION_TYPE))
                (new IdleMovementFactory())->RegisterSelf();
            _map = std::make_unique<LifecycleMap>();

            _prototype = std::make_unique<TestCreature>();
            _prototype->ForceInitValues(990000, OtherEntry);
            _owner = MakeOwner();
            ASSERT_EQ(_owner->GetEntry(), OwnerEntry) << "FIXTURE_PRECONDITION: owner entry";
            ASSERT_TRUE(_owner->IsInWorld()) << "FIXTURE_PRECONDITION: owner world membership";
            ASSERT_TRUE(_owner->IsAlive()) << "FIXTURE_PRECONDITION: owner life state";
        }

        void TearDown() override
        {
            // Safety-only teardown for failing baseline assertions, always while objects are still live.
            for (auto const& lifetime : _lifetimes)
                if (lifetime->Current)
                {
                    lifetime->BeforeDelete = {};
                    if (auto* creature = dynamic_cast<LifecycleCreature*>(lifetime->Current))
                        creature->OnDestroyForPlayer = {};
                    lifetime->Current->RemoveAllFollowers();
                }

            for (auto const& lifetime : _lifetimes)
                if (lifetime->Current)
                    _map->AddObjectToRemoveList(lifetime->Current);

            if (_map)
                _map->RemoveAllObjectsInRemoveList();
            _observer.reset();
            _lifetimes.clear();
            _prototype.reset();
            _map.reset();
            sWorld = std::move(_previousWorld);
        }

        LifecycleSummon* MakeOwner()
        {
            auto lifetime = std::make_shared<Lifetime>();
            auto creature = std::make_unique<LifecycleSummon>(lifetime);
            creature->InitializeForTest(_map.get(), _prototype->GetCreatureTemplate(), ++_nextGuid);
            _map->Register(creature.get());
            creature->GetMotionMaster()->MoveIdle();
            _setupStates.push_back({creature.get(), OwnerEntry, true, 1, 10.0f});
            _lifetimes.push_back(std::move(lifetime));
            return creature.release();
        }

        LifecycleCreature* MakeTarget(float x, bool alive = true, uint32 phase = 1,
            uint32 entry = TargetEntry)
        {
            auto lifetime = std::make_shared<Lifetime>();
            auto creature = std::make_unique<LifecycleCreature>(lifetime);
            creature->SetupForCombatTest(_map.get(), ++_nextGuid, entry);
            creature->SetEntry(entry);
            creature->Relocate(x, 10.0f, 10.0f);
            creature->SetAlive(alive);
            creature->SetPhase(phase);
            creature->SetMaxHealth(100);
            creature->SetHealth(alive ? 100 : 0);
            _map->Register(creature.get());
            creature->GetMotionMaster()->MoveIdle();
            _setupStates.push_back({creature.get(), entry, alive, phase, x});
            _lifetimes.push_back(std::move(lifetime));
            return creature.release();
        }

        void AssertFixturePreconditions()
        {
            for (auto const& expected : _setupStates)
            {
                Creature* creature = expected.CreatureObject;
                ASSERT_EQ(creature->GetEntry(), expected.Entry) << "FIXTURE_PRECONDITION: object entry";
                ASSERT_EQ(creature->GetOriginalEntry(), expected.Entry) << "FIXTURE_PRECONDITION: original entry";
                ASSERT_EQ(creature->IsAlive(), expected.Alive) << "FIXTURE_PRECONDITION: alive state";
                ASSERT_EQ(creature->GetPhaseMask(), expected.Phase) << "FIXTURE_PRECONDITION: phase mask";
                ASSERT_TRUE(creature->IsInWorld()) << "FIXTURE_PRECONDITION: world membership";
                ASSERT_FALSE(creature->IsDuringRemoveFromWorld()) << "FIXTURE_PRECONDITION: removal state";
                ASSERT_TRUE(creature->IsInGrid()) << "FIXTURE_PRECONDITION: grid membership";
                ASSERT_EQ(creature->GetMap(), _map.get()) << "FIXTURE_PRECONDITION: map";
                ASSERT_EQ(_map->GetCreature(creature->GetGUID()), creature) << "FIXTURE_PRECONDITION: object store";
                ASSERT_FLOAT_EQ(creature->GetPositionX(), expected.X) << "FIXTURE_PRECONDITION: x";
                ASSERT_FLOAT_EQ(creature->GetPositionY(), 10.0f) << "FIXTURE_PRECONDITION: y";
                ASSERT_FLOAT_EQ(creature->GetPositionZ(), 10.0f) << "FIXTURE_PRECONDITION: z";
                ASSERT_EQ(_owner->InSamePhase(creature), (expected.Phase & 1) != 0)
                    << "FIXTURE_PRECONDITION: phase relation";
                ASSERT_EQ(_owner->IsWithinDist(creature, 20.0f), std::fabs(expected.X - 10.0f) < 20.0f)
                    << "FIXTURE_PRECONDITION: distance relation";
            }
            RecordProperty("fixture_preconditions_verified", "true");
        }

        std::shared_ptr<Lifetime> Life(Creature const* creature) const
        {
            for (auto const& lifetime : _lifetimes)
                if (lifetime->Current == creature)
                    return lifetime;
            return nullptr;
        }

        FollowMovementGenerator<Creature>* Follow(Creature* owner = nullptr) const
        {
            if (!owner)
                owner = _owner;
            return dynamic_cast<FollowMovementGenerator<Creature>*>(
                owner->GetMotionMaster()->GetMotionSlot(MOTION_SLOT_ACTIVE));
        }

        void BeginFollow(Creature* target, Creature* owner = nullptr)
        {
            if (!owner)
                owner = _owner;
            owner->GetMotionMaster()->MoveFollow(target, 1.0f, 0.0f);
            ASSERT_NE(Follow(owner), nullptr);
            ASSERT_EQ(Follow(owner)->GetTarget(), target);
        }

        void QueueRemoval(Creature* target)
        {
            ObjectGuid guid = target->GetGUID();
            ASSERT_TRUE(target->IsInWorld());
            ASSERT_TRUE(target->IsInGrid());
            ASSERT_EQ(_map->GetCreature(guid), target);
            _map->AddObjectToRemoveList(target);
            ASSERT_FALSE(target->IsInWorld());
            ASSERT_FALSE(target->IsDuringRemoveFromWorld());
            ASSERT_TRUE(target->IsInGrid());
            ASSERT_EQ(_map->GetCreature(guid), nullptr);
        }

        void ObserveRemovalVisibility(LifecycleCreature* target)
        {
            ScriptRegistry<PlayerScript>::InitEnabledHooksIfNeeded(PLAYERHOOK_END);
            // Existing test convention: a socketless session stays alive because its destructor writes to the DB.
            static WorldSession* session = []
            {
                auto* value = new WorldSession(1, "nearest-lifecycle-test", 0, nullptr, SEC_PLAYER,
                    EXPANSION_WRATH_OF_THE_LICH_KING, 0, LOCALE_enUS, 0, false, false, 0);
                value->InitRBACDataForTest();
                return value;
            }();
            _observer = std::make_unique<VisibilityObserver>(session);
            _observer->GetObjectVisibilityContainer().LinkWorldObjectVisibility(target);
        }

        std::unique_ptr<IWorld> _previousWorld;
        std::unique_ptr<LifecycleMap> _map;
        std::unique_ptr<TestCreature> _prototype;
        std::unique_ptr<VisibilityObserver> _observer;
        struct SetupState
        {
            Creature* CreatureObject;
            uint32 Entry;
            bool Alive;
            uint32 Phase;
            float X;
        };
        std::vector<SetupState> _setupStates;
        std::vector<std::shared_ptr<Lifetime>> _lifetimes;
        LifecycleSummon* _owner = nullptr;
        ObjectGuid::LowType _nextGuid = 900100;
    };

    TEST_F(NearestCreatureLifecycleTest, InWorldLookupPreservesEntryPhaseRangeAndNearest)
    {
        MakeTarget(10.25f, true, 1, OtherEntry);
        MakeTarget(10.5f, true, 2);
        MakeTarget(100.0f);
        MakeTarget(18.0f);
        auto* nearest = MakeTarget(13.0f);
        ASSERT_NO_FATAL_FAILURE(AssertFixturePreconditions());
        EXPECT_EQ(_owner->FindNearestCreature(TargetEntry, 20.0f), nearest);
        EXPECT_EQ(_owner->FindNearestCreature(TargetEntry, 0.1f), nullptr);
    }

    TEST_F(NearestCreatureLifecycleTest, DeadButInWorldRemainsSelectable)
    {
        auto* alive = MakeTarget(15.0f);
        auto* dead = MakeTarget(12.0f, false);
        ASSERT_NO_FATAL_FAILURE(AssertFixturePreconditions());
        EXPECT_TRUE(dead->IsInWorld());
        EXPECT_EQ(_owner->FindNearestCreature(TargetEntry, 20.0f, false), dead);
        EXPECT_EQ(_owner->FindNearestCreature(TargetEntry, 20.0f, true), alive);
    }

    TEST_F(NearestCreatureLifecycleTest, OutOfWorldGridCandidateDoesNotHideValidFartherTarget)
    {
        auto* removed = MakeTarget(12.0f);
        auto* valid = MakeTarget(18.0f);
        ASSERT_NO_FATAL_FAILURE(AssertFixturePreconditions());
        ASSERT_NO_FATAL_FAILURE(QueueRemoval(removed));
        EXPECT_TRUE(removed->IsAlive());
        EXPECT_EQ(ObjectAccessor::GetUnit(*_owner, removed->GetGUID()), nullptr);
        Acore::NearestCreatureEntryWithLiveStateInObjectRangeCheck check(*_owner, TargetEntry, true, 20.0f);
        EXPECT_FALSE(check(removed));
        EXPECT_TRUE(check(valid));
        EXPECT_EQ(_owner->FindNearestCreature(TargetEntry, 20.0f), valid);
    }

    TEST_F(NearestCreatureLifecycleTest, RemovedDeadGridCandidateIsNotADeadQueryResult)
    {
        auto* removed = MakeTarget(12.0f, false);
        auto* valid = MakeTarget(18.0f, false);
        ASSERT_NO_FATAL_FAILURE(AssertFixturePreconditions());
        ASSERT_NO_FATAL_FAILURE(QueueRemoval(removed));
        EXPECT_EQ(_owner->FindNearestCreature(TargetEntry, 20.0f, false), valid);
    }

    TEST_F(NearestCreatureLifecycleTest, InWorldCandidateDuringActualRemovalIsExcluded)
    {
        auto* removing = MakeTarget(12.0f);
        auto* valid = MakeTarget(18.0f);
        ASSERT_NO_FATAL_FAILURE(AssertFixturePreconditions());
        ObserveRemovalVisibility(removing);
        bool observed = false;
        removing->OnDestroyForPlayer = [&]
        {
            observed = true;
            EXPECT_TRUE(removing->IsInWorld());
            EXPECT_TRUE(removing->IsDuringRemoveFromWorld());
            Acore::NearestCreatureEntryWithLiveStateInObjectRangeCheck check(*_owner, TargetEntry, true, 20.0f);
            EXPECT_FALSE(check(removing));
            EXPECT_TRUE(check(valid));
            EXPECT_EQ(_owner->FindNearestCreature(TargetEntry, 20.0f), valid);
        };
        ASSERT_NO_FATAL_FAILURE(QueueRemoval(removing));
        EXPECT_TRUE(observed);
        removing->OnDestroyForPlayer = {};
    }

    TEST_F(NearestCreatureLifecycleTest, RemovedTargetCannotBeReboundBeforeDeferredDeleteAndExpiry)
    {
        auto* target = MakeTarget(12.0f);
        ASSERT_NO_FATAL_FAILURE(AssertFixturePreconditions());
        auto lifetime = Life(target);
        ASSERT_NO_FATAL_FAILURE(BeginFollow(target));
        ASSERT_NO_FATAL_FAILURE(QueueRemoval(target));
        ASSERT_EQ(Follow()->GetTarget(), nullptr);

        Creature* selected = _owner->FindNearestCreature(TargetEntry, 20.0f);
        EXPECT_EQ(selected, nullptr);
        if (selected)
            _owner->GetMotionMaster()->MoveFollow(selected, 1.0f, 0.0f);

        bool reachedDestructor = false;
        lifetime->BeforeDelete = [&](Creature* stillLiveTarget)
        {
            reachedDestructor = true;
            ASSERT_NE(Follow(), nullptr);
            EXPECT_EQ(Follow()->GetTarget(), nullptr);
            // Record the baseline failure before freeing the target; never use a freed Unit as the oracle.
            if (Follow()->GetTarget() == stillLiveTarget)
                Follow()->SetTarget(nullptr);
        };
        _map->DelayedUpdate(0);
        EXPECT_TRUE(reachedDestructor);
        EXPECT_TRUE(lifetime->Deleted);
        ASSERT_NE(Follow(), nullptr);
        EXPECT_EQ(Follow()->GetTarget(), nullptr);
        _owner->GetMotionMaster()->UpdateMotion(1);
        EXPECT_EQ(Follow(), nullptr);
    }

    TEST_F(NearestCreatureLifecycleTest, PendingRemovalRetargetsOnlyToValidSurvivor)
    {
        auto* oldTarget = MakeTarget(12.0f);
        auto* survivor = MakeTarget(18.0f);
        ASSERT_NO_FATAL_FAILURE(AssertFixturePreconditions());
        auto lifetime = Life(oldTarget);
        ASSERT_NO_FATAL_FAILURE(BeginFollow(oldTarget));
        ASSERT_NO_FATAL_FAILURE(QueueRemoval(oldTarget));
        ASSERT_EQ(Follow()->GetTarget(), nullptr);

        Creature* selected = _owner->FindNearestCreature(TargetEntry, 20.0f);
        EXPECT_EQ(selected, survivor);
        if (selected)
            _owner->GetMotionMaster()->MoveFollow(selected, 1.0f, 0.0f);

        lifetime->BeforeDelete = [&](Creature* stillLiveTarget)
        {
            ASSERT_NE(Follow(), nullptr);
            EXPECT_EQ(Follow()->GetTarget(), survivor);
            if (Follow()->GetTarget() == stillLiveTarget)
                Follow()->SetTarget(nullptr);
        };
        _map->DelayedUpdate(0);
        ASSERT_TRUE(lifetime->Deleted);
        ASSERT_NE(Follow(), nullptr);
        EXPECT_EQ(Follow()->GetTarget(), survivor);
        _owner->GetMotionMaster()->MovementExpired(false);
        EXPECT_EQ(Follow(), nullptr);
    }

    TEST_F(NearestCreatureLifecycleTest, NormalDespawnDetachesMultipleFollowersBeforeExpiry)
    {
        auto* target = MakeTarget(12.0f);
        auto* secondOwner = MakeOwner();
        ASSERT_NO_FATAL_FAILURE(AssertFixturePreconditions());
        auto lifetime = Life(target);
        ASSERT_NO_FATAL_FAILURE(BeginFollow(target));
        ASSERT_NO_FATAL_FAILURE(BeginFollow(target, secondOwner));
        ASSERT_NO_FATAL_FAILURE(QueueRemoval(target));
        EXPECT_EQ(Follow()->GetTarget(), nullptr);
        EXPECT_EQ(Follow(secondOwner)->GetTarget(), nullptr);
        _map->DelayedUpdate(0);
        EXPECT_TRUE(lifetime->Deleted);
        _owner->GetMotionMaster()->MovementExpired(false);
        secondOwner->GetMotionMaster()->MovementExpired(false);
        EXPECT_EQ(Follow(), nullptr);
        EXPECT_EQ(Follow(secondOwner), nullptr);
    }

    TEST_F(NearestCreatureLifecycleTest, NormalRetargetUnlinksThePreviousTarget)
    {
        auto* first = MakeTarget(12.0f);
        auto* second = MakeTarget(16.0f);
        ASSERT_NO_FATAL_FAILURE(AssertFixturePreconditions());
        ASSERT_NO_FATAL_FAILURE(BeginFollow(first));
        ASSERT_NO_FATAL_FAILURE(BeginFollow(second));
        ASSERT_NO_FATAL_FAILURE(QueueRemoval(first));
        EXPECT_EQ(Follow()->GetTarget(), second);
        _map->DelayedUpdate(0);
        EXPECT_EQ(Follow()->GetTarget(), second);
        ASSERT_NO_FATAL_FAILURE(QueueRemoval(second));
        EXPECT_EQ(Follow()->GetTarget(), nullptr);
        _map->DelayedUpdate(0);
        _owner->GetMotionMaster()->UpdateMotion(1);
        EXPECT_EQ(Follow(), nullptr);
    }

    TEST_F(NearestCreatureLifecycleTest, SummonedFollowerDespawnReleasesFollowWithoutRemovingLiveTarget)
    {
        auto* target = MakeTarget(12.0f);
        ASSERT_NO_FATAL_FAILURE(AssertFixturePreconditions());
        auto ownerLifetime = Life(_owner);
        AbstractFollower observer(target);
        ASSERT_NO_FATAL_FAILURE(BeginFollow(target));
        _owner->UnSummon();
        EXPECT_FALSE(_owner->IsInWorld());
        EXPECT_EQ(Follow(), nullptr);
        EXPECT_EQ(observer.GetTarget(), target);
        EXPECT_TRUE(target->IsInWorld());
        _map->DelayedUpdate(0);
        EXPECT_TRUE(ownerLifetime->Deleted);
        _owner = nullptr;
        ASSERT_NO_FATAL_FAILURE(QueueRemoval(target));
        EXPECT_EQ(observer.GetTarget(), nullptr);
    }
}

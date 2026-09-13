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

#include "AuctionHouseScript.h"
#include "AuctionHouseMgr.h"
#include "ScriptMgr.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace
{
    class DefaultAuctionScript : public AuctionHouseScript
    {
    public:
        DefaultAuctionScript() : AuctionHouseScript("auction_finalization_default_test") { }
    };

    class FinalizationScript : public AuctionHouseScript
    {
    public:
        explicit FinalizationScript(uint16 hook = AUCTIONHOUSEHOOK_ON_BEFORE_AUCTION_FINALIZATION)
            : AuctionHouseScript("auction_finalization_test", {hook}) { }

        MOCK_METHOD(void, OnBeforeAuctionFinalization,
            (AuctionEntry const*, AuctionFinalizationReason, CharacterDatabaseTransaction), (override));
    };

    class AuctionFinalizationTest : public testing::TestWithParam<AuctionFinalizationReason>
    {
    protected:
        using Registry = ScriptRegistry<AuctionHouseScript>;

        void SetUp() override
        {
            Registry::ScriptPointerList.swap(_previousScripts);
            Registry::EnabledHooks.swap(_previousHooks);
            Registry::InitEnabledHooksIfNeeded(AUCTIONHOUSEHOOK_END);
        }

        void TearDown() override
        {
            for (auto const& [id, script] : Registry::ScriptPointerList)
            {
                delete script;
                sScriptMgr->DecreaseScriptCount();
            }

            Registry::ScriptPointerList.clear();
            Registry::ScriptPointerList.swap(_previousScripts);
            Registry::EnabledHooks.swap(_previousHooks);
        }

    private:
        Registry::ScriptMap _previousScripts;
        Registry::EnabledHooksVector _previousHooks;
    };
}

TEST_F(AuctionFinalizationTest, NoListenerLeavesTransactionUnchanged)
{
    AuctionEntry auction{};
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

    sScriptMgr->OnBeforeAuctionFinalization(&auction, AuctionFinalizationReason::Sold, trans);

    EXPECT_EQ(trans->GetSize(), 0u);
}

TEST_F(AuctionFinalizationTest, ExistingListenerNeedsNoOverride)
{
    new DefaultAuctionScript();
    AuctionEntry auction{};
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

    sScriptMgr->OnBeforeAuctionFinalization(&auction, AuctionFinalizationReason::Expired, trans);

    EXPECT_EQ(trans->GetSize(), 0u);
}

TEST_F(AuctionFinalizationTest, DisabledHookIsNotCalled)
{
    auto* script = new testing::StrictMock<FinalizationScript>(AUCTIONHOUSEHOOK_ON_AUCTION_REMOVE);
    AuctionEntry auction{};
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    EXPECT_CALL(*script, OnBeforeAuctionFinalization(testing::_, testing::_, testing::_)).Times(0);

    sScriptMgr->OnBeforeAuctionFinalization(&auction, AuctionFinalizationReason::Cancelled, trans);

    EXPECT_EQ(trans->GetSize(), 0u);
}

TEST_P(AuctionFinalizationTest, DispatchesOnceWithCallerTransactionAndUnchangedReason)
{
    auto* script = new testing::StrictMock<FinalizationScript>();
    AuctionEntry auction{};
    auction.Id = 1;

    for (bool hasBidder : {false, true})
    {
        auction.bidder = hasBidder ? ObjectGuid::Create<HighGuid::Player>(2) : ObjectGuid::Empty;
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

        EXPECT_CALL(*script, OnBeforeAuctionFinalization(&auction, GetParam(), trans))
            .WillOnce([&](AuctionEntry const* entry, AuctionFinalizationReason, CharacterDatabaseTransaction supplied)
            {
                EXPECT_EQ(supplied.get(), trans.get());
                EXPECT_EQ(supplied->GetSize(), 0u);
                // A prepared-statement marker tests append ownership without opening a database.
                auto* marker = new CharacterDatabasePreparedStatement(CHAR_DEL_AUCTION, 1);
                marker->SetData(0, entry->Id);
                supplied->Append(marker);
            });

        sScriptMgr->OnBeforeAuctionFinalization(&auction, GetParam(), trans);

        EXPECT_EQ(trans->GetSize(), 1u);
        EXPECT_EQ(auction.Id, 1u);
        EXPECT_EQ(static_cast<bool>(auction.bidder), hasBidder);
    }
}

INSTANTIATE_TEST_SUITE_P(Reasons, AuctionFinalizationTest, testing::Values(
    AuctionFinalizationReason::Unknown,
    AuctionFinalizationReason::Sold,
    AuctionFinalizationReason::Expired,
    AuctionFinalizationReason::Cancelled));

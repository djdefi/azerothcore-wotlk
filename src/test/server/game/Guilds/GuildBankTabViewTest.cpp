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

#include "Guild.h"
#include "WorldMock.h"
#include "gtest/gtest.h"

namespace
{
    class BankViewGuild : public Guild
    {
    public:
        BankViewGuild()
        {
            m_id = 12;
            m_bankMoney = 123456;
            for (uint8 rankId = 0; rankId < GUILD_RANKS_MIN_COUNT; ++rankId)
                m_ranks.emplace_back(m_id, rankId, "Rank", GR_RIGHT_GCHATLISTEN, 500);

            for (uint8 tabId = 0; tabId < 3; ++tabId)
                m_bankTabs.emplace_back(m_id, tabId);
        }

        void SetTab(uint8 rankId, uint8 tabId, uint8 rights, uint32 slots)
        {
            m_ranks[rankId].SetBankTabSlotsAndRights(GuildBankRightsAndSlots(tabId, rights, slots), false);
        }

        std::vector<RankInfo> const& GetRanks() const { return m_ranks; }
    };

    class GuildBankTabViewTest : public testing::Test
    {
    protected:
        void SetUp() override
        {
            _previousWorld = std::move(sWorld);
            sWorld = std::make_unique<testing::NiceMock<WorldMock>>();
            _guild = std::make_unique<BankViewGuild>();
            _guild->SetTab(1, 0, GUILD_BANK_RIGHT_DEPOSIT_ITEM, 7);
            _guild->SetTab(2, 2, GUILD_BANK_RIGHT_UPDATE_TEXT, 9);
        }

        void TearDown() override
        {
            _guild.reset();
            sWorld = std::move(_previousWorld);
        }

        void ExpectUnchanged(std::vector<Guild::RankInfo> const& before)
        {
            auto const& after = _guild->GetRanks();
            ASSERT_EQ(after.size(), before.size());
            for (std::size_t rankId = 0; rankId < before.size(); ++rankId)
            {
                EXPECT_EQ(after[rankId].GetId(), before[rankId].GetId());
                EXPECT_EQ(after[rankId].GetName(), before[rankId].GetName());
                EXPECT_EQ(after[rankId].GetRights(), before[rankId].GetRights());
                EXPECT_EQ(after[rankId].GetBankMoneyPerDay(), before[rankId].GetBankMoneyPerDay());
                for (uint8 tabId = 0; tabId < GUILD_BANK_MAX_TABS; ++tabId)
                {
                    EXPECT_EQ(after[rankId].GetBankTabRights(tabId), before[rankId].GetBankTabRights(tabId));
                    EXPECT_EQ(after[rankId].GetBankTabSlotsPerDay(tabId), before[rankId].GetBankTabSlotsPerDay(tabId));
                }
            }
            EXPECT_EQ(_guild->GetTotalBankMoney(), 123456u);
        }

        std::unique_ptr<IWorld> _previousWorld;
        std::unique_ptr<BankViewGuild> _guild;
    };
}

TEST_F(GuildBankTabViewTest, ExistingViewPreservesEveryRightAndLimit)
{
    // Exercise every rights byte containing VIEW, including unrelated high bits.
    for (uint16 rights = GUILD_BANK_RIGHT_VIEW_TAB; rights <= 0xFF; rights += 2)
    {
        for (uint32 slots : {0u, 37u, 0x80000000u, 0xFFFFFFFFu})
        {
            _guild->SetTab(1, 2, uint8(rights), slots);
            auto before = _guild->GetRanks();

            EXPECT_TRUE(_guild->GrantBankTabView(1, 2));
            EXPECT_TRUE(_guild->GrantBankTabView(1, 2));

            ExpectUnchanged(before);
        }
    }
}

TEST_F(GuildBankTabViewTest, MissingRanksAreRejectedWithoutMutation)
{
    auto before = _guild->GetRanks();

    EXPECT_FALSE(_guild->GrantBankTabView(GUILD_RANKS_MIN_COUNT, 2));
    EXPECT_FALSE(_guild->GrantBankTabView(GUILD_RANK_NONE, 2));

    ExpectUnchanged(before);
}

TEST_F(GuildBankTabViewTest, UnpurchasedAndInvalidTabsAreRejectedWithoutMutation)
{
    _guild->SetTab(1, 3, GUILD_BANK_RIGHT_VIEW_TAB, 0);
    auto before = _guild->GetRanks();

    EXPECT_FALSE(_guild->GrantBankTabView(1, 3));
    EXPECT_FALSE(_guild->GrantBankTabView(1, GUILD_BANK_MAX_TABS));
    EXPECT_FALSE(_guild->GrantBankTabView(1, 0xFF));

    ExpectUnchanged(before);
}

TEST_F(GuildBankTabViewTest, GuildMasterWithoutViewIsNotNormalizedToFullAccess)
{
    auto before = _guild->GetRanks();

    EXPECT_FALSE(_guild->GrantBankTabView(GR_GUILDMASTER, 2));

    ExpectUnchanged(before);
}

TEST_F(GuildBankTabViewTest, GuildMasterWithViewSucceedsWithoutMutation)
{
    _guild->SetTab(GR_GUILDMASTER, 2, GUILD_BANK_RIGHT_VIEW_TAB, 0);
    auto before = _guild->GetRanks();

    EXPECT_TRUE(_guild->GrantBankTabView(GR_GUILDMASTER, 2));

    ExpectUnchanged(before);
}

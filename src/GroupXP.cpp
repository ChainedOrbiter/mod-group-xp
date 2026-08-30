/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 */

#include "Config.h"
#include "Group.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Chat.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>

#include "Formulas.h"

using namespace Acore::ChatCommands;

namespace Acore::GroupXP
{
namespace
{
    constexpr uint32 PARTY_SIZE_MIN = 2;
    constexpr uint32 PARTY_SIZE_MAX = 5;    // i.e. ignore raids & battlegrounds
    constexpr float MIN_XP_MULTIPLIER = 0.10f;
    constexpr float MAX_XP_MULTIPLIER = 10.0f;

    bool _moduleEnabled = true;
    bool _requireSameMapAndZone = true;

    // XP multipliers ordered by party size
    std::array<float, PARTY_SIZE_MAX - PARTY_SIZE_MIN + 1> _xpMultipliers = { 1.20f, 1.40f, 1.60f, 1.80f };

    bool _OWPSIntegrationEnabled = false;
    bool _owpsOverrideMultiplier = false;
    float _owpsDamageWeight = 1.0f;
    float _owpsHealingWeight = 1.0f;
    float _owpsIncomingDamageWeight = 0.5f;


    float LoadXPMultiplierConfig(std::string const& key, float defaultValue)
    {
        const float value = sConfigMgr->GetOption<float>(key, defaultValue);
        if (!std::isfinite(value))
            return defaultValue;

        return std::clamp(value, MIN_XP_MULTIPLIER, MAX_XP_MULTIPLIER);
    }

    // Load module settings after world server options are available
    void LoadConfig()
    {
        _moduleEnabled = sConfigMgr->GetOption<bool>("GroupXP.Enable",
            true);

        _requireSameMapAndZone = sConfigMgr->GetOption<bool>("GroupXP.RequireSameMapAndZone", true);

#pragma region OWPSIntegration
        // is Open World Party scaling enabled?
        const bool isOWPSenabled = sConfigMgr->GetOption<bool>("OpenWorldPartyScaling.Enable", false);

        // The OWPS integration can only be enabled if OWPS is enabled. There doesn't seem to be any definitions we can use, so let's just piggyback ride the config
        _OWPSIntegrationEnabled = isOWPSenabled && sConfigMgr->GetOption<bool>("GroupXP.OWPS.EnableIntegration", false);

        // If using open world party scaling config, load the corresponding config settings
        if (_OWPSIntegrationEnabled)
        {
            // If set to true we will disregard the non-OWPS values when calculating XP, otherwise they're additive
            _owpsOverrideMultiplier = sConfigMgr->GetOption<bool>("GroupXP.OWPS.OverrideMultiplier", false);

            // How much the OWPS multiplier affects the given XP
            _owpsDamageWeight = sConfigMgr->GetOption<float>("GroupXP.OWPS.DamageMultiplier", 1.0f);
            _owpsHealingWeight = sConfigMgr->GetOption<float>("GroupXP.OWPS.HealingMultiplier", 1.0f);
            _owpsIncomingDamageWeight = sConfigMgr->GetOption<float>("GroupXP.OWPS.IncomingDamageMultiplier", 0.5f);
        }
#pragma endregion

        // Iterate over possible party sizes to store their multipliers from config
        for (uint32 partySize = PARTY_SIZE_MIN; partySize <= PARTY_SIZE_MAX; ++partySize)
        {
            std::string const key = "GroupXP.PartySize" + std::to_string(partySize) + ".XPMultiplier";
            const float defaultValue = 1.0f + (static_cast<float>(partySize - 1) * 0.20f);
            float baseMultiplier = LoadXPMultiplierConfig(key, defaultValue);


#pragma region OWPSAdjustments
            // Open World Party Scaling adjustments
            if (_OWPSIntegrationEnabled)
            {
                // If override is enabled, start from 1.0 instead of the base XP multiplier
                if (_owpsOverrideMultiplier)
                    baseMultiplier = 1.0f;

                // Load OWPS multipliers for this party size
                std::string const prefix = "OpenWorldPartyScaling.PartySize" + std::to_string(partySize);
                float owpsDamage = sConfigMgr->GetOption<float>(prefix + ".DamageMultiplier", 1.0f);
                float owpsHealing = sConfigMgr->GetOption<float>(prefix + ".HealingMultiplier", 1.0f);
                float owpsIncomingDamage = sConfigMgr->GetOption<float>(prefix + ".IncomingDamageMultiplier", 1.0f);

                // Damage and healing are reductions: lower value = more difficulty = more XP
                float damageContribution = (1.0f - owpsDamage) * _owpsDamageWeight;
                float healingContribution = (1.0f - owpsHealing) * _owpsHealingWeight;

                // Incoming damage is an increase: higher value = more difficulty = more XP
                float incomingDamageContribution = (owpsIncomingDamage - 1.0f) * _owpsIncomingDamageWeight;

                // Apply all contributions to the base multiplier
                baseMultiplier += damageContribution + healingContribution + incomingDamageContribution;
            }
#pragma endregion

            // Save our multiplier for the iterated party size for later use in our array
            _xpMultipliers[partySize - PARTY_SIZE_MIN] = baseMultiplier;
        }
    }

    // Returns amount of eligible members when calculating XP scaling for groups.
    // Much thanks to Open World Party Scaling for the implementation
    uint32 GetEligibleMemberCount(Player* player)
    {
        // Offline or removed players cannot contribute to the local party count.
        if (!player || !player->IsInWorld())
            return 0;

        Map* map = player->GetMap();

        // XP modification applies only to actors on open-world maps. Players in dungeons, raids, arenas, and
        // battlegrounds will not have XP scaled.
        if (!map || !map->IsWorldMap())
            return 0;

        Group* group = player->GetGroup();

        // Raid groups are deliberately excluded, even when their members are in the open world.
        if (!group || group->isRaidGroup())
            return 0;

        // Recalculate the count for each event so movement, logout, and map changes take effect immediately.
        // Locality can optionally restrict online, in-world members to this map and zone.
        uint32 eligibleMembers = 0;
        for (Group::MemberSlot const& memberSlot : group->GetMemberSlots())
        {
            const Player* member = ObjectAccessor::FindPlayer(memberSlot.guid);

            // Offline or removed players cannot contribute to the local party count
            if (!member || !member->IsInWorld())
                continue;

            // If we require the same zone and the member is in a different zone, disregard them for scaling
            if (_requireSameMapAndZone && (member->GetMap() != map || member->GetZoneId() != player->GetZoneId()))
                continue;

            ++eligibleMembers;
        }

        return eligibleMembers;
    }

    float GetXPMultiplier(Player* player)
    {
        const uint32 eligibleMembers = GetEligibleMemberCount(player);
        if (eligibleMembers < PARTY_SIZE_MIN)
            return 1.0f;

        const uint32 partySize = std::min(eligibleMembers, PARTY_SIZE_MAX);
        return _xpMultipliers[partySize - PARTY_SIZE_MIN];
    }

    uint32 GetScaledAmount(const uint32 amount, Player* player)
    {
        const float multiplier = GetXPMultiplier(player);
        const double result = static_cast<double>(amount) * static_cast<double>(multiplier);
        const uint32 scaledValue = static_cast<uint32>(std::max(result, 0.0));

        // Temporary chat logging
        bool logging = true;
        if (logging)
        {
            std::ostringstream msg;
            msg << "[Group XP] received " << scaledValue << " base XP with a x" << multiplier << " multiplier;"
                << "would normally be " << amount << " XP.";

            ChatHandler(player->GetSession()).PSendSysMessage(msg.str());

            // is Open World Party scaling enabled?
            const bool isOWPSenabled = sConfigMgr->GetOption<bool>("OpenWorldPartyScaling.Enable", false);
            const bool isIntegrationEnabled = sConfigMgr->GetOption<bool>("GroupXP.OWPS.EnableIntegration", false);

            std::ostringstream msg2;
            msg2 << "[Group XP OWPS] enabled: " << _OWPSIntegrationEnabled << " - bool isOWPSenabled: " << isOWPSenabled << " - bool isIntegrationEnabled: " << isIntegrationEnabled;

            ChatHandler(player->GetSession()).PSendSysMessage(msg2.str());


            std::ostringstream msgRates;
            msgRates << "[Group XP] Rates: 2: " << _xpMultipliers[0]
            << "; 3: " << _xpMultipliers[1]
            << "; 4: " << _xpMultipliers[2]
            << "; 5: " << _xpMultipliers[3] << ".";

            ChatHandler(player->GetSession()).PSendSysMessage(msgRates.str());

        }


        // Give at least 1 XP
        if (amount > 0 && scaledValue == 0)
            return 1;

        return scaledValue;
    }
}

class GroupXPWorldScript : public WorldScript
{
public:
    GroupXPWorldScript() : WorldScript("GroupXPWorldScript",
        { WORLDHOOK_ON_AFTER_CONFIG_LOAD }) { }

    void OnAfterConfigLoad(bool) override
    {
        LOG_INFO("server.loading", "GroupXP: Loading config...");

        LoadConfig();
    }
};

class GroupXPPlayerScript : public PlayerScript
{
public:
    GroupXPPlayerScript() : PlayerScript("GroupXPPlayerScript",
        {PLAYERHOOK_ON_GIVE_EXP}) { }

    void OnPlayerGiveXP(Player* player, uint32& amount, Unit* victim, uint8 xpSource) override
    {
        // Check if module is enabled
            if (!_moduleEnabled)
                return;

            // Only intercept kill XP
            if (xpSource != XPSOURCE_KILL)
                return;

            // Only if player is in a group
            Group* group = player->GetGroup();
            if (!group)
                return;

            // Set the amount to the party based scale
            amount = GetScaledAmount(amount, player);
        }
    };


// Commands for debugging & info
class GroupXPCommandScript : public CommandScript
{
public:
    GroupXPCommandScript() : CommandScript("GroupXPCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable groupxpSubCommandTable =
        {
            { "status", HandleGroupXPStatusCommand, SEC_ADMINISTRATOR, Console::No },
            { "multipliers", HandleGroupXPMultipliersCommand, SEC_ADMINISTRATOR, Console::No },
            { "owps", HandleGroupXPOWPSCommand, SEC_ADMINISTRATOR, Console::No },
            { "all", HandleGroupXPAllCommand, SEC_ADMINISTRATOR, Console::No }
        };

        static ChatCommandTable commandTable =
        {
            { "groupxp", groupxpSubCommandTable }
        };

        return commandTable;
    }

    static bool HandleGroupXPStatusCommand(ChatHandler* handler, Optional<std::string> /*args*/)
    {
        handler->PSendSysMessage("=== Group XP Module Status ===");
        handler->PSendSysMessage("Module Enabled: {}", _moduleEnabled ? "Yes" : "No");
        handler->PSendSysMessage("Require Same Map and Zone: {}", _requireSameMapAndZone ? "Yes" : "No");
        handler->PSendSysMessage("OWPS Integration Enabled: {}", _OWPSIntegrationEnabled ? "Yes" : "No");
        return true;
    }

    static bool HandleGroupXPMultipliersCommand(ChatHandler* handler, Optional<std::string> /*args*/)
    {
        handler->PSendSysMessage("=== XP Multipliers ===");

        // Show effective multipliers (calculated)
        if (_OWPSIntegrationEnabled)
        {
            handler->PSendSysMessage("Effective Multipliers:");
        }

        for (uint32 partySize = PARTY_SIZE_MIN; partySize <= PARTY_SIZE_MAX; ++partySize)
        {
            handler->PSendSysMessage("  Party Size {}: x{:.2f}", partySize, _xpMultipliers[partySize - PARTY_SIZE_MIN]);
        }

        // Base & effective are the same multipliers if OWPS is disabled
        if (!_OWPSIntegrationEnabled)
        {
            return true;
        }

        // Show base multipliers from config
        handler->PSendSysMessage("Base Config Multipliers:");
        for (uint32 partySize = PARTY_SIZE_MIN; partySize <= PARTY_SIZE_MAX; ++partySize)
        {
            std::string const key = "GroupXP.PartySize" + std::to_string(partySize) + ".XPMultiplier";
            const float defaultValue = 1.0f + (static_cast<float>(partySize - 1) * 0.20f);
            const float baseMultiplier = sConfigMgr->GetOption<float>(key, defaultValue);
            handler->PSendSysMessage("  Party Size {}: x{:.2f}", partySize, baseMultiplier);
        }

        // Show multipliers when taking group rate / party size into consideration
        handler->PSendSysMessage("Final multipliers with party rate & size:");
        for (uint32 partySize = PARTY_SIZE_MIN; partySize <= PARTY_SIZE_MAX; ++partySize)
        {
            const float effectiveMultiplier = _xpMultipliers[partySize - PARTY_SIZE_MIN];
            const float partyRate = XP::xp_in_group_rate(partySize, false);
            const float finalMultiplier = effectiveMultiplier * partyRate / static_cast<float>(partySize);

            handler->PSendSysMessage("  Party Size {}: x{:.2f}    (Effective x{:.2f} * GroupRate {:.2f} / {})",
                partySize, finalMultiplier, effectiveMultiplier, partyRate, partySize);
        }

        return true;
    }

    static bool HandleGroupXPOWPSCommand(ChatHandler* handler, Optional<std::string> /*args*/)
    {
        handler->PSendSysMessage("=== OWPS Integration Settings ===");

        if (!_OWPSIntegrationEnabled)
        {
            handler->PSendSysMessage("OWPS Integration is disabled.");
            return true;
        }

        handler->PSendSysMessage("Override Multiplier: {}", _owpsOverrideMultiplier ? "Yes" : "No");
        handler->PSendSysMessage("Damage Weight: {:.2f}", _owpsDamageWeight);
        handler->PSendSysMessage("Healing Weight: {:.2f}", _owpsHealingWeight);
        handler->PSendSysMessage("Incoming Damage Weight: {:.2f}", _owpsIncomingDamageWeight);

        // Show effective contributions for each party size
        handler->PSendSysMessage("Effective OWPS Contributions:");
        for (uint32 partySize = PARTY_SIZE_MIN; partySize <= PARTY_SIZE_MAX; ++partySize)
        {
            std::string const prefix = "OpenWorldPartyScaling.PartySize" + std::to_string(partySize);
            const float owpsDamage = sConfigMgr->GetOption<float>(prefix + ".DamageMultiplier", 1.0f);
            const float owpsHealing = sConfigMgr->GetOption<float>(prefix + ".HealingMultiplier", 1.0f);
            const float owpsIncomingDamage = sConfigMgr->GetOption<float>(prefix + ".IncomingDamageMultiplier", 1.0f);

            const float damageContribution = (1.0f - owpsDamage) * _owpsDamageWeight;
            const float healingContribution = (1.0f - owpsHealing) * _owpsHealingWeight;
            const float incomingDamageContribution = (owpsIncomingDamage - 1.0f) * _owpsIncomingDamageWeight;
            const float totalContribution = damageContribution + healingContribution + incomingDamageContribution;

            handler->PSendSysMessage("  Party Size {}: Damage={:+.2f}, Healing={:+.2f}, IncomingDamage={:+.2f}, Total={:+.2f}",
                partySize, damageContribution, healingContribution, incomingDamageContribution, totalContribution);
        }
        return true;
    }

    static bool HandleGroupXPAllCommand(ChatHandler* handler, Optional<std::string> /*args*/)
    {
        HandleGroupXPStatusCommand(handler, Optional<std::string>());
        HandleGroupXPMultipliersCommand(handler, Optional<std::string>());
        HandleGroupXPOWPSCommand(handler, Optional<std::string>());
        return true;
    }
};
}


void AddGroupXPScripts()
{
    new Acore::GroupXP::GroupXPWorldScript();
    new Acore::GroupXP::GroupXPPlayerScript();
    new Acore::GroupXP::GroupXPCommandScript();
}

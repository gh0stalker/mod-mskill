/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 */

/*
 * mod-mskill
 *
 * Adds an "mskill" party/raid/whisper command that operates on AzerothCore
 * Playerbots (mod-playerbots). See README.md for the full command reference.
 *
 *   mskill                          -> every targeted bot reports each known
 *                                      profession and its current skill level.
 *   mskill profession learn         -> bots learn every profession-trainer spell
 *                                      they currently qualify for, never above
 *                                      the hard skill cap (300).
 *   mskill profession drop <name>   -> (whisper only) bot unlearns the named
 *                                      profession and all of its spells.
 *   mskill class learn              -> bots learn every available class-trainer
 *                                      spell for their class.
 */

#include "Chat.h"
#include "Common.h"
#include "Config.h"
#include "DBCStores.h"
#include "Group.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotMgr.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Trainer.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    // Absolute ceiling. The module will never learn a spell gated above this
    // skill value, nor raise a profession's maximum skill past it. The admin
    // may lower the effective cap via configuration, but never raise it.
    constexpr uint16 MSKILL_HARD_CAP = 300;

    // Per-tier skill maximum for primary/secondary professions (75 per step).
    constexpr uint16 MSKILL_SKILL_PER_STEP = 75;

    // Every primary and secondary profession skill line in WotLK (3.3.5a).
    constexpr std::array<uint16, 14> ProfessionSkills =
    {
        SKILL_ALCHEMY,       SKILL_BLACKSMITHING, SKILL_ENCHANTING,
        SKILL_ENGINEERING,   SKILL_HERBALISM,     SKILL_INSCRIPTION,
        SKILL_JEWELCRAFTING, SKILL_LEATHERWORKING, SKILL_MINING,
        SKILL_SKINNING,      SKILL_TAILORING,     SKILL_COOKING,
        SKILL_FIRST_AID,     SKILL_FISHING
    };

    bool IsModuleEnabled()
    {
        return sConfigMgr->GetOption<bool>("Mskill.Enable", true);
    }

    // Effective cap = min(configured value, hard cap). Cannot exceed 300.
    uint16 GetEffectiveCap()
    {
        uint32 configured = sConfigMgr->GetOption<uint32>("Mskill.MaxProfessionSkill", MSKILL_HARD_CAP);
        return static_cast<uint16>(std::min<uint32>(configured, MSKILL_HARD_CAP));
    }

    // Returns the PlayerbotAI for a player, or nullptr if the player is not a
    // mod-playerbots controlled bot.
    PlayerbotAI* GetBotAI(Player* player)
    {
        if (!player)
            return nullptr;

        return sPlayerbotsMgr.GetPlayerbotAI(player);
    }

    bool IsBot(Player* player)
    {
        return GetBotAI(player) != nullptr;
    }

    // Lower-case + trim a string (ASCII).
    std::string Normalize(std::string const& input)
    {
        std::string out = input;
        std::transform(out.begin(), out.end(), out.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        size_t first = out.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
            return "";

        size_t last = out.find_last_not_of(" \t\r\n");
        return out.substr(first, last - first + 1);
    }

    // Whitespace tokenizer.
    std::vector<std::string> Tokenize(std::string const& input)
    {
        std::vector<std::string> tokens;
        std::istringstream stream(input);
        std::string token;
        while (stream >> token)
            tokens.push_back(token);

        return tokens;
    }

    // Display name for a skill line, falling back to the numeric id.
    std::string SkillName(uint16 skillId)
    {
        if (SkillLineEntry const* entry = sSkillLineStore.LookupEntry(skillId))
            if (entry->name[LOCALE_enUS] && *entry->name[LOCALE_enUS])
                return entry->name[LOCALE_enUS];

        return "Skill #" + std::to_string(skillId);
    }

    bool IsProfessionSkill(uint16 skillId)
    {
        SkillLineEntry const* entry = sSkillLineStore.LookupEntry(skillId);
        if (!entry)
            return false;

        return entry->categoryId == SKILL_CATEGORY_PROFESSION ||
               entry->categoryId == SKILL_CATEGORY_SECONDARY;
    }

    // Resolve a user-supplied profession name (e.g. "first aid", "blacksmith")
    // to a skill id. Returns 0 when unrecognised.
    uint16 ResolveProfession(std::string const& rawName)
    {
        static std::unordered_map<std::string, uint16> const aliases =
        {
            { "alchemy", SKILL_ALCHEMY },
            { "blacksmithing", SKILL_BLACKSMITHING },
            { "blacksmith", SKILL_BLACKSMITHING },
            { "enchanting", SKILL_ENCHANTING },
            { "enchant", SKILL_ENCHANTING },
            { "engineering", SKILL_ENGINEERING },
            { "engineer", SKILL_ENGINEERING },
            { "herbalism", SKILL_HERBALISM },
            { "herb", SKILL_HERBALISM },
            { "inscription", SKILL_INSCRIPTION },
            { "scribe", SKILL_INSCRIPTION },
            { "jewelcrafting", SKILL_JEWELCRAFTING },
            { "jewelcraft", SKILL_JEWELCRAFTING },
            { "jc", SKILL_JEWELCRAFTING },
            { "leatherworking", SKILL_LEATHERWORKING },
            { "leatherwork", SKILL_LEATHERWORKING },
            { "lw", SKILL_LEATHERWORKING },
            { "mining", SKILL_MINING },
            { "skinning", SKILL_SKINNING },
            { "tailoring", SKILL_TAILORING },
            { "tailor", SKILL_TAILORING },
            { "cooking", SKILL_COOKING },
            { "cook", SKILL_COOKING },
            { "firstaid", SKILL_FIRST_AID },
            { "first_aid", SKILL_FIRST_AID },
            { "fishing", SKILL_FISHING }
        };

        // Collapse internal whitespace so "first aid" -> "firstaid".
        std::string key;
        for (char c : Normalize(rawName))
            if (!std::isspace(static_cast<unsigned char>(c)))
                key.push_back(c);

        auto itr = aliases.find(key);
        return itr != aliases.end() ? itr->second : 0;
    }

    // Cached trainer ids, partitioned by trainer type, built lazily once.
    struct TrainerCache
    {
        bool built = false;
        std::vector<uint32> classTrainers;
        std::vector<uint32> tradeTrainers;
    };

    TrainerCache s_trainerCache;

    void BuildTrainerCache()
    {
        if (s_trainerCache.built)
            return;

        CreatureTemplateContainer const* templates = sObjectMgr->GetCreatureTemplates();
        for (auto const& pair : *templates)
        {
            Trainer::Trainer* trainer = sObjectMgr->GetTrainer(pair.first);
            if (!trainer)
                continue;

            switch (trainer->GetTrainerType())
            {
                case Trainer::Type::Class:
                    s_trainerCache.classTrainers.push_back(pair.first);
                    break;
                case Trainer::Type::Tradeskill:
                    s_trainerCache.tradeTrainers.push_back(pair.first);
                    break;
                default:
                    break;
            }
        }

        s_trainerCache.built = true;
    }

    // True when learning this spell would raise the given profession's maximum
    // skill above the cap (i.e. it is an Apprentice/.../Grand Master tier-up
    // spell beyond the allowed tier). Recipes never match this.
    bool TierWouldExceedCap(uint32 spellId, uint16 skillId, uint16 cap)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (!info)
            return false;

        for (SpellEffectInfo const& effect : info->GetEffects())
        {
            if (effect.Effect != SPELL_EFFECT_SKILL)
                continue;

            if (static_cast<uint16>(effect.MiscValue) != skillId)
                continue;

            // The effect value is the skill step (tier index). Each step grants
            // 75 max skill. If that maximum would exceed the cap, refuse it.
            int32 step = effect.CalcValue();
            if (step > 0 && static_cast<uint16>(step * MSKILL_SKILL_PER_STEP) > cap)
                return true;
        }

        return false;
    }

    void TeachTrainerSpell(Player* bot, Trainer::Spell const* trainerSpell)
    {
        if (trainerSpell->IsCastable())
            bot->CastSpell(bot, trainerSpell->SpellId, true);
        else
            bot->learnSpell(trainerSpell->SpellId, false);
    }
} // namespace

class MskillController
{
public:
    // Apply the parsed command to a single bot. Feedback is whispered back to
    // the commanding player, prefixed with the bot's name.
    static void Apply(Player* owner, Player* bot, std::vector<std::string> const& args, bool isWhisper)
    {
        if (!owner || !bot)
            return;

        // args[0] is always "mskill" (already verified by the caller).
        if (args.size() == 1)
        {
            ReportProfessions(owner, bot);
            return;
        }

        std::string const& sub = args[1];

        if (sub == "profession" && args.size() >= 3 && args[2] == "learn")
        {
            LearnProfessions(owner, bot);
            return;
        }

        if (sub == "profession" && args.size() >= 3 && args[2] == "drop")
        {
            if (!isWhisper)
            {
                Tell(owner, bot, "dropping a profession must be whispered: "
                                 "/w <bot> mskill profession drop <name>");
                return;
            }

            if (args.size() < 4)
            {
                Tell(owner, bot, "usage: mskill profession drop <profession name>");
                return;
            }

            // Allow multi-word names such as "first aid".
            std::string name;
            for (size_t i = 3; i < args.size(); ++i)
                name += (i == 3 ? "" : " ") + args[i];

            DropProfession(owner, bot, name);
            return;
        }

        if (sub == "class" && args.size() >= 3 && args[2] == "learn")
        {
            LearnClassSpells(owner, bot);
            return;
        }

        Tell(owner, bot, "unknown command. Try: mskill | mskill profession learn "
                         "| mskill profession drop <name> | mskill class learn");
    }

private:
    // Whisper a single line of feedback to the owner, attributed to the bot.
    static void Tell(Player* owner, Player* bot, std::string const& message)
    {
        ChatHandler(owner->GetSession()).PSendSysMessage("|cff00ff00[{}]|r {}",
            bot->GetName(), message);
    }

    static void ReportProfessions(Player* owner, Player* bot)
    {
        bool any = false;
        for (uint16 skillId : ProfessionSkills)
        {
            if (!bot->HasSkill(skillId))
                continue;

            any = true;
            std::ostringstream out;
            out << SkillName(skillId) << ": "
                << bot->GetPureSkillValue(skillId) << "/"
                << bot->GetPureMaxSkillValue(skillId);
            Tell(owner, bot, out.str());
        }

        if (!any)
            Tell(owner, bot, "I have no professions.");
    }

    static void LearnProfessions(Player* owner, Player* bot)
    {
        BuildTrainerCache();

        uint16 const cap = GetEffectiveCap();
        uint32 learned = 0;

        for (uint32 trainerId : s_trainerCache.tradeTrainers)
        {
            Trainer::Trainer* trainer = sObjectMgr->GetTrainer(trainerId);
            if (!trainer)
                continue;

            for (Trainer::Spell const& spell : trainer->GetSpells())
            {
                Trainer::Spell const* trainerSpell = trainer->GetSpell(spell.SpellId);
                if (!trainerSpell)
                    continue;

                // Determine which profession this spell belongs to. Only act on
                // professions the bot already practices, so we never grant a
                // brand-new profession (unlike the Playerbots maintenance bug).
                uint16 skillId = static_cast<uint16>(trainerSpell->ReqSkillLine);
                if (!skillId || !IsProfessionSkill(skillId) || !bot->HasSkill(skillId))
                    continue;

                // Never learn anything gated above the cap, and never learn a
                // tier-up that would raise the profession past the cap.
                if (trainerSpell->ReqSkillRank > cap)
                    continue;

                if (TierWouldExceedCap(trainerSpell->SpellId, skillId, cap))
                    continue;

                // CanTeachSpell enforces "already known", class/race fit, level,
                // prerequisite ranks, and that the bot's current skill value is
                // high enough (i.e. up to the bot's current profession level).
                if (!trainer->CanTeachSpell(bot, trainerSpell))
                    continue;

                TeachTrainerSpell(bot, trainerSpell);
                ++learned;
            }
        }

        // Belt-and-suspenders: clamp every known profession's maximum to the cap
        // so nothing can be skilled above it later.
        for (uint16 skillId : ProfessionSkills)
        {
            if (!bot->HasSkill(skillId))
                continue;

            if (bot->GetPureMaxSkillValue(skillId) > cap)
            {
                uint16 value = std::min<uint16>(bot->GetPureSkillValue(skillId), cap);
                bot->SetSkill(skillId, bot->GetSkillStep(skillId), value, cap);
            }
        }

        std::ostringstream out;
        out << "learned " << learned << " profession spell(s) (cap " << cap << ").";
        Tell(owner, bot, out.str());
    }

    static void LearnClassSpells(Player* owner, Player* bot)
    {
        BuildTrainerCache();

        uint32 learned = 0;
        for (uint32 trainerId : s_trainerCache.classTrainers)
        {
            Trainer::Trainer* trainer = sObjectMgr->GetTrainer(trainerId);
            if (!trainer)
                continue;

            // For class trainers the requirement encodes the class, so this
            // keeps a warrior from learning mage spells, etc.
            if (!trainer->IsTrainerValidForPlayer(bot))
                continue;

            for (Trainer::Spell const& spell : trainer->GetSpells())
            {
                Trainer::Spell const* trainerSpell = trainer->GetSpell(spell.SpellId);
                if (!trainerSpell)
                    continue;

                if (!trainer->CanTeachSpell(bot, trainerSpell))
                    continue;

                TeachTrainerSpell(bot, trainerSpell);
                ++learned;
            }
        }

        std::ostringstream out;
        out << "learned " << learned << " class spell(s).";
        Tell(owner, bot, out.str());
    }

    static void DropProfession(Player* owner, Player* bot, std::string const& name)
    {
        uint16 skillId = ResolveProfession(name);
        if (!skillId)
        {
            Tell(owner, bot, "I don't recognise the profession '" + name + "'.");
            return;
        }

        if (!bot->HasSkill(skillId))
        {
            Tell(owner, bot, "I don't know " + SkillName(skillId) + ".");
            return;
        }

        // Remove every spell tied to this skill line, then remove the skill.
        for (uint32 i = 0; i < sSkillLineAbilityStore.GetNumRows(); ++i)
        {
            SkillLineAbilityEntry const* ability = sSkillLineAbilityStore.LookupEntry(i);
            if (!ability || ability->SkillLine != skillId)
                continue;

            if (bot->HasSpell(ability->Spell))
                bot->removeSpell(ability->Spell, SPEC_MASK_ALL, false);
        }

        bot->SetSkill(skillId, 0, 0, 0);
        Tell(owner, bot, "dropped " + SkillName(skillId) + ".");
    }
};

// Shared entry point used by both the whisper and the group chat hooks.
static void HandleMskill(Player* owner, Player* bot, std::string const& msg, bool isWhisper)
{
    if (!IsModuleEnabled())
        return;

    // Only real players may issue commands; ignore bot-to-bot chatter.
    if (!owner || IsBot(owner))
        return;

    // Must be a controlled playerbot.
    if (!bot || !IsBot(bot))
        return;

    std::vector<std::string> args = Tokenize(Normalize(msg));
    if (args.empty() || args[0] != "mskill")
        return;

    MskillController::Apply(owner, bot, args, isWhisper);
}

class MskillPlayerScript : public PlayerScript
{
public:
    MskillPlayerScript() : PlayerScript("MskillPlayerScript", {
        PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT,
        PLAYERHOOK_CAN_PLAYER_USE_GROUP_CHAT
    }) { }

    // Whisper: "/w <bot> mskill ..." -> the single whispered bot.
    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 /*lang*/, std::string& msg, Player* receiver) override
    {
        if (type == CHAT_MSG_WHISPER)
            HandleMskill(player, receiver, msg, true);

        // Never block the underlying chat message.
        return true;
    }

    // Party/raid: "mskill ..." -> every bot in the sender's group.
    bool OnPlayerCanUseChat(Player* player, uint32 /*type*/, uint32 /*lang*/, std::string& msg, Group* group) override
    {
        if (!group)
            return true;

        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* member = itr->GetSource();
            if (!member || member == player)
                continue;

            HandleMskill(player, member, msg, false);
        }

        return true;
    }
};

void AddMskillScripts()
{
    new MskillPlayerScript();
}

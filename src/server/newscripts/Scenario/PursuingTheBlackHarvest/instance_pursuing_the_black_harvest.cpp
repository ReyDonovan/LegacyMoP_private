/*
 * Copyright (C) 2008-2012 TrinityCore <http://www.trinitycore.org/>
 * Copyright (C) 2006-2009 ScriptDev2 <https://scriptdev2.svn.sourceforge.net/>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "ScriptMgr.h"
#include "ScriptedCreature.h"
#include "pursuing_the_black_harvest.h"

class instance_pursuing_the_black_harvest : public InstanceMapScript
{
public:
    instance_pursuing_the_black_harvest() : InstanceMapScript("instance_pursuing_the_black_harvest", 1112) { }

    InstanceScript* GetInstanceScript(InstanceMap* map) const
    {
        return new instance_pursuing_the_black_harvest_InstanceMapScript(map);
    }

    struct instance_pursuing_the_black_harvest_InstanceMapScript : public InstanceScript
    {
        instance_pursuing_the_black_harvest_InstanceMapScript(Map* map) : InstanceScript(map)
        { }

        void Initialize()
        {
            essenceData = 0;
            nodelData = 0;
            kanrethadData = 0;
            sceneEventData = 0;
            plunderData = 0;
            stage2Data = 0;

            akamaGUID = 0;
            jubekaGUID = 0;
            doorGUID = 0;
            secondDoorGUID = 0;
            trashP2GUIDs.clear();
            trapGUIDs.clear();
            soulGUIDs.clear();
            treasuresGUIDs.clear();
        }

        void OnPlayerEnter(Player* player)
        {
            std::set<uint32> phaseIds;
            std::set<uint32> terrainswaps;
            std::set<uint32> WorldMapAreaIds;
            WorldMapAreaIds.insert(992);
            WorldMapAreaIds.insert(683);
            phaseIds.insert(1982);
            player->GetSession()->SendSetPhaseShift(phaseIds, terrainswaps, WorldMapAreaIds, 16);

            player->CastSpell(player, SPELL_ENTER_THE_BLACK_TEMPLE);
            player->CastSpell(player, SPELL_CSA_AT_DUMMY_TIMED_AURA);
            player->CastSpell(player, SPELL_WHAT_THE_DRAENEI_FOUND_INTRO);
        }

        void OnCreatureCreate(Creature* creature)
        {
            switch (creature->GetEntry())
            {
                case NPC_ESSENCE_OF_ORDER:
                    creature->RemoveFlag(OBJECT_FIELD_DYNAMIC_FLAGS, UNIT_DYNFLAG_DEAD);
                    creature->SetVisible(false);
                    break;
                case NPC_AKAMA:
                    creature->RemoveFlag(OBJECT_FIELD_DYNAMIC_FLAGS, UNIT_DYNFLAG_DEAD);
                    creature->SetVisible(false);
                    akamaGUID = creature->GetGUID();
                    break;
                case NPC_JUBEKA_SHADOWBREAKER:
                    creature->RemoveFlag(OBJECT_FIELD_DYNAMIC_FLAGS, UNIT_DYNFLAG_DEAD);
                    creature->SetVisible(false);
                    jubekaGUID = creature->GetGUID();
                    break;
                case NPC_UNBOUND_NIGHTLORD:
                case NPC_UNBOUND_CENTURION:
                case NPC_UNBOUND_BONEMENDER:
                case NPC_PORTALS_VISUAL:
                case NPC_FREED_IMP:
                case NPC_UNBOUND_SUCCUB:
                    creature->RemoveFlag(OBJECT_FIELD_DYNAMIC_FLAGS, UNIT_DYNFLAG_DEAD);
                    creature->SetVisible(false);
                    creature->SetReactState(REACT_PASSIVE);
                    trashP2GUIDs.push_back(creature->GetGUID());
                    break;
                case NPC_SUFFERING_SOUL_FRAGMENT:
                    creature->RemoveFlag(OBJECT_FIELD_DYNAMIC_FLAGS, UNIT_DYNFLAG_DEAD);
                    soulGUIDs.push_back(creature->GetGUID());
                    break;
                case NPC_ASHTONGUE_SHAMAN:
                    creature->RemoveFlag(OBJECT_FIELD_DYNAMIC_FLAGS, UNIT_DYNFLAG_DEAD);
                    break;
                case NPC_LOST_SOULS:
                    creature->SetReactState(REACT_PASSIVE);
                    break;
                default:
                    break;
            }
        }

        void OnGameObjectCreate(GameObject* go)
        {
            switch (go->GetEntry())
            {
                case GO_MAIN_TEMPLATE_DOORS:
                    doorGUID = go->GetGUID();
                    break;
                case GO_SECOND_DOOR:
                    secondDoorGUID = go->GetGUID();
                    break;
                case GO_TRAP:
                    trapGUIDs.push_back(go->GetGUID());
                    break;
                case GO_TREASURE_CHEST:
                case GO_GOLDEN_HIGH_ELF_STATUETTE:
                case GO_GOLD_PLATTER:
                case GO_SHINY_YARN:
                case GO_GORGEOUS_GEM:
                case GO_GOLD_FRUIT_BOWL:
                case GO_DUSTY_PAINTING:
                case GO_FLUFFY_PILLOW:
                case GO_ANCIENT_ORCISH_SHIELD:
                case GO_RUSTED_SWORD:
                case GO_FRAGRANT_PERFUME:
                case GO_COLOGNE:
                case GO_EXPENSIVE_RUBY:
                case GO_SPARKLING_SAPPHIRE:
                case GO_JADE_KITTEN:
                case GO_RUBY_NECKLACE:
                case GO_SPELLSTONE_NECKLACE:
                case GO_DIAMOND_RING:
                case GO_RUBY_RING:
                case GO_GOLD_RING:
                case GO_SMALL_PILE_OF_COINS:
                case GO_LARGE_PILE_OF_COINS:
                case GO_GOLDEN_GOBLET:
                    go->SetVisible(false);
                    treasuresGUIDs.push_back(go->GetGUID());
                    break;
                default:
                    break;
            }
        }

        void SetData(uint32 type, uint32 data)
        {
            switch (type)
            {
                case DATA_ESSENCE_OF_ORDER_EVENT:
                    essenceData = data;
                    if (data == DONE)
                    {
                        if (Creature* akama = instance->GetCreature(akamaGUID))
                            akama->AI()->DoAction(ACTION_3);

                        for (std::vector<uint64>::const_iterator itr = trashP2GUIDs.begin(); itr != trashP2GUIDs.end(); itr++)
                            if (Creature* trash = instance->GetCreature(*itr))
                            {
                                trash->SetVisible(true);
                                trash->SetReactState(REACT_AGGRESSIVE);
                            }

                        for (std::vector<uint64>::const_iterator itr = soulGUIDs.begin(); itr != soulGUIDs.end(); itr++)
                            if (Creature* soul = instance->GetCreature(*itr))
                            {
                                soul->RemoveFromWorld();
                                soul->SetFlag(OBJECT_FIELD_DYNAMIC_FLAGS, UNIT_DYNFLAG_DEAD);
                            }

                        for (std::vector<uint64>::const_iterator itr = trapGUIDs.begin(); itr != trapGUIDs.end(); itr++)
                            if (GameObject* trap = instance->GetGameObject(*itr))
                            {
                                trap->SetVisible(false);
                                trap->RemoveFromWorld();
                            }
                    }
                    break;
                case DATA_AKAMA:
                    nodelData = data;
                    break;
                case DATA_KANRETHAD:
                    kanrethadData = data;
                    if (data == DONE)
                        if (Creature* jubeka = instance->GetCreature(jubekaGUID))
                            jubeka->AI()->DoAction(ACTION_1);
                    break;
                case DATA_SCENE_EVENT:
                    sceneEventData = data;
                    break;
                case DATA_NOBEL_EVENT:
                    nodelData = data;
                    break;
                case DATA_PLUNDER_EVENT:
                    plunderData = data;
                    if (data == DONE)
                    {
                        for (std::vector<uint64>::const_iterator itr = treasuresGUIDs.begin(); itr != treasuresGUIDs.end(); itr++)
                            if (GameObject* trap = instance->GetGameObject(*itr))
                                trap->SetVisible(true);
                    }
                    break;
                case DATA_STAGE_2:
                    stage2Data = data;
                    break;
                default:
                    break;
            }
        }

        uint64 GetData64(uint32 type)
        {
            switch (type)
            {
                case DATA_AKAMA:
                    return akamaGUID;
                case DATA_MAIN_DOORS:
                    return doorGUID;
                case DATA_SECOND_DOOR:
                    return secondDoorGUID;
                default:
                    return 0;
            }
        }

        uint32 GetData(uint32 type)
        {
            switch (type)
            {
                case DATA_ESSENCE_OF_ORDER_EVENT:
                    return essenceData;
                case DATA_NOBEL_EVENT:
                    return nodelData;
                case DATA_SCENE_EVENT:
                    return sceneEventData;
                case DATA_PLUNDER_EVENT:
                    return plunderData;
                case DATA_STAGE_2:
                    return stage2Data;
                default:
                    return 0;
            }
        }

    private:
        uint32 essenceData;
        uint32 nodelData;
        uint32 kanrethadData;
        uint32 sceneEventData;
        uint32 plunderData;
        uint32 stage2Data;

        uint64 akamaGUID;
        uint64 jubekaGUID;
        uint64 doorGUID;
        uint64 secondDoorGUID;
        std::vector<uint64> trashP2GUIDs;
        std::vector<uint64> trapGUIDs;
        std::vector<uint64> soulGUIDs;
        std::vector<uint64> treasuresGUIDs;
    };
};

void AddSC_instance_pursuing_the_black_harvest()
{
    new instance_pursuing_the_black_harvest();
}
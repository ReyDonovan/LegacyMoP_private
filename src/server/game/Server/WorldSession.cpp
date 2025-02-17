/*
 * Copyright (C) 2008-2012 TrinityCore <http://www.trinitycore.org/>
 * Copyright (C) 2005-2009 MaNGOS <http://getmangos.com/>
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

/** \file
    \ingroup u2w
*/

#include "WorldSocket.h"                                    // must be first to make ACE happy with ACE includes in it
#include <zlib.h>
#include "Common.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "Opcodes.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "Player.h"
#include "Vehicle.h"
#include "ObjectMgr.h"
#include "GuildMgr.h"
#include "Group.h"
#include "Guild.h"
#include "World.h"
#include "ObjectAccessor.h"
#include "BattlegroundMgr.h"
#include "OutdoorPvPMgr.h"
#include "MapManager.h"
#include "SocialMgr.h"
#include "zlib.h"
#include "ScriptMgr.h"
#include "Transport.h"
#include "WardenWin.h"
#include "WardenMac.h"

bool MapSessionFilter::Process(WorldPacket* packet)
{
    Opcodes opcode = DropHighBytes(packet->GetOpcode());
    OpcodeHandler const* opHandle = opcodeTable[CMSG][opcode];

    //let's check if our opcode can be really processed in Map::Update()
    if (opHandle->packetProcessing == PROCESS_INPLACE)
        return true;

    //we do not process thread-unsafe packets
    if (opHandle->packetProcessing == PROCESS_THREADUNSAFE)
        return false;

    Player* player = m_pSession->GetPlayer();
    if (!player)
        return false;

    //in Map::Update() we do not process packets where player is not in world!
    return player->IsInWorld();
}

//we should process ALL packets when player is not in world/logged in
//OR packet handler is not thread-safe!
bool WorldSessionFilter::Process(WorldPacket* packet)
{
    Opcodes opcode = DropHighBytes(packet->GetOpcode());
    OpcodeHandler const* opHandle = opcodeTable[CMSG][opcode];
    //check if packet handler is supposed to be safe
    if (opHandle->packetProcessing == PROCESS_INPLACE)
        return true;

    //thread-unsafe packets should be processed in World::UpdateSessions()
    if (opHandle->packetProcessing == PROCESS_THREADUNSAFE)
        return true;

    //no player attached? -> our client! ^^
    Player* player = m_pSession->GetPlayer();
    if (!player)
        return true;

    //lets process all packets for non-in-the-world player
    return (player->IsInWorld() == false);
}

/// WorldSession constructor
WorldSession::WorldSession(uint32 id, std::string account_name, WorldSocket* sock, AccountTypes sec, uint8 expansion, time_t mute_time, LocaleConstant locale, uint32 recruiter, bool isARecruiter) :
m_muteTime(mute_time), m_timeOutTime(0), _player(NULL), m_Socket(sock),
_security(sec), _accountId(id), m_expansion(expansion), _logoutTime(0),
m_inQueue(false), m_playerLoading(false), m_playerLogout(false),
m_playerRecentlyLogout(false), m_playerSave(false),
m_sessionDbcLocale(sWorld->GetAvailableDbcLocale(locale)),
m_sessionDbLocaleIndex(locale), _clientOS("Unk"),
m_latency(0), m_TutorialsChanged(false), recruiterId(recruiter),
isRecruiter(isARecruiter), _account_name(account_name), timeCharEnumOpcode(0), playerLoginCounter(0), wardenModuleFailed(false)
{
    _warden = NULL;
    _filterAddonMessages = false;

    if (sock)
    {
        m_Address = sock->GetRemoteAddress();
        sock->AddReference();
        ResetTimeOutTime();
        LoginDatabase.PExecute("UPDATE account SET online = 1 WHERE id = %u;", GetAccountId());     // One-time query
    }

    InitializeQueryCallbackParameters();

    for (uint32 i = 0; i < PACKETS_COUNT; ++i)
    {
        antispamm[i][0] = 0;
        antispamm[i][1] = 0;
    }

    _compressionStream = new z_stream();
    _compressionStream->zalloc = (alloc_func)NULL;
    _compressionStream->zfree = (free_func)NULL;
    _compressionStream->opaque = (voidpf)NULL;
    _compressionStream->avail_in = 0;
    _compressionStream->next_in = NULL;
    int32 z_res = deflateInit2(_compressionStream, sWorld->getIntConfig(CONFIG_COMPRESSION), Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
    if (z_res != Z_OK)
    {
        TC_LOG_ERROR("network", "Can't initialize packet compression (zlib: deflateInit) Error code: %i (%s)", z_res, zError(z_res));
        return;
    }
}

/// WorldSession destructor
WorldSession::~WorldSession()
{
    ///- unload player if not unloaded
    if (_player)
        LogoutPlayer (true);

    /// - If have unclosed socket, close it
    if (m_Socket)
    {
        m_Socket->CloseSocket();
        m_Socket->RemoveReference();
        m_Socket = NULL;
    }

    if (_warden)
        delete _warden;

    ///- empty incoming packet queue
    WorldPacket* packet = NULL;
    while (_recvQueue.next(packet))
        delete packet;

    LoginDatabase.PExecute("UPDATE account SET online = 0 WHERE id = %u;", GetAccountId());     // One-time query

    int32 z_res = deflateEnd(_compressionStream);
    if (z_res != Z_OK && z_res != Z_DATA_ERROR) // Z_DATA_ERROR signals that internal state was BUSY
        TC_LOG_ERROR("network", "Can't close packet compression stream (zlib: deflateEnd) Error code: %i (%s)", z_res, zError(z_res));

    delete _compressionStream;
}

/// Get the player name
std::string WorldSession::GetPlayerName(bool simple /* = true */) const
 {
    std::string name = "[Player: ";
    uint32 guidLow = 0;

    if (Player* player = GetPlayer())
    {
        name.append(player->GetName());
        guidLow = player->GetGUIDLow();
    }
    else
        name.append("<none>");

    if (!simple)
    {
        std::ostringstream ss;
        ss << " (Guid: " << guidLow << ", Account: " << GetAccountId() << ")";
        name.append(ss.str());
    }

    name.append("]");
    return name;
}

/// Get player guid if available. Use for logging purposes only
uint32 WorldSession::GetGuidLow() const
{
    return GetPlayer() ? GetPlayer()->GetGUIDLow() : 0;
}

/// Send a packet to the client
void WorldSession::SendPacket(WorldPacket const* packet, bool forced /*= false*/)
{
    if (!m_Socket)
        return;

    if (packet->GetOpcode() == NULL_OPCODE)
    {
        TC_LOG_ERROR("opcode", "Prevented sending of NULL_OPCODE to %s", GetPlayerName(false).c_str());
        return;
    }
    else if (packet->GetOpcode() == UNKNOWN_OPCODE)
    {
        TC_LOG_ERROR("opcode", "Prevented sending of UNKNOWN_OPCODE to %s", GetPlayerName(false).c_str());
        return;
    }

    if (!forced)
    {
        OpcodeHandler* handler = opcodeTable[SMSG][packet->GetOpcode()];
        if (!handler || handler->status == STATUS_UNHANDLED)
        {
            #ifdef WIN32
            TC_LOG_ERROR("opcode", "Prevented sending disabled opcode %s to %s", GetOpcodeNameForLogging(packet->GetOpcode(), SMSG).c_str(), GetPlayerName(false).c_str());
            #endif
            return;
        }
    }

    const_cast<WorldPacket*>(packet)->FlushBits();


#ifdef TRINITY_DEBUG
    // Code for network use statistic
    static uint64 sendPacketCount = 0;
    static uint64 sendPacketBytes = 0;

    static time_t firstTime = time(NULL);
    static time_t lastTime = firstTime;                     // next 60 secs start time

    static uint64 sendLastPacketCount = 0;
    static uint64 sendLastPacketBytes = 0;

    time_t cur_time = time(NULL);

    if ((cur_time - lastTime) < 60)
    {
        sendPacketCount+=1;
        sendPacketBytes+=packet->size();

        sendLastPacketCount+=1;
        sendLastPacketBytes+=packet->size();
    }
    else
    {
        uint64 minTime = uint64(cur_time - lastTime);
        uint64 fullTime = uint64(lastTime - firstTime);
        TC_LOG_DEBUG("server", "Send all time packets count: " UI64FMTD " bytes: " UI64FMTD " avr.count/sec: %f avr.bytes/sec: %f time: %u", sendPacketCount, sendPacketBytes, float(sendPacketCount)/fullTime, float(sendPacketBytes)/fullTime, uint32(fullTime));
        TC_LOG_DEBUG("server", "Send last min packets count: " UI64FMTD " bytes: " UI64FMTD " avr.count/sec: %f avr.bytes/sec: %f", sendLastPacketCount, sendLastPacketBytes, float(sendLastPacketCount)/minTime, float(sendLastPacketBytes)/minTime);

        lastTime = cur_time;
        sendLastPacketCount = 1;
        sendLastPacketBytes = packet->wpos();               // wpos is real written size
    }
#endif                                                      // !TRINITY_DEBUG

    if (m_Socket->SendPacket(packet) == -1)
        m_Socket->CloseSocket();
}

uint32 WorldSession::CompressPacket(uint8* buffer, WorldPacket const& packet)
{
    uint32 opcode = packet.GetOpcode();
    uint32 bufferSize = deflateBound(_compressionStream, packet.size() + sizeof(opcode));

    _compressionStream->next_out = buffer;
    _compressionStream->avail_out = bufferSize;
    _compressionStream->next_in = (Bytef*)&opcode;
    _compressionStream->avail_in = sizeof(uint32);

    int32 z_res = deflate(_compressionStream, Z_BLOCK);
    if (z_res != Z_OK)
    {
        TC_LOG_ERROR("opcode", "Can't compress packet opcode (zlib: deflate) Error code: %i (%s, msg: %s)", z_res, zError(z_res), _compressionStream->msg);
        return 0;
    }

    _compressionStream->next_in = (Bytef*)packet.contents();
    _compressionStream->avail_in = packet.size();

    z_res = deflate(_compressionStream, Z_SYNC_FLUSH);
    if (z_res != Z_OK)
    {
        TC_LOG_ERROR("opcode", "Can't compress packet data (zlib: deflate) Error code: %i (%s, msg: %s)", z_res, zError(z_res), _compressionStream->msg);
        return 0;
    }


    return bufferSize - _compressionStream->avail_out;
}

/// Add an incoming packet to the queue
void WorldSession::QueuePacket(WorldPacket* new_packet, bool& deletePacket)
{
    if(sWorld->GetAntiSpamm(new_packet->GetOpcode(), 0) == 0 || sWorld->GetAntiSpamm(new_packet->GetOpcode(), 1) == 0)
        _recvQueue.add(new_packet);
    else if(sWorld->GetAntiSpamm(new_packet->GetOpcode(), 0) > antispamm[new_packet->GetOpcode()][0])
    {
        if(antispamm[new_packet->GetOpcode()][1] == 0 || ((time(NULL) - antispamm[new_packet->GetOpcode()][1]) > sWorld->GetAntiSpamm(new_packet->GetOpcode(), 1)))
        {
            antispamm[new_packet->GetOpcode()][0] = 0;
            antispamm[new_packet->GetOpcode()][1] = time(NULL);
        }

        antispamm[new_packet->GetOpcode()][0]++;
        _recvQueue.add(new_packet);
    }
    else
    {
        if((time(NULL) - antispamm[new_packet->GetOpcode()][1]) > sWorld->GetAntiSpamm(new_packet->GetOpcode(), 1))
        {
            antispamm[new_packet->GetOpcode()][0] = 1;
            antispamm[new_packet->GetOpcode()][1] = time(NULL);
            _recvQueue.add(new_packet);
        }
        else
        {
            deletePacket = true;
            new_packet->rfinish();
        }
    }
}

/// Logging helper for unexpected opcodes
void WorldSession::LogUnexpectedOpcode(WorldPacket* packet, const char* status, const char *reason)
{
    #ifdef WIN32
    TC_LOG_ERROR("opcode", "Received unexpected opcode %s Status: %s Reason: %s from %s",
        GetOpcodeNameForLogging(packet->GetOpcode()).c_str(), status, reason, GetPlayerName(false).c_str());
    #endif
}

/// Logging helper for unexpected opcodes
void WorldSession::LogUnprocessedTail(WorldPacket* packet)
{
    #ifdef WIN32
    TC_LOG_ERROR("opcode", "Unprocessed tail data (read stop at %u from %u) Opcode %s from %s",
        uint32(packet->rpos()), uint32(packet->wpos()), GetOpcodeNameForLogging(packet->GetOpcode()).c_str(), GetPlayerName(false).c_str());
    #endif
    packet->print_storage();
}

/// Update the WorldSession (triggered by World update)
bool WorldSession::Update(uint32 diff, PacketFilter& updater)
{
    /// Update Timeout timer.
    UpdateTimeOutTime(diff);

    ///- Before we process anything:
    /// If necessary, kick the player from the character select screen
    if (IsConnectionIdle())
        m_Socket->CloseSocket();

    ///- Retrieve packets from the receive queue and call the appropriate handlers
    /// not process packets if socket already closed
    WorldPacket* packet = NULL;
    //! Delete packet after processing by default
    bool deletePacket = true;
    //! To prevent infinite loop
    WorldPacket* firstDelayedPacket = NULL;
    //! If _recvQueue.peek() == firstDelayedPacket it means that in this Update call, we've processed all
    //! *properly timed* packets, and we're now at the part of the queue where we find
    //! delayed packets that were re-enqueued due to improper timing. To prevent an infinite
    //! loop caused by re-enqueueing the same packets over and over again, we stop updating this session
    //! and continue updating others. The re-enqueued packets will be handled in the next Update call for this session.

    while (m_Socket && !m_Socket->IsClosed() &&
            !_recvQueue.empty() && _recvQueue.peek(true) != firstDelayedPacket &&
            _recvQueue.next(packet, updater))
    {
        const OpcodeHandler* opHandle = opcodeTable[CMSG][packet->GetOpcode()];

        try
        {
            switch (opHandle->status)
            {
                case STATUS_LOGGEDIN:
                    if (!_player)
                    {
                        // skip STATUS_LOGGEDIN opcode unexpected errors if player logout sometime ago - this can be network lag delayed packets
                        //! If player didn't log out a while ago, it means packets are being sent while the server does not recognize
                        //! the client to be in world yet. We will re-add the packets to the bottom of the queue and process them later.
                        if (!m_playerRecentlyLogout)
                        {
                            //! Prevent infinite loop
                            if (!firstDelayedPacket)
                                firstDelayedPacket = packet;
                            //! Because checking a bool is faster than reallocating memory
                            deletePacket = false;
                            QueuePacket(packet, deletePacket);
                            //! Log
                            #ifdef WIN32
                                TC_LOG_DEBUG("network", "Re-enqueueing packet with opcode %s with with status STATUS_LOGGEDIN. "
                                    "Player is currently not in world yet.", GetOpcodeNameForLogging(packet->GetOpcode(), CMSG).c_str());
                            #endif
                        }
                    }
                    else if (_player->IsInWorld())
                    {
                        sScriptMgr->OnPacketReceive(m_Socket, WorldPacket(*packet));
                        (this->*opHandle->handler)(*packet);
                        #ifdef WIN32
                        if (sLog->ShouldLog("network", LOG_LEVEL_TRACE) && packet->rpos() < packet->wpos())
                            LogUnprocessedTail(packet);
                        #endif
                    }
                    // lag can cause STATUS_LOGGEDIN opcodes to arrive after the player started a transfer
                    break;
                case STATUS_LOGGEDIN_OR_RECENTLY_LOGGOUT:
                    if (!_player && !m_playerRecentlyLogout && !m_playerLogout) // There's a short delay between _player = null and m_playerRecentlyLogout = true during logout
                        LogUnexpectedOpcode(packet, "STATUS_LOGGEDIN_OR_RECENTLY_LOGGOUT",
                            "the player has not logged in yet and not recently logout");
                    else
                    {
                        // not expected _player or must checked in packet hanlder
                        sScriptMgr->OnPacketReceive(m_Socket, WorldPacket(*packet));
                        (this->*opHandle->handler)(*packet);
                        if (sLog->ShouldLog("network", LOG_LEVEL_TRACE) && packet->rpos() < packet->wpos())
                            LogUnprocessedTail(packet);
                    }
                    break;
                case STATUS_TRANSFER:
                    if (!_player)
                    {
                        LogUnexpectedOpcode(packet, "STATUS_TRANSFER", "the player has not logged in yet");
                    }
                    else if (_player->IsInWorld())
                    {
                        LogUnexpectedOpcode(packet, "STATUS_TRANSFER", "the player is still in world");
                    }
                    else
                    {
                        sScriptMgr->OnPacketReceive(m_Socket, WorldPacket(*packet));
                        (this->*opHandle->handler)(*packet);
                        if (sLog->ShouldLog("network", LOG_LEVEL_TRACE) && packet->rpos() < packet->wpos())
                            LogUnprocessedTail(packet);
                    }
                    break;
                case STATUS_AUTHED:
                    // prevent cheating with skip queue wait
                    if (m_inQueue)
                    {
                        LogUnexpectedOpcode(packet, "STATUS_AUTHED", "the player not pass queue yet");
                        break;
                    }

                    // some auth opcodes can be recieved before STATUS_LOGGEDIN_OR_RECENTLY_LOGGOUT opcodes
                    // however when we recieve CMSG_CHAR_ENUM we are surely no longer during the logout process.
                    if (packet->GetOpcode() == CMSG_CHAR_ENUM)
                        m_playerRecentlyLogout = false;

                    sScriptMgr->OnPacketReceive(m_Socket, WorldPacket(*packet));
                    (this->*opHandle->handler)(*packet);
                    if (sLog->ShouldLog("network", LOG_LEVEL_TRACE) && packet->rpos() < packet->wpos())
                        LogUnprocessedTail(packet);
                    break;
                case STATUS_NEVER:
                    #ifdef WIN32
                        TC_LOG_ERROR("opcode", "Received not allowed opcode %s from %s", GetOpcodeNameForLogging(packet->GetOpcode()).c_str()
                            , GetPlayerName(false).c_str());
                    #endif
                    break;
                case STATUS_UNHANDLED:
                    #ifdef WIN32
                        TC_LOG_ERROR("opcode", "Received not handled opcode %s from %s", GetOpcodeNameForLogging(packet->GetOpcode()).c_str()
                            , GetPlayerName(false).c_str());
                    #endif
                    break;
            }
        }
        catch(ByteBufferException &)
        {
            TC_LOG_ERROR("network", "WorldSession::Update ByteBufferException occured while parsing a packet (opcode: %u) from client %s, accountid=%i. Skipped packet.",
                    packet->GetOpcode(), GetRemoteAddress().c_str(), GetAccountId());
            packet->hexlike();
        }

        if (deletePacket)
            delete packet;
    }

    if (m_Socket && !m_Socket->IsClosed())
    {
        if (_warden)
            _warden->Update();
    }

    ProcessQueryCallbacks();

    //check if we are safe to proceed with logout
    //logout procedure should happen only in World::UpdateSessions() method!!!
    if (updater.ProcessLogout())
    {
        time_t currTime = time(NULL);
        ///- If necessary, log the player out
        if (ShouldLogOut(currTime) && !m_playerLoading)
            LogoutPlayer(true);

        ///- Cleanup socket pointer if need
        if (m_Socket && m_Socket->IsClosed())
        {
            m_Socket->RemoveReference();
            m_Socket = NULL;
        }

        if (!m_Socket)
            return false;                                       //Will remove this session from the world session map
    }

    return true;
}

/// %Log the player out
void WorldSession::LogoutPlayer(bool Save)
{
    // finish pending transfers before starting the logout
    while (_player && _player->IsBeingTeleportedFar())
        HandleMoveWorldportResponseOpcode();

    m_playerLogout = true;
    m_playerSave = Save;

    if (_player)
    {
        if (uint64 lguid = _player->GetLootGUID())
            DoLootRelease(lguid);
        _player->ClearAoeLootList();

        ///- If the player just died before logging out, make him appear as a ghost
        //FIXME: logout must be delayed in case lost connection with client in time of combat
        if (_player->GetDeathTimer())
        {
            _player->getHostileRefManager().deleteReferences();
            _player->BuildPlayerRepop();
            _player->RepopAtGraveyard();
        }
        else if (!_player->getAttackers().empty())
        {
            bool _killer = false;
            // build set of player who attack _player or who have pet attacking of _player
            std::set<Player*> aset;
            for (Unit::AttackerSet::const_iterator itr = _player->getAttackers().begin(); itr != _player->getAttackers().end(); ++itr)
            {
                Unit* owner = (*itr)->GetOwner();           // including player controlled case
                if (owner && owner->GetTypeId() == TYPEID_PLAYER)
                    aset.insert(owner->ToPlayer());
                else if ((*itr)->GetTypeId() == TYPEID_PLAYER)
                    aset.insert((Player*)(*itr));

                if (!(*itr)->isTrainingDummy())
                    _killer = true;
            }
            // CombatStop() method is removing all attackers from the AttackerSet
            // That is why it must be AFTER building current set of attackers
            _player->CombatStop();
            _player->getHostileRefManager().setOnlineOfflineState(false);

            if (_killer)
            {
                _player->RemoveAllAurasOnDeath();
                _player->SetPvPDeath(!aset.empty());
                _player->KillPlayer();
                _player->BuildPlayerRepop();
                _player->RepopAtGraveyard();
            }

            // give honor to all attackers from set like group case
            for (std::set<Player*>::const_iterator itr = aset.begin(); itr != aset.end(); ++itr)
                (*itr)->RewardHonor(_player, aset.size());

            // give bg rewards and update counters like kill by first from attackers
            // this can't be called for all attackers.
            if (!aset.empty())
                if (Battleground* bg = _player->GetBattleground())
                    bg->HandleKillPlayer(_player, *aset.begin());
        }
        else if (_player->HasAuraType(SPELL_AURA_SPIRIT_OF_REDEMPTION))
        {
            // this will kill character by SPELL_AURA_SPIRIT_OF_REDEMPTION
            _player->RemoveAurasByType(SPELL_AURA_MOD_SHAPESHIFT);
            _player->KillPlayer();
            _player->BuildPlayerRepop();
            _player->RepopAtGraveyard();
        }
        else if (_player->HasPendingBind())
        {
            _player->RepopAtGraveyard();
            _player->SetPendingBind(0, 0);
        }

        if (Battleground* bg = _player->GetBattleground())
            if (bg->isArena())
                if (bg->GetStatus() == STATUS_IN_PROGRESS || bg->GetStatus() == STATUS_WAIT_JOIN)
                    _player->HandleArenaDeserter();

        sBattlefieldMgr->EventPlayerLoggedOut(_player);

        //drop a flag if player is carrying it
        if (Battleground* bg = _player->GetBattleground())
            bg->EventPlayerLoggedOut(_player);

        ///- Teleport to home if the player is in an invalid instance
        if (!_player->m_InstanceValid && !_player->isGameMaster())
            _player->TeleportTo(_player->m_homebindMapId, _player->m_homebindX, _player->m_homebindY, _player->m_homebindZ, _player->GetOrientation());

        sOutdoorPvPMgr->HandlePlayerLeaveZone(_player, _player->getCurrentUpdateZoneID());

        for (int i=0; i < PLAYER_MAX_BATTLEGROUND_QUEUES; ++i)
        {
            if (BattlegroundQueueTypeId bgQueueTypeId = _player->GetBattlegroundQueueTypeId(i))
            {
                if (bgQueueTypeId > BATTLEGROUND_QUEUE_RB && bgQueueTypeId < BATTLEGROUND_QUEUE_KT)
                {
                    if (_player->IsInvitedForBattlegroundQueueType(bgQueueTypeId) && !_player->HasAura(125761))
                        _player->HandleArenaDeserter();
                }
                _player->RemoveBattlegroundQueueId(bgQueueTypeId);
                sBattlegroundMgr->m_BattlegroundQueues[ bgQueueTypeId ].RemovePlayer(_player->GetGUID(), true);
            }
        }

        // Repop at GraveYard or other player far teleport will prevent saving player because of not present map
        // Teleport player immediately for correct player save
        while (_player->IsBeingTeleportedFar())
            HandleMoveWorldportResponseOpcode();

        ///- If the player is in a guild, update the guild roster and broadcast a logout message to other guild members
        if (Guild* guild = sGuildMgr->GetGuildById(_player->GetGuildId()))
            guild->HandleMemberLogout(this);

        ///- Remove pet
        if (_player->getClass() != CLASS_WARLOCK)
            _player->RemovePet(NULL);
        else
        {
            if (Pet* _pet = _player->GetPet())
                _pet->SavePetToDB();
        }

        ///- empty buyback items and save the player in the database
        // some save parts only correctly work in case player present in map/player_lists (pets, etc)
        if (Save)
        {
            uint32 eslot;
            for (int j = BUYBACK_SLOT_START; j < BUYBACK_SLOT_END; ++j)
            {
                eslot = j - BUYBACK_SLOT_START;
                _player->SetUInt64Value(PLAYER_FIELD_VENDORBUYBACK_SLOT_1 + (eslot * 2), 0);
                _player->SetUInt32Value(PLAYER_FIELD_BUYBACK_PRICE_1 + eslot, 0);
                _player->SetUInt32Value(PLAYER_FIELD_BUYBACK_TIMESTAMP_1 + eslot, 0);
            }
            _player->SaveToDB();
        }

        ///- Leave all channels before player delete...
        _player->CleanupChannels();

        ///- If the player is in a group (or invited), remove him. If the group if then only 1 person, disband the group.
        _player->UninviteFromGroup();

        // remove player from the group if he is:
        // a) in group; b) not in raid group; c) logging out normally (not being kicked or disconnected)
        if (_player->GetGroup() && !_player->GetGroup()->isRaidGroup() && m_Socket)
            _player->RemoveFromGroup();

        //! Send update to group and reset stored max enchanting level
        if (_player->GetGroup())
        {
            _player->GetGroup()->SendUpdate();
            _player->GetGroup()->ResetMaxEnchantingLevel();
        }

        //! Broadcast a logout message to the player's friends
        sSocialMgr->SendFriendStatus(_player, FRIEND_OFFLINE, _player->GetGUIDLow(), true);
        sSocialMgr->RemovePlayerSocial(_player->GetGUIDLow());

        //! Call script hook before deletion
        sScriptMgr->OnPlayerLogout(_player);

        //! Remove the player from the world
        // the player may not be in the world when logging out
        // e.g if he got disconnected during a transfer to another map
        // calls to GetMap in this case may cause crashes
        volatile uint32 guidDebug = _player->GetGUIDLow();
        _player->CleanupsBeforeDelete();
        TC_LOG_INFO("char", "Account: %d (IP: %s) Logout Character:[%s] (GUID: %u) Level: %d", GetAccountId(), GetRemoteAddress().c_str(), _player->GetName(), _player->GetGUIDLow(), _player->getLevel());
        if (Map* _map = _player->FindMap())
            _map->RemovePlayerFromMap(_player, true);

        SetPlayer(NULL); //! Pointer already deleted during RemovePlayerFromMap

        //! Send the 'logout complete' packet to the client
        //! Client will respond by sending 3x CMSG_CANCEL_TRADE, which we currently dont handle
        WorldPacket data(SMSG_LOGOUT_COMPLETE, 0);
        SendPacket(&data);
        TC_LOG_DEBUG("network", "SESSION: Sent SMSG_LOGOUT_COMPLETE Message");

        //! Since each account can only have one online character at any given time, ensure all characters for active account are marked as offline
        PreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_ACCOUNT_ONLINE);
        stmt->setUInt32(0, GetAccountId());
        CharacterDatabase.Execute(stmt);
    }

    m_playerLogout = false;
    m_playerSave = false;
    m_playerRecentlyLogout = true;
    LogoutRequest(0);
}

/// Kick a player out of the World
void WorldSession::KickPlayer()
{
    if (m_Socket)
        m_Socket->CloseSocket();
}

void WorldSession::SendNotification(const char *format, ...)
{
    if (format)
    {
        va_list ap;
        char szStr[1024];
        szStr[0] = '\0';
        va_start(ap, format);
        vsnprintf(szStr, 1024, format, ap);
        va_end(ap);

        size_t len = strlen(szStr);

        //! 5.4.1
        WorldPacket data(SMSG_NOTIFICATION, 2 + len);
        data.WriteBits(len, 12);
        data.FlushBits();
        data.append(szStr, len);
        SendPacket(&data);
    }
}

void WorldSession::SendNotification(uint32 string_id, ...)
{
    char const* format = GetTrinityString(string_id);
    if (format)
    {
        va_list ap;
        char szStr[1024];
        szStr[0] = '\0';
        va_start(ap, string_id);
        vsnprintf(szStr, 1024, format, ap);
        va_end(ap);

        size_t len = strlen(szStr);
        WorldPacket data(SMSG_NOTIFICATION, 2 + len);
        data.WriteBits(len, 12);
        data.FlushBits();
        data.append(szStr, len);
        SendPacket(&data);
    }
}

const char *WorldSession::GetTrinityString(int32 entry) const
{
    return sObjectMgr->GetTrinityString(entry, GetSessionDbLocaleIndex());
}

void WorldSession::Handle_NULL(WorldPacket& recvPacket)
{
    TC_LOG_ERROR("opcode", "Received unhandled opcode %s from %s"
        , GetOpcodeNameForLogging(recvPacket.GetOpcode()).c_str(), GetPlayerName(false).c_str());
}

void WorldSession::Handle_EarlyProccess(WorldPacket& recvPacket)
{
    TC_LOG_ERROR("opcode", "Received opcode %s that must be processed in WorldSocket::OnRead from %s"
        , GetOpcodeNameForLogging(recvPacket.GetOpcode()).c_str(), GetPlayerName(false).c_str());
}

void WorldSession::Handle_ServerSide(WorldPacket& recvPacket)
{
    TC_LOG_ERROR("opcode", "Received server-side opcode %s from %s"
        , GetOpcodeNameForLogging(recvPacket.GetOpcode()).c_str(), GetPlayerName(false).c_str());
}

void WorldSession::Handle_Deprecated(WorldPacket& recvPacket)
{
    TC_LOG_ERROR("opcode", "Received deprecated opcode %s from %s"
        , GetOpcodeNameForLogging(recvPacket.GetOpcode()).c_str(), GetPlayerName(false).c_str());
}

void WorldSession::SendAuthWaitQue(uint32 position)
{
    if (position == 0)
    {
        WorldPacket packet(SMSG_AUTH_RESPONSE, 1);
        packet.WriteBit(0); // has account info
        packet.WriteBit(0); // has queue info
        packet << uint8(AUTH_OK);
        packet.FlushBits();
        SendPacket(&packet);
    }
    else
    {
        WorldPacket packet(SMSG_AUTH_RESPONSE, 6);
        packet.WriteBit(0); // has account info
        packet.WriteBit(1); // has queue info
        packet.WriteBit(0); // unk queue bool
        packet << uint8(AUTH_WAIT_QUEUE);
        packet.FlushBits();
        packet << uint32(position);
        SendPacket(&packet);
    }
}

void WorldSession::LoadGlobalAccountData()
{
    PreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_ACCOUNT_DATA);
    stmt->setUInt32(0, GetAccountId());
    LoadAccountData(CharacterDatabase.Query(stmt), GLOBAL_CACHE_MASK);
}

void WorldSession::LoadAccountData(PreparedQueryResult result, uint32 mask)
{
    for (uint32 i = 0; i < NUM_ACCOUNT_DATA_TYPES; ++i)
        if (mask & (1 << i))
            m_accountData[i] = AccountData();

    if (!result)
        return;

    do
    {
        Field* fields = result->Fetch();
        uint32 type = fields[0].GetUInt8();
        if (type >= NUM_ACCOUNT_DATA_TYPES)
        {
            TC_LOG_ERROR("server", "Table `%s` have invalid account data type (%u), ignore.",
                mask == GLOBAL_CACHE_MASK ? "account_data" : "character_account_data", type);
            continue;
        }

        if ((mask & (1 << type)) == 0)
        {
            TC_LOG_ERROR("server", "Table `%s` have non appropriate for table  account data type (%u), ignore.",
                mask == GLOBAL_CACHE_MASK ? "account_data" : "character_account_data", type);
            continue;
        }

        m_accountData[type].Time = time_t(fields[1].GetUInt32());
        m_accountData[type].Data = fields[2].GetString();
    }
    while (result->NextRow());
}

void WorldSession::SetAccountData(AccountDataType type, time_t tm, std::string data)
{
    uint32 id = 0;
    uint32 index = 0;
    if ((1 << type) & GLOBAL_CACHE_MASK)
    {
        id = GetAccountId();
        index = CHAR_REP_ACCOUNT_DATA;
    }
    else
    {
        // _player can be NULL and packet received after logout but m_GUID still store correct guid
        if (!m_GUIDLow)
            return;

        id = m_GUIDLow;
        index = CHAR_REP_PLAYER_ACCOUNT_DATA;
    }

    PreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(index);
    stmt->setUInt32(0, id);
    stmt->setUInt8 (1, type);
    stmt->setUInt32(2, uint32(tm));
    stmt->setString(3, data);
    CharacterDatabase.Execute(stmt);

    m_accountData[type].Time = tm;
    m_accountData[type].Data = data;
}

void WorldSession::SendAccountDataTimes(uint32 mask, bool ready)
{
    WorldPacket data(SMSG_ACCOUNT_DATA_TIMES, 4+1+4+NUM_ACCOUNT_DATA_TYPES*4);
    data << uint32(time(NULL));                             // Server time
    data << uint32(mask);                                   // type mask
    for (uint32 i = 0; i < NUM_ACCOUNT_DATA_TYPES; ++i)
        data << uint32(GetAccountData(AccountDataType(i))->Time);// also unix time
    data.WriteBit(!ready);
    data.FlushBits();
    SendPacket(&data);
    SendTimeZoneInformation();
}

// ToDo: add confing. Are we need it?
void WorldSession::SendTimeZoneInformation()
{
    //Etc/UTC recomendation from skyfire
    std::string zone = "Etc/UTC";    //RTL: Europe/Paris
    WorldPacket data(SMSG_SET_TIME_ZONE_INFORMATION, 30);
    data.WriteBits(zone.size(), 7);
    data.WriteBits(zone.size(), 7);
    data.FlushBits();
    data.WriteString(zone);
    data.WriteString(zone);
    SendPacket(&data);
}

void WorldSession::LoadTutorialsData()
{
    memset(m_Tutorials, 0, sizeof(uint32) * MAX_ACCOUNT_TUTORIAL_VALUES);

    PreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_TUTORIALS);
    stmt->setUInt32(0, GetAccountId());
    if (PreparedQueryResult result = CharacterDatabase.Query(stmt))
        for (uint8 i = 0; i < MAX_ACCOUNT_TUTORIAL_VALUES; ++i)
            m_Tutorials[i] = (*result)[i].GetUInt32();

    m_TutorialsChanged = false;
}

void WorldSession::SendTutorialsData()
{
    WorldPacket data(SMSG_TUTORIAL_FLAGS, 4 * MAX_ACCOUNT_TUTORIAL_VALUES);
    for (uint8 i = 0; i < MAX_ACCOUNT_TUTORIAL_VALUES; ++i)
        data << m_Tutorials[i];
    SendPacket(&data);
}

void WorldSession::SaveTutorialsData(SQLTransaction &trans)
{
    if (!m_TutorialsChanged)
        return;

    PreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_HAS_TUTORIALS);
    stmt->setUInt32(0, GetAccountId());
    bool hasTutorials = !CharacterDatabase.Query(stmt).null();
    // Modify data in DB
    stmt = CharacterDatabase.GetPreparedStatement(hasTutorials ? CHAR_UPD_TUTORIALS : CHAR_INS_TUTORIALS);
    for (uint8 i = 0; i < MAX_ACCOUNT_TUTORIAL_VALUES; ++i)
        stmt->setUInt32(i, m_Tutorials[i]);
    stmt->setUInt32(MAX_ACCOUNT_TUTORIAL_VALUES, GetAccountId());
    trans->Append(stmt);

    m_TutorialsChanged = false;
}

void WorldSession::ReadAddonsInfo(WorldPacket &data)
{
    if (data.rpos() + 4 > data.size())
        return;

    uint32 size;
    data >> size;

    if (!size)
        return;

    if (size > 0xFFFFF)
    {
        TC_LOG_ERROR("server", "WorldSession::ReadAddonsInfo addon info too big, size %u", size);
        return;
    }

    uLongf uSize = size;
    uint32 pos = data.rpos();

    ByteBuffer addonInfo;
    addonInfo.resize(size);

    if (uncompress(const_cast<uint8*>(addonInfo.contents()), &uSize, const_cast<uint8*>(data.contents() + pos), data.size() - pos) == Z_OK)
    {
        uint32 addonsCount;
        addonInfo >> addonsCount;                         // addons count

        for (uint32 i = 0; i < addonsCount; ++i)
        {
            std::string addonName;
            uint8 hasPubKey;
            uint32 CRC, urlCRC;

            // check next addon data format correctness
            if (addonInfo.rpos() + 1 > addonInfo.size())
                return;

            addonInfo >> addonName;
            addonInfo >> hasPubKey >> CRC >> urlCRC;

            TC_LOG_DEBUG("server", "ADDON: Name: %s, hasPubKey: 0x%x, CRC: 0x%x, urlCRC: 0x%x", addonName.c_str(), hasPubKey, CRC, urlCRC);

            AddonInfo addon(addonName, hasPubKey, CRC, 2, true);

            SavedAddon const* savedAddon = AddonMgr::GetAddonInfo(addonName);
            if (savedAddon)
            {
                bool match = true;

                if (addon.CRC != savedAddon->CRC)
                    match = false;

                if (!match)
                    TC_LOG_DEBUG("server", "ADDON: %s was known, but didn't match known CRC (0x%x)!", addon.Name.c_str(), savedAddon->CRC);
                else
                    TC_LOG_DEBUG("server", "ADDON: %s was known, CRC is correct (0x%x)", addon.Name.c_str(), savedAddon->CRC);
            }
            else
            {
                AddonMgr::SaveAddon(addon);
                TC_LOG_DEBUG("server", "ADDON: %s (0x%x) was not known, saving...", addon.Name.c_str(), addon.CRC);
            }

            // TODO: Find out when to not use CRC/pubkey, and other possible states.
            m_addonsList.push_back(addon);
        }

        uint32 latestBannedAddonTimeStamp;
        addonInfo >> latestBannedAddonTimeStamp;
        TC_LOG_DEBUG("network", "ADDON: latestBannedAddonTimeStamp: %u", latestBannedAddonTimeStamp);

        if (addonInfo.rpos() != addonInfo.size())
            TC_LOG_DEBUG("network", "packet under-read!");
    }
    else
        TC_LOG_ERROR("server", "Addon packet uncompress error!");
}

void WorldSession::SendAddonsInfo()
{
    uint8 addonPublicKey[256] =
    {
        0xC3, 0x5B, 0x50, 0x84, 0xB9, 0x3E, 0x32, 0x42, 0x8C, 0xD0, 0xC7, 0x48, 0xFA, 0x0E, 0x5D, 0x54,
        0x5A, 0xA3, 0x0E, 0x14, 0xBA, 0x9E, 0x0D, 0xB9, 0x5D, 0x8B, 0xEE, 0xB6, 0x84, 0x93, 0x45, 0x75,
        0xFF, 0x31, 0xFE, 0x2F, 0x64, 0x3F, 0x3D, 0x6D, 0x07, 0xD9, 0x44, 0x9B, 0x40, 0x85, 0x59, 0x34,
        0x4E, 0x10, 0xE1, 0xE7, 0x43, 0x69, 0xEF, 0x7C, 0x16, 0xFC, 0xB4, 0xED, 0x1B, 0x95, 0x28, 0xA8,
        0x23, 0x76, 0x51, 0x31, 0x57, 0x30, 0x2B, 0x79, 0x08, 0x50, 0x10, 0x1C, 0x4A, 0x1A, 0x2C, 0xC8,
        0x8B, 0x8F, 0x05, 0x2D, 0x22, 0x3D, 0xDB, 0x5A, 0x24, 0x7A, 0x0F, 0x13, 0x50, 0x37, 0x8F, 0x5A,
        0xCC, 0x9E, 0x04, 0x44, 0x0E, 0x87, 0x01, 0xD4, 0xA3, 0x15, 0x94, 0x16, 0x34, 0xC6, 0xC2, 0xC3,
        0xFB, 0x49, 0xFE, 0xE1, 0xF9, 0xDA, 0x8C, 0x50, 0x3C, 0xBE, 0x2C, 0xBB, 0x57, 0xED, 0x46, 0xB9,
        0xAD, 0x8B, 0xC6, 0xDF, 0x0E, 0xD6, 0x0F, 0xBE, 0x80, 0xB3, 0x8B, 0x1E, 0x77, 0xCF, 0xAD, 0x22,
        0xCF, 0xB7, 0x4B, 0xCF, 0xFB, 0xF0, 0x6B, 0x11, 0x45, 0x2D, 0x7A, 0x81, 0x18, 0xF2, 0x92, 0x7E,
        0x98, 0x56, 0x5D, 0x5E, 0x69, 0x72, 0x0A, 0x0D, 0x03, 0x0A, 0x85, 0xA2, 0x85, 0x9C, 0xCB, 0xFB,
        0x56, 0x6E, 0x8F, 0x44, 0xBB, 0x8F, 0x02, 0x22, 0x68, 0x63, 0x97, 0xBC, 0x85, 0xBA, 0xA8, 0xF7,
        0xB5, 0x40, 0x68, 0x3C, 0x77, 0x86, 0x6F, 0x4B, 0xD7, 0x88, 0xCA, 0x8A, 0xD7, 0xCE, 0x36, 0xF0,
        0x45, 0x6E, 0xD5, 0x64, 0x79, 0x0F, 0x17, 0xFC, 0x64, 0xDD, 0x10, 0x6F, 0xF3, 0xF5, 0xE0, 0xA6,
        0xC3, 0xFB, 0x1B, 0x8C, 0x29, 0xEF, 0x8E, 0xE5, 0x34, 0xCB, 0xD1, 0x2A, 0xCE, 0x79, 0xC3, 0x9A,
        0x0D, 0x36, 0xEA, 0x01, 0xE0, 0xAA, 0x91, 0x20, 0x54, 0xF0, 0x72, 0xD8, 0x1E, 0xC7, 0x89, 0xD2
    };

    WorldPacket data(SMSG_ADDON_INFO, 8);
    data.WriteBits(sObjectMgr->GetBannedAddons()->size(), 18);  // banned addons count
    data.WriteBits(m_addonsList.size(), 23);                    // addons count

    ByteBuffer buffer;
    ByteBuffer buffer1;
    for (AddonsList::iterator itr = m_addonsList.begin(); itr != m_addonsList.end(); ++itr)
    {
        bool bit0 = true; //itr->UsePublicKeyOrCRC;
        bool bit1 = itr->CRC != STANDARD_ADDON_CRC;
        bool bit2 = false;

        data.WriteBit(bit0);
        data.WriteBit(bit1);
        data.WriteBit(bit2);

        if (bit2)
        {
            data.WriteBits(0, 8);                           // string length
            //buffer.WriteStringData(..);                   // use <Addon>\<Addon>.url file or not
        }
        if (bit1)
        {
            TC_LOG_DEBUG("server", "ADDON: CRC (0x%x) for addon %s is wrong (does not match expected 0x%x), sending pubkey",
                itr->CRC, itr->Name.c_str(), STANDARD_ADDON_CRC);

#pragma region crcOrder
            buffer << uint8(addonPublicKey[181]);
            buffer << uint8(addonPublicKey[205]);
            buffer << uint8(addonPublicKey[44]);
            buffer << uint8(addonPublicKey[93]);
            buffer << uint8(addonPublicKey[236]);
            buffer << uint8(addonPublicKey[21]);
            buffer << uint8(addonPublicKey[91]);
            buffer << uint8(addonPublicKey[221]);
            buffer << uint8(addonPublicKey[133]);
            buffer << uint8(addonPublicKey[152]);
            buffer << uint8(addonPublicKey[226]);
            buffer << uint8(addonPublicKey[172]);
            buffer << uint8(addonPublicKey[168]);
            buffer << uint8(addonPublicKey[146]);
            buffer << uint8(addonPublicKey[83]);
            buffer << uint8(addonPublicKey[113]);
            buffer << uint8(addonPublicKey[100]);
            buffer << uint8(addonPublicKey[50]);
            buffer << uint8(addonPublicKey[191]);
            buffer << uint8(addonPublicKey[241]);
            buffer << uint8(addonPublicKey[142]);
            buffer << uint8(addonPublicKey[96]);
            buffer << uint8(addonPublicKey[17]);
            buffer << uint8(addonPublicKey[244]);
            buffer << uint8(addonPublicKey[155]);
            buffer << uint8(addonPublicKey[160]);
            buffer << uint8(addonPublicKey[36]);
            buffer << uint8(addonPublicKey[196]);
            buffer << uint8(addonPublicKey[45]);
            buffer << uint8(addonPublicKey[165]);
            buffer << uint8(addonPublicKey[32]);
            buffer << uint8(addonPublicKey[15]);
            buffer << uint8(addonPublicKey[98]);
            buffer << uint8(addonPublicKey[20]);
            buffer << uint8(addonPublicKey[137]);
            buffer << uint8(addonPublicKey[88]);
            buffer << uint8(addonPublicKey[136]);
            buffer << uint8(addonPublicKey[22]);
            buffer << uint8(addonPublicKey[231]);
            buffer << uint8(addonPublicKey[224]);
            buffer << uint8(addonPublicKey[188]);
            buffer << uint8(addonPublicKey[158]);
            buffer << uint8(addonPublicKey[192]);
            buffer << uint8(addonPublicKey[217]);
            buffer << uint8(addonPublicKey[253]);
            buffer << uint8(addonPublicKey[81]);
            buffer << uint8(addonPublicKey[210]);
            buffer << uint8(addonPublicKey[122]);
            buffer << uint8(addonPublicKey[220]);
            buffer << uint8(addonPublicKey[77]);
            buffer << uint8(addonPublicKey[35]);
            buffer << uint8(addonPublicKey[180]);
            buffer << uint8(addonPublicKey[132]);
            buffer << uint8(addonPublicKey[73]);
            buffer << uint8(addonPublicKey[223]);
            buffer << uint8(addonPublicKey[153]);
            buffer << uint8(addonPublicKey[242]);
            buffer << uint8(addonPublicKey[190]);
            buffer << uint8(addonPublicKey[74]);
            buffer << uint8(addonPublicKey[34]);
            buffer << uint8(addonPublicKey[80]);
            buffer << uint8(addonPublicKey[144]);
            buffer << uint8(addonPublicKey[48]);
            buffer << uint8(addonPublicKey[252]);
            buffer << uint8(addonPublicKey[161]);
            buffer << uint8(addonPublicKey[13]);
            buffer << uint8(addonPublicKey[38]);
            buffer << uint8(addonPublicKey[239]);
            buffer << uint8(addonPublicKey[97]);
            buffer << uint8(addonPublicKey[9]);
            buffer << uint8(addonPublicKey[166]);
            buffer << uint8(addonPublicKey[43]);
            buffer << uint8(addonPublicKey[40]);
            buffer << uint8(addonPublicKey[4]);
            buffer << uint8(addonPublicKey[41]);
            buffer << uint8(addonPublicKey[37]);
            buffer << uint8(addonPublicKey[3]);
            buffer << uint8(addonPublicKey[173]);
            buffer << uint8(addonPublicKey[211]);
            buffer << uint8(addonPublicKey[194]);
            buffer << uint8(addonPublicKey[186]);
            buffer << uint8(addonPublicKey[193]);
            buffer << uint8(addonPublicKey[111]);
            buffer << uint8(addonPublicKey[92]);
            buffer << uint8(addonPublicKey[249]);
            buffer << uint8(addonPublicKey[105]);
            buffer << uint8(addonPublicKey[53]);
            buffer << uint8(addonPublicKey[106]);
            buffer << uint8(addonPublicKey[116]);
            buffer << uint8(addonPublicKey[107]);
            buffer << uint8(addonPublicKey[76]);
            buffer << uint8(addonPublicKey[204]);
            buffer << uint8(addonPublicKey[202]);
            buffer << uint8(addonPublicKey[118]);
            buffer << uint8(addonPublicKey[232]);
            buffer << uint8(addonPublicKey[159]);
            buffer << uint8(addonPublicKey[49]);
            buffer << uint8(addonPublicKey[228]);
            buffer << uint8(addonPublicKey[203]);
            buffer << uint8(addonPublicKey[60]);
            buffer << uint8(addonPublicKey[179]);
            buffer << uint8(addonPublicKey[99]);
            buffer << uint8(addonPublicKey[31]);
            buffer << uint8(addonPublicKey[101]);
            buffer << uint8(addonPublicKey[0]);
            buffer << uint8(addonPublicKey[164]);
            buffer << uint8(addonPublicKey[89]);
            buffer << uint8(addonPublicKey[141]);
            buffer << uint8(addonPublicKey[234]);
            buffer << uint8(addonPublicKey[149]);
            buffer << uint8(addonPublicKey[117]);
            buffer << uint8(addonPublicKey[198]);
            buffer << uint8(addonPublicKey[16]);
            buffer << uint8(addonPublicKey[82]);
            buffer << uint8(addonPublicKey[135]);
            buffer << uint8(addonPublicKey[148]);
            buffer << uint8(addonPublicKey[94]);
            buffer << uint8(addonPublicKey[57]);
            buffer << uint8(addonPublicKey[230]);
            buffer << uint8(addonPublicKey[238]);
            buffer << uint8(addonPublicKey[51]);
            buffer << uint8(addonPublicKey[109]);
            buffer << uint8(addonPublicKey[66]);
            buffer << uint8(addonPublicKey[240]);
            buffer << uint8(addonPublicKey[103]);
            buffer << uint8(addonPublicKey[187]);
            buffer << uint8(addonPublicKey[119]);
            buffer << uint8(addonPublicKey[108]);
            buffer << uint8(addonPublicKey[171]);
            buffer << uint8(addonPublicKey[195]);
            buffer << uint8(addonPublicKey[90]);
            buffer << uint8(addonPublicKey[130]);
            buffer << uint8(addonPublicKey[56]);
            buffer << uint8(addonPublicKey[246]);
            buffer << uint8(addonPublicKey[39]);
            buffer << uint8(addonPublicKey[185]);
            buffer << uint8(addonPublicKey[47]);
            buffer << uint8(addonPublicKey[184]);
            buffer << uint8(addonPublicKey[8]);
            buffer << uint8(addonPublicKey[11]);
            buffer << uint8(addonPublicKey[95]);
            buffer << uint8(addonPublicKey[213]);
            buffer << uint8(addonPublicKey[139]);
            buffer << uint8(addonPublicKey[178]);
            buffer << uint8(addonPublicKey[183]);
            buffer << uint8(addonPublicKey[182]);
            buffer << uint8(addonPublicKey[243]);
            buffer << uint8(addonPublicKey[67]);
            buffer << uint8(addonPublicKey[218]);
            buffer << uint8(addonPublicKey[229]);
            buffer << uint8(addonPublicKey[61]);
            buffer << uint8(addonPublicKey[248]);
            buffer << uint8(addonPublicKey[245]);
            buffer << uint8(addonPublicKey[27]);
            buffer << uint8(addonPublicKey[214]);
            buffer << uint8(addonPublicKey[174]);
            buffer << uint8(addonPublicKey[28]);
            buffer << uint8(addonPublicKey[42]);
            buffer << uint8(addonPublicKey[162]);
            buffer << uint8(addonPublicKey[112]);
            buffer << uint8(addonPublicKey[216]);
            buffer << uint8(addonPublicKey[209]);
            buffer << uint8(addonPublicKey[120]);
            buffer << uint8(addonPublicKey[86]);
            buffer << uint8(addonPublicKey[64]);
            buffer << uint8(addonPublicKey[121]);
            buffer << uint8(addonPublicKey[29]);
            buffer << uint8(addonPublicKey[131]);
            buffer << uint8(addonPublicKey[129]);
            buffer << uint8(addonPublicKey[6]);
            buffer << uint8(addonPublicKey[176]);
            buffer << uint8(addonPublicKey[19]);
            buffer << uint8(addonPublicKey[225]);
            buffer << uint8(addonPublicKey[62]);
            buffer << uint8(addonPublicKey[233]);
            buffer << uint8(addonPublicKey[199]);
            buffer << uint8(addonPublicKey[26]);
            buffer << uint8(addonPublicKey[59]);
            buffer << uint8(addonPublicKey[123]);
            buffer << uint8(addonPublicKey[145]);
            buffer << uint8(addonPublicKey[55]);
            buffer << uint8(addonPublicKey[78]);
            buffer << uint8(addonPublicKey[125]);
            buffer << uint8(addonPublicKey[69]);
            buffer << uint8(addonPublicKey[30]);
            buffer << uint8(addonPublicKey[126]);
            buffer << uint8(addonPublicKey[207]);
            buffer << uint8(addonPublicKey[24]);
            buffer << uint8(addonPublicKey[147]);
            buffer << uint8(addonPublicKey[71]);
            buffer << uint8(addonPublicKey[115]);
            buffer << uint8(addonPublicKey[14]);
            buffer << uint8(addonPublicKey[25]);
            buffer << uint8(addonPublicKey[46]);
            buffer << uint8(addonPublicKey[157]);
            buffer << uint8(addonPublicKey[215]);
            buffer << uint8(addonPublicKey[250]);
            buffer << uint8(addonPublicKey[197]);
            buffer << uint8(addonPublicKey[237]);
            buffer << uint8(addonPublicKey[140]);
            buffer << uint8(addonPublicKey[206]);
            buffer << uint8(addonPublicKey[85]);
            buffer << uint8(addonPublicKey[219]);
            buffer << uint8(addonPublicKey[212]);
            buffer << uint8(addonPublicKey[227]);
            buffer << uint8(addonPublicKey[10]);
            buffer << uint8(addonPublicKey[251]);
            buffer << uint8(addonPublicKey[222]);
            buffer << uint8(addonPublicKey[58]);
            buffer << uint8(addonPublicKey[254]);
            buffer << uint8(addonPublicKey[1]);
            buffer << uint8(addonPublicKey[23]);
            buffer << uint8(addonPublicKey[154]);
            buffer << uint8(addonPublicKey[169]);
            buffer << uint8(addonPublicKey[138]);
            buffer << uint8(addonPublicKey[84]);
            buffer << uint8(addonPublicKey[151]);
            buffer << uint8(addonPublicKey[7]);
            buffer << uint8(addonPublicKey[70]);
            buffer << uint8(addonPublicKey[68]);
            buffer << uint8(addonPublicKey[33]);
            buffer << uint8(addonPublicKey[2]);
            buffer << uint8(addonPublicKey[208]);
            buffer << uint8(addonPublicKey[63]);
            buffer << uint8(addonPublicKey[54]);
            buffer << uint8(addonPublicKey[134]);
            buffer << uint8(addonPublicKey[167]);
            buffer << uint8(addonPublicKey[127]);
            buffer << uint8(addonPublicKey[189]);
            buffer << uint8(addonPublicKey[177]);
            buffer << uint8(addonPublicKey[124]);
            buffer << uint8(addonPublicKey[75]);
            buffer << uint8(addonPublicKey[65]);
            buffer << uint8(addonPublicKey[175]);
            buffer << uint8(addonPublicKey[79]);
            buffer << uint8(addonPublicKey[235]);
            buffer << uint8(addonPublicKey[52]);
            buffer << uint8(addonPublicKey[201]);
            buffer << uint8(addonPublicKey[102]);
            buffer << uint8(addonPublicKey[5]);
            buffer << uint8(addonPublicKey[104]);
            buffer << uint8(addonPublicKey[150]);
            buffer << uint8(addonPublicKey[163]);
            buffer << uint8(addonPublicKey[200]);
            buffer << uint8(addonPublicKey[156]);
            buffer << uint8(addonPublicKey[72]);
            buffer << uint8(addonPublicKey[110]);
            buffer << uint8(addonPublicKey[247]);
            buffer << uint8(addonPublicKey[143]);
            buffer << uint8(addonPublicKey[114]);
            buffer << uint8(addonPublicKey[87]);
            buffer << uint8(addonPublicKey[255]);
            buffer << uint8(addonPublicKey[18]);
            buffer << uint8(addonPublicKey[12]);
            buffer << uint8(addonPublicKey[170]);
            buffer << uint8(addonPublicKey[128]);

#pragma endregion
        }
        if (bit0)
        {
            buffer << uint8(1);
            buffer << uint32(0);
        }

        buffer << uint8(itr->State);
    }

    for (BannedAddonDataMap::iterator itr = sBannedAddonDataMap.begin(); itr != sBannedAddonDataMap.end(); ++itr)
    {
        uint32 index = itr->first;
        BannedAddon const * ba = sObjectMgr->GetBannedAddon(index);

        for (uint8 i = 0; i < 16; i += 4)
        {
            // md5 name
            buffer1 << uint8(ba->MD5_name[i]);
            buffer1 << uint8(ba->MD5_name[i+1]);
            buffer1 << uint8(ba->MD5_name[i+2]);
            buffer1 << uint8(ba->MD5_name[i+3]);

            // md5 version
            buffer1 << uint8(ba->MD5_version[i]);
            buffer1 << uint8(ba->MD5_version[i+1]);
            buffer1 << uint8(ba->MD5_version[i+2]);
            buffer1 << uint8(ba->MD5_version[i+3]);
        }

        buffer1 << uint32(1);                 // state, low bit must be 1, client checks (state & 1)
        buffer1 << uint32(ba->timestamp);
        buffer1 << uint32(index);
    }

    data.FlushBits();

    if (!buffer.empty())
        data.append(buffer);

    if (!buffer1.empty())
        data.append(buffer1);

    m_addonsList.clear();
    SendPacket(&data);
}

bool WorldSession::IsAddonRegistered(const std::string& prefix)
{
    if (!_filterAddonMessages) // if we have hit the softcap (64) nothing should be filtered
        return true;

    if (_registeredAddonPrefixes.empty())
        return false;

    std::lock_guard<std::mutex> lock(_registeredAddonLock);
    std::vector<std::string>::const_iterator itr = std::find(_registeredAddonPrefixes.begin(), _registeredAddonPrefixes.end(), prefix);
    return itr != _registeredAddonPrefixes.end();
}

void WorldSession::HandleUnregisterAddonPrefixesOpcode(WorldPacket& /*recvPacket*/) // empty packet
{
    TC_LOG_DEBUG("network", "WORLD: Received CMSG_UNREGISTER_ALL_ADDON_PREFIXES");

    std::lock_guard<std::mutex> lock(_registeredAddonLock);
    _registeredAddonPrefixes.clear();
}

void WorldSession::HandleAddonRegisteredPrefixesOpcode(WorldPacket& recvPacket)
{
    TC_LOG_DEBUG("network", "WORLD: Received CMSG_ADDON_REGISTERED_PREFIXES");

    // This is always sent after CMSG_UNREGISTER_ALL_ADDON_PREFIXES

    uint32 count = recvPacket.ReadBits(24);

    if (count > REGISTERED_ADDON_PREFIX_SOFTCAP)
    {
        // if we have hit the softcap (64) nothing should be filtered
        _filterAddonMessages = false;
        recvPacket.rfinish();
        return;
    }

    std::vector<uint8> lengths(count);
    for (uint32 i = 0; i < count; ++i)
        lengths[i] = recvPacket.ReadBits(5);

    std::lock_guard<std::mutex> lock(_registeredAddonLock);
    for (uint32 i = 0; i < count; ++i)
        _registeredAddonPrefixes.push_back(recvPacket.ReadString(lengths[i]));

    if (_registeredAddonPrefixes.size() > REGISTERED_ADDON_PREFIX_SOFTCAP) // shouldn't happen
    {
        _filterAddonMessages = false;
        return;
    }

    _filterAddonMessages = true;
}

void WorldSession::SetPlayer(Player* player)
{
    _player = player;

    // set m_GUID that can be used while player loggined and later until m_playerRecentlyLogout not reset
    if (_player)
        m_GUIDLow = _player->GetGUIDLow();
}

void WorldSession::InitializeQueryCallbackParameters()
{
    // Callback parameters that have pointers in them should be properly
    // initialized to NULL here.
    _charCreateCallback.SetParam(NULL);
}

void WorldSession::ProcessQueryCallbacks()
{
    PreparedQueryResult result;

    //! HandleCharEnumOpcode
    if (_charEnumCallback.ready())
    {
        _charEnumCallback.get(result);
        HandleCharEnum(result);
        _charEnumCallback.cancel();
    }

    if (_charCreateCallback.IsReady())
    {
        _charCreateCallback.GetResult(result);
        HandleCharCreateCallback(result, _charCreateCallback.GetParam());
        // Don't call FreeResult() here, the callback handler will do that depending on the events in the callback chain
    }

    //! HandlePlayerLoginOpcode
    if (_charLoginCallback.ready())
    {
        SQLQueryHolder* param;
        _charLoginCallback.get(param);
        HandlePlayerLogin((LoginQueryHolder*)param);
        _charLoginCallback.cancel();
    }

    //! HandleAddFriendOpcode
    if (_addFriendCallback.IsReady())
    {
        std::string param = _addFriendCallback.GetParam();
        _addFriendCallback.GetResult(result);
        HandleAddFriendOpcodeCallBack(result, param);
        _addFriendCallback.FreeResult();
    }

    //- HandleCharRenameOpcode
    if (_charRenameCallback.IsReady())
    {
        std::string param = _charRenameCallback.GetParam();
        _charRenameCallback.GetResult(result);
        HandleChangePlayerNameOpcodeCallBack(result, param);
        _charRenameCallback.FreeResult();
    }

    //- HandleCharAddIgnoreOpcode
    if (_addIgnoreCallback.ready())
    {
        _addIgnoreCallback.get(result);
        HandleAddIgnoreOpcodeCallBack(result);
        _addIgnoreCallback.cancel();
    }

    //- SendStabledPet
    if (_sendStabledPetCallback.IsReady())
    {
        uint64 param = _sendStabledPetCallback.GetParam();
        _sendStabledPetCallback.GetResult(result);
        SendStablePetCallback(result, param);
        _sendStabledPetCallback.FreeResult();
    }

    //- HandleStableChangeSlot
    if (_stableChangeSlotCallback.IsReady())
    {
        uint8 param = _stableChangeSlotCallback.GetParam();
        _stableChangeSlotCallback.GetResult(result);
        HandleStableChangeSlotCallback(result, param);
        _stableChangeSlotCallback.FreeResult();
    }
}

bool WorldSession::InitWarden(BigNumber* k, std::string os)
{
    if (os == "Win")
        _warden = new WardenWin(this);
    else
        _warden = new WardenMac(this);

    if (_warden && _warden->Init(k))
        return true;

    return false;
}

PacketSendEvent::~PacketSendEvent()
{
    delete m_packet;
}

bool PacketSendEvent::Execute(uint64 , uint32)
{
    m_owner->SendDirectMessage(m_packet);
    return true;
}

void PacketSendEvent::Schedule()
{
    m_owner->m_Events.AddEvent(this, m_owner->m_Events.CalculateTime(m_delay));
}


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

#include <ace/Message_Block.h>
#include <ace/OS_NS_string.h>
#include <ace/OS_NS_unistd.h>
#include <ace/os_include/arpa/os_inet.h>
#include <ace/os_include/netinet/os_tcp.h>
#include <ace/os_include/sys/os_types.h>
#include <ace/os_include/sys/os_socket.h>
#include <ace/OS_NS_string.h>
#include <ace/Reactor.h>
#include <ace/Auto_Ptr.h>

#include "WorldSocket.h"
#include "Common.h"

#include "Util.h"
#include "World.h"
#include "WorldPacket.h"
#include "SharedDefines.h"
#include "ByteBuffer.h"
#include "Opcodes.h"
#include "DatabaseEnv.h"
#include "BigNumber.h"
#include "SHA1.h"
#include "WorldSession.h"
#include "WorldSocketMgr.h"
#include "Log.h"
#include "PacketLog.h"
#include "ScriptMgr.h"
#include "AccountMgr.h"
#include "zlib.h"

#if defined(__GNUC__)
#pragma pack(1)
#else
#pragma pack(push, 1)
#endif

struct CompressedWorldPacket
{
    uint32 UncompressedSize;
    uint32 UncompressedAdler;
    uint32 CompressedAdler;
};

union ServerPktHeader
{
    struct
    {
        uint16 Size;
        uint32 Command;
    } Setup;

    struct
    {
        uint32 Command : 13;
        uint32 Size : 19;
    } Normal;
};

struct AuthClientPktHeader
{
    uint16 size;
    uint32 cmd;
};

struct WorldClientPktHeader
{
    uint16 size;
    uint16 cmd;
};

uint32 const SizeOfClientHeader[2][2] =
{
    { 2, 0 },
    { 6, 4 }
};

uint32 const SizeOfServerHeader[2] = { /*sizeof(uint16) +*/ sizeof(uint32), sizeof(uint32) };

#if defined(__GNUC__)
#pragma pack()
#else
#pragma pack(pop)
#endif

WorldSocket::WorldSocket (void): WorldHandler(),
    m_LastPingTime(ACE_Time_Value::zero), m_OverSpeedPings(0), m_Session(0),
    m_RecvWPct(0), m_RecvPct(), m_Header(sizeof(AuthClientPktHeader)), m_WorldHeader(sizeof(WorldClientPktHeader)),
    m_OutBuffer(0), m_OutBufferSize(65536), m_OutActive(false),
    m_Seed(static_cast<uint32> (rand32()))
{
    reference_counting_policy().value (ACE_Event_Handler::Reference_Counting_Policy::ENABLED);

    msg_queue()->high_water_mark(8 * 1024 * 1024);
    msg_queue()->low_water_mark(8 * 1024 * 1024);
}

WorldSocket::~WorldSocket (void)
{
    delete m_RecvWPct;

    if (m_OutBuffer)
        m_OutBuffer->release();

    closing_ = true;

    peer().close();
}

bool WorldSocket::IsClosed (void) const
{
    return closing_;
}

void WorldSocket::CloseSocket (void)
{
    {
        ACE_GUARD (LockType, Guard, m_OutBufferLock);

        if (closing_)
            return;

        closing_ = true;
        peer().close_writer();
    }

    {
        ACE_GUARD (LockType, Guard, m_SessionLock);

        m_Session = NULL;
    }
}

const std::string& WorldSocket::GetRemoteAddress (void) const
{
    return m_Address;
}

void WorldSocket::WritePacketToBuffer(WorldPacket const& packet, MessageBuffer& buffer)
{
    ServerPktHeader header;
    uint32 sizeOfHeader = SizeOfServerHeader[m_Crypt.IsInitialized()];
    uint32 opcode = packet.GetOpcode();
    uint32 packetSize = packet.size();

    // Reserve space for buffer
    uint8* headerPos = buffer.GetWritePointer();
    buffer.WriteCompleted(sizeOfHeader);

    if (packetSize > 0x400 && m_Session)
    {
        CompressedWorldPacket cmp;
        cmp.UncompressedSize = packetSize + 4;
        cmp.UncompressedAdler = adler32(adler32(0x9827D8F1, (Bytef*)&opcode, 4), packet.contents(), packetSize);

        // Reserve space for compression info - uncompressed size and checksums
        uint8* compressionInfo = buffer.GetWritePointer();
        buffer.WriteCompleted(sizeof(CompressedWorldPacket));

        uint32 compressedSize = m_Session->CompressPacket(buffer.GetWritePointer(), packet);

        cmp.CompressedAdler = adler32(0x9827D8F1, buffer.GetWritePointer(), compressedSize);

        memcpy(compressionInfo, &cmp, sizeof(CompressedWorldPacket));
        buffer.WriteCompleted(compressedSize);
        packetSize = compressedSize + sizeof(CompressedWorldPacket);

        opcode = SMSG_COMPRESSED_OPCODE;
    }
    else if (!packet.empty())
        buffer.Write(packet.contents(), packet.size());

    if (m_Crypt.IsInitialized())
    {
        //uint8 _header[5];
        //uint64 data = (packetSize << 13) | opcode & 0x1FFF;
        //memcpy(&_header[0], &data, sizeOfHeader);
        //m_Crypt.EncryptSend((uint8*)&_header[0], sizeOfHeader);
        //memcpy(headerPos, &_header, sizeOfHeader);
        //return;

        header.Normal.Size = packetSize;
        header.Normal.Command = opcode;
        m_Crypt.EncryptSend((uint8*)&header, sizeOfHeader);
    }
    else
    {
        //uint8 _header[5];
        //packetSize += 2;
        //memcpy(&_header[0], &packetSize, 2);
        //memcpy(&_header[2], &opcode, 2);
        //memcpy(headerPos, &_header, 4);
        //return;

        header.Setup.Size = packetSize + 2;
        header.Setup.Command = opcode;
    }

    memcpy(headerPos, &header, sizeOfHeader);
}

int WorldSocket::SendPacket(WorldPacket const* pct)
{
    ACE_GUARD_RETURN (LockType, Guard, m_OutBufferLock, -1);

    if (closing_)
        return -1;

    size_t size = pct->wpos();

    if (pct->GetOpcode() != SMSG_MONSTER_MOVE)
    {
        if (m_Session)
            if(Player* _player = m_Session->GetPlayer())
                if (sObjectMgr->IsPlayerInLogList(_player))
                {
                    // Dump outgoing packet
                    if (sPacketLog->CanLogPacket())
                        sPacketLog->LogPacket(*pct, SERVER_TO_CLIENT);
                    TC_LOG_DEBUG("dupe", "S->C: %s", GetOpcodeNameForLogging(pct->GetOpcode()).c_str());
                }
        #ifdef WIN32
        TC_LOG_INFO("opcode", "S->C: %s len %u", GetOpcodeNameForLogging(pct->GetOpcode()).c_str(), pct->wpos());
        #endif
    }

    uint32 packetSize = pct->size();
    uint32 sizeOfHeader = SizeOfServerHeader[m_Crypt.IsInitialized()];
    if (packetSize > 0x400 && m_Session)
        packetSize = compressBound(packetSize) + sizeof(CompressedWorldPacket);

    SendSize[pct->GetOpcode()] += packetSize;
    ++SendCount[pct->GetOpcode()];

    sScriptMgr->OnPacketSend(this, *pct);

    MessageBuffer buffer(sizeOfHeader + packetSize);
    WritePacketToBuffer(*pct, buffer);

    if (m_OutBuffer->space() >= buffer.GetActiveSize() && msg_queue()->is_empty())
    {
        // Put the packet on the buffer.
        if (m_OutBuffer->copy((char*)buffer.GetBasePointer(), buffer.GetActiveSize()) == -1)
            ACE_ASSERT (false);
    }
    else
    {
        // Enqueue the packet.
        ACE_Message_Block* mb;

        ACE_NEW_RETURN(mb, ACE_Message_Block(buffer.GetActiveSize()), -1);

        mb->copy((const char*)buffer.GetBasePointer(), buffer.GetActiveSize());

        if (msg_queue()->enqueue_tail(mb, (ACE_Time_Value*)&ACE_Time_Value::zero) == -1)
        {
            TC_LOG_ERROR("network", "WorldSocket::SendPacket enqueue_tail failed");
            mb->release();
            return -1;
        }
    }

    return 0;
}

long WorldSocket::AddReference (void)
{
    return static_cast<long> (add_reference());
}

long WorldSocket::RemoveReference (void)
{
    return static_cast<long> (remove_reference());
}

int WorldSocket::open (void *a)
{
    ACE_UNUSED_ARG (a);

    // Prevent double call to this func.
    if (m_OutBuffer)
        return -1;

    // This will also prevent the socket from being Updated
    // while we are initializing it.
    m_OutActive = true;

    // Hook for the manager.
    if (sWorldSocketMgr->OnSocketOpen(this) == -1)
        return -1;

    // Allocate the buffer.
    ACE_NEW_RETURN (m_OutBuffer, ACE_Message_Block (m_OutBufferSize), -1);

    // Store peer address.
    ACE_INET_Addr remote_addr;

    if (peer().get_remote_addr(remote_addr) == -1)
    {
        TC_LOG_ERROR("network", "WorldSocket::open: peer().get_remote_addr errno = %s", ACE_OS::strerror (errno));
        return -1;
    }

    m_Address = remote_addr.get_host_addr();

    // not an opcode. this packet sends raw string WORLD OF WARCRAFT CONNECTION - SERVER TO CLIENT"
    // because of our implementation, bytes "WO" become the opcode
    WorldPacket packet(MSG_VERIFY_CONNECTIVITY);
    packet << "RLD OF WARCRAFT CONNECTION - SERVER TO CLIENT";

    if (SendPacket(&packet) == -1)
        return -1;

    // Register with ACE Reactor
    if (reactor()->register_handler(this, ACE_Event_Handler::READ_MASK | ACE_Event_Handler::WRITE_MASK) == -1)
    {
        TC_LOG_ERROR("network", "WorldSocket::open: unable to register client handler errno = %s", ACE_OS::strerror (errno));
        return -1;
    }

    // reactor takes care of the socket from now on
    remove_reference();

    return 0;
}

int WorldSocket::close (u_long)
{
    shutdown();

    closing_ = true;

    remove_reference();

    return 0;
}

int WorldSocket::handle_input (ACE_HANDLE)
{
    if (closing_)
        return -1;

    switch (handle_input_missing_data())
    {
    case -1 :
        {
            if ((errno == EWOULDBLOCK) ||
                (errno == EAGAIN))
            {
                return Update();                           // interesting line, isn't it ?
            }

            TC_LOG_DEBUG("network", "WorldSocket::handle_input: Peer error closing connection errno = %s", ACE_OS::strerror (errno));

            errno = ECONNRESET;
            return -1;
        }
    case 0:
        {
            TC_LOG_DEBUG("network", "WorldSocket::handle_input: Peer has closed connection");

            errno = ECONNRESET;
            return -1;
        }
    case 1:
        return 1;
    default:
        return Update();                               // another interesting line ;)
    }

    ACE_NOTREACHED(return -1);
}

int WorldSocket::handle_output (ACE_HANDLE)
{
    ACE_GUARD_RETURN (LockType, Guard, m_OutBufferLock, -1);

    if (closing_)
        return -1;

    size_t send_len = m_OutBuffer->length();

    if (send_len == 0)
        return handle_output_queue(Guard);

#ifdef MSG_NOSIGNAL
    ssize_t n = peer().send (m_OutBuffer->rd_ptr(), send_len, MSG_NOSIGNAL);
#else
    ssize_t n = peer().send (m_OutBuffer->rd_ptr(), send_len);
#endif // MSG_NOSIGNAL

    if (n == 0)
        return -1;
    else if (n == -1)
    {
        if (errno == EWOULDBLOCK || errno == EAGAIN)
            return schedule_wakeup_output (Guard);

        return -1;
    }
    else if (n < (ssize_t)send_len) //now n > 0
    {
        m_OutBuffer->rd_ptr (static_cast<size_t> (n));

        // move the data to the base of the buffer
        m_OutBuffer->crunch();

        return schedule_wakeup_output (Guard);
    }
    else //now n == send_len
    {
        m_OutBuffer->reset();

        return handle_output_queue (Guard);
    }

    ACE_NOTREACHED (return 0);
}

int WorldSocket::handle_output_queue (GuardType& g)
{
    if (msg_queue()->is_empty())
        return cancel_wakeup_output(g);

    ACE_Message_Block* mblk;

    if (msg_queue()->dequeue_head(mblk, (ACE_Time_Value*)&ACE_Time_Value::zero) == -1)
    {
        TC_LOG_ERROR("network", "WorldSocket::handle_output_queue dequeue_head");
        return -1;
    }

    const size_t send_len = mblk->length();

#ifdef MSG_NOSIGNAL
    ssize_t n = peer().send (mblk->rd_ptr(), send_len, MSG_NOSIGNAL);
#else
    ssize_t n = peer().send (mblk->rd_ptr(), send_len);
#endif // MSG_NOSIGNAL

    if (n == 0)
    {
        mblk->release();

        return -1;
    }
    else if (n == -1)
    {
        if (errno == EWOULDBLOCK || errno == EAGAIN)
        {
            msg_queue()->enqueue_head(mblk, (ACE_Time_Value*) &ACE_Time_Value::zero);
            return schedule_wakeup_output (g);
        }

        mblk->release();
        return -1;
    }
    else if (n < (ssize_t)send_len) //now n > 0
    {
        mblk->rd_ptr(static_cast<size_t> (n));

        if (msg_queue()->enqueue_head(mblk, (ACE_Time_Value*) &ACE_Time_Value::zero) == -1)
        {
            TC_LOG_ERROR("network", "WorldSocket::handle_output_queue enqueue_head");
            mblk->release();
            return -1;
        }

        return schedule_wakeup_output (g);
    }
    else //now n == send_len
    {
        mblk->release();

        return msg_queue()->is_empty() ? cancel_wakeup_output(g) : ACE_Event_Handler::WRITE_MASK;
    }

    ACE_NOTREACHED(return -1);
}

int WorldSocket::handle_close (ACE_HANDLE h, ACE_Reactor_Mask)
{
    // Critical section
    {
        ACE_GUARD_RETURN (LockType, Guard, m_OutBufferLock, -1);

        closing_ = true;

        if (h == ACE_INVALID_HANDLE)
            peer().close_writer();
    }

    // Critical section
    {
        ACE_GUARD_RETURN (LockType, Guard, m_SessionLock, -1);

        m_Session = NULL;
    }

    reactor()->remove_handler(this, ACE_Event_Handler::DONT_CALL | ACE_Event_Handler::ALL_EVENTS_MASK);
    return 0;
}

int WorldSocket::Update (void)
{
    if (closing_)
        return -1;

    if (m_OutActive || (m_OutBuffer->length() == 0 && msg_queue()->is_empty()))
        return 0;

    int ret;
    do
    ret = handle_output(get_handle());
    while (ret > 0);

    return ret;
}

int WorldSocket::handle_input_header (void)
{
    ACE_ASSERT(m_RecvWPct == NULL);

    if (m_Crypt.IsInitialized())
    {
        uint8* clientHeader = (uint8*)m_WorldHeader.rd_ptr();
        WorldClientPktHeader& header = *((WorldClientPktHeader*)clientHeader);

        m_Crypt.DecryptRecv(clientHeader, 4);

        uint32 value = *(uint32*)clientHeader;
        uint32 opcode = value & 0x1FFF;
        uint16 size = (uint16)((value & ~(uint32)0x1FFF) >> 13);

        header.size = size + 4;
        header.cmd = opcode; 

        if (header.size < 4 || header.size > 20240 || header.cmd >= NUM_OPCODE_HANDLERS)
        {
            Player* _player = m_Session ? m_Session->GetPlayer() : NULL;
            TC_LOG_ERROR("network", "WorldSocket::handle_input_header(): client (account: %u, char [GUID: %u, name: %s]) sent malformed packet (size: %d, cmd: %d) IP:%s",
                m_Session ? m_Session->GetAccountId() : 0,
                _player ? _player->GetGUIDLow() : 0,
                _player ? _player->GetName() : "<none>",
                header.size, header.cmd, GetRemoteAddress().c_str());

            errno = EINVAL;
            return -1;
        }

        header.size -= 4; 

        ACE_NEW_RETURN(m_RecvWPct, WorldPacket(Opcodes(header.cmd), header.size), -1);

        if (header.size > 0)
        {
            m_RecvWPct->resize(header.size);
            m_RecvPct.base((char*) m_RecvWPct->contents(), m_RecvWPct->size());
        }
        else
            ACE_ASSERT(m_RecvPct.space() == 0);
    }
    else
    {
        uint8* clientHeader = (uint8*)m_Header.rd_ptr();
        AuthClientPktHeader& header = *((AuthClientPktHeader*)clientHeader);

        EndianConvert(header.size);
        EndianConvert(header.cmd);

        if (header.size < 4 || header.size > 10240 || header.cmd >= NUM_OPCODE_HANDLERS && header.cmd != 0x4C524F57)
        {
            Player* _player = m_Session ? m_Session->GetPlayer() : NULL;
            TC_LOG_ERROR("network", "WorldSocket::handle_input_header(): client (account: %u, char [GUID: %u, name: %s]) sent malformed packet (size: %d, cmd: %d) IP:%s",
                m_Session ? m_Session->GetAccountId() : 0,
                _player ? _player->GetGUIDLow() : 0,
                _player ? _player->GetName() : "<none>",
                header.size, header.cmd, GetRemoteAddress().c_str());

            if(!m_Session && !_player && sWorld->getBoolConfig(CONFIG_IPSET_ENABLE))
            {
                char buffer[200];
                sprintf(buffer,"ipset -A badip %s>/dev/null 2>/dev/null &",GetRemoteAddress().c_str());
                system(buffer);
            }

            errno = EINVAL;
            return -1;
        }

        header.size -= 4;

        ACE_NEW_RETURN(m_RecvWPct, WorldPacket(Opcodes(header.cmd), header.size), -1);

        if (header.size > 0)
        {
            m_RecvWPct->resize(header.size);
            m_RecvPct.base((char*) m_RecvWPct->contents(), m_RecvWPct->size());
        }
        else
            ACE_ASSERT(m_RecvPct.space() == 0);
    }

    return 0;
}

int WorldSocket::handle_input_payload (void)
{
    // set errno properly here on error !!!
    // now have a header and payload

    if (m_Crypt.IsInitialized())
    {
        ACE_ASSERT(m_RecvPct.space() == 0);
        ACE_ASSERT(m_WorldHeader.space() == 0);
        ACE_ASSERT(m_RecvWPct != NULL);

        const int ret = ProcessIncoming(m_RecvWPct);

        m_RecvPct.base(NULL, 0);
        m_RecvPct.reset();
        m_RecvWPct = NULL;

        m_WorldHeader.reset();

        if (ret == -1)
            errno = EINVAL;

        return ret;
    }
    else
    {
        ACE_ASSERT(m_RecvPct.space() == 0);
        ACE_ASSERT(m_Header.space() == 0);
        ACE_ASSERT(m_RecvWPct != NULL);

        const int ret = ProcessIncoming(m_RecvWPct);

        m_RecvPct.base(NULL, 0);
        m_RecvPct.reset();
        m_RecvWPct = NULL;

        m_Header.reset();

        if (ret == -1)
            errno = EINVAL;

        return ret;
    }
}

int WorldSocket::handle_input_missing_data (void)
{
    char buf [4096];

    ACE_Data_Block db (sizeof (buf),
        ACE_Message_Block::MB_DATA,
        buf,
        0,
        0,
        ACE_Message_Block::DONT_DELETE,
        0);

    ACE_Message_Block message_block(&db,
        ACE_Message_Block::DONT_DELETE,
        0);

    const size_t recv_size = message_block.space();

    const ssize_t n = peer().recv (message_block.wr_ptr(),
        recv_size);

    if (n <= 0)
        return int(n);

    message_block.wr_ptr (n);

    while (message_block.length() > 0)
    {
        if (m_Crypt.IsInitialized())
        {
            if (m_WorldHeader.space() > 0)
            {
                // need to receive the header
                const size_t to_header = (message_block.length() > m_WorldHeader.space() ? m_WorldHeader.space() : message_block.length());
                m_WorldHeader.copy(message_block.rd_ptr(), to_header);
                message_block.rd_ptr(to_header);

                if (m_WorldHeader.space() > 0)
                {
                    // Couldn't receive the whole header this time.
                    ACE_ASSERT(message_block.length() == 0);
                    errno = EWOULDBLOCK;
                    return -1;
                }

                // We just received nice new header
                if (handle_input_header() == -1)
                {
                    ACE_ASSERT((errno != EWOULDBLOCK) && (errno != EAGAIN));
                    return -1;
                }
            }
        }
        else
        {
            if (m_Header.space() > 0)
            {
                // need to receive the header
                const size_t to_header = (message_block.length() > m_Header.space() ? m_Header.space() : message_block.length());
                m_Header.copy(message_block.rd_ptr(), to_header);
                message_block.rd_ptr(to_header);
                if (m_Header.space() > 0)
                {
                    // Couldn't receive the whole header this time.
                    ACE_ASSERT(message_block.length() == 0);
                    errno = EWOULDBLOCK;
                    return -1;
                }

                // We just received nice new header
                if (handle_input_header() == -1)
                {
                    ACE_ASSERT((errno != EWOULDBLOCK) && (errno != EAGAIN));
                    return -1;
                }
            }
        }

        // Its possible on some error situations that this happens
        // for example on closing when epoll receives more chunked data and stuff
        // hope this is not hack, as proper m_RecvWPct is asserted around
        if (!m_RecvWPct)
        {
            TC_LOG_ERROR("network", "Forcing close on input m_RecvWPct = NULL");
            errno = EINVAL;
            return -1;
        }

        // We have full read header, now check the data payload
        if (m_RecvPct.space() > 0)
        {
            //need more data in the payload
            const size_t to_data = (message_block.length() > m_RecvPct.space() ? m_RecvPct.space() : message_block.length());
            m_RecvPct.copy (message_block.rd_ptr(), to_data);
            message_block.rd_ptr (to_data);

            if (m_RecvPct.space() > 0)
            {
                // Couldn't receive the whole data this time.
                ACE_ASSERT (message_block.length() == 0);
                errno = EWOULDBLOCK;
                return -1;
            }
        }

        //just received fresh new payload
        if (handle_input_payload() == -1)
        {
            ACE_ASSERT ((errno != EWOULDBLOCK) && (errno != EAGAIN));
            return -1;
        }
    }

    return size_t(n) == recv_size ? 1 : 2;
}

int WorldSocket::cancel_wakeup_output (GuardType& g)
{
    if (!m_OutActive)
        return 0;

    m_OutActive = false;

    g.release();

    if (reactor()->cancel_wakeup
        (this, ACE_Event_Handler::WRITE_MASK) == -1)
    {
        // would be good to store errno from reactor with errno guard
        TC_LOG_ERROR("network", "WorldSocket::cancel_wakeup_output");
        return -1;
    }

    return 0;
}

int WorldSocket::schedule_wakeup_output (GuardType& g)
{
    if (m_OutActive)
        return 0;

    m_OutActive = true;

    g.release();

    if (reactor()->schedule_wakeup
        (this, ACE_Event_Handler::WRITE_MASK) == -1)
    {
        TC_LOG_ERROR("network", "WorldSocket::schedule_wakeup_output");
        return -1;
    }

    return 0;
}

int WorldSocket::ProcessIncoming(WorldPacket* new_pct)
{
    ACE_ASSERT (new_pct);

    // manage memory ;)
    ACE_Auto_Ptr<WorldPacket> aptr(new_pct);

    Opcodes opcode = PacketFilter::DropHighBytes(new_pct->GetOpcode());

    if (closing_)
        return -1;

    if (opcode != CMSG_MOVE_START_FORWARD)
    {
        #ifdef WIN32
        TC_LOG_INFO("opcode", "C->S: %s", GetOpcodeNameForLogging(opcode).c_str());
        #endif
        if (m_Session)
            if(Player* _player = m_Session->GetPlayer())
                if (sObjectMgr->IsPlayerInLogList(_player))
                {
                    // Dump received packet.
                    if (sPacketLog->CanLogPacket())
                        sPacketLog->LogPacket(*new_pct, CLIENT_TO_SERVER);
                    TC_LOG_DEBUG("dupe", "C->S: %s", GetOpcodeNameForLogging(opcode).c_str());
                }
    }

    try
    {
        switch (opcode)
        {
        case CMSG_PING:
            return HandlePing(*new_pct);
        case CMSG_AUTH_SESSION:
            if (m_Session)
            {
                TC_LOG_ERROR("network", "WorldSocket::ProcessIncoming: received duplicate CMSG_AUTH_SESSION from %s", m_Session->GetPlayerName(false).c_str());
                return -1;
            }

            sScriptMgr->OnPacketReceive(this, WorldPacket(*new_pct));
            return HandleAuthSession(*new_pct);
        case CMSG_KEEP_ALIVE:
            #ifdef WIN32
            TC_LOG_DEBUG("network", "%s", GetOpcodeNameForLogging(opcode).c_str());
            #endif
            sScriptMgr->OnPacketReceive(this, WorldPacket(*new_pct));
            return 0;
        case CMSG_LOG_DISCONNECT:
            new_pct->rfinish(); // contains uint32 disconnectReason;
            #ifdef WIN32
            TC_LOG_DEBUG("network", "%s", GetOpcodeNameForLogging(opcode).c_str());
            #endif
            sScriptMgr->OnPacketReceive(this, WorldPacket(*new_pct));
            return 0;
        case CMSG_REORDER_CHARACTERS:
            sScriptMgr->OnPacketReceive(this, WorldPacket(*new_pct));


            if (m_Session)
                if (OpcodeHandler* opHandle = opcodeTable[CMSG][CMSG_REORDER_CHARACTERS])
                    (m_Session->*opHandle->handler)(*new_pct);

            return 0;
            // not an opcode, client sends string "WORLD OF WARCRAFT CONNECTION - CLIENT TO SERVER" without opcode
            // first 4 bytes become the opcode (2 dropped)
        case MSG_VERIFY_CONNECTIVITY:
            {
                #ifdef WIN32
                TC_LOG_DEBUG("network", "%s", GetOpcodeNameForLogging(opcode).c_str());
                #endif
                sScriptMgr->OnPacketReceive(this, WorldPacket(*new_pct));
                std::string str;
                *new_pct >> str;
                if (str != "D OF WARCRAFT CONNECTION - CLIENT TO SERVER")
                    return -1;
                return HandleSendAuthSession();
            }
        case CMSG_ENABLE_NAGLE:
            {
                #ifdef WIN32
                TC_LOG_DEBUG("network", "%s", GetOpcodeNameForLogging(opcode).c_str());
                #endif
                sScriptMgr->OnPacketReceive(this, WorldPacket(*new_pct));
                return m_Session ? m_Session->HandleEnableNagleAlgorithm() : -1;
            }
        default:
            {
                ACE_GUARD_RETURN(LockType, Guard, m_SessionLock, -1);
                if (!m_Session)
                {
                    TC_LOG_ERROR("opcode", "ProcessIncoming: Client not authed opcode = %u", uint32(opcode));
                    return -1;
                }

                // prevent invalid memory access/crash with custom opcodes
                if (opcode >= NUM_OPCODE_HANDLERS)
                    return 0;

                OpcodeHandler* handler = opcodeTable[CMSG][opcode];
                if (!handler || handler->status == STATUS_UNHANDLED)
                {
                    #ifdef WIN32
                    TC_LOG_ERROR("opcode", "No defined handler for opcode %s sent by %s", GetOpcodeNameForLogging(new_pct->GetOpcode()).c_str(), m_Session->GetPlayerName(false).c_str());
                    #endif
                    return 0;
                }

                // Our Idle timer will reset on any non PING opcodes.
                // Catches people idling on the login screen and any lingering ingame connections.
                m_Session->ResetTimeOutTime();

                // OK, give the packet to WorldSession
                aptr.release();
                // WARNING here we call it with locks held.
                // Its possible to cause deadlock if QueuePacket calls back
                bool deletePacket = true;
                m_Session->QueuePacket (new_pct, deletePacket);
                return 0;
            }
        }
    }
    catch (ByteBufferException &)
    {
        TC_LOG_ERROR("network", "WorldSocket::ProcessIncoming ByteBufferException occured while parsing an instant handled packet %s from client %s, accountid=%i. Disconnected client.",
            GetOpcodeNameForLogging(opcode).c_str(), GetRemoteAddress().c_str(), m_Session ? int32(m_Session->GetAccountId()) : -1);
        new_pct->hexlike();
        return -1;
    }

    ACE_NOTREACHED (return 0);
}

int WorldSocket::HandleSendAuthSession()
{
    WorldPacket packet(SMSG_AUTH_CHALLENGE, 37);

    packet << uint16(0);                                    // crap
    packet << uint8(1);

    BigNumber seed1;
    seed1.SetRand(16 * 8);
    packet.append(seed1.AsByteArray(16), 16);               // new encryption seeds

    BigNumber seed2;
    seed2.SetRand(16 * 8);
    packet.append(seed2.AsByteArray(16), 16);               // new encryption seeds

    packet << m_Seed;

    return SendPacket(&packet);
}

int WorldSocket::HandleAuthSession(WorldPacket& recvPacket)
{
    uint8 digest[20];
    uint32 clientSeed;
    uint8 security;
    uint16 clientBuild;
    uint32 id;
    uint32 addonSize;
    LocaleConstant locale;
    std::string account;
    SHA1Hash sha;
    BigNumber k;
    WorldPacket addonsData;

    recvPacket.read_skip<uint32>();
    recvPacket.read_skip<uint32>();
    recvPacket >> digest[18];
    recvPacket >> digest[14];
    recvPacket >> digest[3];
    recvPacket >> digest[4];
    recvPacket >> digest[0];
    recvPacket.read_skip<uint32>();
    recvPacket >> digest[11];
    recvPacket >> clientSeed;
    recvPacket >> digest[19];
    recvPacket.read_skip<uint8>();
    recvPacket.read_skip<uint8>();
    recvPacket >> digest[2];
    recvPacket >> digest[9];
    recvPacket >> digest[12];
    recvPacket.read_skip<uint64>();
    recvPacket.read_skip<uint32>();
    recvPacket >> digest[16];
    recvPacket >> digest[5];
    recvPacket >> digest[6];
    recvPacket >> digest[8];
    recvPacket >> clientBuild;
    recvPacket >> digest[17];
    recvPacket >> digest[7];
    recvPacket >> digest[13];
    recvPacket >> digest[15];
    recvPacket >> digest[1];
    recvPacket >> digest[10];
    recvPacket >> addonSize;

    recvPacket >> addonSize;
    addonsData.resize(addonSize);
    recvPacket.read((uint8*)addonsData.contents(), addonSize);

    uint32 accountNameLength = recvPacket.ReadBits(11);
    account = recvPacket.ReadString(accountNameLength);
    
    // if (ConfigMgr::GetBoolDefault("Login.with.email", false))
    {
        const char* c_login = account.c_str();
        if (c_login)
        {
            char* sobaka = strchr((char*)c_login, '@');
            if (sobaka != NULL) // email
            {
                PreparedStatement* stmt1 = LoginDatabase.GetPreparedStatement(LOGIN_SEL_ACCOUNT_BY_EMAIL);
                stmt1->setString(0, account);
                if (PreparedQueryResult result = LoginDatabase.Query(stmt1)) // select acc for auth process
                {
                    Field* field = result->Fetch();
                    account = field[0].GetString();
                }
            }
        }
    } 

    if (sWorld->IsClosed())
    {
        SendAuthResponseError(AUTH_REJECT);
        TC_LOG_ERROR("network", "WorldSocket::HandleAuthSession: World closed, denying client (%s).", GetRemoteAddress().c_str());
        return -1;
    }

    // Get the account information from the realmd database
    PreparedStatement* stmt = LoginDatabase.GetPreparedStatement(LOGIN_SEL_ACCOUNT_INFO_BY_NAME);

    stmt->setString(0, account);

    PreparedQueryResult result = LoginDatabase.Query(stmt);

    // Stop if the account is not found
    if (!result)
    {
        SendAuthResponseError(AUTH_UNKNOWN_ACCOUNT);
        TC_LOG_ERROR("network", "WorldSocket::HandleAuthSession: Sent Auth Response (unknown account).");
        return -1;
    }

    Field* fields = result->Fetch();

    //uint8 expansion = fields[6].GetUInt8();
    uint8 expansion = 4;
    uint32 world_expansion = sWorld->getIntConfig(CONFIG_EXPANSION);
    if (expansion > world_expansion)
        expansion = world_expansion;

    TC_LOG_DEBUG("network", "WorldSocket::HandleAuthSession: (s,v) check s: %s v: %s",
        fields[5].GetCString(),
        fields[4].GetCString());

    ///- Re-check ip locking (same check as in realmd).
    if (fields[3].GetUInt8() == 1) // if ip is locked
    {
        if (strcmp (fields[2].GetCString(), GetRemoteAddress().c_str()))
        {
            SendAuthResponseError(AUTH_FAILED);
            TC_LOG_DEBUG("network", "WorldSocket::HandleAuthSession: Sent Auth Response (Account IP differs).");
            return -1;
        }
    }

    id = fields[0].GetUInt32();
    /*
    if (security > SEC_ADMINISTRATOR)                        // prevent invalid security settings in DB
    security = SEC_ADMINISTRATOR;
    */

    k.SetHexStr(fields[1].GetCString());

    int64 mutetime = fields[7].GetInt64();
    //! Negative mutetime indicates amount of seconds to be muted effective on next login - which is now.
    if (mutetime < 0)
    {
        mutetime = time(NULL) + llabs(mutetime);

        PreparedStatement* stmt = LoginDatabase.GetPreparedStatement(LOGIN_UPD_MUTE_TIME);

        stmt->setInt64(0, mutetime);
        stmt->setUInt32(1, id);

        LoginDatabase.Execute(stmt);
    }

    locale = LocaleConstant (fields[8].GetUInt8());
    if (locale >= TOTAL_LOCALES)
        locale = LOCALE_enUS;

    uint32 recruiter = fields[9].GetUInt32();
    std::string os = fields[10].GetString();

    // Checks gmlevel per Realm
    stmt = LoginDatabase.GetPreparedStatement(LOGIN_GET_GMLEVEL_BY_REALMID);

    stmt->setUInt32(0, id);
    stmt->setInt32(1, int32(realmID));

    result = LoginDatabase.Query(stmt);

    if (!result)
        security = 0;
    else
    {
        fields = result->Fetch();
        security = fields[0].GetUInt8();
    }

    // Re-check account ban (same check as in realmd)
    stmt = LoginDatabase.GetPreparedStatement(LOGIN_SEL_BANS);

    stmt->setUInt32(0, id);
    stmt->setString(1, GetRemoteAddress());

    PreparedQueryResult banresult = LoginDatabase.Query(stmt);

    if (banresult) // if account banned
    {
        SendAuthResponseError(AUTH_BANNED);
        TC_LOG_ERROR("network", "WorldSocket::HandleAuthSession: Sent Auth Response (Account banned).");
        return -1;
    }

    // Check locked state for server
    AccountTypes allowedAccountType = sWorld->GetPlayerSecurityLimit();
    TC_LOG_DEBUG("network", "Allowed Level: %u Player Level %u", allowedAccountType, AccountTypes(security));
    if (allowedAccountType > SEC_PLAYER && AccountTypes(security) < allowedAccountType)
    {
        SendAuthResponseError(AUTH_UNAVAILABLE);
        TC_LOG_INFO("network", "WorldSocket::HandleAuthSession: User tries to login but his security level is not enough");
        return -1;
    }

    // Check that Key and account name are the same on client and server
    uint32 t = 0;
    uint32 seed = m_Seed;

    sha.UpdateData(account);
    sha.UpdateData((uint8*)&t, 4);
    sha.UpdateData((uint8*)&clientSeed, 4);
    sha.UpdateData((uint8*)&seed, 4);
    sha.UpdateBigNumbers(&k, NULL);
    sha.Finalize();

    std::string address = GetRemoteAddress();

    /*if (memcmp(sha.GetDigest(), digest, 20))
    {
    WorldPacket packet(SMSG_AUTH_RESPONSE, 1);
    packet.WriteBit(0); // has queue info
    packet.WriteBit(0); // has account info
    packet << uint8(AUTH_FAILED);
    SendPacket(packet);

    TC_LOG_ERROR("network", "WorldSocket::HandleAuthSession: Authentication failed for account: %u ('%s') address: %s", id, account.c_str(), address.c_str());
    return -1;
    }*/

    TC_LOG_DEBUG("network", "WorldSocket::HandleAuthSession: Client '%s' authenticated successfully from %s.",
        account.c_str(),
        address.c_str());

    // Check if this user is by any chance a recruiter
    stmt = LoginDatabase.GetPreparedStatement(LOGIN_SEL_ACCOUNT_RECRUITER);

    stmt->setUInt32(0, id);

    result = LoginDatabase.Query(stmt);

    bool isRecruiter = false;
    if (result)
        isRecruiter = true;

    // Update the last_ip in the database

    stmt = LoginDatabase.GetPreparedStatement(LOGIN_UPD_LAST_IP);

    stmt->setString(0, address);
    stmt->setString(1, account);

    LoginDatabase.Execute(stmt);

    // NOTE ATM the socket is single-threaded, have this in mind ...
    ACE_NEW_RETURN(m_Session, WorldSession(id, account, this, AccountTypes(security), expansion, mutetime, locale, recruiter, isRecruiter), -1);

    m_Crypt.Init(&k);

    m_Session->LoadGlobalAccountData();
    m_Session->LoadTutorialsData();
    m_Session->ReadAddonsInfo(addonsData);

    m_Session->SetOS(os);

    // Initialize Warden system only if it is enabled by config
    if (sWorld->getBoolConfig(CONFIG_WARDEN_ENABLED))
    {
        if (!m_Session->InitWarden(&k, os))
        {
            TC_LOG_ERROR("network", "WorldSocket::HandleAuthSession: Warden initializtion failed for account: %u ('%s') address: %s", id, account.c_str(), address.c_str());
            return -1;
        }
    }

    // Sleep this Network thread for
    uint32 sleepTime = sWorld->getIntConfig(CONFIG_SESSION_ADD_DELAY);
    ACE_OS::sleep(ACE_Time_Value(0, sleepTime));

    sWorld->AddSession(m_Session);
    return 0;
}

int WorldSocket::HandlePing (WorldPacket& recvPacket)
{
    uint32 ping;
    uint32 latency;

    // Get the ping packet content
    recvPacket >> latency;
    recvPacket >> ping;

    //TC_LOG_DEBUG("network", "WorldSocket::HandlePing: latency %u ping %u", latency, ping);

    if (m_LastPingTime == ACE_Time_Value::zero)
        m_LastPingTime = ACE_OS::gettimeofday(); // for 1st ping
    else
    {
        ACE_Time_Value cur_time = ACE_OS::gettimeofday();
        ACE_Time_Value diff_time (cur_time);
        diff_time -= m_LastPingTime;
        m_LastPingTime = cur_time;

        if (diff_time < ACE_Time_Value (27))
        {
            ++m_OverSpeedPings;

            uint32 max_count = sWorld->getIntConfig (CONFIG_MAX_OVERSPEED_PINGS);

            if (max_count && m_OverSpeedPings > max_count)
            {
                ACE_GUARD_RETURN (LockType, Guard, m_SessionLock, -1);

                if (m_Session && AccountMgr::IsPlayerAccount(m_Session->GetSecurity()))
                {
                    TC_LOG_ERROR("network", "WorldSocket::HandlePing: %s kicked for over-speed pings (address: %s)",
                        m_Session->GetPlayerName(false).c_str(), GetRemoteAddress().c_str());

                    return -1;
                }
            }
        }
        else
            m_OverSpeedPings = 0;
    }

    // critical section
    {
        ACE_GUARD_RETURN (LockType, Guard, m_SessionLock, -1);

        if (m_Session)
            m_Session->SetLatency (latency);
        else
        {
            TC_LOG_ERROR("network", "WorldSocket::HandlePing: peer sent CMSG_PING, "
                "but is not authenticated or got recently kicked, "
                " address = %s",
                GetRemoteAddress().c_str());
            return -1;
        }
    }

    WorldPacket packet(SMSG_PONG, 4);
    packet << ping;
    return SendPacket(&packet);
}

void WorldSocket::SendAuthResponseError(uint8 code)
{
        WorldPacket packet(SMSG_AUTH_RESPONSE, 1);
        packet.WriteBit(0); // has account info
        packet.WriteBit(0); // has queue info
        packet << uint8(code);
        SendPacket(&packet);
}
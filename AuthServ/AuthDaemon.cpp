/******************************************************************************
 * This file is part of dirtsand.                                             *
 *                                                                            *
 * dirtsand is free software: you can redistribute it and/or modify           *
 * it under the terms of the GNU General Public License as published by       *
 * the Free Software Foundation, either version 3 of the License, or          *
 * (at your option) any later version.                                        *
 *                                                                            *
 * dirtsand is distributed in the hope that it will be useful,                *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of             *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the              *
 * GNU General Public License for more details.                               *
 *                                                                            *
 * You should have received a copy of the GNU General Public License          *
 * along with dirtsand.  If not, see <http://www.gnu.org/licenses/>.          *
 ******************************************************************************/

#include "AuthServer_Private.h"
#include "encodings.h"
#include "settings.h"
#include "errors.h"
#include <unistd.h>

pthread_t s_authDaemonThread;
DS::MsgChannel s_authChannel;
PGconn* s_postgres;

#define SEND_REPLY(msg, result) \
    msg->m_client->m_channel.putMessage(result)

static inline void check_postgres()
{
    if (PQstatus(s_postgres) == CONNECTION_BAD)
        PQreset(s_postgres);
    DS_DASSERT(PQstatus(s_postgres) == CONNECTION_OK);
}

void dm_auth_addacct(Auth_AccountInfo* info)
{
    DS::StringBuffer<chr8_t> pwBuf = info->m_password.toUtf8();
    DS::ShaHash pwHash = DS::ShaHash::Sha1(pwBuf.data(), pwBuf.length());
    PostgresStrings<3> iparms;
    iparms.set(0, gen_uuid().toString());
    iparms.set(1, pwHash.toString());
    iparms.set(2, info->m_acctName);
    PGresult* result = PQexecParams(s_postgres,
            "INSERT INTO auth.\"Accounts\""
            "    (\"AcctUuid\", \"PassHash\", \"Login\", \"AcctFlags\", \"BillingType\")"
            "    VALUES ($1, $2, $3, 0, 1)"
            "    RETURNING idx",
            3, 0, iparms.m_values, 0, 0, 0);
    delete info;
    if (PQresultStatus(result) != PGRES_TUPLES_OK) {
        fprintf(stderr, "%s:%d:\n    Postgres INSERT error: %s\n",
                __FILE__, __LINE__, PQerrorMessage(s_postgres));
        PQclear(result);
        return;
    }
    DS_DASSERT(PQntuples(result) == 1);
    PQclear(result);
}

void dm_auth_shutdown()
{
    pthread_mutex_lock(&s_authClientMutex);
    std::list<AuthServer_Private*>::iterator client_iter;
    for (client_iter = s_authClients.begin(); client_iter != s_authClients.end(); ++client_iter)
        DS::CloseSock((*client_iter)->m_sock);
    pthread_mutex_unlock(&s_authClientMutex);

    bool complete = false;
    for (int i=0; i<50 && !complete; ++i) {
        pthread_mutex_lock(&s_authClientMutex);
        size_t alive = s_authClients.size();
        pthread_mutex_unlock(&s_authClientMutex);
        if (alive == 0)
            complete = true;
        usleep(100000);
    }
    if (!complete)
        fprintf(stderr, "[Auth] Clients didn't die after 5 seconds!\n");

    pthread_mutex_destroy(&s_authClientMutex);
    PQfinish(s_postgres);
}

void dm_auth_login(Auth_LoginInfo* info)
{
    check_postgres();

#ifdef DEBUG
    printf("[Auth] Login U:%s P:%s T:%s O:%s\n",
           info->m_acctName.c_str(), info->m_passHash.toString().c_str(),
           info->m_token.c_str(), info->m_os.c_str());
#endif

    // Reset UUID in case authentication fails
    AuthServer_Private* client = reinterpret_cast<AuthServer_Private*>(info->m_client);
    client->m_acctUuid = DS::Uuid();

    PostgresStrings<1> parm;
    parm.set(0, info->m_acctName);
    PGresult* result = PQexecParams(s_postgres,
            "SELECT \"PassHash\", \"AcctUuid\", \"AcctFlags\", \"BillingType\""
            "    FROM auth.\"Accounts\""
            "    WHERE LOWER(\"Login\")=LOWER($1)",
            1, 0, parm.m_values, 0, 0, 0);
    if (PQresultStatus(result) != PGRES_TUPLES_OK) {
        fprintf(stderr, "%s:%d:\n    Postgres SELECT error: %s\n",
                __FILE__, __LINE__, PQerrorMessage(s_postgres));
        PQclear(result);
        SEND_REPLY(info, DS::e_NetInternalError);
        return;
    }
    if (PQntuples(result) == 0) {
        printf("[Auth] %s: Account %s does not exist\n",
               DS::SockIpAddress(info->m_client->m_sock).c_str(),
               info->m_acctName.c_str());
        PQclear(result);
        // This should be NetAccountNotFound, but that's technically a
        // security flaw...
        SEND_REPLY(info, DS::e_NetAuthenticationFailed);
        return;
    }
#ifdef DEBUG
    if (PQntuples(result) != 1) {
        PQclear(result);
        DS_PASSERT(0);
    }
#endif

    DS::ShaHash passhash = PQgetvalue(result, 0, 0);
    if (info->m_acctName.find("@") != -1 && info->m_acctName.find("@gametap") == -1) {
        DS::ShaHash challengeHash = DS::BuggyHashLogin(passhash,
                client->m_serverChallenge, info->m_clientChallenge);
        if (challengeHash != info->m_passHash) {
            printf("[Auth] %s: Failed login to account %s\n",
                   DS::SockIpAddress(info->m_client->m_sock).c_str(),
                   info->m_acctName.c_str());
            PQclear(result);
            SEND_REPLY(info, DS::e_NetAuthenticationFailed);
            return;
        }
    } else {
        // In this case, the Sha1 hash is Big Endian...  Yeah, really...
        info->m_passHash.swapBytes();
        if (passhash != info->m_passHash) {
            printf("[Auth] %s: Failed login to account %s\n",
                   DS::SockIpAddress(info->m_client->m_sock).c_str(),
                   info->m_acctName.c_str());
            PQclear(result);
            SEND_REPLY(info, DS::e_NetAuthenticationFailed);
            return;
        }
    }

    client->m_acctUuid = DS::Uuid(PQgetvalue(result, 0, 1));
    info->m_acctFlags = strtoul(PQgetvalue(result, 0, 2), 0, 10);
    info->m_billingType = strtoul(PQgetvalue(result, 0, 3), 0, 10);
    printf("[Auth] %s logged in as %s {%s}\n",
           DS::SockIpAddress(info->m_client->m_sock).c_str(),
           info->m_acctName.c_str(), client->m_acctUuid.toString().c_str());
    PQclear(result);

    // Get list of players
    DS::String uuidString = client->m_acctUuid.toString();
    parm.m_values[0] = uuidString.c_str();
    result = PQexecParams(s_postgres,
            "SELECT \"PlayerIdx\", \"PlayerName\", \"AvatarShape\", \"Explorer\""
            "    FROM auth.\"Players\""
            "    WHERE \"AcctUuid\"=$1",
            1, 0, parm.m_values, 0, 0, 0);
    if (PQresultStatus(result) != PGRES_TUPLES_OK) {
        fprintf(stderr, "%s:%d:\n    Postgres SELECT error: %s\n",
                __FILE__, __LINE__, PQerrorMessage(s_postgres));
        PQclear(result);
        SEND_REPLY(info, DS::e_NetInternalError);
        return;
    }
    info->m_players.resize(PQntuples(result));
    for (size_t i = 0; i < info->m_players.size(); ++i) {
        info->m_players[i].m_playerId = strtoul(PQgetvalue(result, i, 0), 0, 10);
        info->m_players[i].m_playerName = PQgetvalue(result, i, 1);
        info->m_players[i].m_avatarModel = PQgetvalue(result, i, 2);
        info->m_players[i].m_explorer = strtoul(PQgetvalue(result, i, 3), 0, 10);
    }
    PQclear(result);

    SEND_REPLY(info, DS::e_NetSuccess);
}

void dm_auth_disconnect(Auth_ClientMessage* msg)
{
    AuthServer_Private* client = reinterpret_cast<AuthServer_Private*>(msg->m_client);
    if (client->m_player.m_playerId) {
        // Mark player as offline
        check_postgres();
        PostgresStrings<2> parms;
        parms.set(0, DS::Vault::e_NodePlayerInfo);
        parms.set(1, client->m_player.m_playerId);
        PGresult* result = PQexecParams(s_postgres,
                "UPDATE vault.\"Nodes\" SET"
                "    \"Int32_1\"=0, \"String64_1\"='',"
                "    \"Uuid_1\"='00000000-0000-0000-0000-000000000000'"
                "    WHERE \"NodeType\"=$1 AND \"Uint32_1\"=$2",
                2, 0, parms.m_values, 0, 0, 0);
        if (PQresultStatus(result) != PGRES_COMMAND_OK) {
            fprintf(stderr, "%s:%d:\n    Postgres UPDATE error: %s\n",
                    __FILE__, __LINE__, PQerrorMessage(s_postgres));
            // This doesn't block continuing...
        }
        PQclear(result);
    }
    SEND_REPLY(msg, DS::e_NetSuccess);
}

void dm_auth_setPlayer(Auth_ClientMessage* msg)
{
    check_postgres();

    AuthServer_Private* client = reinterpret_cast<AuthServer_Private*>(msg->m_client);
    PostgresStrings<2> parms;
    parms.set(0, client->m_acctUuid.toString());
    parms.set(1, client->m_player.m_playerId);
    PGresult* result = PQexecParams(s_postgres,
            "SELECT \"PlayerName\", \"AvatarShape\", \"Explorer\""
            "    FROM auth.\"Players\""
            "    WHERE \"AcctUuid\"=$1 AND \"PlayerIdx\"=$2",
            2, 0, parms.m_values, 0, 0, 0);
    if (PQresultStatus(result) != PGRES_TUPLES_OK) {
        fprintf(stderr, "%s:%d:\n    Postgres SELECT error: %s\n",
                __FILE__, __LINE__, PQerrorMessage(s_postgres));
        PQclear(result);
        client->m_player.m_playerId = 0;
        SEND_REPLY(msg, DS::e_NetInternalError);
        return;
    }
    if (PQntuples(result) == 0) {
        printf("[Auth] {%s} requested invalid player ID (%u)\n",
               client->m_acctUuid.toString().c_str(),
               client->m_player.m_playerId);
        PQclear(result);
        client->m_player.m_playerId = 0;
        SEND_REPLY(msg, DS::e_NetPlayerNotFound);
        return;
    }

#ifdef DEBUG
    if (PQntuples(result) != 1) {
        PQclear(result);
        client->m_player.m_playerId = 0;
        DS_PASSERT(0);
    }
#endif

    pthread_mutex_lock(&s_authClientMutex);
    std::list<AuthServer_Private*>::iterator client_iter;
    for (client_iter = s_authClients.begin(); client_iter != s_authClients.end(); ++client_iter) {
        if (client != *client_iter && (*client_iter)->m_player.m_playerId == client->m_player.m_playerId) {
            printf("[Auth] {%s} requested already-active player (%u)\n",
                   client->m_acctUuid.toString().c_str(),
                   client->m_player.m_playerId);
            PQclear(result);
            client->m_player.m_playerId = 0;
            SEND_REPLY(msg, DS::e_NetLoggedInElsewhere);
            pthread_mutex_unlock(&s_authClientMutex);
            return;
        }
    }
    pthread_mutex_unlock(&s_authClientMutex);

    client->m_player.m_playerName = PQgetvalue(result, 0, 0);
    client->m_player.m_avatarModel = PQgetvalue(result, 0, 1);
    client->m_player.m_explorer = strtoul(PQgetvalue(result, 0, 2), 0, 10);
    PQclear(result);

    // Mark player as online
    parms.set(0, DS::Vault::e_NodePlayerInfo);
    parms.set(1, client->m_player.m_playerId);
    result = PQexecParams(s_postgres,
            "UPDATE vault.\"Nodes\" SET"
            "    \"Int32_1\"=1, \"String64_1\"='Lobby',"
            "    \"Uuid_1\"='00000000-0000-0000-0000-000000000000'"
            "    WHERE \"NodeType\"=$1 AND \"Uint32_1\"=$2",
            2, 0, parms.m_values, 0, 0, 0);
    if (PQresultStatus(result) != PGRES_COMMAND_OK) {
        fprintf(stderr, "%s:%d:\n    Postgres UPDATE error: %s\n",
                __FILE__, __LINE__, PQerrorMessage(s_postgres));
        // This doesn't block continuing...
    }
    PQclear(result);

    printf("[Auth] {%s} signed in as %s (%u)\n",
           client->m_acctUuid.toString().c_str(),
           client->m_player.m_playerName.c_str(),
           client->m_player.m_playerId);
    SEND_REPLY(msg, DS::e_NetSuccess);
}

void dm_auth_createPlayer(Auth_PlayerCreate* msg)
{
    if (msg->m_player.m_avatarModel != "male" && msg->m_player.m_avatarModel != "female") {
        // Cheater!
        msg->m_player.m_avatarModel = "male";
    }

    // Check for existing player
    PostgresStrings<1> sparms;
    sparms.set(0, msg->m_player.m_playerName);
    PGresult* result = PQexecParams(s_postgres,
            "SELECT idx FROM auth.\"Players\""
            "    WHERE \"PlayerName\"=$1",
            1, 0, sparms.m_values, 0, 0, 0);
    if (PQresultStatus(result) != PGRES_TUPLES_OK) {
        fprintf(stderr, "%s:%d:\n    Postgres SELECT error: %s\n",
                __FILE__, __LINE__, PQerrorMessage(s_postgres));
        PQclear(result);
        SEND_REPLY(msg, DS::e_NetInternalError);
        return;
    }
    if (PQntuples(result) != 0) {
        fprintf(stderr, "[Auth] %s: Player %s already exists!\n",
                DS::SockIpAddress(msg->m_client->m_sock).c_str(),
                msg->m_player.m_playerName.c_str());
        PQclear(result);
        SEND_REPLY(msg, DS::e_NetPlayerAlreadyExists);
        return;
    }
    PQclear(result);

    AuthServer_Private* client = reinterpret_cast<AuthServer_Private*>(msg->m_client);
    msg->m_player.m_playerId = v_create_player(client->m_acctUuid, msg->m_player).first;
    if (msg->m_player.m_playerId == 0)
        SEND_REPLY(msg, DS::e_NetInternalError);

    PostgresStrings<5> iparms;
    iparms.set(0, client->m_acctUuid.toString());
    iparms.set(1, msg->m_player.m_playerId);
    iparms.set(2, msg->m_player.m_playerName);
    iparms.set(3, msg->m_player.m_avatarModel);
    iparms.set(4, msg->m_player.m_explorer);
    result = PQexecParams(s_postgres,
            "INSERT INTO auth.\"Players\""
            "    (\"AcctUuid\", \"PlayerIdx\", \"PlayerName\", \"AvatarShape\", \"Explorer\")"
            "    VALUES ($1, $2, $3, $4, $5)"
            "    RETURNING idx",
            5, 0, iparms.m_values, 0, 0, 0);
    if (PQresultStatus(result) != PGRES_TUPLES_OK) {
        fprintf(stderr, "%s:%d:\n    Postgres INSERT error: %s\n",
                __FILE__, __LINE__, PQerrorMessage(s_postgres));
        PQclear(result);
        SEND_REPLY(msg, DS::e_NetInternalError);
        return;
    }
    DS_DASSERT(PQntuples(result) == 1);
    PQclear(result);
    SEND_REPLY(msg, DS::e_NetSuccess);
}

void dm_auth_createAge(Auth_AgeCreate* msg)
{
    std::pair<uint32_t, uint32_t> ageNodes = v_create_age(msg->m_age, 0);
    if (ageNodes.first == 0)
        SEND_REPLY(msg, DS::e_NetInternalError);

    msg->m_ageIdx = ageNodes.first;
    msg->m_infoIdx = ageNodes.second;
    SEND_REPLY(msg, DS::e_NetSuccess);
}

void dm_auth_findAge(Auth_GameAge* msg)
{
#ifdef DEBUG
    printf("[Auth] %s Requesting game server {%s} %s\n",
           DS::SockIpAddress(msg->m_client->m_sock).c_str(),
           msg->m_instanceId.toString().c_str(), msg->m_name.c_str());
#endif

    PostgresStrings<4> parms;
    parms.set(0, msg->m_instanceId.toString());
    PGresult* result = PQexecParams(s_postgres,
            "SELECT idx, \"AgeIdx\", \"DisplayName\" FROM game.\"Servers\""
            "    WHERE \"AgeUuid\"=$1",
            1, 0, parms.m_values, 0, 0, 0);
    if (PQresultStatus(result) != PGRES_TUPLES_OK) {
        fprintf(stderr, "%s:%d:\n    Postgres SELECT error: %s\n",
                __FILE__, __LINE__, PQerrorMessage(s_postgres));
        PQclear(result);
        SEND_REPLY(msg, DS::e_NetInternalError);
        return;
    }
    DS::String ageDesc;
    if (PQntuples(result) == 0) {
        fprintf(stderr, "[Auth] %s Requested age {%s} %s not found\n",
                DS::SockIpAddress(msg->m_client->m_sock).c_str(),
                msg->m_instanceId.toString().c_str(), msg->m_name.c_str());
        SEND_REPLY(msg, DS::e_NetAgeNotFound);
        PQclear(result);
        return;
    } else {
        DS_DASSERT(PQntuples(result) == 1);
        msg->m_ageNodeIdx = strtoul(PQgetvalue(result, 0, 1), 0, 10);
        msg->m_mcpId = strtoul(PQgetvalue(result, 0, 0), 0, 10);
        msg->m_serverAddress = DS::GetAddress4(DS::Settings::GameServerAddress().c_str());
        ageDesc = PQgetvalue(result, 0, 2);
    }
    PQclear(result);

    // Update the player info to show up in the age
    parms.set(0, DS::Vault::e_NodePlayerInfo);
    parms.set(1, reinterpret_cast<AuthServer_Private*>(msg->m_client)->m_player.m_playerId);
    parms.set(2, ageDesc);
    parms.set(3, msg->m_instanceId.toString());
    result = PQexecParams(s_postgres,
            "UPDATE vault.\"Nodes\" SET"
            "    \"String64_1\"=$3, \"Uuid_1\"=$4"
            "    WHERE \"NodeType\"=$1 AND \"Uint32_1\"=$2",
            4, 0, parms.m_values, 0, 0, 0);
    if (PQresultStatus(result) != PGRES_COMMAND_OK) {
        fprintf(stderr, "%s:%d:\n    Postgres UPDATE error: %s\n",
                __FILE__, __LINE__, PQerrorMessage(s_postgres));
        // This doesn't block continuing...
    }
    PQclear(result);
    SEND_REPLY(msg, DS::e_NetSuccess);
}

void dm_auth_get_public(Auth_PubAgeRequest* msg) {
    PostgresStrings<4> parms;
    parms.set(0, msg->m_agename);
    PGresult* result = PQexecParams(s_postgres,
                                    "SELECT idx, \"AgeUuid\", \"AgeInstName\", \"AgeUserName\", \"AgeDesc\", \"SeqNumber\", \"Language\", \"Population\" FROM game.\"PublicAges\""
                                    "    WHERE \"AgeFilename\"=$1",
                                    1, 0, parms.m_values, 0, 0, 0);
    if (PQresultStatus(result) != PGRES_TUPLES_OK) {
      fprintf(stderr, "%s:%d:\n    Postgres SELECT error: %s\n",
              __FILE__, __LINE__, PQerrorMessage(s_postgres));
      PQclear(result);
      SEND_REPLY(msg, DS::e_NetInternalError);
      return;
    }
    for (int i = 0; i < PQntuples(result); i++) {
      Auth_PubAgeRequest::pnNetAgeInfo ai;
      ai.m_instance = DS::Uuid(PQgetvalue(result, 0, 1));
      ai.m_instancename = PQgetvalue(result, 0, 2);
      ai.m_username = PQgetvalue(result, 0, 3);
      ai.m_description = PQgetvalue(result, 0, 4);
      ai.m_sequence = strtoul(PQgetvalue(result, 0, 5), 0, 10);
      ai.m_language = strtoul(PQgetvalue(result, 0, 6), 0, 10);
      ai.m_population = strtoul(PQgetvalue(result, 0, 7), 0, 10);
      msg->m_ages.push_back(ai);
    }
    PQclear(result);
    SEND_REPLY(msg, DS::e_NetSuccess);
}

void dm_auth_bcast_node(uint32_t nodeIdx, const DS::Uuid& revision)
{
    uint8_t buffer[22];  // Msg ID, Node ID, Revision Uuid
    *reinterpret_cast<uint16_t*>(buffer    ) = e_AuthToCli_VaultNodeChanged;
    *reinterpret_cast<uint32_t*>(buffer + 2) = nodeIdx;
    *reinterpret_cast<DS::Uuid*>(buffer + 6) = revision;

    pthread_mutex_lock(&s_authClientMutex);
    std::list<AuthServer_Private*>::iterator client_iter;
    for (client_iter = s_authClients.begin(); client_iter != s_authClients.end(); ++client_iter) {
        try {
            DS::CryptSendBuffer((*client_iter)->m_sock, (*client_iter)->m_crypt, buffer, 22);
        } catch (DS::SockHup) {
            // Client ignored us.  Return the favor
        }
    }
    pthread_mutex_unlock(&s_authClientMutex);
}

void dm_auth_bcast_ref(const DS::Vault::NodeRef& ref)
{
    uint8_t buffer[14];  // Msg ID, Parent, Child, Owner
    *reinterpret_cast<uint16_t*>(buffer     ) = e_AuthToCli_VaultNodeAdded;
    *reinterpret_cast<uint32_t*>(buffer +  2) = ref.m_parent;
    *reinterpret_cast<uint32_t*>(buffer +  6) = ref.m_child;
    *reinterpret_cast<uint32_t*>(buffer + 10) = ref.m_owner;

    pthread_mutex_lock(&s_authClientMutex);
    std::list<AuthServer_Private*>::iterator client_iter;
    for (client_iter = s_authClients.begin(); client_iter != s_authClients.end(); ++client_iter) {
        try {
            DS::CryptSendBuffer((*client_iter)->m_sock, (*client_iter)->m_crypt, buffer, 14);
        } catch (DS::SockHup) {
            // Client ignored us.  Return the favor
        }
    }
    pthread_mutex_unlock(&s_authClientMutex);
}

void dm_auth_bcast_unref(const DS::Vault::NodeRef& ref)
{
    uint8_t buffer[10];  // Msg ID, Parent, Child
    *reinterpret_cast<uint16_t*>(buffer    ) = e_AuthToCli_VaultNodeRemoved;
    *reinterpret_cast<uint32_t*>(buffer + 2) = ref.m_parent;
    *reinterpret_cast<uint32_t*>(buffer + 6) = ref.m_child;

    pthread_mutex_lock(&s_authClientMutex);
    std::list<AuthServer_Private*>::iterator client_iter;
    for (client_iter = s_authClients.begin(); client_iter != s_authClients.end(); ++client_iter) {
        try {
            DS::CryptSendBuffer((*client_iter)->m_sock, (*client_iter)->m_crypt, buffer, 10);
        } catch (DS::SockHup) {
            // Client ignored us.  Return the favor
        }
    }
    pthread_mutex_unlock(&s_authClientMutex);
}

void* dm_authDaemon(void*)
{
    s_postgres = PQconnectdb(DS::String::Format(
                    "host='%s' port='%s' user='%s' password='%s' dbname='%s'",
                    DS::Settings::DbHostname(), DS::Settings::DbPort(),
                    DS::Settings::DbUsername(), DS::Settings::DbPassword(),
                    DS::Settings::DbDbaseName()).c_str());
    if (PQstatus(s_postgres) != CONNECTION_OK) {
        fprintf(stderr, "Error connecting to postgres: %s", PQerrorMessage(s_postgres));
        PQfinish(s_postgres);
        s_postgres = 0;
        return 0;
    }

    if (!dm_vault_init()) {
        fprintf(stderr, "[Auth] Vault failed to initialize\n");
        return 0;
    }

    for ( ;; ) {
        DS::FifoMessage msg = s_authChannel.getMessage();
        try {
            switch (msg.m_messageType) {
            case e_AuthShutdown:
                dm_auth_shutdown();
                return 0;
            case e_AuthClientLogin:
                dm_auth_login(reinterpret_cast<Auth_LoginInfo*>(msg.m_payload));
                break;
            case e_AuthSetPlayer:
                dm_auth_setPlayer(reinterpret_cast<Auth_ClientMessage*>(msg.m_payload));
                break;
            case e_AuthCreatePlayer:
                dm_auth_createPlayer(reinterpret_cast<Auth_PlayerCreate*>(msg.m_payload));
                break;
            case e_VaultCreateNode:
                {
                    Auth_NodeInfo* info = reinterpret_cast<Auth_NodeInfo*>(msg.m_payload);
                    uint32_t nodeIdx = v_create_node(info->m_node);
                    if (nodeIdx != 0) {
                        info->m_node.set_NodeIdx(nodeIdx);
                        SEND_REPLY(info, DS::e_NetSuccess);
                    } else {
                        SEND_REPLY(info, DS::e_NetInternalError);
                    }
                }
                break;
            case e_VaultFetchNode:
                {
                    Auth_NodeInfo* info = reinterpret_cast<Auth_NodeInfo*>(msg.m_payload);
                    info->m_node = v_fetch_node(info->m_node.m_NodeIdx);
                    if (info->m_node.isNull())
                        SEND_REPLY(info, DS::e_NetVaultNodeNotFound);
                    else
                        SEND_REPLY(info, DS::e_NetSuccess);
                }
                break;
            case e_VaultUpdateNode:
                {
                    Auth_NodeInfo* info = reinterpret_cast<Auth_NodeInfo*>(msg.m_payload);
                    if (v_update_node(info->m_node)) {
                        // Broadcast the change
                        dm_auth_bcast_node(info->m_node.m_NodeIdx, info->m_revision);
                        SEND_REPLY(info, DS::e_NetSuccess);
                    } else {
                        SEND_REPLY(info, DS::e_NetInternalError);
                    }
                }
                break;
            case e_VaultRefNode:
                {
                    Auth_NodeRef* info = reinterpret_cast<Auth_NodeRef*>(msg.m_payload);
                    if (v_ref_node(info->m_ref.m_parent, info->m_ref.m_child, info->m_ref.m_owner)) {
                        // Broadcast the change
                        dm_auth_bcast_ref(info->m_ref);
                        SEND_REPLY(info, DS::e_NetSuccess);
                    } else {
                        SEND_REPLY(info, DS::e_NetInternalError);
                    }
                }
                break;
            case e_VaultUnrefNode:
                {
                    Auth_NodeRef* info = reinterpret_cast<Auth_NodeRef*>(msg.m_payload);
                    if (v_unref_node(info->m_ref.m_parent, info->m_ref.m_child)) {
                        // Broadcast the change
                        dm_auth_bcast_unref(info->m_ref);
                        SEND_REPLY(info, DS::e_NetSuccess);
                    } else {
                        SEND_REPLY(info, DS::e_NetInternalError);
                    }
                }
                break;
            case e_VaultFetchNodeTree:
                {
                    Auth_NodeRefList* info = reinterpret_cast<Auth_NodeRefList*>(msg.m_payload);
                    if (v_fetch_tree(info->m_nodeId, info->m_refs))
                        SEND_REPLY(info, DS::e_NetSuccess);
                    else
                        SEND_REPLY(info, DS::e_NetInternalError);
                }
                break;
            case e_VaultFindNode:
                {
                    Auth_NodeFindList* info = reinterpret_cast<Auth_NodeFindList*>(msg.m_payload);
                    if (v_find_nodes(info->m_template, info->m_nodes))
                        SEND_REPLY(info, DS::e_NetSuccess);
                    else
                        SEND_REPLY(info, DS::e_NetInternalError);
                }
                break;
            case e_VaultInitAge:
                dm_auth_createAge(reinterpret_cast<Auth_AgeCreate*>(msg.m_payload));
                break;
            case e_AuthFindGameServer:
                dm_auth_findAge(reinterpret_cast<Auth_GameAge*>(msg.m_payload));
                break;
            case e_AuthDisconnect:
                dm_auth_disconnect(reinterpret_cast<Auth_ClientMessage*>(msg.m_payload));
                break;
            case e_AuthAddAcct:
                dm_auth_addacct(reinterpret_cast<Auth_AccountInfo*>(msg.m_payload));
                break;
            case e_AuthGetPublic:
                dm_auth_get_public(reinterpret_cast<Auth_PubAgeRequest*>(msg.m_payload));
                break;
            default:
                /* Invalid message...  This shouldn't happen */
                DS_DASSERT(0);
                break;
            }
        } catch (DS::AssertException ex) {
            fprintf(stderr, "[Auth] Assertion failed at %s:%ld:  %s\n",
                    ex.m_file, ex.m_line, ex.m_cond);
            if (msg.m_payload) {
                // Keep clients from blocking on a reply
                SEND_REPLY(reinterpret_cast<Auth_ClientMessage*>(msg.m_payload),
                           DS::e_NetInternalError);
            }
        }
    }

    dm_auth_shutdown();
    return 0;
}

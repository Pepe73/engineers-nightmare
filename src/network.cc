#include <cassert>
#include "network.h"
#include <stdio.h>

static bool send_packet(ENetPeer *peer, ENetPacket *packet)
{
    if(!packet) {
        fprintf(stderr, "failed to create packet!\n");
        return false;
    }

    if(enet_peer_send(peer, 0, packet) < 0) {
        fprintf(stderr, "failed to send packet!\n");
        return false;
    }

    return true;
}

static bool send_version_message(ENetPeer *peer, uint8_t subtype,
        uint8_t major, uint8_t minor, uint8_t patch)
{
    ENetPacket *packet;

    assert(peer);

    uint8_t data[5] = { SERVER_MSG, subtype, major, minor, patch };
    packet = enet_packet_create(data, 5, ENET_PACKET_FLAG_RELIABLE);
    return send_packet(peer, packet);
}

bool send_client_version(ENetPeer *peer, uint8_t major, uint8_t minor,
        uint8_t patch)
{
    return send_version_message(peer, CLIENT_VSN_MSG, major, minor, patch);
}

bool send_server_version(ENetPeer *peer, uint8_t major, uint8_t minor,
        uint8_t patch)
{
    return send_version_message(peer, SERVER_VSN_MSG, major, minor, patch);
}

bool send_incompatible_version(ENetPeer *peer, uint8_t major, uint8_t minor,
        uint8_t patch)
{
    return send_version_message(peer, INCOMPAT_VSN_MSG, major, minor, patch);
}

bool basic_server_message(ENetPeer *peer, uint8_t subtype)
{
    ENetPacket *packet;

    assert(peer);

    uint8_t data[2] = {SERVER_MSG, subtype};
    packet = enet_packet_create(data, 2, ENET_PACKET_FLAG_RELIABLE);
    return send_packet(peer, packet);
}

bool request_slot(ENetPeer *peer)
{
    return basic_server_message(peer, SLOT_REQUEST);
}

bool send_register_required(ENetPeer *peer)
{
    return basic_server_message(peer, REGISTER_REQUIRED);
}

bool send_slots_full(ENetPeer *peer)
{
    return basic_server_message(peer, SERVER_FULL);
}

bool send_slot_granted(ENetPeer *peer)
{
    return basic_server_message(peer, SLOT_GRANTED);
}

bool send_not_in_slot(ENetPeer *peer)
{
    return basic_server_message(peer, NOT_IN_SLOT);
}

bool basic_ship_message(ENetPeer *peer, uint8_t subtype)
{
    ENetPacket *packet;

    assert(peer);

    uint8_t data[2] = {SHIP_MSG, subtype};
    packet = enet_packet_create(data, 2, ENET_PACKET_FLAG_RELIABLE);
    return send_packet(peer, packet);
}

bool request_whole_ship(ENetPeer *peer) {
    return basic_ship_message(peer, ALL_SHIP_REQUEST);
}

/* TODO: actually send the whole ship */
bool reply_whole_ship(ENetPeer *peer, ship_space *space) {
    ENetPacket *packet;

    assert(peer && space);

    uint8_t data[2] = {SHIP_MSG, ALL_SHIP_REPLY};
    packet = enet_packet_create(data, 2, ENET_PACKET_FLAG_RELIABLE);
    return send_packet(peer, packet);
}

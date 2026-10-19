// SkillEffect Studio v3 - client/ClientInfo.h
// Identity of a client, read from the root of Media (never written):
//   SV.T              client version (u32 length + Blowfish("SILKROAD") block -> "188")
//   type.txt          Language / Country / GameType (Language picks the textdata column the game shows)
//   DIVISIONINFO.TXT  locale byte, divisions and their gateway hosts
//   GATEPORT.TXT      gateway port
#pragma once

#include "Vfs.h"

#include <string>
#include <vector>

namespace se::client {

struct Division {
    std::wstring name;
    std::vector<std::wstring> gateways;
};

struct ClientInfo {
    int version = -1;            // 188 for vSRO 1.188; -1 unknown
    std::wstring language;       // type.txt Language ("Vietnam", "English", ...)
    std::wstring country, gameType;
    int locale = -1;             // DIVISIONINFO first byte (22 = vSRO)
    std::vector<Division> divisions;
    int gatePort = 0;
    // textdata name column the game displays, relative to the key column (+7 English, +8 Vietnam, +11 Turkey ...);
    // -1 when the language is not known
    int gameNameOffset = -1;
    std::wstring Describe(bool english) const;  // "vSRO 1.188, dil Vietnam, DIV01 (46.4.9.89:15779)"
};

ClientInfo ReadClientInfo(const Vfs& vfs);
// Language name of type.txt -> column offset from the textdata key column (-1 unknown)
int GameNameOffset(const std::wstring& language);

}  // namespace se::client

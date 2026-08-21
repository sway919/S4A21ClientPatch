#pragma once

#include <string>

#include "EquipmentSwapModel.h"

namespace equipment_swap
{
class FileStore
{
public:
    FileStore() = default;

    bool Open(const std::wstring& dataDirectory, std::wstring& error);
    void Close();
    bool LoadCharacter(unsigned int characterId, ProfileCollection& profiles,
        std::wstring& error);
    bool SaveProfile(unsigned int characterId, int profileIndex,
        const ProfileSetting& profile, std::wstring& error);

private:
    std::wstring dataDirectory_;

    std::wstring FilePath(unsigned int characterId) const;
    bool ReadFile(const std::wstring& path, ProfileCollection& profiles,
        unsigned int expectedId, std::wstring& error) const;
    bool WriteFile(const std::wstring& path, unsigned int characterId,
        const ProfileCollection& profiles, std::wstring& error) const;
};
}

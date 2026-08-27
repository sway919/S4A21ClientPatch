#include "FileStore.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include <windows.h>

namespace equipment_swap
{
namespace
{
constexpr char kMagic[4] = { 'E', 'S', 'W', '1' };
constexpr unsigned int kFormatVersion = 2;
// v1 只有 24 个槽位（装备 12 + 时装 11 + 宠物 1），没有宠物装备和勋章。
// v2 追加的槽位排在末尾，旧档前 24 槽原样迁移。
constexpr unsigned int kLegacyFormatVersion = 1;
constexpr int kLegacySlotCount = 24;
constexpr int kNameCapacity = 32;

#pragma pack(push, 1)
struct FileHeader
{
    char magic[4];
    unsigned int version;
    unsigned int characterId;
    unsigned int reserved;
};

struct FileSlot
{
    int itemId;
    int qualitySeed;
    short preferredSourceSlot;
    short reserved;
};

struct FileProfile
{
    unsigned short nameLength;
    wchar_t name[kNameCapacity];
    unsigned int modifiers;
    unsigned int virtualKey;
    FileSlot slots[kSlotCount];
};

struct FileProfileV1
{
    unsigned short nameLength;
    wchar_t name[kNameCapacity];
    unsigned int modifiers;
    unsigned int virtualKey;
    FileSlot slots[kLegacySlotCount];
};
#pragma pack(pop)

constexpr size_t kFileSize = sizeof(FileHeader) +
    sizeof(FileProfile) * kProfileCount;
constexpr size_t kLegacyFileSize = sizeof(FileHeader) +
    sizeof(FileProfileV1) * kProfileCount;

void ProfilesToFile(const ProfileCollection& profiles, FileProfile* out)
{
    for (int index = 0; index < kProfileCount; ++index)
    {
        FileProfile& dest = out[index];
        dest = {};
        const std::wstring& name = profiles[index].name;
        dest.nameLength = static_cast<unsigned short>(
            name.size() > kNameCapacity ? kNameCapacity : name.size());
        if (dest.nameLength)
            std::memcpy(dest.name, name.data(),
                dest.nameLength * sizeof(wchar_t));
        dest.modifiers = profiles[index].hotkey.modifiers;
        dest.virtualKey = profiles[index].hotkey.virtualKey;
        for (int slot = 0; slot < kSlotCount; ++slot)
        {
            dest.slots[slot].itemId = profiles[index].slots[slot].itemId;
            dest.slots[slot].qualitySeed =
                profiles[index].slots[slot].qualitySeed;
            dest.slots[slot].preferredSourceSlot =
                profiles[index].slots[slot].preferredSourceSlot;
        }
    }
}

void FileToProfiles(const FileProfile* source, ProfileCollection& profiles)
{
    ResetProfiles(profiles);
    for (int index = 0; index < kProfileCount; ++index)
    {
        const FileProfile& src = source[index];
        unsigned short length = src.nameLength;
        if (length > kNameCapacity)
            length = kNameCapacity;
        if (length)
            profiles[index].name.assign(src.name, src.name + length);
        profiles[index].hotkey.modifiers = src.modifiers;
        profiles[index].hotkey.virtualKey = src.virtualKey;
        for (int slot = 0; slot < kSlotCount; ++slot)
        {
            profiles[index].slots[slot].itemId = src.slots[slot].itemId;
            profiles[index].slots[slot].qualitySeed =
                src.slots[slot].qualitySeed;
            profiles[index].slots[slot].preferredSourceSlot =
                src.slots[slot].preferredSourceSlot;
        }
    }
}

void FileV1ToProfiles(const FileProfileV1* source, ProfileCollection& profiles)
{
    ResetProfiles(profiles);
    for (int index = 0; index < kProfileCount; ++index)
    {
        const FileProfileV1& src = source[index];
        unsigned short length = src.nameLength;
        if (length > kNameCapacity)
            length = kNameCapacity;
        if (length)
            profiles[index].name.assign(src.name, src.name + length);
        profiles[index].hotkey.modifiers = src.modifiers;
        profiles[index].hotkey.virtualKey = src.virtualKey;
        for (int slot = 0; slot < kLegacySlotCount; ++slot)
        {
            profiles[index].slots[slot].itemId = src.slots[slot].itemId;
            profiles[index].slots[slot].qualitySeed =
                src.slots[slot].qualitySeed;
            profiles[index].slots[slot].preferredSourceSlot =
                src.slots[slot].preferredSourceSlot;
        }
    }
}
}

bool FileStore::Open(const std::wstring& dataDirectory, std::wstring& error)
{
    dataDirectory_.clear();
    if (dataDirectory.empty())
    {
        error = L"数据目录为空";
        return false;
    }
    if (!CreateDirectoryW(dataDirectory.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS)
    {
        error = L"无法创建数据目录";
        return false;
    }
    dataDirectory_ = dataDirectory;
    return true;
}

void FileStore::Close()
{
    dataDirectory_.clear();
}

std::wstring FileStore::FilePath(unsigned int characterId) const
{
    return dataDirectory_ + L"\\" + std::to_wstring(characterId) + L".mq";
}

bool FileStore::ReadFile(const std::wstring& path, ProfileCollection& profiles,
    unsigned int expectedId, std::wstring& error) const
{
    ResetProfiles(profiles);
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file)
    {
        error = L"无法打开换装数据";
        return false;
    }
    std::vector<unsigned char> bytes(kFileSize);
    const size_t read = fread(bytes.data(), 1, bytes.size(), file);
    fclose(file);
    if (read != kFileSize && read != kLegacyFileSize)
    {
        error = L"换装数据不完整";
        return false;
    }

    FileHeader header = {};
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (std::memcmp(header.magic, kMagic, sizeof(kMagic)) != 0 ||
        (header.version != kFormatVersion &&
            header.version != kLegacyFormatVersion))
    {
        error = L"换装数据格式无法识别";
        return false;
    }
    const bool legacy = header.version == kLegacyFormatVersion;
    if (legacy ? read != kLegacyFileSize : read != kFileSize)
    {
        error = L"换装数据不完整";
        return false;
    }
    if (header.characterId != expectedId)
    {
        error = L"换装数据与角色ID不一致";
        return false;
    }

    if (legacy)
    {
        FileProfileV1 legacyProfiles[kProfileCount] = {};
        std::memcpy(legacyProfiles, bytes.data() + sizeof(header),
            sizeof(legacyProfiles));
        FileV1ToProfiles(legacyProfiles, profiles);
        return true;
    }

    FileProfile fileProfiles[kProfileCount] = {};
    std::memcpy(fileProfiles, bytes.data() + sizeof(header),
        sizeof(fileProfiles));
    FileToProfiles(fileProfiles, profiles);
    return true;
}

bool FileStore::WriteFile(const std::wstring& path, unsigned int characterId,
    const ProfileCollection& profiles, std::wstring& error) const
{
    FileHeader header = {};
    std::memcpy(header.magic, kMagic, sizeof(kMagic));
    header.version = kFormatVersion;
    header.characterId = characterId;

    FileProfile fileProfiles[kProfileCount] = {};
    ProfilesToFile(profiles, fileProfiles);

    std::vector<unsigned char> bytes(kFileSize);
    std::memcpy(bytes.data(), &header, sizeof(header));
    std::memcpy(bytes.data() + sizeof(header), fileProfiles,
        sizeof(fileProfiles));

    const std::wstring tempPath = path + L".tmp";
    FILE* file = nullptr;
    if (_wfopen_s(&file, tempPath.c_str(), L"wb") != 0 || !file)
    {
        error = L"无法写入换装数据";
        return false;
    }
    const size_t written = fwrite(bytes.data(), 1, bytes.size(), file);
    const bool flushed = fflush(file) == 0;
    fclose(file);
    if (written != bytes.size() || !flushed)
    {
        DeleteFileW(tempPath.c_str());
        error = L"写入换装数据失败";
        return false;
    }
    if (!MoveFileExW(tempPath.c_str(), path.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DeleteFileW(tempPath.c_str());
        error = L"无法替换换装数据";
        return false;
    }
    return true;
}

bool FileStore::LoadCharacter(unsigned int characterId,
    ProfileCollection& profiles, std::wstring& error)
{
    ResetProfiles(profiles);
    if (dataDirectory_.empty() || characterId == 0)
    {
        error = L"角色ID或数据目录尚未就绪";
        return false;
    }

    const std::wstring path = FilePath(characterId);
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        return true;
    return ReadFile(path, profiles, characterId, error);
}

bool FileStore::SaveProfile(unsigned int characterId, int profileIndex,
    const ProfileSetting& profile, std::wstring& error)
{
    if (dataDirectory_.empty() || characterId == 0 || profileIndex < 0 ||
        profileIndex >= kProfileCount)
    {
        error = L"角色ID或方案序号无效";
        return false;
    }

    ProfileCollection profiles;
    ResetProfiles(profiles);
    const std::wstring path = FilePath(characterId);
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES &&
        !ReadFile(path, profiles, characterId, error))
        return false;
    profiles[profileIndex] = profile;
    return WriteFile(path, characterId, profiles, error);
}
}

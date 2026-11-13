#pragma once
// Unit: a soldier class assembled from CfgVehicles (uniform = the class's model, head from its face, linkedItems gear,
// backpack), each part shown / hidden / replaced, and why a part is not shown.
#include "Session.h"

#include <filesystem>
#include <string>
#include <vector>

// What the Unit window asks of the viewer (implemented by App).
class IUnitHost
{
public:
    virtual ~IUnitHost() = default;
    virtual void AssembleUnit(const std::string& cls) = 0;                 // "" puts the unit away; may open the class's model
    virtual void ReplaceUnitPart(UnitSlot slot, const std::string& cls) = 0; // "" removes the part
    virtual void UnitPartsChanged() = 0;                                    // shown / hidden: geometry again
    virtual const ArmaConfig* UnitConfig() const = 0;
    // Outfit (try-on)
    virtual void OutfitBody(const std::filesystem::path& p3d) = 0;           // the body: opened if another model
    virtual std::filesystem::path SamplesArmaMan() const = 0;               // A3_character_example.p3d of the Samples ("" if not set)
    virtual void AddUnitFile(UnitSlot slot, const std::filesystem::path& p3d) = 0;
    virtual void RemoveUnitPart(size_t index) = 0;
    virtual std::filesystem::path PickP3d() = 0;                           // open dialog ("" cancelled)
    virtual std::vector<std::string> OutfitPresets() const = 0;            // names of the saved presets
    virtual bool SaveOutfitPreset(const std::string& name) = 0;
    virtual bool LoadOutfitPreset(const std::string& name) = 0;
    virtual bool DeleteOutfitPreset(const std::string& name) = 0;          // to the recycle bin
    virtual bool ControlOn() const = 0;                                    // CfgMoves control mode (WASD)
    virtual void SetControl(bool on) = 0;
};

class UnitPanel
{
public:
    UnitPanel(Session& session, IUnitHost& host) : session_(session), host_(host) {}
    void Draw(const std::string& windowName);
    void SelectTab(int tab) { selectTab_ = tab; } // 0 Class, 1 Outfit (ui-script)

private:
    void DrawClassPicker();
    void DrawOutfit();
    void DrawParts();
    void DrawAddPart();
    bool PickFromList(const char* id, const std::vector<std::string>& items, char* filter, size_t filterSize, std::string& picked);
    Session& session_;
    IUnitHost& host_;
    const ArmaConfig* listedFor_ = nullptr; // config the class lists were made from
    std::vector<std::string> soldiers_;
    std::vector<std::string> candidates_[int(UnitSlot::Count)];
    bool candidatesRead_[int(UnitSlot::Count)] = {};
    char classFilter_[128] = {};
    char partFilter_[128] = {};
    int replaceSlot_ = -1;
    int selectTab_ = -1;
    int fileSlot_ = int(UnitSlot::Other);
    char presetName_[64] = {};
    std::vector<std::string> presets_;
    bool presetsRead_ = false;
};

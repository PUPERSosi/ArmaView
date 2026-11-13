#pragma once
// What the Reload sync and Optics windows ask of the viewer (implemented by App): the weapon modes show a character,
// a comparison weapon and slot items as extra geometry, so a few commands change the view as well as the core.
#include <string>

class IWeaponView
{
public:
    virtual ~IWeaponView() = default;
    virtual bool SaveWindows(bool write) = 0;                // model.cfg windows: diff, or save (.bak) + reload + Validation
    virtual bool CompareLoad(const std::string& clsOrPath) = 0; // a second weapon on the reload timeline ("" clears)
    virtual void ComputeTouch() = 0;                         // hand bone to weapon part distance over the reload
    virtual void CharacterPoseChanged() = 0;                 // pose the shown character again
    virtual void SlotItemsChanged() = 0;                     // items on the slot proxies changed: geometry again
};

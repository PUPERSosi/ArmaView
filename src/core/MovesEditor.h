#pragma once
// Editing CfgMoves in terms of states: which states and file are the user's own (BI states are read only), links with
// their costs, values, the RTM file, rename / remove / copy a BI state into the own config / new state. Every edit goes
// to the ConfigDoc (text kept as written, undo through the Session's CommandStack, .bak + diff on save).
#include "ArmaConfig.h"
#include "Moves.h"
#include "MovesEdit.h"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

class MovesEditor
{
public:
    MovesEditor(ConfigDoc& doc, const MovesGraph& moves) : doc_(doc), moves_(moves) {}

    std::string preferredFile; // the own file to edit when the states come from several (UTF-8 path)
    std::string msg;           // result of the last edit for the panel
    std::vector<ScenarioResult> scenarios; // test scenarios of the own config, last run
    std::vector<MoveTrap> traps;           // states the player can get stuck in, last search
    bool trapsOwnOnly = true, trapsPose = false;
    bool pendingCheck = false; // saved: validate + find traps once the hot reload rebuilt the graph

    std::string StatePath(const std::string& state) const { return moves_.cls + "/States/" + state; }
    bool IsOwn(int state) const; // defined in the user's files (not the game's)
    // the user's file with the states of this CfgMoves (cfg: the config the graph was built from)
    bool OwnFile(const ArmaConfig* cfg, std::filesystem::path& file) const;
    void Open(const ArmaConfig* cfg); // loads the own file unless it is open with unsaved edits

    bool Link(const ArmaConfig* cfg, int from, int to, bool interpolate);  // add / replace from -> to (cost 0.05)
    bool SetLinks(int state, bool interpolate, const std::vector<std::pair<std::string, float>>& links);
    bool SetValue(int state, const std::string& entry, const std::string& value, bool quoted);
    bool SetFile(int state, const std::string& armaPath);
    bool Rename(int state, const std::string& newName);
    bool Remove(int state);
    bool CopyIntoOwn(int state, const std::string& newName); // a BI state as a new class (file, speed, looped ...), no links
    bool AddState(const std::string& name, const std::string& base);

    static std::string Quote(const std::string& s) { return "\"" + s + "\""; }
    static std::string PairsText(const std::vector<std::pair<std::string, float>>& v); // {"A",0.1,"B",0.2}

private:
    ConfigDoc& doc_;
    const MovesGraph& moves_;
};

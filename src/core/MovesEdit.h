#pragma once
// Editing of the user's own config text (CfgMoves classes): entries and classes changed inside their blocks with the
// rest of the file (formatting, comments) untouched; undo / redo of whole-text snapshots; line diff before saving;
// .bak on save. Plus move test scenarios and the trap search over a move graph.
#include "CommandStack.h"
#include "Moves.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

class ConfigDoc
{
public:
    // Every undoable edit (one snapshot) and every load (snapshots dropped): the document's CommandStack follows the doc.
    std::function<void(const std::string& what)> onEdit;
    std::function<void()> onLoad;

    bool Load(const std::filesystem::path& file, std::string& error);
    bool Loaded() const { return !file_.empty(); }
    const std::filesystem::path& File() const { return file_; }
    bool Dirty() const { return text_ != saved_; }
    const std::string& Text() const { return text_; }

    // class path: "CfgMovesMyMod/States/WalkFront" (case-insensitive). Values are written as config text
    // ("\"x.rtm\"", "0.5", "{\"A\",0.1}"). Each edit is one undo step.
    bool SetEntry(const std::string& classPath, const std::string& name, const std::string& value, bool isArray, std::string& error);
    bool RemoveEntry(const std::string& classPath, const std::string& name, std::string& error);
    bool AddClass(const std::string& parentPath, const std::string& name, const std::string& base, const std::vector<std::string>& body, std::string& error);
    bool RemoveClass(const std::string& classPath, std::string& error);
    // text (whole lines, file's own line ends applied) right after the line that closes classPath; undoable
    bool InsertAfterClass(const std::string& classPath, const std::string& text, std::string& error);
    // The statement of an entry as written (with its own line breaks), and its replacement by such a text (undoable).
    bool EntryText(const std::string& classPath, const std::string& name, std::string& out) const;
    bool ReplaceEntryText(const std::string& classPath, const std::string& name, const std::string& text, std::string& error);
    bool Reindent(const std::string& classPath, std::string& error); // undoable: a class without subclasses, as its siblings
    bool RenameClass(const std::string& classPath, const std::string& newName, std::string& error); // also "OldName" strings in the parent block
    bool HasClass(const std::string& classPath) const;

    bool Undo();
    bool Redo();
    bool CanUndo() const { return !undo_.empty(); }
    bool CanRedo() const { return !redo_.empty(); }
    void Revert();

    struct DiffLine { char kind; int oldLine, newLine; std::string text; }; // ' ' same, '-' removed, '+' added
    std::vector<DiffLine> Diff(int context = 3) const;
    bool Save(std::string& error); // backup of the file on disk (.bak, or .bak2... - never overwritten), then write
    const std::filesystem::path& LastBackup() const { return lastBackup_; }

private:
    struct Range { size_t open = 0, close = 0, header = 0; }; // '{' and '}' offsets, start of "class" line
    bool FindClass(const std::string& path, Range& r) const;
    bool FindEntry(const Range& r, const std::string& name, size_t& begin, size_t& end) const; // statement span
    std::string IndentAt(size_t pos) const;
    std::string IndentUnit() const;                // "\t" or 4 spaces, whichever the file uses more
    std::string BodyIndent(const Range& r) const;  // indent of the first statement line inside a class body
    bool ReindentFlat(const std::string& classPath, std::string& error); // header / braces / body like the previous sibling
    void Push();
    std::filesystem::path file_;
    std::filesystem::path lastBackup_;
    std::string text_, saved_, eol_ = "\n";
    std::vector<std::string> undo_, redo_;
};

// One ConfigDoc edit on the document's CommandStack: undo / redo step the doc's own snapshots.
class ConfigDocEdit : public ICommand
{
public:
    ConfigDocEdit(ConfigDoc& doc, std::string what) : doc_(doc), what_(std::move(what)) {}
    std::string Name() const override { return what_; }
    bool Undo() override { return doc_.Undo(); }
    bool Redo() override { return doc_.Redo(); }

private:
    ConfigDoc& doc_;
    std::string what_;
};

// Test scenarios next to a config: "test <name>", "start <state>", "do <action or state> [seconds]",
// "expect <state>" (# comments). Run on a copy of the player with simulated time.
struct MoveScenario
{
    std::string name;
    std::string start;
    struct Step { std::string action; double seconds = -1; }; // -1: until the move path has finished
    std::vector<Step> steps;
    std::string expect;
};
std::vector<MoveScenario> LoadScenarios(const std::filesystem::path& file);
struct ScenarioResult { std::string name, reached, log; bool ok = false; };
ScenarioResult RunScenario(const MovesGraph& g, const MoveScenario& s);

// Traps: states reached by actions that cannot get back to an idle state (targets of Stop / Default), closed loops
// without an exit, a random walk over the actions; pose jumps on ConnectTo transitions (last frame of A vs first of B)
// for the states of the given config files only (RTM reads).
struct MoveTrap { int state = -1; std::string text; bool error = false; };
std::vector<MoveTrap> FindMoveTraps(const MovesGraph& g, bool poseJumps, const struct Skeleton* skel, bool onlyOwn, float jumpMeters = 0.05f,
                                    float jumpDegrees = 12.0f);

#pragma once
// Command pattern for the document: every edit is an ICommand on one undo / redo stack (Session::commands), and named
// commands (ui-script, command palette, CLI) are looked up in a CommandRegistry instead of if-chains.
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

class ICommand
{
public:
    virtual ~ICommand() = default;
    virtual std::string Name() const = 0;
    virtual bool Redo() = 0; // apply again after an Undo
    virtual bool Undo() = 0;
};

// Edits recorded after they were applied (the editor that made them keeps the state; see ConfigDocEdit).
class CommandStack
{
public:
    void Record(std::unique_ptr<ICommand> c)
    {
        undo_.push_back(std::move(c));
        redo_.clear();
        if (undo_.size() > kLimit) undo_.erase(undo_.begin());
    }
    bool Undo()
    {
        if (undo_.empty() || !undo_.back()->Undo()) return false;
        redo_.push_back(std::move(undo_.back()));
        undo_.pop_back();
        return true;
    }
    bool Redo()
    {
        if (redo_.empty() || !redo_.back()->Redo()) return false;
        undo_.push_back(std::move(redo_.back()));
        redo_.pop_back();
        return true;
    }
    bool CanUndo() const { return !undo_.empty(); }
    bool CanRedo() const { return !redo_.empty(); }
    std::string UndoName() const { return undo_.empty() ? std::string() : undo_.back()->Name(); }
    std::string RedoName() const { return redo_.empty() ? std::string() : redo_.back()->Name(); }
    void Clear() { undo_.clear(); redo_.clear(); }
    size_t Size() const { return undo_.size(); }

private:
    static constexpr size_t kLimit = 500;
    std::vector<std::unique_ptr<ICommand>> undo_, redo_;
};

// Named commands with a text argument line; the result text goes to the script report / console.
class CommandRegistry
{
public:
    using Fn = std::function<std::string(std::istringstream& args)>;
    struct Entry
    {
        std::string name, args, help;
        Fn run;
    };
    void Add(std::string name, std::string args, std::string help, Fn run)
    {
        entries_.push_back({ std::move(name), std::move(args), std::move(help), std::move(run) });
    }
    const Entry* Find(const std::string& name) const
    {
        for (const auto& e : entries_)
            if (e.name == name) return &e;
        return nullptr;
    }
    // false if there is no such command
    bool Run(const std::string& name, const std::string& args, std::string& result) const
    {
        const Entry* e = Find(name);
        if (!e) return false;
        std::istringstream is(args);
        result = e->run(is);
        return true;
    }
    const std::vector<Entry>& All() const { return entries_; }

private:
    std::vector<Entry> entries_;
};

#pragma once
// Observer: the core announces what changed, the UI panels subscribe instead of the core knowing them.
// Synchronous, on the publishing thread (the UI thread for everything the Session does).
#include <functional>
#include <utility>
#include <vector>

enum class SessionEvent
{
    ModelLoaded,     // a model was opened or reloaded (model, model.cfg, skeleton, animations are new)
    ConfigReloaded,  // model.cfg / config.cpp reloaded without a new model
    AnimationChanged,// layers added / removed / rebound
    LayerAdded,      // Playback::AddLayer: a new last layer (the view logs its coverage, frames the first one)
    ValidationDone,  // a validation report is ready
};

class EventBus
{
public:
    using Handler = std::function<void(SessionEvent)>;
    int Subscribe(Handler h)
    {
        handlers_.emplace_back(++nextId_, std::move(h));
        return nextId_;
    }
    void Unsubscribe(int id) { std::erase_if(handlers_, [id](const auto& p) { return p.first == id; }); }
    void Publish(SessionEvent e) const
    {
        const auto copy = handlers_; // a handler may (un)subscribe
        for (const auto& [id, h] : copy) h(e);
    }

private:
    std::vector<std::pair<int, Handler>> handlers_;
    int nextId_ = 0;
};

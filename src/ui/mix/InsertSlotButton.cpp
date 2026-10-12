#include "InsertSlotButton.h"

#include "plugins/PluginWindow.h"
#include "ui/Theme.h"

namespace meridian
{
namespace
{
constexpr int pluginManagerItem = 100000;
} // namespace

InsertSlotButton::InsertSlotButton (AppController& a, juce::ValueTree t, int slotIndex)
    : app (a), trackNode (std::move (t)), slot (slotIndex)
{
    refresh();
}

void InsertSlotButton::refresh()
{
    const auto node = app.getBridge().getInsertNode (trackNode, slot);
    const auto letter = juce::String::charToString ((juce::juce_wchar) ('A' + slot));
    if (! node.isValid())
    {
        setTooltip ("Insert " + letter + ": click to add a plug-in");
    }
    else
    {
        const auto s = app.getBridge().getInsert (track::id (trackNode), slot);
        const auto name = s != nullptr ? s->description.name : juce::String ("?");
        setTooltip ("Insert " + letter + ": " + name + ". Click to open, right-click for options.");
    }
    repaint();
}

void InsertSlotButton::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat().reduced (1.0f, 0.5f);
    const auto node = app.getBridge().getInsertNode (trackNode, slot);
    const auto s = node.isValid() ? app.getBridge().getInsert (track::id (trackNode), slot) : nullptr;

    juce::Colour fill = theme::well;
    juce::String text;
    juce::Colour textColour = theme::text;

    if (node.isValid())
    {
        if (s == nullptr || s->plugin == nullptr)
        {
            text = "[" + (s != nullptr ? s->description.name : juce::String ("missing")) + "]";
            textColour = theme::record;
        }
        else
        {
            text = s->description.name;
            if (s->bypassed.load())
                fill = theme::bypass;
            else
                fill = theme::panelRaised;
        }
    }

    g.setColour (fill);
    g.fillRect (bounds);
    g.setColour (theme::divider);
    g.drawRect (bounds, 1.0f);

    g.setColour (textColour);
    g.setFont (theme::font (11.0f));
    g.drawText (text, bounds.reduced (3.0f, 0.0f), juce::Justification::centredLeft, true);
}

void InsertSlotButton::mouseDown (const juce::MouseEvent& e)
{
    const auto node = app.getBridge().getInsertNode (trackNode, slot);

    if (! node.isValid())
    {
        showPluginChooser();
    }
    else if (e.mods.isPopupMenu())
    {
        showSlotMenu();
    }
    else
    {
        const auto letter = juce::String::charToString ((juce::juce_wchar) ('A' + slot));
        PluginWindow::open (app.getBridge().getInsert (track::id (trackNode), slot),
                            track::name (trackNode) + " - Insert " + letter);
    }
}

void InsertSlotButton::showPluginChooser()
{
    const auto plugins = app.getPluginHost().getEffectPlugins();

    juce::PopupMenu menu;
    if (plugins.isEmpty())
        menu.addItem (-1, "No plug-ins scanned yet", false);
    else
        juce::KnownPluginList::addToMenu (menu, plugins, juce::KnownPluginList::sortByManufacturer);
    menu.addSeparator();
    menu.addItem (pluginManagerItem, "Plug-in Manager (scan for plug-ins)...");

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                        [safe = juce::Component::SafePointer<InsertSlotButton> (this), plugins] (int result)
                        {
                            if (safe == nullptr || result == 0)
                                return;
                            auto& self = *safe;

                            if (result == pluginManagerItem)
                            {
                                self.app.getPluginHost().showPluginManager();
                                return;
                            }

                            const int index = juce::KnownPluginList::getIndexChosenByMenu (plugins, result);
                            if (! juce::isPositiveAndBelow (index, plugins.size()))
                                return;

                            juce::String error;
                            if (! self.app.getBridge().insertPlugin (self.trackNode, self.slot, plugins.getReference (index), error)
                                && self.app.showProblem)
                                self.app.showProblem ("Insert Plug-in", error);
                        });
}

void InsertSlotButton::showSlotMenu()
{
    const auto s = app.getBridge().getInsert (track::id (trackNode), slot);
    const bool bypassed = s != nullptr && s->bypassed.load();

    juce::PopupMenu menu;
    menu.addItem (1, "Bypass", s != nullptr && s->plugin != nullptr, bypassed);
    menu.addItem (2, "Replace...");
    menu.addSeparator();
    menu.addItem (3, "Remove");

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                        [safe = juce::Component::SafePointer<InsertSlotButton> (this), bypassed] (int result)
                        {
                            if (safe == nullptr)
                                return;
                            auto& self = *safe;
                            if (result == 1)
                                self.app.getBridge().setPluginBypassed (self.trackNode, self.slot, ! bypassed);
                            else if (result == 2)
                                self.showPluginChooser();
                            else if (result == 3)
                                self.app.getBridge().removePlugin (self.trackNode, self.slot);
                        });
}

} // namespace meridian

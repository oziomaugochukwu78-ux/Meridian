#include "DashboardComponent.h"

#include "DialogUtils.h"
#include "model/SessionTemplates.h"

namespace meridian
{
namespace
{
constexpr const char* showOnStartupKey = "showDashboardOnStartup";
constexpr const char* lastLocationKey = "lastSessionLocation";

const double sampleRates[] = { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };
const int bitDepths[] = { 16, 24, 32 };
} // namespace

void DashboardComponent::show (AppController& app, UnsavedGuard guard)
{
    dialogs::launch (new DashboardComponent (app, std::move (guard)), "Dashboard");
}

DashboardComponent::DashboardComponent (AppController& a, UnsavedGuard g) : app (a), guard (std::move (g))
{
    for (auto* tab : { &createTab, &recentTab })
    {
        tab->setClickingTogglesState (false);
        tab->setColour (juce::TextButton::buttonOnColourId, theme::selectionEdge);
        addAndMakeVisible (tab);
    }
    createTab.onClick = [this] { showPage (Page::create); };
    recentTab.onClick = [this] { showPage (Page::recent); };

    dialogs::styleFieldLabel (nameLabel, "Name:");
    nameEditor.setText ("Untitled");
    nameEditor.setSelectAllWhenFocused (true);
    nameEditor.onReturnKey = [this] { createSession(); };

    fromTemplate.onClick = [this] { updateEnablement(); };
    for (const auto& group : templateGroups())
        templateGroup.addItem (group, templateGroup.getNumItems() + 1);
    templateGroup.setSelectedItemIndex (0, juce::dontSendNotification);
    templateGroup.onChange = [this] { refreshTemplateList(); };
    templateList.setRowHeight (36);
    templateList.setColour (juce::ListBox::outlineColourId, theme::divider);
    templateList.setOutlineThickness (1);

    dialogs::styleSectionLabel (settingsHeader, "BLANK SESSION SETTINGS");
    dialogs::styleFieldLabel (fileTypeLabel, "File Type:");
    fileType.addItem ("BWF (.WAV)", 1);
    fileType.addItem ("AIFF", 2);
    fileType.setSelectedId (1, juce::dontSendNotification);

    dialogs::styleFieldLabel (sampleRateLabel, "Sample Rate:");
    for (int i = 0; i < (int) std::size (sampleRates); ++i)
        sampleRate.addItem (juce::String (sampleRates[i] / 1000.0, sampleRates[i] == 44100.0 || sampleRates[i] == 88200.0
                                                                       || sampleRates[i] == 176400.0 ? 1 : 0) + " kHz", i + 1);
    sampleRate.setSelectedId (2, juce::dontSendNotification);

    dialogs::styleFieldLabel (bitDepthLabel, "Bit Depth:");
    bitDepth.addItem ("16-bit", 1);
    bitDepth.addItem ("24-bit", 2);
    bitDepth.addItem ("32-bit float", 3);
    bitDepth.setSelectedId (2, juce::dontSendNotification);

    dialogs::styleFieldLabel (ioLabel, "I/O Settings:");
    ioSettings.addItem ("Stereo Mix", 1);
    ioSettings.setSelectedId (1, juce::dontSendNotification);
    ioSettings.setEnabled (false);
    ioSettings.setTooltip ("Custom I/O setups (planned). Every track currently routes to the stereo master bus.");

    interleaved.setToggleState (true, juce::dontSendNotification);
    interleaved.setEnabled (false);
    interleaved.setTooltip ("Multichannel audio files are always written interleaved.");

    dialogs::styleSectionLabel (locationHeader, "LOCATION");
    location = juce::File (app.getSettings().getValue (lastLocationKey, app.getDefaultSessionsFolder().getFullPathName()));
    locationPath.setText (location.getFullPathName(), juce::dontSendNotification);
    locationPath.setFont (theme::font (12.0f));
    locationPath.setColour (juce::Label::backgroundColourId, theme::well);
    locationPath.setMinimumHorizontalScale (0.6f);
    chooseLocationButton.onClick = [this] { chooseLocation(); };

    status.setColour (juce::Label::textColourId, theme::record);
    status.setFont (theme::font (12.0f));

    recentList.setRowHeight (40);
    recentList.setColour (juce::ListBox::outlineColourId, theme::divider);
    recentList.setOutlineThickness (1);

    showOnStartup.setToggleState (app.getSettings().getBoolValue (showOnStartupKey, true), juce::dontSendNotification);
    showOnStartup.onClick = [this] { app.getSettings().setValue (showOnStartupKey, showOnStartup.getToggleState()); };

    openFromDiskButton.onClick = [this] { openFromDisk(); };
    cancelButton.onClick = [this] { dialogs::close (*this); };
    primaryButton.onClick = [this] { page == Page::create ? createSession() : openSelectedRecent(); };
    primaryButton.setColour (juce::TextButton::buttonColourId, theme::selectionEdge);
    primaryButton.setColour (juce::TextButton::textColourOffId, theme::textOnLight);

    for (juce::Component* c : std::initializer_list<juce::Component*> {
             &nameLabel, &nameEditor, &fromTemplate, &templateGroup, &templateList, &settingsHeader,
             &fileTypeLabel, &fileType, &sampleRateLabel, &sampleRate, &bitDepthLabel, &bitDepth, &ioLabel,
             &ioSettings, &interleaved, &locationHeader, &locationPath, &chooseLocationButton, &status, &recentList,
             &showOnStartup, &openFromDiskButton, &cancelButton, &primaryButton })
        addChildComponent (c);

    for (juce::Component* c : std::initializer_list<juce::Component*> { &showOnStartup, &openFromDiskButton, &cancelButton, &primaryButton, &status })
        c->setVisible (true);

    setSize (780, 540);
    refreshTemplateList();
    showPage (app.getRecentSessions().getNumFiles() > 0 && app.getSession() == nullptr ? Page::recent : Page::create);
}

DashboardComponent::~DashboardComponent()
{
    templateList.setModel (nullptr);
    recentList.setModel (nullptr);
}

//==============================================================================
std::vector<const SessionTemplate*> DashboardComponent::templatesInGroup() const
{
    std::vector<const SessionTemplate*> result;
    for (const auto& t : builtInTemplates())
        if (t.group == templateGroup.getText())
            result.push_back (&t);
    return result;
}

void DashboardComponent::refreshTemplateList()
{
    templateList.updateContent();
    templateList.selectRow (0);
    templateList.repaint();
}

void DashboardComponent::showPage (Page newPage)
{
    page = newPage;
    createTab.setToggleState (page == Page::create, juce::dontSendNotification);
    recentTab.setToggleState (page == Page::recent, juce::dontSendNotification);

    const bool creating = page == Page::create;
    for (juce::Component* c : std::initializer_list<juce::Component*> {
             &nameLabel, &nameEditor, &fromTemplate, &templateGroup, &templateList, &settingsHeader, &fileTypeLabel,
             &fileType, &sampleRateLabel, &sampleRate, &bitDepthLabel, &bitDepth, &ioLabel, &ioSettings, &interleaved,
             &locationHeader, &locationPath, &chooseLocationButton })
        c->setVisible (creating);
    recentList.setVisible (! creating);
    recentList.updateContent();

    primaryButton.setButtonText (creating ? "Create" : "Open");
    status.setText ({}, juce::dontSendNotification);
    updateEnablement();
    repaint();
}

void DashboardComponent::updateEnablement()
{
    const bool useTemplate = fromTemplate.getToggleState();
    templateGroup.setEnabled (useTemplate);
    templateList.setEnabled (useTemplate);
    templateList.setAlpha (useTemplate ? 1.0f : 0.4f); // a disabled ListBox does not dim itself
    for (auto* c : { &fileType, &sampleRate, &bitDepth })
        c->setEnabled (true); // template sessions still use these format settings

    if (page == Page::recent)
        primaryButton.setEnabled (recentList.getSelectedRow() >= 0);
    else
        primaryButton.setEnabled (true);
}

//==============================================================================
int DashboardComponent::getNumRows()
{
    if (page == Page::recent)
        return app.getRecentSessions().getNumFiles();
    return (int) templatesInGroup().size();
}

void DashboardComponent::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (selected)
        g.fillAll (theme::selection);

    juce::String title, subtitle;
    if (page == Page::recent)
    {
        const auto file = app.getRecentSessions().getFile (row);
        title = file.getFileNameWithoutExtension();
        subtitle = file.getParentDirectory().getFullPathName();
    }
    else
    {
        const auto templates = templatesInGroup();
        if (! juce::isPositiveAndBelow (row, (int) templates.size()))
            return;
        title = templates[(size_t) row]->name;
        subtitle = templates[(size_t) row]->description + "  (" + juce::String ((int) templates[(size_t) row]->tracks.size()) + " tracks)";
    }

    g.setColour (theme::text);
    g.setFont (theme::font (13.0f, true));
    g.drawText (title, 10, 2, width - 20, height / 2, juce::Justification::bottomLeft, true);
    g.setColour (theme::textDim);
    g.setFont (theme::font (11.0f));
    g.drawText (subtitle, 10, height / 2, width - 20, height / 2 - 2, juce::Justification::topLeft, true);
}

void DashboardComponent::listBoxItemDoubleClicked (int, const juce::MouseEvent&)
{
    if (page == Page::recent)
        openSelectedRecent();
    else
        createSession();
}

void DashboardComponent::selectedRowsChanged (int)
{
    updateEnablement();
}

//==============================================================================
void DashboardComponent::createSession()
{
    Session::Settings settings;
    settings.name = nameEditor.getText().trim();
    settings.sampleRate = sampleRates[juce::jlimit (0, (int) std::size (sampleRates) - 1, sampleRate.getSelectedId() - 1)];
    settings.bitDepth = bitDepths[juce::jlimit (0, 2, bitDepth.getSelectedId() - 1)];
    settings.fileFormat = fileType.getSelectedId() == 2 ? "aiff" : "wav";

    const SessionTemplate* chosenTemplate = nullptr;
    if (fromTemplate.getToggleState())
    {
        const auto templates = templatesInGroup();
        const int row = templateList.getSelectedRow();
        if (! juce::isPositiveAndBelow (row, (int) templates.size()))
        {
            status.setText ("Choose a template, or untick \"Create From Template\".", juce::dontSendNotification);
            return;
        }
        chosenTemplate = templates[(size_t) row];
    }

    guard ([safe = juce::Component::SafePointer<DashboardComponent> (this), settings, chosenTemplate]
    {
        if (safe == nullptr)
            return;
        const auto result = safe->app.createSession (safe->location, settings, chosenTemplate);
        if (result.failed())
        {
            safe->status.setText (result.getErrorMessage(), juce::dontSendNotification);
            return;
        }
        safe->app.getSettings().setValue (lastLocationKey, safe->location.getFullPathName());
        dialogs::close (*safe);
    });
}

void DashboardComponent::openSelectedRecent()
{
    const int row = recentList.getSelectedRow();
    if (row < 0)
        return;

    const auto file = app.getRecentSessions().getFile (row);
    guard ([safe = juce::Component::SafePointer<DashboardComponent> (this), file]
    {
        if (safe == nullptr)
            return;
        const auto result = safe->app.openSession (file);
        if (result.failed())
            safe->status.setText (result.getErrorMessage(), juce::dontSendNotification);
        else
            dialogs::close (*safe);
    });
}

void DashboardComponent::openFromDisk()
{
    chooser = std::make_unique<juce::FileChooser> ("Open Session", app.getDefaultSessionsFolder(),
                                                   juce::String ("*") + Session::fileExtension);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safe = juce::Component::SafePointer<DashboardComponent> (this)] (const juce::FileChooser& fc)
                          {
                              const auto file = fc.getResult();
                              if (safe == nullptr || file == juce::File())
                                  return;
                              safe->guard ([safe, file]
                              {
                                  if (safe == nullptr)
                                      return;
                                  const auto result = safe->app.openSession (file);
                                  if (result.failed())
                                      safe->status.setText (result.getErrorMessage(), juce::dontSendNotification);
                                  else
                                      dialogs::close (*safe);
                              });
                          });
}

void DashboardComponent::chooseLocation()
{
    chooser = std::make_unique<juce::FileChooser> ("Where should the session folder be created?", location);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [safe = juce::Component::SafePointer<DashboardComponent> (this)] (const juce::FileChooser& fc)
                          {
                              if (safe == nullptr || fc.getResult() == juce::File())
                                  return;
                              safe->location = fc.getResult();
                              safe->locationPath.setText (safe->location.getFullPathName(), juce::dontSendNotification);
                          });
}

//==============================================================================
void DashboardComponent::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    g.setColour (theme::window);
    g.fillRect (getLocalBounds().removeFromLeft (150));
    g.setColour (theme::divider);
    g.drawVerticalLine (150, 0.0f, (float) getHeight());
    g.drawHorizontalLine (getHeight() - 52, 150.0f, (float) getWidth());

    g.setColour (theme::text);
    g.setFont (theme::font (18.0f, true));
    g.drawText (JUCE_APPLICATION_NAME_STRING, 16, 14, 130, 28, juce::Justification::centredLeft);
}

void DashboardComponent::resized()
{
    auto nav = getLocalBounds().removeFromLeft (150).reduced (12).withTrimmedTop (44);
    createTab.setBounds (nav.removeFromTop (30));
    nav.removeFromTop (6);
    recentTab.setBounds (nav.removeFromTop (30));

    auto area = getLocalBounds().withTrimmedLeft (151);
    auto footer = area.removeFromBottom (52).reduced (16, 12);
    showOnStartup.setBounds (footer.removeFromLeft (150));
    primaryButton.setBounds (footer.removeFromRight (100));
    footer.removeFromRight (8);
    cancelButton.setBounds (footer.removeFromRight (90));
    footer.removeFromRight (8);
    openFromDiskButton.setBounds (footer.removeFromRight (140));

    area.reduce (20, 16);
    recentList.setBounds (area);

    auto row = area.removeFromTop (26);
    nameLabel.setBounds (row.removeFromLeft (90));
    row.removeFromLeft (8);
    nameEditor.setBounds (row);
    area.removeFromTop (12);

    row = area.removeFromTop (24);
    fromTemplate.setBounds (row.removeFromLeft (200));
    templateGroup.setBounds (row.removeFromLeft (180));
    area.removeFromTop (6);
    templateList.setBounds (area.removeFromTop (118));
    area.removeFromTop (14);

    settingsHeader.setBounds (area.removeFromTop (18));
    auto left = area.removeFromTop (60);
    auto right = left.removeFromRight (left.getWidth() / 2);

    auto field = [] (juce::Rectangle<int>& column, juce::Label& label, juce::Component& control)
    {
        auto r = column.removeFromTop (26);
        label.setBounds (r.removeFromLeft (90));
        r.removeFromLeft (8);
        control.setBounds (r.removeFromLeft (150).reduced (0, 1));
        column.removeFromTop (4);
    };
    field (left, fileTypeLabel, fileType);
    field (left, sampleRateLabel, sampleRate);
    field (right, bitDepthLabel, bitDepth);
    field (right, ioLabel, ioSettings);
    interleaved.setBounds (area.removeFromTop (24).withTrimmedLeft (98).withWidth (150));
    area.removeFromTop (10);

    locationHeader.setBounds (area.removeFromTop (18));
    row = area.removeFromTop (26);
    chooseLocationButton.setBounds (row.removeFromRight (90));
    row.removeFromRight (8);
    locationPath.setBounds (row);
    area.removeFromTop (8);
    status.setBounds (area.removeFromTop (36));
}

} // namespace meridian

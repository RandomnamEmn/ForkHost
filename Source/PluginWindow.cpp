#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginWindow.h"
#include "IconMenu.hpp"
#include "PluginChain.hpp"
#include "HostTheme.hpp"
#include <atomic>
#include <cmath>

class PluginWindow;
static Array <PluginWindow*> activePluginWindows;
// Keep third-party editor controls on JUCE's default palette when the host theme changes.
static LookAndFeel_V4 pluginEditorLookAndFeel;
static constexpr int toolbarHeight = 28;

//==============================================================================
/**
    Listens for editor component resize events and resizes the PluginWindow
    to match. Handles both initial sizing (when the editor first reports a
    valid size that differs from our default) and runtime self-resizing
    (plugins that call resizeView / setSize during operation).
*/
class EditorResizeListener : public ComponentListener
{
public:
    EditorResizeListener (PluginWindow& w) : window (w) {}

    void componentMovedOrResized (Component& comp, bool, bool wasResized) override
    {
        if (! wasResized)   return;
        if (window.isResizingInternally)  return;

        auto w = comp.getWidth();
        auto h = comp.getHeight();
        if (w <= 50 || h <= 50)  return;

        window.updateSizeFromEditor();
    }

private:
    PluginWindow& window;
};

//==============================================================================
class ToolbarComponent : public Component, private AudioProcessorListener
{
public:
    ToolbarComponent (PluginWindow& pw, Component* editor, AudioProcessor* processor)
        : pluginWindow (pw), editor (editor), proc (processor)
    {
        proc->addListener (this);

        addAndMakeVisible (bypassButton);
        addAndMakeVisible (moveUpButton);
        addAndMakeVisible (moveDownButton);
        addAndMakeVisible (pinButton);
        addAndMakeVisible (scaleButton);
        addAndMakeVisible (latencyLabel);
        addAndMakeVisible (editor);

        bypassButton.setButtonText (String::fromUTF8 (nonBypassedPluginEmoji) + " Bypass");
        bypassButton.setClickingTogglesState (true);
        bypassButton.onClick = [this]
        {
            if (pluginWindow.getIconMenu() != nullptr)
                pluginWindow.getIconMenu()->togglePluginBypass (pluginWindow.getChainPosition());
        };

        moveUpButton.setButtonText ("Up");
        moveUpButton.setTooltip ("Move up");
        moveUpButton.onClick = [this]
        {
            if (pluginWindow.getIconMenu() != nullptr)
                pluginWindow.getIconMenu()->movePluginUp (pluginWindow.getChainPosition());
        };

        moveDownButton.setButtonText ("Down");
        moveDownButton.setTooltip ("Move down");
        moveDownButton.onClick = [this]
        {
            if (pluginWindow.getIconMenu() != nullptr)
                pluginWindow.getIconMenu()->movePluginDown (pluginWindow.getChainPosition());
        };

        pinButton.setButtonText ("Pin");
        pinButton.setTooltip ("Always on top");
        pinButton.setClickingTogglesState (true);
        pinButton.onClick = [this] { pluginWindow.toggleAlwaysOnTop(); };

        scaleButton.setTooltip ("Scale plug-in editor");
        scaleButton.onClick = [this]
        {
            PopupMenu menu;
            const auto currentScale = pluginWindow.getEditorScaleFactor();
            menu.addItem (1, "50%", true, std::abs (currentScale - 0.5f) < 0.01f);
            menu.addItem (2, "100%", true, std::abs (currentScale - 1.0f) < 0.01f);
            menu.addItem (3, "200%", true, std::abs (currentScale - 2.0f) < 0.01f);

            auto safeThis = Component::SafePointer<ToolbarComponent> (this);
            menu.showMenuAsync (PopupMenu::Options().withTargetComponent (&scaleButton),
                [safeThis] (int result)
                {
                    if (auto* self = safeThis.getComponent())
                    {
                        if (result == 1) self->pluginWindow.setEditorScaleFactor (0.5f);
                        if (result == 2) self->pluginWindow.setEditorScaleFactor (1.0f);
                        if (result == 3) self->pluginWindow.setEditorScaleFactor (2.0f);
                    }
                });
        };

        // Latency display (always visible, including 0)
        int latencySamples = proc->getLatencySamples();
        double sampleRate = proc->getSampleRate();
        int ms = (sampleRate > 0) ? (int) (latencySamples / sampleRate * 1000) : 0;
        latencyLabel.setText ("Latency:" + String (ms) + "ms (" + String (latencySamples) + "sample)",
                              dontSendNotification);

        setSize (400, 300);
    }

    ~ToolbarComponent() override
    {
        proc->removeListener (this);

        // addAndMakeVisible() only attaches a child component; it does not
        // transfer ownership.  Editors returned by createEditorIfNeeded()
        // remain registered with their AudioProcessor until their destructor
        // calls editorBeingDeleted().  Destroy the editor while the processor
        // is still alive, otherwise a removed plugin may continue delivering
        // GUI events into a freed processor/plugin instance.
        if (auto* audioEditor = dynamic_cast<AudioProcessorEditor*> (editor.getComponent()))
            delete audioEditor;
    }

    void audioProcessorParameterChanged (AudioProcessor*, int, float) override
    {
        // Audio thread — use atomic guard to batch rapid changes
        schedulePresetDirtyNotification();
    }

    void audioProcessorChanged (AudioProcessor*, const AudioProcessorListener::ChangeDetails& details) override
    {
        if (details.programChanged || details.nonParameterStateChanged)
            schedulePresetDirtyNotification();

        if (details.latencyChanged)
        {
            // Use SafePointer to prevent use-after-free if this ToolbarComponent
            // is destroyed before the lambda fires (e.g. during preset switch).
            auto safeThis = Component::SafePointer<ToolbarComponent> (this);
            MessageManager::callAsync ([safeThis]
            {
                if (auto* self = safeThis.getComponent())
                {
                    int current = self->proc->getLatencySamples();
                    double sr = self->proc->getSampleRate();
                    int ms = (sr > 0) ? (int) (current / sr * 1000) : 0;
                    self->latencyLabel.setText ("Latency:" + String (ms) + "ms (" + String (current) + "samples)",
                                                dontSendNotification);
                }
            });
        }
    }

    void schedulePresetDirtyNotification()
    {
        // Processor callbacks may arrive on the audio thread. Marshal the UI
        // and preset bookkeeping to the message thread, coalescing bursts.
        if (!pendingDirtyNotification.exchange (true))
        {
            auto safeThis = Component::SafePointer<ToolbarComponent> (this);
            MessageManager::callAsync ([safeThis]
            {
                if (auto* self = safeThis.getComponent())
                {
                    self->pendingDirtyNotification = false;
                    if (auto* menu = self->pluginWindow.getIconMenu())
                        menu->markPresetDirty (false);
                }
            });
        }
    }

    void paint (Graphics& g) override
    {
        g.fillAll (LookAndFeel::getDefaultLookAndFeel().findColour (ForkHostTheme::panelBackgroundColourId));
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto toolbar = r.removeFromTop (::toolbarHeight).reduced (2);

        bypassButton.setBounds (toolbar.removeFromLeft (58).reduced (1));
        moveUpButton.setBounds (toolbar.removeFromLeft (36).reduced (1));
        moveDownButton.setBounds (toolbar.removeFromLeft (44).reduced (1));
        pinButton.setBounds (toolbar.removeFromLeft (38).reduced (1));
        scaleButton.setBounds (toolbar.removeFromLeft (48).reduced (1));

        const auto latencyWidth = jmin (170, toolbar.getWidth());
        const bool showLatency = latencyWidth >= 110;
        latencyLabel.setVisible (showLatency);
        if (showLatency)
            latencyLabel.setBounds (toolbar.removeFromRight (latencyWidth).reduced (1));

        if (editor != nullptr)
        {
            if (pluginWindow.editorScaleChangesBounds())
                editor->setBounds (r);
            else
                editor->setBounds (0, ::toolbarHeight,
                                   jmax (1, roundToInt ((float) r.getWidth() / pluginWindow.getEditorScaleFactor())),
                                   jmax (1, roundToInt ((float) r.getHeight() / pluginWindow.getEditorScaleFactor())));
        }
    }

    void updateButtonStates()
    {
        if (pluginWindow.getIconMenu() == nullptr)
            return;

        int totalPlugins = pluginWindow.getIconMenu()->getPluginChain().size();
        int pos = pluginWindow.getChainPosition();

        bool isBypassed = pluginWindow.getIconMenu()->isBypassed (pos);
        bypassButton.setToggleState (isBypassed, dontSendNotification);
        bypassButton.setButtonText (isBypassed ? "Byp" : "Bypass");
        moveUpButton.setEnabled (pos > 0);
        moveDownButton.setEnabled (pos < totalPlugins - 1);
        pinButton.setToggleState (pluginWindow.isAlwaysOnTop(), dontSendNotification);
        scaleButton.setButtonText (String (roundToInt (pluginWindow.getEditorScaleFactor() * 100.0f)) + "%");
    }

    Component* getEditor() const { return editor.getComponent(); }

private:
    PluginWindow& pluginWindow;
    Component::SafePointer<Component> editor;
    AudioProcessor* proc = nullptr;
    TextButton bypassButton, moveUpButton, moveDownButton, pinButton, scaleButton;
    Label latencyLabel;
    std::atomic<bool> pendingDirtyNotification{false};
};

PluginWindow::PluginWindow (Component* const pluginEditor,
                            AudioProcessorGraph::Node::Ptr o,
                            WindowFormatType t,
                            IconMenu* menu,
                            int prefW, int prefH)
    : DocumentWindow (pluginEditor->getName(),
                      LookAndFeel::getDefaultLookAndFeel().findColour(DocumentWindow::backgroundColourId),
                      DocumentWindow::minimiseButton | DocumentWindow::closeButton),
      owner (std::move (o)),
      type (t),
      iconMenu (menu),
      editorPrefW (prefW),
      editorPrefH (prefH),
      unscaledEditorWidth (prefW > 50 ? prefW : 400),
      unscaledEditorHeight (prefH > 50 ? prefH : 300)
{
    // Isolate the plug-in editor from ForkHost's selectable host UI theme.
    if (&pluginEditor->getLookAndFeel() == &LookAndFeel::getDefaultLookAndFeel())
        pluginEditor->setLookAndFeel (&pluginEditorLookAndFeel);

    // Step 1: Set flags that affect getContentComponentBorder() FIRST
    setUsingNativeTitleBar (false);

    {
        bool allowResize = false;
        if (auto* apEditor = dynamic_cast<AudioProcessorEditor*> (pluginEditor))
            allowResize = apEditor->isResizable();
        setResizable (allowResize, false);
    }

    setResizeLimits (240, 160, 4096, 4096);

    // Step 2: Set content component
    auto* wrapper = new ToolbarComponent (*this, pluginEditor, owner->getProcessor());
    setContentOwned (wrapper, true);

    // Step 3: Determine initial size — saved size > editor preferred size > fallback
    if (owner->properties.contains (getLastWProp (type))
        && owner->properties.contains (getLastHProp (type)))
    {
        setSize (owner->properties[getLastWProp (type)],
                 owner->properties[getLastHProp (type)]);
    }
    else if (editorPrefW > 50 && editorPrefH > 50)
    {
        setContentComponentSize (editorPrefW, editorPrefH + toolbarHeight);
    }
    else
    {
        setContentComponentSize (400, 300 + toolbarHeight);
    }

    // Clamp window position to the current desktop so the window
    // is always at least partially visible (avoids Bug 10 where a
    // preset saved on a larger display loads off-screen).
    {
        auto totalBounds = Desktop::getInstance().getDisplays().getTotalBounds (true);
        int storedX = owner->properties.getWithDefault (getLastXProp (type), Random::getSystemRandom().nextInt (500));
        int storedY = owner->properties.getWithDefault (getLastYProp (type), Random::getSystemRandom().nextInt (500));
        int clampedX = jlimit (totalBounds.getX(), jmax (totalBounds.getX(), totalBounds.getRight()  - 200), storedX);
        int clampedY = jlimit (totalBounds.getY(), jmax (totalBounds.getY(), totalBounds.getBottom() - 100), storedY);
        setTopLeftPosition (clampedX, clampedY);
    }

    owner->properties.set (getOpenProp (type), true);

    // Add listener to detect editor self-resize at runtime
    editorResizeListener = std::make_unique<EditorResizeListener> (*this);
    pluginEditor->addComponentListener (editorResizeListener.get());

    updateTitleAndToolbar();

    setVisible (true);
    activePluginWindows.add (this);

    // Bring to front reliably (bypasses Windows focus-stealing prevention)
    forceToFront();

    // Restore user's always-on-top preference for this window type
    if (owner->properties.getWithDefault (getAlwaysOnTopProp (type), false))
        setAlwaysOnTop (true);

    // Async check: after construction & layout, ensure window fits the editor's real size
    Component::SafePointer<PluginWindow> safeThis (this);
    MessageManager::callAsync ([safeThis]
    {
        if (auto* pw = safeThis.getComponent())
        {
            const auto scaleKey = pw->getEditorScalePropertyKey();
            auto* settings = getAppProperties().getUserSettings();

            if (scaleKey.isNotEmpty() && settings->containsKey (scaleKey))
                pw->setEditorScaleFactor ((float) settings->getDoubleValue (scaleKey, 1.0));

            pw->updateSizeFromEditor();
        }
    });
}

void PluginWindow::updateTitleAndToolbar()
{
    if (iconMenu == nullptr)
        return;

    chainPosition = iconMenu->getPluginChain().getChainPositionForNode (owner->nodeID);

    String pluginName = chainPosition >= 0
        ? iconMenu->getPluginChain().getDisplayName (chainPosition)
        : owner->getProcessor()->getName();
    bool dirty = iconMenu->getPresetManager() != nullptr
                 && iconMenu->getPresetManager()->isDirty();
    setName ("[" + String (chainPosition + 1) + "] " + pluginName + (dirty ? " *" : ""));

    // Update toolbar button states
    if (auto* wrapper = dynamic_cast<ToolbarComponent*> (getContentComponent()))
        wrapper->updateButtonStates();
}

void PluginWindow::updateAllTitlesAndToolbars (IconMenu* iconMenu)
{
    ignoreUnused (iconMenu);
    for (auto* w : activePluginWindows)
        w->updateTitleAndToolbar();
}

void PluginWindow::updateHostTheme()
{
    const auto& lookAndFeel = LookAndFeel::getDefaultLookAndFeel();
    for (auto* window : activePluginWindows)
    {
        window->setBackgroundColour (lookAndFeel.findColour (DocumentWindow::backgroundColourId));
        ForkHostTheme::refreshHostComponentTree (*window);
    }
}

void PluginWindow::closeCurrentlyOpenWindowsFor (AudioProcessorGraph::NodeID nodeId)
{
    for (int i = activePluginWindows.size(); --i >= 0;)
        if (activePluginWindows.getUnchecked(i)->owner->nodeID == nodeId)
            delete activePluginWindows.getUnchecked (i);
}

void PluginWindow::closeAllCurrentlyOpenWindows()
{
    if (activePluginWindows.size() > 0)
    {
        for (int i = activePluginWindows.size(); --i >= 0;)
            delete activePluginWindows.getUnchecked (i);
    }
}

bool PluginWindow::containsActiveWindows()
{
    return activePluginWindows.size() > 0;
}

bool PluginWindow::isWindowOpenFor(AudioProcessorGraph::NodeID nodeId)
{
    for (auto* w : activePluginWindows)
        if (w->owner->nodeID == nodeId)
            return true;
    return false;
}

//==============================================================================
class ProcessorProgramPropertyComp : public PropertyComponent,
                                     private AudioProcessorListener
{
public:
    ProcessorProgramPropertyComp (const String& name, AudioProcessor& p, int index_)
        : PropertyComponent (name),
          owner (p),
          index (index_)
    {
        owner.addListener (this);
    }

    ~ProcessorProgramPropertyComp()
    {
        owner.removeListener (this);
    }

    void refresh() { }
    void audioProcessorChanged (AudioProcessor*, const ChangeDetails&) override { }
    void audioProcessorParameterChanged(AudioProcessor* processor, int, float) override { }

private:
    AudioProcessor& owner;
    const int index;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ProcessorProgramPropertyComp)
};

class ProgramAudioProcessorEditor : public AudioProcessorEditor
{
public:
    ProgramAudioProcessorEditor (AudioProcessor* const p)
        : AudioProcessorEditor (p)
    {
        jassert (p != nullptr);
        setOpaque (true);

        addAndMakeVisible (panel);

        Array<PropertyComponent*> programs;

        const int numPrograms = p->getNumPrograms();
        int totalHeight = 0;

        for (int i = 0; i < numPrograms; ++i)
        {
            String name (p->getProgramName (i).trim());

            if (name.isEmpty())
                name = "Unnamed";

            ProcessorProgramPropertyComp* const pc = new ProcessorProgramPropertyComp (name, *p, i);
            programs.add (pc);
            totalHeight += pc->getPreferredHeight();
        }

        panel.addProperties (programs);

        setSize (400, jlimit (25, 400, totalHeight));
    }

    void paint (Graphics& g)
    {
        g.fillAll (findColour(DocumentWindow::backgroundColourId));
    }

    void resized()
    {
        panel.setBounds (getLocalBounds());
    }

private:
    PropertyPanel panel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ProgramAudioProcessorEditor)
};

//==============================================================================
PluginWindow* PluginWindow::getWindowFor (AudioProcessorGraph::Node::Ptr node,
                                          WindowFormatType type,
                                          IconMenu* menu)
{
    if (node == nullptr)
        return nullptr;

    for (int i = activePluginWindows.size(); --i >= 0;)
        if (activePluginWindows.getUnchecked(i)->owner == node
             && activePluginWindows.getUnchecked(i)->type == type)
            return activePluginWindows.getUnchecked(i);

    AudioProcessor* processor = node->getProcessor();

    if (processor == nullptr)
        return nullptr;
    AudioProcessorEditor* ui = nullptr;

    if (type == Normal)
    {
        ui = processor->createEditorIfNeeded();

        if (ui == nullptr)
            type = Generic;
    }

    if (ui == nullptr)
    {
        if (type == Generic || type == Parameters)
            ui = new GenericAudioProcessorEditor (processor);
        else if (type == Programs)
            ui = new ProgramAudioProcessorEditor (processor);
    }

    if (ui != nullptr)
    {
        if (AudioPluginInstance* const plugin = dynamic_cast<AudioPluginInstance*> (processor))
            ui->setName (plugin->getName());

        // Capture editor's preferred size BEFORE any window manipulation
        int prefW = ui->getWidth();
        int prefH = ui->getHeight();

        return new PluginWindow (ui, std::move (node), type, menu, prefW, prefH);
    }

    return nullptr;
}

PluginWindow::~PluginWindow()
{
    activePluginWindows.removeFirstMatchingValue (this);

    // Remove listener from editor before both are destroyed — prevents
    // dangling pointer crash when the editor fires events later.
    if (auto* editor = getEditorComponent())
        editor->removeComponentListener (editorResizeListener.get());

    if (auto* editor = getEditorComponent())
        if (&editor->getLookAndFeel() == &pluginEditorLookAndFeel)
            editor->setLookAndFeel (nullptr);

    clearContentComponent();
}

void PluginWindow::moved()
{
    // Window position changes are saved to app properties but
    // intentionally do NOT mark the preset dirty.  Dragging and
    // resizing windows is a high-frequency cosmetic operation
    // that should not trigger a dirty "*" indicator.
    //
    // Window geometry IS persisted in preset files via
    // PresetManager::savePresetToFile() (reads winX/Y from app
    // properties and writes them to the preset XML), so on
    // explicit user save the current layout is captured.

    owner->properties.set (getLastXProp (type), getX());
    owner->properties.set (getLastYProp (type), getY());

    if (iconMenu != nullptr)
    {
        auto nodeId = owner->nodeID;
        int activeIdx = iconMenu->getPluginChain().getSlotIndexForNode (nodeId);
        if (activeIdx >= 0 && activeIdx < iconMenu->getPluginChain().size())
        {
            auto& slot = iconMenu->getPluginChain()[activeIdx];
            auto& props = *getAppProperties().getUserSettings();
            props.setValue (getPluginKey ("winX", slot.desc), getX());
            props.setValue (getPluginKey ("winY", slot.desc), getY());
        }
    }
}

void PluginWindow::resized()
{
    // Same as moved(): window resize is a cosmetic, high-frequency
    // operation that intentionally does NOT mark the preset dirty.
    // Geometry IS captured in preset files on explicit save.
    // Guard: prevent EditorResizeListener from fighting host-initiated
    // resize (user dragging the window edge).  The prevResizing pattern
    // correctly handles re-entrancy: an outer listener's setSize triggers
    // this resized(), and we restore the outer listener's flag value.
    bool prevResizing = isResizingInternally;
    isResizingInternally = true;

    DocumentWindow::resized();

    isResizingInternally = prevResizing;

    owner->properties.set (getLastWProp (type), getWidth());
    owner->properties.set (getLastHProp (type), getHeight());

    if (iconMenu != nullptr)
    {
        auto nodeId = owner->nodeID;
        int activeIdx = iconMenu->getPluginChain().getSlotIndexForNode (nodeId);
        if (activeIdx >= 0 && activeIdx < iconMenu->getPluginChain().size())
        {
            auto& slot = iconMenu->getPluginChain()[activeIdx];
            auto& props = *getAppProperties().getUserSettings();
            props.setValue (getPluginKey ("winW", slot.desc), getWidth());
            props.setValue (getPluginKey ("winH", slot.desc), getHeight());
        }
    }
}

void PluginWindow::closeButtonPressed()
{
    if (closePending)
        return;

    closePending = true;
    owner->properties.set (getOpenProp (type), false);
    setVisible (false);

    // JUCE may continue using the DocumentWindow after this callback returns.
    // Defer deletion until the callback stack has unwound.
    Component::SafePointer<PluginWindow> safeThis (this);
    MessageManager::callAsync ([safeThis]
    {
        if (auto* window = safeThis.getComponent())
            delete window;
    });
}

bool PluginWindow::keyPressed(const KeyPress& key)
{
    // Ctrl+S (Windows/Linux) or Cmd+S (macOS) → save the current preset
    if (key == KeyPress('s', ModifierKeys::commandModifier, 0))
    {
        if (iconMenu != nullptr)
        {
            iconMenu->saveCurrentPreset();
            return true;
        }
    }
    return false;
}

void PluginWindow::forceToFront()
{
    Process::makeForegroundProcess();
    setAlwaysOnTop (true);
    toFront (true);
    grabKeyboardFocus();
    setAlwaysOnTop (false);
}

void PluginWindow::toggleAlwaysOnTop()
{
    bool newState = ! isAlwaysOnTop();
    setAlwaysOnTop (newState);
    owner->properties.set (getAlwaysOnTopProp (type), newState);
}

String PluginWindow::getEditorScalePropertyKey() const
{
    if (iconMenu == nullptr)
        return {};

    const auto index = iconMenu->getPluginChain().getSlotIndexForNode (owner->nodeID);
    if (index < 0 || index >= iconMenu->getPluginChain().size())
        return {};

    return getPluginKey ("editorScale", iconMenu->getPluginChain()[index].desc);
}

void PluginWindow::applyEditorScaleFactor (float scale, bool persist)
{
    auto* editor = dynamic_cast<AudioProcessorEditor*> (getEditorComponent());
    if (editor == nullptr)
        return;

    scale = jlimit (0.5f, 2.0f, scale);
    const auto oldBounds = editor->getBounds();
    const bool previousResizingState = isResizingInternally;
    isResizingInternally = true;
    editor->setScaleFactor (scale);
    isResizingInternally = previousResizingState;

    const auto newBounds = editor->getBounds();
    editorScaleFactor = scale;
    editorScaleChangesComponentBounds = newBounds.getWidth() != oldBounds.getWidth()
                                     || newBounds.getHeight() != oldBounds.getHeight();

    if (editorScaleChangesComponentBounds)
    {
        unscaledEditorWidth = jmax (1, roundToInt ((float) newBounds.getWidth() / scale));
        unscaledEditorHeight = jmax (1, roundToInt ((float) newBounds.getHeight() / scale));
    }
    else
    {
        unscaledEditorWidth = jmax (1, oldBounds.getWidth());
        unscaledEditorHeight = jmax (1, oldBounds.getHeight());
    }

    if (auto* toolbar = dynamic_cast<ToolbarComponent*> (getContentComponent()))
        toolbar->updateButtonStates();

    if (persist)
    {
        allowAutomaticUpscale = false;
        const auto scaleKey = getEditorScalePropertyKey();
        if (scaleKey.isNotEmpty())
        {
            auto* settings = getAppProperties().getUserSettings();
            settings->setValue (scaleKey, (double) scale);
            settings->saveIfNeeded();
        }
    }
}

void PluginWindow::setEditorScaleFactor (float scale)
{
    applyEditorScaleFactor (scale, true);
    updateSizeFromEditor();
}

void PluginWindow::updateSizeFromEditor()
{
    if (isResizingInternally)
        return;

    auto* editor = getEditorComponent();
    if (editor == nullptr)
        return;

    int w = editor->getWidth();
    int h = editor->getHeight();

    // Use preferred size if the editor hasn't established its real size yet
    if ((w <= 50 || h <= 50) && editorPrefW > 50 && editorPrefH > 50)
    {
        w = editorPrefW;
        h = editorPrefH;
    }

    if (w <= 50 || h <= 50)
        return;

    if (allowAutomaticUpscale && editorScaleFactor < 2.0f && w < 300 && h < 220)
    {
        applyEditorScaleFactor (2.0f, true);
        w = editor->getWidth();
        h = editor->getHeight();

        if (w <= 50 || h <= 50)
        {
            w = editorPrefW;
            h = editorPrefH;
        }
    }

    if (editorScaleChangesComponentBounds)
    {
        unscaledEditorWidth = jmax (1, roundToInt ((float) w / editorScaleFactor));
        unscaledEditorHeight = jmax (1, roundToInt ((float) h / editorScaleFactor));
    }
    else
    {
        unscaledEditorWidth = w;
        unscaledEditorHeight = h;
    }

    const auto& displays = Desktop::getInstance().getDisplays();
    const auto* display = displays.getDisplayForRect (getBounds());
    const auto available = display != nullptr ? display->userArea : displays.getTotalBounds (true);
    const auto maxEditorWidth = jmax (240, available.getWidth() - 32);
    const auto maxEditorHeight = jmax (160, available.getHeight() - toolbarHeight - 88);

    auto scaledWidth = editorScaleChangesComponentBounds
                     ? w : roundToInt ((float) unscaledEditorWidth * editorScaleFactor);
    auto scaledHeight = editorScaleChangesComponentBounds
                      ? h : roundToInt ((float) unscaledEditorHeight * editorScaleFactor);

    if ((scaledWidth > maxEditorWidth || scaledHeight > maxEditorHeight)
        && editorScaleFactor > 0.5f)
    {
        const auto fitsAtOne = unscaledEditorWidth <= maxEditorWidth
                            && unscaledEditorHeight <= maxEditorHeight;
        const auto smallerScale = editorScaleFactor > 1.0f && fitsAtOne ? 1.0f : 0.5f;
        applyEditorScaleFactor (smallerScale, true);

        w = editor->getWidth();
        h = editor->getHeight();
        scaledWidth = editorScaleChangesComponentBounds
                    ? w : roundToInt ((float) unscaledEditorWidth * editorScaleFactor);
        scaledHeight = editorScaleChangesComponentBounds
                     ? h : roundToInt ((float) unscaledEditorHeight * editorScaleFactor);
    }

    isResizingInternally = true;
    setContentComponentSize (jmin (scaledWidth, maxEditorWidth),
                             jmin (scaledHeight, maxEditorHeight) + toolbarHeight);
    isResizingInternally = false;

    const auto maxX = jmax (available.getX(), available.getRight() - getWidth());
    const auto maxY = jmax (available.getY(), available.getBottom() - getHeight());
    setTopLeftPosition (jlimit (available.getX(), maxX, getX()),
                        jlimit (available.getY(), maxY, getY()));
}

Component* PluginWindow::getEditorComponent() const
{
    if (auto* toolbar = dynamic_cast<ToolbarComponent*> (getContentComponent()))
        return toolbar->getEditor();
    return nullptr;
}

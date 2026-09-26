#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include <functional>
#include <memory>
#include <optional>
#include <vector>
#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace LightHostTheme
{
using namespace juce;

inline juce::Colour getWindowsSystemColour (int colourIndex, juce::Colour fallback)
{
#if JUCE_WINDOWS
    const auto value = GetSysColor (colourIndex);
    return juce::Colour::fromRGB (GetRValue (value), GetGValue (value), GetBValue (value));
#else
    juce::ignoreUnused (colourIndex);
    return fallback;
#endif
}

inline void configureCustomizationLookAndFeel (juce::LookAndFeel_V4& lookAndFeel)
{
    const auto window = getWindowsSystemColour (
#if JUCE_WINDOWS
        COLOR_WINDOW,
#else
        0,
#endif
        juce::Colour (0xfff0f0f0));
    const auto windowText = getWindowsSystemColour (
#if JUCE_WINDOWS
        COLOR_WINDOWTEXT,
#else
        0,
#endif
        juce::Colour (0xff000000));
    const auto button = getWindowsSystemColour (
#if JUCE_WINDOWS
        COLOR_BTNFACE,
#else
        0,
#endif
        juce::Colour (0xfff0f0f0));
    const auto buttonText = getWindowsSystemColour (
#if JUCE_WINDOWS
        COLOR_BTNTEXT,
#else
        0,
#endif
        juce::Colour (0xff000000));
    const auto menu = getWindowsSystemColour (
#if JUCE_WINDOWS
        COLOR_MENU,
#else
        0,
#endif
        window);
    const auto menuText = getWindowsSystemColour (
#if JUCE_WINDOWS
        COLOR_MENUTEXT,
#else
        0,
#endif
        windowText);
    const auto highlight = getWindowsSystemColour (
#if JUCE_WINDOWS
        COLOR_HIGHLIGHT,
#else
        0,
#endif
        juce::Colour (0xff0078d4));
    const auto highlightText = getWindowsSystemColour (
#if JUCE_WINDOWS
        COLOR_HIGHLIGHTTEXT,
#else
        0,
#endif
        juce::Colours::white);
    const auto outline = getWindowsSystemColour (
#if JUCE_WINDOWS
        COLOR_3DSHADOW,
#else
        0,
#endif
        juce::Colour (0xff808080));

    lookAndFeel.setColourScheme ({ window, button, menu, outline, windowText,
                                   highlight, highlightText, highlight, menuText });
    lookAndFeel.setColour (juce::ResizableWindow::backgroundColourId, window);
    lookAndFeel.setColour (juce::DocumentWindow::backgroundColourId, window);
    lookAndFeel.setColour (juce::DocumentWindow::textColourId, windowText);
    lookAndFeel.setColour (juce::Label::textColourId, windowText);
    lookAndFeel.setColour (juce::TextButton::buttonColourId, button);
    lookAndFeel.setColour (juce::TextButton::buttonOnColourId, highlight);
    lookAndFeel.setColour (juce::TextButton::textColourOffId, buttonText);
    lookAndFeel.setColour (juce::TextButton::textColourOnId, highlightText);
    lookAndFeel.setColour (juce::ComboBox::backgroundColourId, window);
    lookAndFeel.setColour (juce::ComboBox::textColourId, windowText);
    lookAndFeel.setColour (juce::ComboBox::outlineColourId, outline);
    lookAndFeel.setColour (juce::ComboBox::arrowColourId, windowText);
    lookAndFeel.setColour (juce::ComboBox::buttonColourId, button);
    lookAndFeel.setColour (juce::TextEditor::backgroundColourId, window);
    lookAndFeel.setColour (juce::TextEditor::textColourId, windowText);
    lookAndFeel.setColour (juce::TextEditor::outlineColourId, outline);
    lookAndFeel.setColour (juce::TextEditor::highlightColourId, highlight);
    lookAndFeel.setColour (juce::TextEditor::highlightedTextColourId, highlightText);
    lookAndFeel.setColour (juce::ListBox::backgroundColourId, window);
    lookAndFeel.setColour (juce::ListBox::textColourId, windowText);
    lookAndFeel.setColour (juce::PopupMenu::backgroundColourId, menu);
    lookAndFeel.setColour (juce::PopupMenu::textColourId, menuText);
    lookAndFeel.setColour (juce::PopupMenu::headerTextColourId, menuText);
    lookAndFeel.setColour (juce::PopupMenu::highlightedBackgroundColourId, highlight);
    lookAndFeel.setColour (juce::PopupMenu::highlightedTextColourId, highlightText);
}

inline juce::LookAndFeel_V4& customizationLookAndFeel()
{
    static juce::LookAndFeel_V4 lookAndFeel;
    static const bool configured = []
    {
        configureCustomizationLookAndFeel (lookAndFeel);
        return true;
    }();
    juce::ignoreUnused (configured);
    return lookAndFeel;
}

// Custom IDs used by Light Host's own custom-painted controls.
constexpr int panelBackgroundColourId = 0x2410001;
constexpr int controlBackgroundColourId = 0x2410002;
constexpr int secondaryTextColourId = 0x2410003;
constexpr int selectionBackgroundColourId = 0x2410004;
constexpr int activePluginColourId = 0x2410005;
constexpr int bypassedPluginColourId = 0x2410006;
constexpr int failedPluginColourId = 0x2410007;
constexpr int titleBarBackgroundColourId = 0x2410008;

struct Palette
{
    Colour window       { 0xff30363c };
    Colour panel        { 0xff343a40 };
    Colour control      { 0xff252a30 };
    Colour text         { 0xfff1f3f5 };
    Colour secondary    { 0xffb8c0c8 };
    Colour accent       { 0xff55a8e8 };
    Colour selection    { 0xff28659a };
    Colour border       { 0xff49515a };
    Colour activePlugin { 0xff2e9b57 };
    Colour bypassed     { 0xff737982 };
    Colour failed       { 0xffd94a4a };
    Colour titleBar     { 0xff252a30 };
    Colour buttonBackground { 0xff252a30 };
    Colour buttonText       { 0xfff1f3f5 };
    Colour dropdownBackground { 0xff252a30 };
    Colour dropdownText       { 0xfff1f3f5 };
};

enum class Role
{
    window, titleBar, panel, control, buttonBackground, buttonText,
    dropdownBackground, dropdownText, text, secondaryText, accent, selection, border,
    activePlugin, bypassedPlugin, failedPlugin
};

inline Colour& colourFor (Palette& p, Role r)
{
    switch (r)
    {
        case Role::window:         return p.window;
        case Role::titleBar:       return p.titleBar;
        case Role::panel:          return p.panel;
        case Role::control:        return p.control;
        case Role::buttonBackground: return p.buttonBackground;
        case Role::buttonText:       return p.buttonText;
        case Role::dropdownBackground: return p.dropdownBackground;
        case Role::dropdownText:       return p.dropdownText;
        case Role::text:           return p.text;
        case Role::secondaryText:  return p.secondary;
        case Role::accent:         return p.accent;
        case Role::selection:      return p.selection;
        case Role::border:         return p.border;
        case Role::activePlugin:   return p.activePlugin;
        case Role::bypassedPlugin: return p.bypassed;
        case Role::failedPlugin:   return p.failed;
    }
    return p.window;
}

inline const juce::Colour& colourFor (const Palette& p, Role r)
{
    switch (r)
    {
        case Role::window:         return p.window;
        case Role::titleBar:       return p.titleBar;
        case Role::panel:          return p.panel;
        case Role::control:        return p.control;
        case Role::buttonBackground: return p.buttonBackground;
        case Role::buttonText:       return p.buttonText;
        case Role::dropdownBackground: return p.dropdownBackground;
        case Role::dropdownText:       return p.dropdownText;
        case Role::text:           return p.text;
        case Role::secondaryText:  return p.secondary;
        case Role::accent:         return p.accent;
        case Role::selection:      return p.selection;
        case Role::border:         return p.border;
        case Role::activePlugin:   return p.activePlugin;
        case Role::bypassedPlugin: return p.bypassed;
        case Role::failedPlugin:   return p.failed;
    }
    return p.window;
}

inline String nameFor (Role r)
{
    switch (r)
    {
        case Role::window:         return "Window background";
        case Role::titleBar:       return "Title bar background";
        case Role::panel:          return "Panels and lists";
        case Role::control:        return "Textbox background";
        case Role::buttonBackground: return "Button background";
        case Role::buttonText:       return "Button text";
        case Role::dropdownBackground: return "Drop-down background";
        case Role::dropdownText:       return "Drop-down text";
        case Role::text:           return "Main text";
        case Role::secondaryText:  return "Secondary text";
        case Role::accent:         return "Accent";
        case Role::selection:      return "Selection highlight";
        case Role::border:         return "Borders";
        case Role::activePlugin:   return "Active plug-in indicator";
        case Role::bypassedPlugin: return "Bypassed plug-in indicator";
        case Role::failedPlugin:   return "Failed plug-in indicator";
    }
    return {};
}

inline String keyFor (Role r)
{
    switch (r)
    {
        case Role::window:         return "themeColourWindow";
        case Role::titleBar:       return "themeColourTitleBar";
        case Role::panel:          return "themeColourPanel";
        case Role::control:        return "themeColourControl";
        case Role::buttonBackground: return "themeColourButtonBackground";
        case Role::buttonText:       return "themeColourButtonText";
        case Role::dropdownBackground: return "themeColourDropdownBackground";
        case Role::dropdownText:       return "themeColourDropdownText";
        case Role::text:           return "themeColourText";
        case Role::secondaryText:  return "themeColourSecondaryText";
        case Role::accent:         return "themeColourAccent";
        case Role::selection:      return "themeColourSelection";
        case Role::border:         return "themeColourBorder";
        case Role::activePlugin:   return "themeColourPluginActive";
        case Role::bypassedPlugin: return "themeColourPluginBypassed";
        case Role::failedPlugin:   return "themeColourPluginFailed";
    }
    return {};
}

inline std::optional<juce::Colour> parseColourCode (juce::String value)
{
    value = value.trim();
    if (value.length() != 6 && value.length() != 8) // 8 digits retained for old saved themes.
        return {};
    for (auto c : value)
        if (juce::CharacterFunctions::getHexDigitValue (c) < 0)
            return {};
    if (value.length() == 8)
        value = value.substring (2); // Discard the legacy alpha byte; themes now use opaque RGB.
    return juce::Colour::fromString ("FF" + value);
}

inline void refreshHostComponentTree (juce::Component& component)
{
    auto& defaultLookAndFeel = juce::LookAndFeel::getDefaultLookAndFeel();
    if (&component.getLookAndFeel() != &defaultLookAndFeel)
        return; // A plug-in editor with its own look-and-feel is intentionally excluded.
    component.sendLookAndFeelChange();
    for (int i = 0; i < component.getNumChildComponents(); ++i)
        if (auto* child = component.getChildComponent (i))
            refreshHostComponentTree (*child);
}

class HostLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    class TitleBarButton final : public juce::Button
    {
    public:
        TitleBarButton (const juce::String& name, juce::Path normalShape_, juce::Path toggledShape_)
            : Button (name), normalShape (std::move (normalShape_)), toggledShape (std::move (toggledShape_))
        {
            setWantsKeyboardFocus (false);
        }

        void paintButton (juce::Graphics& g, bool highlighted, bool down) override
        {
            auto* window = findParentComponentOfClass<juce::DocumentWindow>();
            const auto background = window != nullptr
                ? window->findColour (titleBarBackgroundColourId)
                : juce::LookAndFeel::getDefaultLookAndFeel().findColour (titleBarBackgroundColourId);
            const auto textColour = window != nullptr
                ? window->findColour (juce::DocumentWindow::textColourId)
                : juce::LookAndFeel::getDefaultLookAndFeel().findColour (juce::DocumentWindow::textColourId);

            g.fillAll (highlighted || down ? background.contrasting (down ? 0.16f : 0.09f) : background);
            g.setColour (textColour);
            const auto bounds = juce::Justification (juce::Justification::centred)
                .appliedToRectangle (juce::Rectangle<int> (getHeight(), getHeight()), getLocalBounds())
                .toFloat().reduced ((float) getHeight() * 0.3f);
            const auto& shape = getToggleState() ? toggledShape : normalShape;
            g.fillPath (shape, shape.getTransformToScaleToFit (bounds, true));
        }

    private:
        juce::Path normalShape, toggledShape;
    };

    juce::Button* createDocumentWindowButton (int buttonType) override
    {
        juce::Path shape;
        shape.addLineSegment ({ 0.0f, 0.0f, 1.0f, 1.0f }, 0.15f);
        shape.addLineSegment ({ 1.0f, 0.0f, 0.0f, 1.0f }, 0.15f);
        if (buttonType == juce::DocumentWindow::closeButton)
            return new TitleBarButton ("close", shape, shape);

        if (buttonType == juce::DocumentWindow::minimiseButton)
        {
            shape.clear();
            shape.addLineSegment ({ 0.0f, 0.5f, 1.0f, 0.5f }, 0.15f);
            return new TitleBarButton ("minimise", shape, shape);
        }

        if (buttonType == juce::DocumentWindow::maximiseButton)
        {
            shape.clear();
            shape.addLineSegment ({ 0.5f, 0.0f, 0.5f, 1.0f }, 0.15f);
            shape.addLineSegment ({ 0.0f, 0.5f, 1.0f, 0.5f }, 0.15f);
            juce::Path maximisedShape;
            maximisedShape.addRectangle (0.15f, 0.15f, 0.7f, 0.7f);
            return new TitleBarButton ("maximise", shape, maximisedShape);
        }

        jassertfalse;
        return nullptr;
    }

    void drawDocumentWindowTitleBar (juce::DocumentWindow& window, juce::Graphics& g,
                                     int width, int height, int titleSpaceX, int titleSpaceWidth,
                                     const juce::Image* icon, bool drawTitleTextOnLeft) override
    {
        if (width <= 0 || height <= 0)
            return;

        g.fillAll (findColour (titleBarBackgroundColourId));
        const auto font = juce::Font (juce::FontOptions { (float) height * 0.65f });
        g.setFont (font);
        auto textWidth = juce::GlyphArrangement::getStringWidthInt (font, window.getName());
        auto iconWidth = 0;
        auto iconHeight = 0;
        if (icon != nullptr && icon->getHeight() > 0)
        {
            iconHeight = static_cast<int> (font.getHeight());
            iconWidth = icon->getWidth() * iconHeight / icon->getHeight() + 4;
        }

        textWidth = juce::jmin (titleSpaceWidth, textWidth + iconWidth);
        auto textX = drawTitleTextOnLeft ? titleSpaceX
                                         : juce::jmax (titleSpaceX, (width - textWidth) / 2);
        if (textX + textWidth > titleSpaceX + titleSpaceWidth)
            textX = titleSpaceX + titleSpaceWidth - textWidth;

        if (icon != nullptr && iconWidth > 0)
        {
            g.setOpacity (window.isActiveWindow() ? 1.0f : 0.6f);
            g.drawImageWithin (*icon, textX, (height - iconHeight) / 2, iconWidth, iconHeight,
                               juce::RectanglePlacement::centred, false);
            textX += iconWidth;
            textWidth -= iconWidth;
        }

        g.setOpacity (1.0f);
        g.setColour (window.findColour (juce::DocumentWindow::textColourId));
        g.drawText (window.getName(), textX, 0, textWidth, height,
                    juce::Justification::centredLeft, true);
    }
};

inline Palette preset (const String& name)
{
    Palette p;
    if (name == "Light")
    {
        p.window = Colour (0xfff1f3f5); p.panel = Colour (0xffffffff); p.control = Colour (0xffe4e8ec);
        p.text = Colour (0xff20242a); p.secondary = Colour (0xff59636e); p.accent = Colour (0xff1976d2);
        p.selection = Colour (0xffc9e3fa); p.border = Colour (0xffc4cbd2);
        p.activePlugin = Colour (0xff178343); p.bypassed = Colour (0xff858b91); p.failed = Colour (0xffc83030);
        p.titleBar = Colour (0xffe4e8ec);
        p.buttonBackground = Colour (0xffe4e8ec); p.buttonText = Colour (0xff20242a);
        p.dropdownBackground = Colour (0xffe4e8ec); p.dropdownText = Colour (0xff20242a);
    }
    else if (name == "Midnight")
    {
        p.window = Colour (0xff101923); p.panel = Colour (0xff172331); p.control = Colour (0xff223244);
        p.text = Colour (0xffe8f0f7); p.secondary = Colour (0xffa9bacb); p.accent = Colour (0xff35b6c8);
        p.selection = Colour (0xff22556a); p.border = Colour (0xff34485c);
        p.activePlugin = Colour (0xff32a867); p.bypassed = Colour (0xff78899b); p.failed = Colour (0xffe04d5d);
        p.titleBar = Colour (0xff0b121b);
        p.buttonBackground = Colour (0xff223244); p.buttonText = Colour (0xffe8f0f7);
        p.dropdownBackground = Colour (0xff223244); p.dropdownText = Colour (0xffe8f0f7);
    }
    return p;
}

inline constexpr Role paletteRoles[] = {
    Role::window, Role::titleBar, Role::panel, Role::control,
    Role::buttonBackground, Role::buttonText, Role::dropdownBackground,
    Role::dropdownText, Role::text, Role::secondaryText,
    Role::accent, Role::selection, Role::border, Role::activePlugin,
    Role::bypassedPlugin, Role::failedPlugin
};

inline Palette loadCustom (PropertiesFile& settings)
{
    auto p = preset ("Dark");
    for (auto r : paletteRoles)
    {
        auto& c = colourFor (p, r);
        c = parseColourCode (settings.getValue (keyFor (r), c.toDisplayString (false)))
                .value_or (c);
    }
    return p;
}

inline Palette load (PropertiesFile& settings)
{
    const auto name = settings.getValue ("hostTheme", "Dark");
    if (name == "Custom") return loadCustom (settings);
    if (name == "Light" || name == "Midnight") return preset (name);
    return preset ("Dark");
}

inline void saveCustom (PropertiesFile& settings, const Palette& p)
{
    settings.setValue ("hostTheme", "Custom");
    for (auto r : paletteRoles)
        settings.setValue (keyFor (r), colourFor (p, r).toDisplayString (false));
    settings.saveIfNeeded();
}

inline void apply (LookAndFeel_V4& lf, const Palette& p)
{
    lf.setColour (ResizableWindow::backgroundColourId, p.window);
    lf.setColour (DocumentWindow::backgroundColourId, p.window);
    lf.setColour (DocumentWindow::textColourId, p.text);
    lf.setColour (titleBarBackgroundColourId, p.titleBar);
    lf.setColour (TabbedComponent::backgroundColourId, p.window);
    lf.setColour (Label::textColourId, p.text);
    lf.setColour (Label::textWhenEditingColourId, p.text);
    lf.setColour (Label::backgroundWhenEditingColourId, p.control);
    lf.setColour (TextButton::buttonColourId, p.buttonBackground);
    lf.setColour (TextButton::buttonOnColourId, p.accent);
    lf.setColour (TextButton::textColourOffId, p.buttonText);
    lf.setColour (TextButton::textColourOnId, p.buttonText);
    lf.setColour (ComboBox::backgroundColourId, p.dropdownBackground);
    lf.setColour (ComboBox::textColourId, p.dropdownText);
    lf.setColour (ComboBox::outlineColourId, p.border);
    lf.setColour (ComboBox::arrowColourId, p.dropdownText);
    lf.setColour (ComboBox::buttonColourId, p.dropdownBackground);
    lf.setColour (ComboBox::focusedOutlineColourId, p.accent);
    lf.setColour (TextEditor::backgroundColourId, p.control);
    lf.setColour (TextEditor::textColourId, p.text);
    lf.setColour (TextEditor::outlineColourId, p.border);
    lf.setColour (TextEditor::highlightColourId, p.selection);
    lf.setColour (TextEditor::highlightedTextColourId, p.text);
    lf.setColour (TextEditor::focusedOutlineColourId, p.accent);
    lf.setColour (ListBox::backgroundColourId, p.panel);
    lf.setColour (ListBox::textColourId, p.text);
    lf.setColour (TabbedButtonBar::frontTextColourId, p.text);
    lf.setColour (TabbedButtonBar::tabOutlineColourId, p.border);
    lf.setColour (ScrollBar::thumbColourId, p.secondary.withAlpha (0.65f));
    lf.setColour (PopupMenu::backgroundColourId, p.dropdownBackground);
    lf.setColour (PopupMenu::textColourId, p.dropdownText);
    lf.setColour (PopupMenu::headerTextColourId, p.text);
    lf.setColour (PopupMenu::highlightedBackgroundColourId, p.selection);
    lf.setColour (PopupMenu::highlightedTextColourId, p.dropdownText);
    lf.setColour (ColourSelector::backgroundColourId, p.panel);
    lf.setColour (ColourSelector::labelTextColourId, p.text);
    lf.setColour (panelBackgroundColourId, p.panel);
    lf.setColour (controlBackgroundColourId, p.control);
    lf.setColour (secondaryTextColourId, p.secondary);
    lf.setColour (selectionBackgroundColourId, p.selection);
    lf.setColour (activePluginColourId, p.activePlugin);
    lf.setColour (bypassedPluginColourId, p.bypassed);
    lf.setColour (failedPluginColourId, p.failed);
}

class Editor final : public Component
{
public:
    using Changed = std::function<void (const Palette&)>;

    Editor (Palette initial, Changed changed_)
        : palette (std::move (initial)), changed (std::move (changed_))
    {
        setSize (580, 560);
        setLookAndFeel (&customizationLookAndFeel());
        description.setText ("Enter a 6-digit RGB hex color. Changes apply and save immediately.", dontSendNotification);
        addAndMakeVisible (description);
        rowsViewport.setViewedComponent (&rowsContent, false);
        rowsViewport.setScrollBarsShown (true, false);
        addAndMakeVisible (rowsViewport);
        for (auto r : allRoles)
        {
            auto row = std::make_unique<Row> (*this, r);
            rowsContent.addAndMakeVisible (*row);
            rows.push_back (std::move (row));
        }
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (12);
        description.setBounds (area.removeFromTop (30));
        rowsViewport.setBounds (area);
        rowsContent.setSize (rowsViewport.getWidth(), static_cast<int> (rows.size()) * 38);
        for (int i = 0; i < (int) rows.size(); ++i)
            rows[(size_t) i]->setBounds (0, i * 38, rowsViewport.getWidth(), 38);
    }

private:
    static constexpr Role allRoles[] = {
        Role::window, Role::titleBar, Role::panel, Role::control,
        Role::buttonBackground, Role::buttonText, Role::dropdownBackground,
        Role::dropdownText, Role::text, Role::secondaryText,
        Role::accent, Role::selection, Role::border, Role::activePlugin,
        Role::bypassedPlugin, Role::failedPlugin
    };

    class Row final : public Component
    {
    public:
        Row (Editor& owner_, Role role_) : owner (owner_), role (role_)
        {
            label.setText (nameFor (role), dontSendNotification);
            addAndMakeVisible (label);
            // The customization editor follows Windows/JUCE system colors, not the host palette.
            colourCode.setLookAndFeel (&customizationLookAndFeel());
            colourCode.setInputRestrictions (6, "0123456789abcdefABCDEF");
            colourCode.setJustification (Justification::centred);
            colourCode.onTextChange = [this]
            {
                if (colourCode.getText().length() == 6)
                    if (auto parsed = parseColourCode (colourCode.getText()))
                    owner.setColour (role, *parsed);
            };
            addAndMakeVisible (colourCode);
            button.setTooltip ("Current color preview. Enter its RGB value in the field.");
            button.setInterceptsMouseClicks (false, false);
            addAndMakeVisible (button);
            updateSwatch();
        }
        void resized() override
        {
            auto area = getLocalBounds();
            label.setBounds (area.removeFromLeft (195));
            button.setBounds (area.removeFromRight (112).reduced (0, 3));
            colourCode.setBounds (area.reduced (3, 4));
        }
        void updateSwatch()
        {
            const auto colour = colourFor (owner.palette, role);
            button.setButtonText ({});
            button.setColour (TextButton::buttonColourId, colour);
            button.setColour (TextButton::textColourOffId, colour.contrasting());
            const auto code = colour.toDisplayString (false);
            if (colourCode.getText() != code)
                colourCode.setText (code, dontSendNotification);
            button.repaint();
            colourCode.repaint();
            repaint();
        }
    private:
        Editor& owner;
        Role role;
        Label label;
        TextEditor colourCode;
        TextButton button;
    };

    void setColour (Role role, Colour colour)
    {
        colourFor (palette, role) = colour;
        if (changed)
            changed (palette);
        for (auto& row : rows)
            row->updateSwatch();
        rowsViewport.repaint();
        rowsContent.repaint();
        repaint();
    }

    Palette palette;
    Changed changed;
    Label description;
    Viewport rowsViewport;
    Component rowsContent;
    std::vector<std::unique_ptr<Row>> rows;
};

} // namespace LightHostTheme

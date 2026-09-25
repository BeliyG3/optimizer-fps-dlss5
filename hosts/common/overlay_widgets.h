#pragma once

// ImGui pieces shared by the add-on's own tab and the remote tab that 32-bit games show, so a setting
// looks and resets the same way in both. Every setting is one row: the slider, a reset icon, then the
// label as plain text, so the label and the icon never fall off the right edge of the window.

namespace ofps::ui {

// Keep long schema labels within a row; the full description remains in help.
const char *DisplayLabel(const char *label);

// Width for a control whose label is drawn to its right: the rest of the line minus the label's real
// width, and minus a reset icon when one follows.
float ControlWidthFor(const char *label, bool withIcon);

// A square button with a circular arrow drawn by hand (the overlay font has no icons). True on click.
bool ResetIconButton(const char *id, const char *tooltip);

struct SliderRow {
    bool changed; // edited, or reset to the default
    bool active;  // being dragged right now (callers apply such edits on release)
};

// One setting row. The icon restores defaultValue; resetTooltip null means "Reset to the default (x)"
// with x printed through `format`. With doubleClickResets a double click on the slider resets it too.
// idScope keeps the ImGui ID a row had before it used this helper; null means the label.
SliderRow SliderWithReset(const char *label, float *value, float lo, float hi, float defaultValue,
                          const char *format = "%.1f", const char *resetTooltip = nullptr,
                          bool doubleClickResets = false, const char *idScope = nullptr);

SliderRow SliderIntWithReset(const char *label, int *value, int lo, int hi, int defaultValue);

} // namespace ofps::ui

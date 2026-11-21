// The settings form of Picture and sound and Advanced (docs/design/launcher.md, "Picture and
// sound"): one row per setting, its name on the left and its control on the right, the focused
// row lit by an amber band. Sizes are logical pixels times the scale.
#pragma once

#include <functional>
#include <initializer_list>
#include <string>
#include <vector>

namespace bb::launcher {

class Form {
public:
    // The rows' geometry, logical pixels: eleven rows fill a page's 448.
    static constexpr float row_h = 40;
    static constexpr float control_x = 230;  // from the window's left edge
    static constexpr float control_w = 226;

    // Opens the form as a child window across the page from the cursor, `height` high: all rows
    // fit at the design's scale, at a small one the child scrolls and gamepad navigation scrolls
    // it to the focused row. `focus_first` moves the gamepad focus to the first row.
    Form(const char* id, float height, float scale, bool focus_first);
    ~Form();
    Form(const Form&) = delete;
    Form& operator=(const Form&) = delete;

    // One row: `label` on the left, the control `draw` puts at the cursor on the right (its
    // width set to control_w). The row of the focused or hovered control gets a band behind it
    // and a lighter label. Returns what `draw` returns: whether the choice changed.
    bool row(const char* label, const std::function<bool()>& draw);

    float scale() const { return scale_; }

private:
    float scale_;
    bool focus_first_;
    int row_ = 0;
};

// A dropdown of `labels`. The closed dropdown shows the choice shortened to its width; the list
// shows the whole names. Returns true when the choice changed.
bool dropdown(const char* id, const std::vector<std::string>& labels, int& index);

// A dropdown of `names` with `first` (the choice that writes no key) at the top. A name that is
// set but no longer listed (a card or monitor that is gone) stays as the last entry. An empty
// `value` is `first`.
bool name_dropdown(const char* id, const char* first, std::vector<std::string> names,
                   std::string& value);

// The width of the value column of a form's sliders: the widest of `values` and a gap.
float value_column(std::initializer_list<std::string> values, float scale);

// A slider of whole numbers from `min` to `max` in a row's control space: a thin line with a
// diamond at the value and a small tick at `mark` (the default), with `label(value)'
// right-aligned in the last `value_w` of the row, where the diamond can't cover it. Left and
// right move it by `step` while it has the focus, without pressing A first, as in a game's
// settings. Returns true when it moved.
bool line_slider(const char* id, int& value, int min, int max, int mark, int step, float value_w,
                 const std::function<std::string(int)>& label, float scale);

}  // namespace bb::launcher

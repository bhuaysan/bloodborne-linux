// The keyboard, text and mouse events the window hands to ImGui for the IME dialog (#795): queued
// on the thread that pumps them, handed to ImGui's input queue in the next NewFrame
// (src/shadps4_host/imgui_impl_sdl3.cpp). Needs no window or GPU.

#include <cstring>
#include <string>
#include <thread>

#include <SDL3/SDL.h>
#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include "imgui_impl_sdl3.h"

namespace {

class ImGuiInput : public ::testing::Test {
protected:
    void SetUp() override {
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().LogFilename = nullptr;
        ASSERT_TRUE(ImGui::Sdl::Init(nullptr));
        ImGui::Sdl::ClearInput();
        ImGui::Sdl::NewFrame(false);
        ImGui::GetCurrentContext()->InputEventsQueue.clear();
    }
    void TearDown() override {
        ImGui::Sdl::Shutdown();
        ImGui::DestroyContext();
    }

    static const ImVector<ImGuiInputEvent>& Events() {
        return ImGui::GetCurrentContext()->InputEventsQueue;
    }
};

SDL_Event KeyEvent(bool down, SDL_Keycode key, SDL_Keymod mod = SDL_KMOD_NONE) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.key = key;
    e.key.mod = mod;
    e.key.down = down;
    return e;
}

const ImGuiInputEvent* FindKey(const ImVector<ImGuiInputEvent>& events, ImGuiKey key) {
    for (const ImGuiInputEvent& e : events) {
        if (e.Type == ImGuiInputEventType_Key && e.Key.Key == key) {
            return &e;
        }
    }
    return nullptr;
}

// Typed text keeps its characters and order, and the text is copied: SDL frees an event's text
// at the next poll, before the presenter's frame.
TEST_F(ImGuiInput, TextArrivesInOrderAndIsCopied) {
    for (const char* piece : {"Ge", "h", "\xC3\xA4"}) {  // "ä" as UTF-8
        std::string owned = piece;
        SDL_Event e{};
        e.type = SDL_EVENT_TEXT_INPUT;
        e.text.text = owned.c_str();
        EXPECT_TRUE(ImGui::Sdl::ProcessEvent(&e));
        std::memset(owned.data(), 'x', owned.size());
    }
    ImGui::Sdl::NewFrame(false);
    std::u32string text;
    for (const ImGuiInputEvent& e : Events()) {
        ASSERT_EQ(e.Type, ImGuiInputEventType_Text);
        text += static_cast<char32_t>(e.Text.Char);
    }
    EXPECT_EQ(text, U"Gehä");
}

// The editing keys and Ctrl shortcuts map to ImGui's keys, with the modifiers first.
TEST_F(ImGuiInput, KeysAndModifiers) {
    for (const SDL_Event& e : {KeyEvent(true, SDLK_RETURN), KeyEvent(false, SDLK_RETURN),
                               KeyEvent(true, SDLK_ESCAPE), KeyEvent(true, SDLK_BACKSPACE),
                               KeyEvent(true, SDLK_V, SDL_KMOD_LCTRL)}) {
        EXPECT_TRUE(ImGui::Sdl::ProcessEvent(&e));
    }
    SDL_Event keypad = KeyEvent(true, SDLK_KP_ENTER);
    keypad.key.scancode = SDL_SCANCODE_KP_ENTER;
    EXPECT_TRUE(ImGui::Sdl::ProcessEvent(&keypad));
    ImGui::Sdl::NewFrame(false);

    const ImGuiInputEvent* enter = FindKey(Events(), ImGuiKey_Enter);
    ASSERT_NE(enter, nullptr);
    EXPECT_TRUE(enter->Key.Down);
    EXPECT_NE(FindKey(Events(), ImGuiKey_Escape), nullptr);
    EXPECT_NE(FindKey(Events(), ImGuiKey_Backspace), nullptr);
    EXPECT_NE(FindKey(Events(), ImGuiKey_KeypadEnter), nullptr);
    const ImGuiInputEvent* v = FindKey(Events(), ImGuiKey_V);
    ASSERT_NE(v, nullptr);
    const ImGuiInputEvent* ctrl = FindKey(Events(), ImGuiMod_Ctrl);
    ASSERT_NE(ctrl, nullptr);
    EXPECT_LT(ctrl, v);
    // The last Ctrl state before V is down.
    bool ctrl_down = false;
    for (const ImGuiInputEvent* e = Events().begin(); e != v; ++e) {
        if (e->Type == ImGuiInputEventType_Key && e->Key.Key == ImGuiMod_Ctrl) {
            ctrl_down = e->Key.Down;
        }
    }
    EXPECT_TRUE(ctrl_down);
}

// The mouse moves and clicks; buttons other than the three ImGui knows are not taken.
TEST_F(ImGuiInput, Mouse) {
    SDL_Event motion{};
    motion.type = SDL_EVENT_MOUSE_MOTION;
    motion.motion.x = 12.0f;
    motion.motion.y = 40.0f;
    SDL_Event down{};
    down.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    down.button.button = SDL_BUTTON_LEFT;
    SDL_Event x1 = down;
    x1.button.button = SDL_BUTTON_X1;
    EXPECT_TRUE(ImGui::Sdl::ProcessEvent(&motion));
    EXPECT_TRUE(ImGui::Sdl::ProcessEvent(&down));
    EXPECT_FALSE(ImGui::Sdl::ProcessEvent(&x1));
    ImGui::Sdl::NewFrame(false);

    bool moved = false;
    bool clicked = false;
    for (const ImGuiInputEvent& e : Events()) {
        if (e.Type == ImGuiInputEventType_MousePos) {
            moved = e.MousePos.PosX == 12.0f && e.MousePos.PosY == 40.0f;
        } else if (e.Type == ImGuiInputEventType_MouseButton) {
            EXPECT_EQ(e.MouseButton.Button, ImGuiMouseButton_Left);
            clicked = e.MouseButton.Down;
        }
    }
    EXPECT_TRUE(moved);
    EXPECT_TRUE(clicked);
}

// ClearInput drops what was queued before it and releases what ImGui holds down, so keys typed
// before the dialog opened never reach it.
TEST_F(ImGuiInput, ClearInputDropsEarlierEvents) {
    const SDL_Event held = KeyEvent(true, SDLK_A);
    ImGui::Sdl::ProcessEvent(&held);
    ImGui::Sdl::NewFrame(false);
    ImGui::GetIO().ClearEventsQueue();
    ImGui::GetKeyData(ImGuiKey_A)->Down = true;  // as if a frame had applied it

    const SDL_Event stale = KeyEvent(true, SDLK_RETURN);
    ImGui::Sdl::ProcessEvent(&stale);
    ImGui::Sdl::ClearInput();
    const SDL_Event fresh = KeyEvent(true, SDLK_ESCAPE);
    ImGui::Sdl::ProcessEvent(&fresh);
    ImGui::Sdl::NewFrame(false);

    EXPECT_FALSE(ImGui::GetKeyData(ImGuiKey_A)->Down);
    EXPECT_EQ(FindKey(Events(), ImGuiKey_Enter), nullptr);
    EXPECT_NE(FindKey(Events(), ImGuiKey_Escape), nullptr);
}

// Events queued on another thread arrive whole.
TEST_F(ImGuiInput, QueuedFromAnotherThread) {
    std::thread pump([] {
        for (int i = 0; i < 100; ++i) {
            SDL_Event e{};
            e.type = SDL_EVENT_TEXT_INPUT;
            e.text.text = "a";
            ImGui::Sdl::ProcessEvent(&e);
        }
    });
    pump.join();
    ImGui::Sdl::NewFrame(false);
    EXPECT_EQ(Events().Size, 100);
}

// Other events are not ImGui's.
TEST_F(ImGuiInput, IgnoresOtherEvents) {
    SDL_Event e{};
    e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    EXPECT_FALSE(ImGui::Sdl::ProcessEvent(&e));
    ImGui::Sdl::NewFrame(false);
    EXPECT_EQ(Events().Size, 0);
}

}  // namespace

#include "launcher/logic.hpp"

#include <stdexcept>

#include <gtest/gtest.h>

#include "launcher/text.hpp"

namespace {

using namespace bb::launcher;

TEST(LauncherLogic, ChoosesTheLanguageFromTheLocale) {
    EXPECT_EQ(choose_language("de_DE.UTF-8"), Lang::de);
    EXPECT_EQ(choose_language("de"), Lang::de);
    EXPECT_EQ(choose_language("de:en"), Lang::de);
    EXPECT_EQ(choose_language("en_US.UTF-8"), Lang::en);
    EXPECT_EQ(choose_language("en:de"), Lang::en);
    EXPECT_EQ(choose_language("C"), Lang::en);
    EXPECT_EQ(choose_language(""), Lang::en);
    EXPECT_EQ(choose_language("deu_XX"), Lang::en);
}

TEST(LauncherLogic, ShowsTheVersionWithoutALeadingZero) {
    EXPECT_EQ(display_version("01.09"), "1.09");
    EXPECT_EQ(display_version("00.50"), "0.50");
    EXPECT_EQ(display_version("10.01"), "10.01");
    EXPECT_EQ(display_version("odd"), "odd");
    EXPECT_EQ(display_version("a.1"), "a.1");
}

TEST(LauncherLogic, CropsAroundTheCentre) {
    const CropRect wide = crop_around_centre(1920, 1080, 480, 216);
    EXPECT_EQ(wide.w, 1920);
    EXPECT_EQ(wide.h, 864);
    EXPECT_EQ(wide.x, 0);
    EXPECT_EQ(wide.y, 108);
    const CropRect tall = crop_around_centre(100, 400, 100, 100);
    EXPECT_EQ(tall.x, 0);
    EXPECT_EQ(tall.y, 150);
    EXPECT_EQ(tall.w, 100);
    EXPECT_EQ(tall.h, 100);
    const CropRect same = crop_around_centre(480, 216, 480, 216);
    EXPECT_EQ(same.w, 480);
    EXPECT_EQ(same.h, 216);
    EXPECT_EQ(crop_around_centre(0, 10, 5, 5).w, 0);
    EXPECT_EQ(crop_around_centre(10, 10, 5, 0).w, 0);
    // The narrowest case keeps at least one pixel.
    EXPECT_GE(crop_around_centre(10, 1, 1, 100).w, 1);
}

TEST(LauncherLogic, SelectsTheStateInTheOrderOfTheDesign) {
    StateInput in;
    EXPECT_EQ(select_state(in), MainState::no_folder);
    in.have_folder = true;
    EXPECT_EQ(select_state(in), MainState::check_failed);
    in.folder_ok = true;
    EXPECT_EQ(select_state(in), MainState::ready);
    in.controller = false;
    EXPECT_EQ(select_state(in), MainState::no_controller);
    in.last_run_failed = true;
    EXPECT_EQ(select_state(in), MainState::last_run_failed);
    in.gpu_ok = false;
    EXPECT_EQ(select_state(in), MainState::no_gpu);
    in.folder_ok = false;
    EXPECT_EQ(select_state(in), MainState::check_failed);
    in.have_folder = false;
    EXPECT_EQ(select_state(in), MainState::no_folder);
}

TEST(LauncherLogic, FirstButtonAndHeaderPerState) {
    EXPECT_EQ(first_button(MainState::no_folder), Action::set_up);
    EXPECT_EQ(first_button(MainState::check_failed), Action::set_up);
    EXPECT_EQ(first_button(MainState::ready), Action::play);
    EXPECT_EQ(first_button(MainState::no_controller), Action::play);
    EXPECT_EQ(first_button(MainState::no_gpu), Action::play);
    EXPECT_EQ(first_button(MainState::last_run_failed), Action::play);
    EXPECT_FALSE(shows_picture(MainState::no_folder));
    EXPECT_FALSE(shows_picture(MainState::check_failed));
    EXPECT_TRUE(shows_picture(MainState::no_gpu));
}

TEST(LauncherLogic, BbRunGetsTheConfigAndNothingElse) {
    const auto args = bb_run_arguments("/opt/bb/bb-run", "/srv/cfg.toml");
    EXPECT_EQ(args, (std::vector<std::string>{"/opt/bb/bb-run", "--config", "/srv/cfg.toml"}));
}

TEST(LauncherScript, ParsesEveryStep) {
    const auto steps = parse_script(
        "# a comment\n\nwait 5\n  drop /a b/c  \npress accept\npress back\nshot /s.png\n"
        "config /c.toml\nwaitgame 90\nquit\r\n");
    ASSERT_EQ(steps.size(), 8u);
    EXPECT_EQ(steps[0].kind, StepKind::wait);
    EXPECT_EQ(steps[0].number, 5);
    EXPECT_EQ(steps[1].kind, StepKind::drop);
    EXPECT_EQ(steps[1].text, "/a b/c");
    EXPECT_EQ(steps[2].button, PadButton::accept);
    EXPECT_EQ(steps[3].button, PadButton::back);
    EXPECT_EQ(steps[4].kind, StepKind::shot);
    EXPECT_EQ(steps[5].kind, StepKind::config);
    EXPECT_EQ(steps[6].kind, StepKind::waitgame);
    EXPECT_EQ(steps[6].number, 90);
    EXPECT_EQ(steps[7].kind, StepKind::quit);
}

TEST(LauncherScript, RefusesBadLinesWithTheirNumber) {
    for (const char* bad : {"jump\n", "wait\n", "wait x\n", "wait -1\n", "drop\n", "press fire\n",
                            "waitgame 1 2\n"}) {
        EXPECT_THROW(parse_script(bad), std::invalid_argument) << bad;
    }
    try {
        parse_script("wait 1\nwat\n");
        FAIL();
    } catch (const std::invalid_argument& e) {
        EXPECT_NE(std::string(e.what()).find("line 2"), std::string::npos);
    }
}

TEST(LauncherText, GermanHasEveryStringAndMessage) {
    const Strings& de = strings(Lang::de);
    const Strings& en = strings(Lang::en);
    const char* const Strings::*fields[] = {
        &Strings::play,           &Strings::set_up,        &Strings::picture_sound,
        &Strings::saves,          &Strings::quit,
        &Strings::back,           &Strings::status_no_game, &Strings::status_version,
        &Strings::status_controller, &Strings::status_no_controller, &Strings::status_no_gpu,
        &Strings::status_last_run_failed, &Strings::drop_hint, &Strings::set_up_first,
        &Strings::dialog_title};
    for (const auto field : fields) {
        ASSERT_NE(de.*field, nullptr);
        ASSERT_NE(en.*field, nullptr);
        EXPECT_STRNE(de.*field, "");
        // The version label is the same word in both languages.
        if (field != &Strings::status_version) {
            EXPECT_STRNE(de.*field, en.*field) << en.*field;
        }
    }
    bb::common::FolderResult r;
    for (const auto kind : {bb::common::FolderCheck::not_a_folder, bb::common::FolderCheck::no_eboot,
                            bb::common::FolderCheck::no_param_sfo, bb::common::FolderCheck::bad_param_sfo,
                            bb::common::FolderCheck::other_title, bb::common::FolderCheck::missing_update,
                            bb::common::FolderCheck::unknown_eboot}) {
        r.kind = kind;
        r.message = "english";
        EXPECT_FALSE(check_message(Lang::de, r).empty());
        EXPECT_NE(check_message(Lang::de, r), "english");
        EXPECT_EQ(check_message(Lang::en, r), "english");
    }
}

}  // namespace

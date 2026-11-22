#include "launcher/logic.hpp"

#include <stdlib.h>
#include <sys/stat.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>

#include <gtest/gtest.h>

#include "common/runtime_dirs.hpp"
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
    in.folder_ok = false;
    EXPECT_EQ(select_state(in), MainState::check_failed);
    in.have_folder = false;
    EXPECT_EQ(select_state(in), MainState::no_folder);
}

TEST(LauncherLogic, FirstButtonAndHeaderPerState) {
    EXPECT_EQ(first_button(MainState::no_folder), Action::set_up);
    EXPECT_EQ(first_button(MainState::check_failed), Action::set_up);
    EXPECT_EQ(first_button(MainState::ready), Action::play);
    EXPECT_FALSE(shows_picture(MainState::no_folder));
    EXPECT_FALSE(shows_picture(MainState::check_failed));
    EXPECT_TRUE(shows_picture(MainState::ready));
}

// The GPU is checked before the controller: without a GPU, a controller doesn't help.
TEST(LauncherLogic, PlayAsksAboutTheGpuThenTheController) {
    EXPECT_EQ(before_play(true, true), Prompt::none);
    EXPECT_EQ(before_play(true, false), Prompt::no_controller);
    EXPECT_EQ(before_play(false, true), Prompt::no_gpu);
    EXPECT_EQ(before_play(false, false), Prompt::no_gpu);
}

TEST(LauncherLogic, BbRunGetsTheConfigAndNothingElse) {
    const auto args = bb_run_arguments("/opt/bb/bb-run", "/srv/cfg.toml");
    EXPECT_EQ(args, (std::vector<std::string>{"/opt/bb/bb-run", "--config", "/srv/cfg.toml"}));
}

TEST(LauncherScript, ParsesEveryStep) {
    const auto steps = parse_script(
        "# a comment\n\nwait 5\n  drop /a b/c  \npress accept\npress back\nshot /s.png\n"
        "config /c.toml\nwaitgame 90\nnopad\nsearch\nquit\r\notherpad down\n");
    ASSERT_EQ(steps.size(), 11u);
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
    EXPECT_EQ(steps[7].kind, StepKind::nopad);
    EXPECT_EQ(steps[8].kind, StepKind::search);
    EXPECT_EQ(steps[9].kind, StepKind::quit);
    EXPECT_EQ(steps[10].kind, StepKind::otherpad);
    EXPECT_EQ(steps[10].button, PadButton::down);
}

TEST(LauncherScript, RefusesBadLinesWithTheirNumber) {
    for (const char* bad : {"jump\n", "wait\n", "wait x\n", "wait -1\n", "drop\n", "press fire\n", "otherpad\n",
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
        &Strings::play,           &Strings::set_up,        &Strings::settings,
        &Strings::picture_sound,  &Strings::advanced,      &Strings::controller,    &Strings::saves,
        &Strings::quit,           &Strings::back,
        &Strings::status_no_game, &Strings::status_version, &Strings::no_controller_title,
        &Strings::no_controller_text, &Strings::play_anyway, &Strings::cancel,
        &Strings::no_gpu_title,   &Strings::status_no_gpu, &Strings::status_last_run_failed,
        &Strings::run_failed_text, &Strings::notice_title, &Strings::ok,
        &Strings::drop_hint,      &Strings::set_up_first,  &Strings::dialog_title};
    for (const auto field : fields) {
        ASSERT_NE(de.*field, nullptr);
        ASSERT_NE(en.*field, nullptr);
        EXPECT_STRNE(de.*field, "");
        // These are the same words in both languages.
        if (field != &Strings::status_version && field != &Strings::controller &&
            field != &Strings::ok) {
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

TEST(LauncherLogic, ParsesTheDataFolderSteps) {
    const auto steps = parse_script("datafolder /tmp/some folder\ndatadefault\n");
    ASSERT_EQ(steps.size(), 2u);
    EXPECT_EQ(steps[0].kind, StepKind::datafolder);
    EXPECT_EQ(steps[0].text, "/tmp/some folder");
    EXPECT_EQ(steps[1].kind, StepKind::datadefault);
    EXPECT_THROW(parse_script("datafolder"), std::invalid_argument);
}

TEST(LauncherLogic, ParsesTheMoveSteps) {
    const auto steps = parse_script("movedata /srv/a b\nmovedefault\n");
    ASSERT_EQ(steps.size(), 2u);
    EXPECT_EQ(steps[0].kind, StepKind::movedata);
    EXPECT_EQ(steps[0].text, "/srv/a b");
    EXPECT_EQ(steps[1].kind, StepKind::movedefault);
    EXPECT_THROW(parse_script("movedata"), std::invalid_argument);
}

TEST(LauncherLogic, ProposesTheFolderNextToTheAppImageOrInHome) {
    EXPECT_EQ(propose_data_folder("/games/BB.AppImage", "/srv/p", true),
              std::filesystem::path("/games/Bloodborne-Linux-data"));
    EXPECT_EQ(propose_data_folder("/games/BB.AppImage", "/srv/p", false),
              std::filesystem::path("/srv/p/Bloodborne-Linux-data"));
    EXPECT_EQ(propose_data_folder("", "/srv/p", true),
              std::filesystem::path("/srv/p/Bloodborne-Linux-data"));
    EXPECT_TRUE(propose_data_folder("", "", false).empty());
    EXPECT_TRUE(propose_data_folder("/games/BB.AppImage", "relative", false).empty());
}

TEST(LauncherLogic, SearchesGameInTheDataFolderInAnAppImage) {
    EXPECT_EQ(game_search_dir(true, "/data", "/tmp/.mount_x/usr/bin"),
              std::filesystem::path("/data/game"));
    EXPECT_EQ(game_search_dir(false, "/data", "/opt/bb"), std::filesystem::path("/opt/bb/game"));
}

namespace {
DataFolderResult adopt_into_config(const std::filesystem::path& folder) {
    return adopt_data_folder(folder, bb::common::data_dir_pointer_path());
}
}  // namespace

TEST(LauncherLogic, AdoptingADataFolderCreatesItAndWritesThePointer) {
    namespace fs = std::filesystem;
    std::string templ = (fs::temp_directory_path() / "bb-datafolder-test-XXXXXX").string();
    ASSERT_NE(::mkdtemp(templ.data()), nullptr);
    const fs::path tmp = fs::canonical(templ);
    const char* saved = std::getenv("XDG_CONFIG_HOME");
    const std::string saved_value = saved != nullptr ? saved : "";
    ::setenv("XDG_CONFIG_HOME", (tmp / "config").c_str(), 1);

    const DataFolderResult ok = adopt_into_config(tmp / "a/data");
    EXPECT_TRUE(ok.ok()) << ok.detail;
    struct stat st {};
    ASSERT_EQ(::stat((tmp / "a/data").c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 07777, 0700u);
    EXPECT_EQ(bb::common::read_data_dir_pointer(), tmp / "a/data");

    EXPECT_EQ(adopt_into_config("relative").problem, DataFolderProblem::not_absolute);
    // Another pointer file is the one written, and none at all is no_place (nothing is created).
    EXPECT_TRUE(adopt_data_folder(tmp / "b/data", tmp / "elsewhere/data-dir").ok());
    EXPECT_EQ(bb::common::read_data_dir_pointer(tmp / "elsewhere/data-dir"), tmp / "b/data");
    EXPECT_EQ(bb::common::read_data_dir_pointer(), tmp / "a/data");
    EXPECT_EQ(adopt_data_folder(tmp / "c/data", {}).problem, DataFolderProblem::no_place);
    EXPECT_FALSE(fs::exists(tmp / "c"));
    std::ofstream(tmp / "a/data/eboot.bin") << "x";
    EXPECT_EQ(adopt_into_config(tmp / "a/data").problem, DataFolderProblem::is_game);
    std::ofstream(tmp / "file") << "x";
    EXPECT_EQ(adopt_into_config(tmp / "file/sub").problem, DataFolderProblem::cannot_create);
    EXPECT_EQ(adopt_into_config(bb::common::source_dir()).problem, DataFolderProblem::overlaps);
    // A refused folder leaves the pointer as it was.
    EXPECT_EQ(bb::common::read_data_dir_pointer(), tmp / "a/data");

    if (saved != nullptr) {
        ::setenv("XDG_CONFIG_HOME", saved_value.c_str(), 1);
    } else {
        ::unsetenv("XDG_CONFIG_HOME");
    }
    fs::remove_all(tmp);
}

// An AppImage kept inside the dump must not get a data folder in the game's folder (the dump is
// read-only for us), and a folder inside it is refused whether it exists or not.
TEST(LauncherLogic, ADataFolderInsideAGameFolderIsNeitherProposedNorAdopted) {
    namespace fs = std::filesystem;
    std::string templ = (fs::temp_directory_path() / "bb-datafolder-game-XXXXXX").string();
    ASSERT_NE(::mkdtemp(templ.data()), nullptr);
    const fs::path tmp = fs::canonical(templ);
    const char* saved = std::getenv("XDG_CONFIG_HOME");
    const std::string saved_value = saved != nullptr ? saved : "";
    ::setenv("XDG_CONFIG_HOME", (tmp / "config").c_str(), 1);

    const fs::path dump = tmp / "CUSA03173";
    fs::create_directories(dump / "sub");
    std::ofstream(dump / "eboot.bin") << "x";

    EXPECT_TRUE(inside_game_folder(dump));
    EXPECT_TRUE(inside_game_folder(dump / "sub"));
    EXPECT_TRUE(inside_game_folder(dump / "not/yet/there"));
    EXPECT_FALSE(inside_game_folder(tmp));
    EXPECT_FALSE(inside_game_folder(tmp / "Bloodborne-Linux-data"));

    // The AppImage lies in the dump: the proposal falls back to the home folder, and to nothing
    // when that lies in a dump as well.
    EXPECT_EQ(propose_data_folder(dump / "BB.AppImage", (tmp / "home").string(), true),
              tmp / "home/Bloodborne-Linux-data");
    EXPECT_TRUE(propose_data_folder(dump / "BB.AppImage", dump.string(), true).empty());
    EXPECT_EQ(propose_data_folder(tmp / "BB.AppImage", (tmp / "home").string(), true),
              tmp / "Bloodborne-Linux-data");

    EXPECT_EQ(adopt_into_config(dump).problem, DataFolderProblem::is_game);
    EXPECT_EQ(adopt_into_config(dump / "sub").problem, DataFolderProblem::inside_game);
    EXPECT_EQ(adopt_into_config(dump / "Bloodborne-Linux-data").problem,
              DataFolderProblem::inside_game);
    // Nothing was created in the dump, and no pointer was written.
    EXPECT_FALSE(fs::exists(dump / "Bloodborne-Linux-data"));
    EXPECT_FALSE(fs::exists(tmp / "config"));

    if (saved != nullptr) {
        ::setenv("XDG_CONFIG_HOME", saved_value.c_str(), 1);
    } else {
        ::unsetenv("XDG_CONFIG_HOME");
    }
    fs::remove_all(tmp);
}

}  // namespace

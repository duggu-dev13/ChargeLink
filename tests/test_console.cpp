#include <gtest/gtest.h>

#include <string>

#include "chargelink/sim/console.hpp"

using namespace chargelink::sim;

TEST(Console, BlankLinesAreEmpty) {
    EXPECT_TRUE(parse_command("").type == CommandType::Empty);
    EXPECT_TRUE(parse_command("   \t  ").type == CommandType::Empty);
}

TEST(Console, SimpleCommandsAndAliases) {
    EXPECT_TRUE(parse_command("help").type == CommandType::Help);
    EXPECT_TRUE(parse_command("?").type == CommandType::Help);
    EXPECT_TRUE(parse_command("unplug").type == CommandType::Unplug);
    EXPECT_TRUE(parse_command("pause").type == CommandType::Pause);
    EXPECT_TRUE(parse_command("resume").type == CommandType::Resume);
    EXPECT_TRUE(parse_command("fault").type == CommandType::Fault);
    EXPECT_TRUE(parse_command("clear").type == CommandType::Clear);
    EXPECT_TRUE(parse_command("status").type == CommandType::Status);
    EXPECT_TRUE(parse_command("quit").type == CommandType::Quit);
    EXPECT_TRUE(parse_command("exit").type == CommandType::Quit);
}

TEST(Console, CaseAndSpacingDoNotMatter) {
    EXPECT_TRUE(parse_command("  UNPLUG  ").type == CommandType::Unplug);
    EXPECT_TRUE(parse_command("Status").type == CommandType::Status);
}

TEST(Console, NoArgumentCommandsRejectArguments) {
    Command c = parse_command("unplug now");
    EXPECT_TRUE(c.type == CommandType::Invalid);
    EXPECT_FALSE(c.text.empty());
}

TEST(Console, PlugWithAndWithoutAmps) {
    Command a = parse_command("plug");
    EXPECT_TRUE(a.type == CommandType::Plug);
    EXPECT_FALSE(a.has_number);

    Command b = parse_command("plug 16");
    EXPECT_TRUE(b.type == CommandType::Plug);
    EXPECT_TRUE(b.has_number);
    EXPECT_DOUBLE_EQ(b.number, 16.0);

    EXPECT_DOUBLE_EQ(parse_command("plug 10.5").number, 10.5);
}

TEST(Console, PlugRejectsBadAmps) {
    EXPECT_TRUE(parse_command("plug 5").type == CommandType::Invalid);    // below 6 A
    EXPECT_TRUE(parse_command("plug 81").type == CommandType::Invalid);   // above 80 A
    EXPECT_TRUE(parse_command("plug abc").type == CommandType::Invalid);
    EXPECT_TRUE(parse_command("plug 16A").type == CommandType::Invalid);  // trailing junk
    EXPECT_TRUE(parse_command("plug nan").type == CommandType::Invalid);
    EXPECT_TRUE(parse_command("plug inf").type == CommandType::Invalid);
    EXPECT_TRUE(parse_command("plug 16 32").type == CommandType::Invalid);
}

TEST(Console, TapDefaultsToTag1) {
    Command c = parse_command("tap");
    EXPECT_TRUE(c.type == CommandType::Tap);
    EXPECT_EQ(c.text, "TAG1");
}

TEST(Console, TapKeepsTheTagAsTyped) {
    Command c = parse_command("tap BAD01");
    EXPECT_TRUE(c.type == CommandType::Tap);
    EXPECT_EQ(c.text, "BAD01");  // tags are case-sensitive: not lowercased
}

TEST(Console, TapRejectsLongTagsAndExtraWords) {
    EXPECT_TRUE(parse_command("tap " + std::string(20, 'x')).type == CommandType::Tap);
    EXPECT_TRUE(parse_command("tap " + std::string(21, 'x')).type == CommandType::Invalid);
    EXPECT_TRUE(parse_command("tap A B").type == CommandType::Invalid);
}

TEST(Console, HeatNeedsANumberInRange) {
    Command c = parse_command("heat 90");
    EXPECT_TRUE(c.type == CommandType::Heat);
    EXPECT_DOUBLE_EQ(c.number, 90.0);
    EXPECT_DOUBLE_EQ(parse_command("heat -10").number, -10.0);

    EXPECT_TRUE(parse_command("heat").type == CommandType::Invalid);
    EXPECT_TRUE(parse_command("heat hot").type == CommandType::Invalid);
    EXPECT_TRUE(parse_command("heat 151").type == CommandType::Invalid);
    EXPECT_TRUE(parse_command("heat -41").type == CommandType::Invalid);
}

TEST(Console, SpeedNeedsANumberInRange) {
    Command c = parse_command("speed 60");
    EXPECT_TRUE(c.type == CommandType::Speed);
    EXPECT_DOUBLE_EQ(c.number, 60.0);
    EXPECT_TRUE(parse_command("speed 0").type == CommandType::Invalid);
    EXPECT_TRUE(parse_command("speed 0.5").type == CommandType::Invalid);
    EXPECT_TRUE(parse_command("speed 3601").type == CommandType::Invalid);
    EXPECT_TRUE(parse_command("speed").type == CommandType::Invalid);
}

TEST(Console, UnknownCommandIsInvalidAndNamed) {
    Command c = parse_command("teleport");
    EXPECT_TRUE(c.type == CommandType::Invalid);
    EXPECT_TRUE(c.text.find("teleport") != std::string::npos);
}

TEST(Console, HelpTextMentionsEveryCommand) {
    const std::string help = help_text();
    for (const char* word : { "plug", "unplug", "tap", "pause", "resume", "fault", "clear",
                             "heat", "speed", "status", "quit" }) {
        EXPECT_TRUE(help.find(word) != std::string::npos);
    }
}
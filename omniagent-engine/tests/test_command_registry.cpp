#include <gtest/gtest.h>

#include <commands/command_registry.h>
#include <core/slash_commands.h>

using namespace omni::engine;

// ---------------------------------------------------------------------------
// CommandRegistry — RegisterAndHandle
// ---------------------------------------------------------------------------
TEST(CommandRegistry, RegisterAndHandle) {
    CommandRegistry reg;
    reg.add("greet", "Say hello", [](const std::string& args) -> CommandResult {
        return {.handled = true, .response = "Hello, " + args + "!"};
    });

    const auto result = reg.handle("greet", "world");
    EXPECT_TRUE(result.handled);
    EXPECT_EQ(result.response, "Hello, world!");
    EXPECT_FALSE(result.clear_messages);
}

// ---------------------------------------------------------------------------
// CommandRegistry — UnknownCommand
// ---------------------------------------------------------------------------
TEST(CommandRegistry, UnknownCommand) {
    CommandRegistry reg;
    const auto result = reg.handle("nonexistent", "");
    EXPECT_FALSE(result.handled);
    EXPECT_TRUE(result.response.empty());
}

// ---------------------------------------------------------------------------
// CommandRegistry — RemoveCommand
// ---------------------------------------------------------------------------
TEST(CommandRegistry, RemoveCommand) {
    CommandRegistry reg;
    reg.add("temp", "Temporary command", [](const std::string&) -> CommandResult {
        return {.handled = true, .response = "temp"};
    });
    EXPECT_EQ(reg.count(), 1u);

    reg.remove("temp");
    EXPECT_EQ(reg.count(), 0u);

    const auto result = reg.handle("temp", "");
    EXPECT_FALSE(result.handled);
}

// ---------------------------------------------------------------------------
// CommandRegistry — ListCommands
// ---------------------------------------------------------------------------
TEST(CommandRegistry, ListCommands) {
    CommandRegistry reg;
    reg.add("alpha", "First command",  [](const std::string&) -> CommandResult { return {true, "a"}; });
    reg.add("beta",  "Second command", [](const std::string&) -> CommandResult { return {true, "b"}; });

    EXPECT_EQ(reg.count(), 2u);

    const auto cmds = reg.list();
    ASSERT_EQ(cmds.size(), 2u);

    // Both names must be present (order is unspecified for unordered_map).
    auto has = [&](const std::string& name) {
        for (const auto& c : cmds) {
            if (c.name == name) return true;
        }
        return false;
    };
    EXPECT_TRUE(has("alpha"));
    EXPECT_TRUE(has("beta"));
}

// ---------------------------------------------------------------------------
// CommandRegistry — ClearMessagesFlagPropagated
// ---------------------------------------------------------------------------
TEST(CommandRegistry, ClearMessagesFlagPropagated) {
    CommandRegistry reg;
    reg.add("wipe", "Wipe history", [](const std::string&) -> CommandResult {
        return {.handled = true, .response = "wiped", .clear_messages = true};
    });

    const auto result = reg.handle("wipe", "");
    EXPECT_TRUE(result.handled);
    EXPECT_TRUE(result.clear_messages);
}

// ---------------------------------------------------------------------------
// SlashCommand — HelpCommand lists built-in + custom commands
// ---------------------------------------------------------------------------
TEST(SlashCommand, HelpCommand) {
    CommandRegistry reg;
    reg.add("mycommand", "A custom command", [](const std::string&) -> CommandResult {
        return {.handled = true, .response = "custom"};
    });

    std::vector<Message> msgs;
    Usage usage{};

    const auto result = handle_slash_command("/help", msgs, usage, "test-model", &reg);
    EXPECT_TRUE(result.handled);
    EXPECT_FALSE(result.clear_messages);

    // Must mention built-ins.
    EXPECT_NE(result.response_text.find("/clear"), std::string::npos);
    EXPECT_NE(result.response_text.find("/model"), std::string::npos);
    EXPECT_NE(result.response_text.find("/cost"),  std::string::npos);
    EXPECT_NE(result.response_text.find("/help"),  std::string::npos);

    // Must mention the custom command.
    EXPECT_NE(result.response_text.find("mycommand"), std::string::npos);
}

// ---------------------------------------------------------------------------
// SlashCommand — ModelCommand
// ---------------------------------------------------------------------------
TEST(SlashCommand, ModelCommand) {
    std::vector<Message> msgs;
    Usage usage{};

    const auto result = handle_slash_command("/model", msgs, usage, "test-model");
    EXPECT_TRUE(result.handled);
    EXPECT_NE(result.response_text.find("test-model"), std::string::npos);
}

TEST(SlashCommand, ModelCommandEmptyName) {
    std::vector<Message> msgs;
    Usage usage{};

    const auto result = handle_slash_command("/model", msgs, usage);
    EXPECT_TRUE(result.handled);
    // When no model name supplied should indicate it's not set.
    EXPECT_NE(result.response_text.find("not set"), std::string::npos);
}

// ---------------------------------------------------------------------------
// SlashCommand — CostCommand
// ---------------------------------------------------------------------------
TEST(SlashCommand, CostCommand) {
    std::vector<Message> msgs;
    Usage usage{};
    usage.input_tokens      = 1000;
    usage.output_tokens     = 500;
    usage.cache_read_tokens = 200;

    const auto result = handle_slash_command("/cost", msgs, usage);
    EXPECT_TRUE(result.handled);
    EXPECT_NE(result.response_text.find("1000"), std::string::npos);
    EXPECT_NE(result.response_text.find("500"),  std::string::npos);
    EXPECT_NE(result.response_text.find("200"),  std::string::npos);
    // Total = 1700
    EXPECT_NE(result.response_text.find("1700"), std::string::npos);
}

// ---------------------------------------------------------------------------
// SlashCommand — CustomCommandDispatch
// ---------------------------------------------------------------------------
TEST(SlashCommand, CustomCommandDispatch) {
    CommandRegistry reg;
    reg.add("ping", "Ping the engine", [](const std::string& args) -> CommandResult {
        return {.handled = true, .response = "pong:" + args};
    });

    std::vector<Message> msgs;
    Usage usage{};

    const auto result = handle_slash_command("/ping foo", msgs, usage, "", &reg);
    EXPECT_TRUE(result.handled);
    EXPECT_EQ(result.response_text, "pong:foo");
}

TEST(SlashCommand, UnknownCommandWithRegistry) {
    CommandRegistry reg;
    std::vector<Message> msgs;
    Usage usage{};

    // Neither built-in nor registered — should not be handled.
    const auto result = handle_slash_command("/unknown", msgs, usage, "", &reg);
    EXPECT_FALSE(result.handled);
}

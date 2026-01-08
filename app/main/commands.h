/**
 * @file commands.h
 * @brief Console/Telnet command registration and argument parsing helpers.
 *
 * This module exposes functions to register interactive commands (e.g. via uuid::console /
 * Telnet console) and small helpers to parse arguments safely.
 *
 * Typical usage:
 * @code
 * auto cmds = std::make_shared<uuid::console::Commands>();
 * registerCommands(cmds);
 * @endcode
 */

#pragma once

#include <memory>
#include <string>
#include <vector>

#include <uuid/common.h>
#include <uuid/console.h>
#include <uuid/telnet.h>
#include <uuid/log.h>

#include <PubSubClient.h>

#include "settings.h"
#include "librelinkup.h"
#include "mqtt.h"
#include "hba1c.h"
#include "main.h"
#include "helper.h"

/**
 * @defgroup console_commands Console Commands
 * @brief Command handlers and utilities for interactive control via console/telnet.
 * @{
 */

/**
 * @brief Register all available console commands.
 *
 * Adds command handlers (callbacks) to the provided command registry. These commands can
 * then be executed by a user through the console/telnet interface.
 *
 * @param commands Shared pointer to the uuid command registry.
 *
 * @note The concrete set of commands is defined in commands.cpp.
 * @note Handlers typically depend on other modules (MQTT, LibreLinkUp, HBA1C, settings).
 */
void registerCommands(std::shared_ptr<uuid::console::Commands> commands);

/**
 * @brief Parse an integer argument from a vector of command arguments.
 *
 * This helper reads the argument at @p index and converts it to an integer.
 * If the index is out of range or conversion fails, @p defaultValue is returned.
 *
 * @param arguments Vector containing command arguments (argv-like).
 * @param index Index of the argument to parse.
 * @param defaultValue Value returned on missing/invalid argument.
 * @return Parsed integer value, or @p defaultValue on error.
 */
int parseArgument(const std::vector<std::string> &arguments, size_t index, int defaultValue);

/** @} */ // end of console_commands
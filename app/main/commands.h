/**
 * @file commands.h
 * @brief Command registration and utility functions for handling console commands.
 *
 * This file provides the function declarations for registering commands,
 * parsing arguments, and handling command-line interactions.
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
 * @brief Registers available console commands.
 *
 * This function registers commands that can be executed via the console.
 * Commands are added to the given command registry.
 *
 * @param commands Shared pointer to the command registry.
 */
void registerCommands(std::shared_ptr<uuid::console::Commands> commands);

/**
 * @brief Parses an integer argument from a command argument list.
 *
 * This helper function extracts an integer value from the specified argument index.
 * If the index is out of range or parsing fails, a default value is returned.
 *
 * @param arguments The vector containing command arguments.
 * @param index The index of the argument to parse.
 * @param defaultValue The default value to return if parsing fails.
 * @return Parsed integer value or defaultValue if parsing fails.
 */
int parseArgument(const std::vector<std::string> &arguments, size_t index, int defaultValue);
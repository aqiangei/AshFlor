#pragma once
// Localize server system / squad chat lines and notices for the active UI language.
#include <string>
#include <string_view>
namespace flix {
namespace ui {
/// Rewrite known English system/squad phrases when language is Zh. Markup kept.
std::string localizeChatText(std::string text);
std::string localizeNotificationText(const std::string& text, const std::string& petalId);
/// Match the server's marked chat or plain feed form against the login name.
bool isOtherPermanentHyperCraft(std::string_view text, std::string_view username);
} // namespace ui
} // namespace flix

#pragma once
// Localize server system / squad chat lines and notices for the active UI language.
#include <string>
#include <string_view>
namespace flix {
namespace ui {
/// Rewrite known English system/squad phrases when language is Zh. Markup kept.
std::string localizeChatText(std::string text);
std::string localizeNotificationText(const std::string& text, const std::string& petalId);
} // namespace ui
} // namespace flix

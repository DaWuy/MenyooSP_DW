#pragma once

#include <string>

class Vector2;
class RGBA;

namespace UnicodeTextRenderer
{
	void SetStyle(const Vector2& scale, bool centred, bool rightJustified, bool outline, const RGBA& colour);
	bool DrawIfNeeded(const std::string& text, float x, float y);
}

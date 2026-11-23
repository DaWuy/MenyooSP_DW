#include "UnicodeTextRenderer.h"

#include "../main.h"
#include "../Natives/natives2.h"
#include "../Natives/types.h"
#include "../Util/ExePath.h"
#include "../Util/FileLogger.h"
#include "../Util/GTAmath.h"

#include <Windows.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>

namespace UnicodeTextRenderer
{
	namespace
	{
		struct DrawStyle
		{
			float scaleY = 0.4f;
			bool centred = false;
			bool rightJustified = false;
			bool outline = false;
			RGBA colour = { 255, 255, 255, 255 };
		};

		struct CachedTexture
		{
			int id = 0;
			int width = 0;
			int height = 0;
		};

		class GdiplusSession
		{
		public:
			GdiplusSession()
			{
				Gdiplus::GdiplusStartupInput input;
				if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok)
					token = 0;
			}

			~GdiplusSession()
			{
				if (token != 0)
					Gdiplus::GdiplusShutdown(token);
			}

			bool IsReady() const { return token != 0; }

		private:
			ULONG_PTR token = 0;
		};

		class FontRuntime
		{
		public:
			FontRuntime()
			{
				if (!gdiplus.IsReady())
					return;

				fontPath = ExePathW(true) + L"menyooStuff\\Fonts\\NotoSans-Regular.ttf";
				cacheDirectory = ExePathW(true) + L"menyooStuff\\Fonts\\Cache";

				if (GetFileAttributesW(fontPath.c_str()) == INVALID_FILE_ATTRIBUTES)
				{
					addlog(ige::LogType::LOG_ERROR, "[Unicode Font] Missing menyooStuff\\Fonts\\NotoSans-Regular.ttf");
					return;
				}

				if (fonts.AddFontFile(fontPath.c_str()) != Gdiplus::Ok)
				{
					addlog(ige::LogType::LOG_ERROR, "[Unicode Font] Unable to load NotoSans-Regular.ttf");
					return;
				}

				int familyCount = 0;
				if (fonts.GetFamilies(1, &family, &familyCount) != Gdiplus::Ok || familyCount == 0)
					return;

				std::error_code error;
				std::filesystem::create_directories(cacheDirectory, error);
				ready = !error;
				if (ready)
					addlog(ige::LogType::LOG_INFO, "[Unicode Font] Loaded Noto Sans Vietnamese renderer");
			}

			bool IsReady() const { return ready; }
			const Gdiplus::FontFamily& Family() const { return family; }
			const std::wstring& CacheDirectory() const { return cacheDirectory; }

		private:
			// Declared first so it is destroyed last, after all GDI+ font objects.
			GdiplusSession gdiplus;
			bool ready = false;
			std::wstring fontPath;
			std::wstring cacheDirectory;
			Gdiplus::PrivateFontCollection fonts;
			Gdiplus::FontFamily family;
		};

		DrawStyle currentStyle;
		std::unordered_map<std::string, CachedTexture> textureCache;

		bool ContainsUnicode(const std::string& text)
		{
			return std::any_of(text.begin(), text.end(), [](unsigned char value) { return value >= 0x80; });
		}

		std::string StripGtaFormatting(const std::string& text)
		{
			std::string result;
			result.reserve(text.size());

			for (std::size_t i = 0; i < text.size();)
			{
				if (text[i] == '~')
				{
					const std::size_t end = text.find('~', i + 1);
					if (end != std::string::npos)
					{
						const std::string tag = text.substr(i + 1, end - i - 1);
						if (tag == "n" || tag == "N")
							result.push_back('\n');
						i = end + 1;
						continue;
					}
				}

				result.push_back(text[i++]);
			}

			return result;
		}

		std::wstring Utf8ToWide(const std::string& text)
		{
			if (text.empty()) return {};

			const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
			if (length <= 0) return {};

			std::wstring result(length, L'\0');
			MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), static_cast<int>(text.size()), result.data(), length);
			return result;
		}

		std::uint64_t HashText(const std::string& text)
		{
			std::uint64_t hash = 14695981039346656037ULL;
			for (unsigned char value : text)
			{
				hash ^= value;
				hash *= 1099511628211ULL;
			}
			return hash;
		}

		int GetPngEncoderClsid(CLSID& clsid)
		{
			UINT count = 0;
			UINT bytes = 0;
			Gdiplus::GetImageEncodersSize(&count, &bytes);
			if (bytes == 0) return -1;

			std::unique_ptr<unsigned char[]> storage(new unsigned char[bytes]);
			auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(storage.get());
			if (Gdiplus::GetImageEncoders(count, bytes, encoders) != Gdiplus::Ok) return -1;

			for (UINT i = 0; i < count; ++i)
			{
				if (wcscmp(encoders[i].MimeType, L"image/png") == 0)
				{
					clsid = encoders[i].Clsid;
					return static_cast<int>(i);
				}
			}
			return -1;
		}

		bool RenderPng(FontRuntime& runtime, const std::wstring& text, const std::wstring& path, int& width, int& height)
		{
			// Render well above the final on-screen size so ScriptHookV can
			// downsample the texture cleanly instead of magnifying small glyphs.
			constexpr float rasterFontSize = 160.0f;
			constexpr float padding = 16.0f;

			Gdiplus::Font font(&runtime.Family(), rasterFontSize, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
			if (font.GetLastStatus() != Gdiplus::Ok) return false;

			Gdiplus::Bitmap measureBitmap(1, 1, PixelFormat32bppARGB);
			Gdiplus::Graphics measureGraphics(&measureBitmap);
			Gdiplus::StringFormat format(Gdiplus::StringFormat::GenericTypographic());
			format.SetFormatFlags(format.GetFormatFlags() | Gdiplus::StringFormatFlagsMeasureTrailingSpaces);

			Gdiplus::RectF bounds;
			measureGraphics.MeasureString(text.c_str(), static_cast<int>(text.size()), &font, Gdiplus::PointF(0.0f, 0.0f), &format, &bounds);

			width = (std::max)(1, static_cast<int>(std::ceil(bounds.Width + padding * 2.0f)));
			height = (std::max)(1, static_cast<int>(std::ceil(bounds.Height + padding * 2.0f)));
			width = (std::min)(width, 4096);
			height = (std::min)(height, 2048);

			Gdiplus::Bitmap bitmap(width, height, PixelFormat32bppARGB);
			Gdiplus::Graphics graphics(&bitmap);
			graphics.Clear(Gdiplus::Color(0, 0, 0, 0));
			graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
			graphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
			Gdiplus::SolidBrush brush(Gdiplus::Color(255, 255, 255, 255));
			graphics.DrawString(text.c_str(), static_cast<int>(text.size()), &font, Gdiplus::PointF(padding, padding), &format, &brush);

			CLSID pngEncoder;
			if (GetPngEncoderClsid(pngEncoder) < 0) return false;
			return bitmap.Save(path.c_str(), &pngEncoder, nullptr) == Gdiplus::Ok;
		}

		CachedTexture* GetTexture(const std::string& displayText)
		{
			auto cached = textureCache.find(displayText);
			if (cached != textureCache.end()) return &cached->second;

			static FontRuntime runtime;
			if (!runtime.IsReady()) return nullptr;

			const std::wstring wideText = Utf8ToWide(displayText);
			if (wideText.empty()) return nullptr;

			std::ostringstream name;
			// Prefix changes whenever raster settings change, preventing an old
			// runtime cache from being reused with different dimensions.
			name << "v2_" << std::hex << std::setfill('0') << std::setw(16) << HashText(displayText) << ".png";
			const std::filesystem::path pngPath = std::filesystem::path(runtime.CacheDirectory()) / name.str();

			CachedTexture texture;
			if (!std::filesystem::exists(pngPath) && !RenderPng(runtime, wideText, pngPath.wstring(), texture.width, texture.height))
				return nullptr;

			if (texture.width == 0 || texture.height == 0)
			{
				std::unique_ptr<Gdiplus::Image> image(Gdiplus::Image::FromFile(pngPath.c_str(), false));
				if (!image || image->GetLastStatus() != Gdiplus::Ok) return nullptr;
				texture.width = static_cast<int>(image->GetWidth());
				texture.height = static_cast<int>(image->GetHeight());
			}

			texture.id = createTexture(pngPath.string().c_str());
			if (texture.id == 0) return nullptr;

			auto inserted = textureCache.emplace(displayText, texture);
			return &inserted.first->second;
		}
	}

	void SetStyle(const Vector2& scale, bool centred, bool rightJustified, bool outline, const RGBA& colour)
	{
		currentStyle.scaleY = scale.y > 0.0f ? scale.y : scale.x;
		currentStyle.centred = centred;
		currentStyle.rightJustified = rightJustified;
		currentStyle.outline = outline;
		currentStyle.colour = colour;
	}

	bool DrawIfNeeded(const std::string& text, float x, float y)
	{
		if (!ContainsUnicode(text)) return false;

		const std::string displayText = StripGtaFormatting(text);
		CachedTexture* texture = GetTexture(displayText);
		if (!texture) return false;

		const float aspect = GET_ASPECT_RATIO(false);
		const float sizeY = (std::max)(0.009f, currentStyle.scaleY * 0.045f);
		const float pixelAspect = static_cast<float>(texture->width) / static_cast<float>(texture->height);
		const float sizeX = sizeY * pixelAspect / (aspect > 0.0f ? aspect : (16.0f / 9.0f));
		const float centreX = currentStyle.centred ? 0.5f : (currentStyle.rightJustified ? 1.0f : 0.0f);

		drawTexture(texture->id, 0, 10000, 55, sizeX, sizeY, centreX, 0.0f, x, y, 0.0f,
			aspect,
			static_cast<float>(currentStyle.colour.R) / 255.0f,
			static_cast<float>(currentStyle.colour.G) / 255.0f,
			static_cast<float>(currentStyle.colour.B) / 255.0f,
			static_cast<float>(currentStyle.colour.A) / 255.0f);
		return true;
	}
}

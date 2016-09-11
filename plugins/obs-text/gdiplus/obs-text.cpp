#include <graphics/math-defs.h>
#include <util/platform.h>
#include <util/util.hpp>
#include <obs-module.h>
#include <windows.h>
#include <gdiplus.h>
#include <algorithm>
#include <string>
#include <memory>

using namespace std;
using namespace Gdiplus;

#define warning(format, ...) blog(LOG_WARNING, "[%s] " format, \
		obs_source_get_name(source), ##__VA_ARGS__)

#define warn_stat(call) \
	do { \
		if (stat != Ok) \
			warning("%s: %s failed (%d)", __FUNCTION__, call, \
					(int)stat); \
	} while (false)

/* ------------------------------------------------------------------------- */

#define S_FONT                          "font"
#define S_USE_FILE                      "read_from_file"
#define S_FILE                          "file"
#define S_TEXT                          "text"
#define S_COLOR                         "color"
#define S_ALIGN                         "align"
#define S_VALIGN                        "valign"
#define S_OPACITY                       "opacity"
#define S_VERTICAL                      "vertical"
#define S_OUTLINE                       "outline"
#define S_OUTLINE_SIZE                  "outline_size"
#define S_OUTLINE_COLOR                 "outline_color"
#define S_OUTLINE_OPACITY               "outline_opacity"

#define S_ALIGN_LEFT                    "left"
#define S_ALIGN_CENTER                  "center"
#define S_ALIGN_RIGHT                   "right"

#define S_VALIGN_TOP                    "top"
#define S_VALIGN_CENTER                 S_ALIGN_CENTER
#define S_VALIGN_BOTTOM                 "bottom"

#define T_(v)                           obs_module_text(v)
#define T_FONT                          T_("Font")
#define T_USE_FILE                      T_("ReadFromFile")
#define T_FILE                          T_("TextFile")
#define T_TEXT                          T_("Text")
#define T_COLOR                         T_("Color")
#define T_ALIGN                         T_("Alignment")
#define T_VALIGN                        T_("VerticalAlignment")
#define T_OPACITY                       T_("Opacity")
#define T_VERTICAL                      T_("Veritcal")
#define T_OUTLINE                       T_("Outline")
#define T_OUTLINE_SIZE                  T_("Outline.Size")
#define T_OUTLINE_COLOR                 T_("Outline.Color")
#define T_OUTLINE_OPACITY               T_("Outline.Opacity")

#define T_FILTER_TEXT_FILES             T_("Filter.TextFiles")
#define T_FILTER_ALL_FILES              T_("Filter.AllFiles")

#define T_ALIGN_LEFT                    T_("Alignment.Left")
#define T_ALIGN_CENTER                  T_("Alignment.Center")
#define T_ALIGN_RIGHT                   T_("Alignment.Right")

#define T_VALIGN_TOP                    T_("VerticalAlignment.Top")
#define T_VALIGN_CENTER                 T_ALIGN_CENTER
#define T_VALIGN_BOTTOM                 T_("VerticalAlignment.Bottom")

#ifndef clamp
#define clamp(val, min_val, max_val) \
	if (val < min_val) val = min_val; \
	else if (val > max_val) val = max_val;
#endif

#define MIN_SIZE_CX 32
#define MIN_SIZE_CY 32
#define MAX_SIZE_CX 8192
#define MAX_SIZE_CY 8192

static inline DWORD get_alpha_val(uint32_t opacity)
{
	return ((opacity * 255 / 100) & 0xFF) << 24;
}

static inline DWORD calc_color(uint32_t color, uint32_t opacity)
{
	return color & 0xFFFFFF | get_alpha_val(opacity);
}

static inline wstring to_wide(const char *utf8)
{
	wstring text;

	size_t len = os_utf8_to_wcs(utf8, 0, nullptr, 0);
	text.resize(len);
	if (len)
		os_utf8_to_wcs(utf8, 0, &text[0], len + 1);

	return text;
}

static inline uint32_t rgb_to_bgr(uint32_t rgb)
{
	return ((rgb & 0xFF) << 16) | (rgb & 0xFF00) | ((rgb & 0xFF0000) >> 16);
}

/* ------------------------------------------------------------------------- */

template<typename T, typename T2, BOOL deleter(T2)> class GDIObj {
	T obj = nullptr;

	inline GDIObj &Replace(T obj_)
	{
		if (obj) deleter(obj);
		obj = obj_;
		return *this;
	}

public:
	inline GDIObj() {}
	inline GDIObj(T obj_) : obj(obj_) {}
	inline ~GDIObj() {deleter(obj);}

	inline T operator=(T obj_) {Replace(obj_); return obj;}

	inline operator T() const {return obj;}

	inline bool operator==(T obj_) const {return obj == obj_;}
	inline bool operator!=(T obj_) const {return obj != obj_;}
};

using HDCObj = GDIObj<HDC, HDC, DeleteDC>;
using HFONTObj = GDIObj<HFONT, HGDIOBJ, DeleteObject>;
using HBITMAPObj = GDIObj<HBITMAP, HGDIOBJ, DeleteObject>;

/* ------------------------------------------------------------------------- */

enum class Align {
	Left,
	Center,
	Right
};

enum class VAlign {
	Top,
	Center,
	Bottom
};

struct TextSource {
	obs_source_t *source = nullptr;

	wstring text;
	gs_texture_t *tex = nullptr;
	uint32_t cx = 0;
	uint32_t cy = 0;

	HDCObj hdc;
	Graphics graphics;

	HFONTObj hfont;
	unique_ptr<Font> font;

	bool read_from_file = false;
	string file;

	wstring face;
	int face_size = 0;
	uint32_t color = 0xFFFFFF;
	uint32_t opacity = 100;
	uint32_t bk_color = 0;
	uint32_t bk_opacity = 0;
	Align align = Align::Left;
	VAlign valign = VAlign::Top;
	bool bold = false;
	bool italic = false;
	bool underline = false;
	bool strikeout = false;
	bool vertical = false;

	bool use_outline = false;
	float outline_size = 0.0f;
	uint32_t outline_color = 0;
	uint32_t outline_opacity = 100;

	bool use_extents = false;
	bool wrap = false;
	uint32_t extents_cx = 0;
	uint32_t extents_cy = 0;

	/* --------------------------- */

	inline TextSource(obs_source_t *source_, obs_data_t *settings) :
		source                (source_),
		hdc                   (CreateCompatibleDC(nullptr)),
		graphics              (hdc)
	{
		obs_source_update(source, settings);
	}

	inline ~TextSource()
	{
		if (tex) {
			obs_enter_graphics();
			gs_texture_destroy(tex);
			obs_leave_graphics();
		}
	}

	void UpdateFont();
	void GetStringFormat(StringFormat &format);
	void CalculateTextSizes(const StringFormat &format,
			RectF &bounding_box, SIZE &text_size);
	void RenderOutlineText(Graphics &graphics,
			const GraphicsPath &path,
			const Brush &brush);
	void RenderText();

	inline void Update(obs_data_t *settings);
	inline void Render(gs_effect_t *effect);
};

void TextSource::UpdateFont()
{
	hfont = nullptr;
	font.reset(nullptr);

	LOGFONT lf = {};
	lf.lfHeight = face_size;
	lf.lfWeight = bold ? FW_BOLD : FW_DONTCARE;
	lf.lfItalic = italic;
	lf.lfUnderline = underline;
	lf.lfStrikeOut = strikeout;
	lf.lfQuality = ANTIALIASED_QUALITY;

	if (!face.empty()) {
		wcscpy(lf.lfFaceName, face.c_str());
		hfont = CreateFontIndirect(&lf);
	}

	if (!hfont) {
		wcscpy(lf.lfFaceName, L"Arial");
		hfont = CreateFontIndirect(&lf);
	}

	if (hfont)
		font.reset(new Font(hdc, hfont));
}

void TextSource::GetStringFormat(StringFormat &format)
{
	UINT flags = StringFormatFlagsNoFitBlackBox |
		StringFormatFlagsMeasureTrailingSpaces;

	if (vertical)
		flags |= StringFormatFlagsDirectionVertical |
			StringFormatFlagsDirectionRightToLeft;

	format.SetFormatFlags(flags);
	format.SetTrimming(StringTrimmingWord);

	switch (align) {
	case Align::Left:
		if (vertical)
			format.SetLineAlignment(StringAlignmentFar);
		else
			format.SetAlignment(StringAlignmentNear);
		break;
	case Align::Center:
		if (vertical)
			format.SetLineAlignment(StringAlignmentCenter);
		else
			format.SetAlignment(StringAlignmentCenter);
		break;
	case Align::Right:
		if (vertical)
			format.SetLineAlignment(StringAlignmentNear);
		else
			format.SetAlignment(StringAlignmentFar);
	}

	switch (valign) {
	case VAlign::Top:
		if (vertical)
			format.SetAlignment(StringAlignmentNear);
		else
			format.SetLineAlignment(StringAlignmentNear);
		break;
	case VAlign::Center:
		if (vertical)
			format.SetAlignment(StringAlignmentCenter);
		else
			format.SetLineAlignment(StringAlignmentCenter);
		break;
	case VAlign::Bottom:
		if (vertical)
			format.SetAlignment(StringAlignmentFar);
		else
			format.SetLineAlignment(StringAlignmentFar);
	}
}

void TextSource::CalculateTextSizes(const StringFormat &format,
		RectF &bounding_box, SIZE &text_size)
{
	RectF layout_box;
	Status stat;

	if (!text.empty()) {
		if (use_extents && wrap) {
			layout_box.X = layout_box.Y = 0;
			layout_box.Width  = float(extents_cx);
			layout_box.Height = float(extents_cy);

			if (use_outline) {
				layout_box.Width  -= outline_size;
				layout_box.Height -= outline_size;
			}

			stat = graphics.MeasureString(text.c_str(),
					(int)text.size() + 1, font.get(),
					layout_box, &format,
					&bounding_box);
			warn_stat("MeasureString");
		} else {
			stat = graphics.MeasureString(text.c_str(),
					(int)text.size() + 1, font.get(),
					PointF(0.0f, 0.0f), &format,
					&bounding_box);
			warn_stat("MeasureString");

			bounding_box.X = 0.0f;
			bounding_box.Y = 0.0f;

			if (use_outline) {
				bounding_box.Width  += outline_size;
				bounding_box.Height += outline_size;
			}
		}
	}

	if (vertical) {
		if (bounding_box.Width < face_size) {
			text_size.cx = face_size;
			bounding_box.Width = float(face_size);
		} else {
			text_size.cx = LONG(bounding_box.Width + EPSILON);
		}

		text_size.cy = LONG(bounding_box.Height + EPSILON);
	} else {
		if (bounding_box.Height < face_size) {
			text_size.cy = face_size;
			bounding_box.Height = float(face_size);
		} else {
			text_size.cy = LONG(bounding_box.Height + EPSILON);
		}

		text_size.cx = LONG(bounding_box.Width + EPSILON);
	}

	if (use_extents) {
		if (wrap) {
			text_size.cx = extents_cx;
			text_size.cy = extents_cy;
		} else {
			if (LONG(extents_cx) > text_size.cx)
				text_size.cx = extents_cx;
			else if (LONG(extents_cy) > text_size.cy)
				text_size.cy = extents_cy;
		}
	}

	text_size.cx += text_size.cx % 2;
	text_size.cy += text_size.cy % 2;

	clamp(text_size.cx, MIN_SIZE_CX, MAX_SIZE_CX);
	clamp(text_size.cy, MIN_SIZE_CY, MAX_SIZE_CY);
}

void TextSource::RenderOutlineText(Graphics &graphics,
		const GraphicsPath &path,
		const Brush &brush)
{
	DWORD outline_rgba = calc_color(outline_color, outline_opacity);

	Pen pen(Color(outline_rgba), outline_size);
	pen.SetLineJoin(LineJoinRound);

	graphics.DrawPath(&pen, &path);
	graphics.FillPath(&brush, &path);
}

void TextSource::RenderText()
{
	StringFormat format(StringFormat::GenericTypographic());
	Status stat;

	RectF box;
	SIZE size;

	GetStringFormat(format);
	CalculateTextSizes(format, box, size);

	unique_ptr<uint8_t> bits(new uint8_t[size.cx * size.cy * 4]);
	Bitmap bitmap(size.cx, size.cy, 4 * size.cx, PixelFormat32bppARGB,
			bits.get());

	Graphics graphics_bitmap(&bitmap);
	SolidBrush brush(Color(get_alpha_val(opacity) | (color & 0xFFFFFF)));
	DWORD full_bk_color = bk_color & 0xFFFFFF;

	if (!text.empty() || use_extents)
		full_bk_color |= get_alpha_val(bk_opacity);

	if ((size.cx > box.Width || size.cy > box.Height) && !use_extents) {
		stat = graphics_bitmap.Clear(Color(0));
		warn_stat("graphics_bitmap.Clear");

		SolidBrush bk_brush = Color(full_bk_color);
		stat = graphics_bitmap.FillRectangle(&bk_brush, box);
		warn_stat("graphics_bitmap.FillRectangle");
	} else {
		stat = graphics_bitmap.Clear(Color(full_bk_color));
		warn_stat("graphics_bitmap.Clear");
	}

	graphics_bitmap.SetTextRenderingHint(TextRenderingHintAntiAlias);
	graphics_bitmap.SetCompositingMode(CompositingModeSourceOver);
	graphics_bitmap.SetSmoothingMode(SmoothingModeAntiAlias);

	if (!text.empty()) {
		text.push_back('\n');

		if (use_outline) {
			box.Offset(outline_size / 2, outline_size / 2);

			FontFamily family;
			GraphicsPath path;

			font->GetFamily(&family);
			path.AddString(text.c_str(), (int)text.size(),
					&family, font->GetStyle(),
					font->GetSize(), box, &format);

			RenderOutlineText(graphics_bitmap, path, brush);
		} else {
			graphics_bitmap.DrawString(text.c_str(),
					(int)text.size(), font.get(),
					box, &format, &brush);
		}

		text.pop_back();
	}

	if (!tex || (LONG)cx != size.cx || (LONG)cy != size.cy) {
		obs_enter_graphics();
		if (tex)
			gs_texture_destroy(tex);

		const uint8_t *data = (uint8_t*)bits.get();
		tex = gs_texture_create(size.cx, size.cy, GS_BGRA, 1, &data,
				GS_DYNAMIC);

		obs_leave_graphics();

		cx = (uint32_t)size.cx;
		cy = (uint32_t)size.cy;

	} else if (tex) {
		obs_enter_graphics();
		gs_texture_set_image(tex, bits.get(), size.cx * 4, false);
		obs_leave_graphics();
	}
}

#define obs_data_get_uint32 (uint32_t)obs_data_get_int

inline void TextSource::Update(obs_data_t *s)
{
	const char *new_text   = obs_data_get_string(s, S_TEXT);
	obs_data_t *font_obj   = obs_data_get_obj(s, S_FONT);
	const char *align_str  = obs_data_get_string(s, S_ALIGN);
	const char *valign_str = obs_data_get_string(s, S_VALIGN);
	uint32_t new_color     = obs_data_get_uint32(s, S_COLOR);
	uint32_t new_opacity   = obs_data_get_uint32(s, S_OPACITY);
	bool new_vertical      = obs_data_get_bool(s, S_VERTICAL);
	bool new_outline       = obs_data_get_bool(s, S_OUTLINE);
	uint32_t new_o_color   = obs_data_get_uint32(s, S_OUTLINE_COLOR);
	uint32_t new_o_opacity = obs_data_get_uint32(s, S_OUTLINE_OPACITY);
	uint32_t new_o_size    = obs_data_get_uint32(s, S_OUTLINE_SIZE);
	bool new_use_file      = obs_data_get_bool(s, S_USE_FILE);
	const char *new_file   = obs_data_get_string(s, S_FILE);

	const char *font_face  = obs_data_get_string(font_obj, "face");
	int64_t   font_size    = obs_data_get_int(font_obj, "size");
	int64_t   font_flags   = obs_data_get_int(font_obj, "flags");

	/* ----------------------------- */

	wstring new_face = to_wide(font_face);

	new_color = rgb_to_bgr(new_color);
	new_o_color = rgb_to_bgr(new_o_color);

	face = new_face;
	face_size = (int)font_size;
	bold = (font_flags & OBS_FONT_BOLD) != 0;
	italic = (font_flags & OBS_FONT_ITALIC) != 0;
	underline = (font_flags & OBS_FONT_UNDERLINE) != 0;
	strikeout = (font_flags & OBS_FONT_STRIKEOUT) != 0;
	color = new_color;
	opacity = new_opacity;
	vertical = new_vertical;

	read_from_file = new_use_file;

	if (read_from_file) {
		file = new_file;

		BPtr<char> file_text = os_quick_read_utf8_file(new_file);
		text = to_wide(file_text);
	} else {
		text = to_wide(new_text);
	}

	if (!text.empty()) {
		text.push_back('\n');
		text.pop_back();
	}

	use_outline = new_outline;
	outline_color = new_o_color;
	outline_opacity = new_o_opacity;
	outline_size = roundf(float(new_o_size));

	if (strcmp(align_str, S_ALIGN_CENTER) == 0)
		align = Align::Center;
	else if (strcmp(align_str, S_ALIGN_RIGHT) == 0)
		align = Align::Right;
	else
		align = Align::Left;

	if (strcmp(valign_str, S_VALIGN_CENTER) == 0)
		valign = VAlign::Center;
	else if (strcmp(valign_str, S_VALIGN_BOTTOM) == 0)
		valign = VAlign::Bottom;
	else
		valign = VAlign::Top;

	/* ----------------------------- */

	UpdateFont();
	RenderText();

	/* ----------------------------- */

	obs_data_release(font_obj);
}

inline void TextSource::Render(gs_effect_t *effect)
{
	if (!tex)
		return;

	gs_reset_blend_state();
	gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), tex);
	gs_draw_sprite(tex, 0, cx, cy);
}

/* ------------------------------------------------------------------------- */

static ULONG_PTR gdip_token = 0;

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-text", "en-US")


static bool use_file_changed(obs_properties_t *props, obs_property_t *p,
		obs_data_t *s)
{
	bool use_file = obs_data_get_bool(s, S_USE_FILE);

#define set_vis(val, show) \
	do { \
		p = obs_properties_get(props, val); \
		obs_property_set_visible(p, use_file == show); \
	} while (false)

	set_vis(S_TEXT, false);
	set_vis(S_FILE, true);
#undef set_vis

	return true;
}

static bool outline_changed(obs_properties_t *props, obs_property_t *p,
		obs_data_t *s)
{
	bool outline = obs_data_get_bool(s, S_OUTLINE);

#define set_vis(val) \
	do { \
		p = obs_properties_get(props, val); \
		obs_property_set_visible(p, outline); \
	} while (false)

	set_vis(S_OUTLINE_SIZE);
	set_vis(S_OUTLINE_COLOR);
	set_vis(S_OUTLINE_OPACITY);
#undef set_vis

	return true;
}

static obs_properties_t *get_properties(void *data)
{
	TextSource *s = reinterpret_cast<TextSource*>(data);
	string path;

	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;

	obs_properties_add_font(props, S_FONT, T_FONT);

	p = obs_properties_add_bool(props, S_USE_FILE, T_USE_FILE);
	obs_property_set_modified_callback(p, use_file_changed);

	string filter;
	filter += T_FILTER_TEXT_FILES;
	filter += " (*.txt);;";
	filter += T_FILTER_ALL_FILES;
	filter += " (*.*)";

	if (s && !s->file.empty()) {
		const char *slash;

		path = s->file;
		replace(path.begin(), path.end(), '\\', '/');
		slash = strrchr(path.c_str(), '/');
		if (slash)
			path.resize(slash - path.c_str() + 1);
	}

	obs_properties_add_text(props, S_TEXT, T_TEXT, OBS_TEXT_MULTILINE);
	obs_properties_add_path(props, S_FILE, T_FILE, OBS_PATH_FILE,
			filter.c_str(), path.c_str());

	obs_properties_add_bool(props, S_VERTICAL, T_VERTICAL);
	obs_properties_add_color(props, S_COLOR, T_COLOR);

	obs_properties_add_int_slider(props, S_OPACITY, T_OPACITY, 0, 100, 1);

	p = obs_properties_add_list(props, S_ALIGN, T_ALIGN,
			OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, T_ALIGN_LEFT,   S_ALIGN_LEFT);
	obs_property_list_add_string(p, T_ALIGN_CENTER, S_ALIGN_CENTER);
	obs_property_list_add_string(p, T_ALIGN_RIGHT,  S_ALIGN_RIGHT);

	p = obs_properties_add_list(props, S_VALIGN, T_VALIGN,
			OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, T_VALIGN_TOP,    S_VALIGN_TOP);
	obs_property_list_add_string(p, T_VALIGN_CENTER, S_VALIGN_CENTER);
	obs_property_list_add_string(p, T_VALIGN_BOTTOM, S_VALIGN_BOTTOM);

	p = obs_properties_add_bool(props, S_OUTLINE, T_OUTLINE);
	obs_property_set_modified_callback(p, outline_changed);

	obs_properties_add_int(props, S_OUTLINE_SIZE, T_OUTLINE_SIZE, 1, 20, 1);
	obs_properties_add_color(props, S_OUTLINE_COLOR, T_OUTLINE_COLOR);
	obs_properties_add_int_slider(props, S_OUTLINE_OPACITY,
			T_OUTLINE_OPACITY, 0, 100, 1);

	return props;
}

bool obs_module_load(void)
{
	obs_source_info si = {};
	si.id = "text_gdiplus";
	si.type = OBS_SOURCE_TYPE_INPUT;
	si.output_flags = OBS_SOURCE_VIDEO;
	si.get_properties = get_properties;

	si.get_name = [] (void*)
	{
		return obs_module_text("TextGDIPlus");
	};
	si.create = [] (obs_data_t *settings, obs_source_t *source)
	{
		return (void*)new TextSource(source, settings);
	};
	si.destroy = [] (void *data)
	{
		delete reinterpret_cast<TextSource*>(data);
	};
	si.get_width = [] (void *data)
	{
		return reinterpret_cast<TextSource*>(data)->cx;
	};
	si.get_height = [] (void *data)
	{
		return reinterpret_cast<TextSource*>(data)->cy;
	};
	si.get_defaults = [] (obs_data_t *settings)
	{
		obs_data_t *font_obj = obs_data_create();
		obs_data_set_default_string(font_obj, "face", "Arial");
		obs_data_set_default_int(font_obj, "size", 22);

		obs_data_set_default_obj(settings, S_FONT, font_obj);
		obs_data_set_default_string(settings, S_ALIGN, S_ALIGN_LEFT);
		obs_data_set_default_string(settings, S_VALIGN, S_VALIGN_TOP);
		obs_data_set_default_int(settings, S_COLOR, 0xFFFFFF);
		obs_data_set_default_int(settings, S_OPACITY, 100);
		obs_data_set_default_int(settings, S_OUTLINE_SIZE, 2);
		obs_data_set_default_int(settings, S_OUTLINE_COLOR, 0xFFFFFF);
		obs_data_set_default_int(settings, S_OUTLINE_OPACITY, 100);

		obs_data_release(font_obj);
	};
	si.update = [] (void *data, obs_data_t *settings)
	{
		reinterpret_cast<TextSource*>(data)->Update(settings);
	};
	si.video_render = [] (void *data, gs_effect_t *effect)
	{
		reinterpret_cast<TextSource*>(data)->Render(effect);
	};

	obs_register_source(&si);

	const GdiplusStartupInput gdip_input;
	GdiplusStartup(&gdip_token, &gdip_input, nullptr);
	return true;
}

void obs_module_unload(void)
{
	GdiplusShutdown(gdip_token);
}

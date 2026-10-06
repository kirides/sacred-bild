#include "ui/form.h"
#include "ui/form_res.h"
#include "input/gamepad.h"
#include "log.h"

#include <commctrl.h>
#include <uxtheme.h>
#include <algorithm>
#include <climits>
#include <cstring>

namespace
{
    constexpr const wchar_t* kFont = L"Segoe UI";
    constexpr WORD kFontSize = 9;

    // Layout, in dialog units.
    constexpr int kMargin = 7;           // around the window's contents
    constexpr int kPagePadding = 6;      // inside a tab
    constexpr int kColumnGap = 8;
    constexpr int kMinColumnWidth = 180;
    constexpr int kBoxPadding = 8;       // group box: left and right of its rows, above (below the title) and below
    constexpr int kBoxTop = 13;
    constexpr int kBoxBottom = 6;
    constexpr int kBlockGap = 6;
    constexpr int kGap = 4;              // between controls in a row, and a label and its control
    constexpr int kRowGap = 4;
    constexpr int kIndent = 10;
    constexpr int kInputHeight = 12;
    constexpr int kCheckHeight = 10;
    constexpr int kTextHeight = 8;
    constexpr int kCheckBoxWidth = 16;   // the box and the gap to its text
    constexpr int kComboArrowWidth = 20;
    constexpr int kMinInputWidth = 40;
    constexpr int kButtonWidth = 50;
    constexpr int kButtonHeight = 14;
    constexpr int kComboDropHeight = 150;
    constexpr int kTabChromeX = 6;       // tab control around its pages
    constexpr int kTabChromeY = 18;

    constexpr UINT_PTR kGamepadTimer = 1;
    constexpr UINT kGamepadMs = 16;
    constexpr DWORD kRepeatDelayMs = 400;   // a held direction moves again after this, then every kRepeatMs
    constexpr DWORD kRepeatMs = 100;
    constexpr float kStickPush = 0.5f;      // the left stick as the D-pad past this

    constexpr int kTabsId = 900;
    constexpr int kFirstId = 1000;       // controls: kFirstId + 2 * index, their labels one more

    // The predefined control classes' atoms in dialog templates.
    const wchar_t* const kButton = MAKEINTRESOURCEW(0x0080);
    const wchar_t* const kEdit = MAKEINTRESOURCEW(0x0081);
    const wchar_t* const kStatic = MAKEINTRESOURCEW(0x0082);
    const wchar_t* const kComboBox = MAKEINTRESOURCEW(0x0085);

    // Text sizes in dialog units of the form's font.
    class Metrics
    {
    public:
        Metrics()
        {
            m_dc = CreateCompatibleDC(nullptr);
            m_font = CreateFontW(-MulDiv(kFontSize, 96, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, kFont);
            m_old = SelectObject(m_dc, m_font);
            // The dialog manager's base units for the font.
            TEXTMETRICW metrics = {};
            GetTextMetricsW(m_dc, &metrics);
            SIZE size = {};
            GetTextExtentPoint32W(m_dc, L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz", 52, &size);
            m_baseX = std::max<int>((size.cx / 26 + 1) / 2, 1);
            m_baseY = std::max<int>(metrics.tmHeight, 1);
        }

        ~Metrics()
        {
            SelectObject(m_dc, m_old);
            DeleteObject(m_font);
            DeleteDC(m_dc);
        }

        Metrics(const Metrics&) = delete;
        Metrics& operator=(const Metrics&) = delete;

        int width(const std::wstring& text) const
        {
            SIZE size = {};
            GetTextExtentPoint32W(m_dc, text.c_str(), static_cast<int>(text.size()), &size);
            return (size.cx * 4 + m_baseX - 1) / m_baseX + 1;
        }

        // Height of the text wrapped to the width.
        int height(const std::wstring& text, int width) const
        {
            RECT rect = {0, 0, MulDiv(width, m_baseX, 4), 0};
            DrawTextW(m_dc, text.c_str(), -1, &rect, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
            return (rect.bottom * 8 + m_baseY - 1) / m_baseY;
        }

    private:
        HDC m_dc = nullptr;
        HFONT m_font = nullptr;
        HGDIOBJ m_old = nullptr;
        int m_baseX = 1;
        int m_baseY = 1;
    };

    std::vector<std::wstring> split(const std::wstring& s)
    {
        std::vector<std::wstring> parts;
        size_t start = 0;
        for (size_t comma; (comma = s.find(L',', start)) != std::wstring::npos; start = comma + 1)
        {
            parts.push_back(s.substr(start, comma - start));
        }
        parts.push_back(s.substr(start));
        return parts;
    }

    std::wstring join(const std::vector<std::wstring>& parts)
    {
        std::wstring s;
        for (size_t i = 0; i < parts.size(); ++i)
        {
            s += (i ? L"," : L"") + parts[i];
        }
        return s;
    }

    // Common Controls 6 (themed controls, tooltips) while the form runs, from the manifest in form.rc: the game exe
    // has no manifest asking for it.
    class VisualStyles
    {
    public:
        explicit VisualStyles(HMODULE module)
        {
            wchar_t path[MAX_PATH] = {};
            GetModuleFileNameW(module, path, MAX_PATH);
            ACTCTXW ctx = {sizeof(ctx)};
            ctx.dwFlags = ACTCTX_FLAG_HMODULE_VALID | ACTCTX_FLAG_RESOURCE_NAME_VALID;
            ctx.lpSource = path;
            ctx.hModule = module;
            ctx.lpResourceName = MAKEINTRESOURCEW(IDR_FORM_MANIFEST);
            m_context = CreateActCtxW(&ctx);
            if (m_context == INVALID_HANDLE_VALUE || !ActivateActCtx(m_context, &m_cookie))
            {
                LOG("Ui::Form: no Common Controls 6 activation context: {}", GetLastError());
            }
        }

        ~VisualStyles()
        {
            if (m_cookie)
            {
                DeactivateActCtx(0, m_cookie);
            }
            if (m_context != INVALID_HANDLE_VALUE)
            {
                ReleaseActCtx(m_context);
            }
        }

        VisualStyles(const VisualStyles&) = delete;
        VisualStyles& operator=(const VisualStyles&) = delete;

    private:
        HANDLE m_context = INVALID_HANDLE_VALUE;
        ULONG_PTR m_cookie = 0;
    };
}

namespace Ui
{
    // An in-memory DLGTEMPLATEEX with its DLGITEMTEMPLATEEX entries.
    class DialogTemplate
    {
    public:
        DialogTemplate(DWORD style, DWORD exStyle, int cx, int cy, const wchar_t* title)
        {
            put<WORD>(1);       // dlgVer
            put<WORD>(0xFFFF);  // signature
            put<DWORD>(0);      // helpID
            put<DWORD>(exStyle);
            put<DWORD>(style | DS_SETFONT);
            m_countAt = m_data.size();
            put<WORD>(0);
            put<short>(0);
            put<short>(0);
            put<short>(static_cast<short>(cx));
            put<short>(static_cast<short>(cy));
            put<WORD>(0);  // menu
            put<WORD>(0);  // class
            string(title);
            put<WORD>(kFontSize);
            put<WORD>(FW_NORMAL);
            put<BYTE>(FALSE);
            put<BYTE>(DEFAULT_CHARSET);
            string(kFont);
        }

        // `cls`: a class name or one of the atoms above.
        void add(const wchar_t* cls, int id, int x, int y, int cx, int cy, DWORD style, DWORD exStyle, const std::wstring& text)
        {
            m_data.resize((m_data.size() + 3) & ~size_t(3));
            put<DWORD>(0);  // helpID
            put<DWORD>(exStyle);
            put<DWORD>(style | WS_CHILD | WS_VISIBLE);
            put<short>(static_cast<short>(x));
            put<short>(static_cast<short>(y));
            put<short>(static_cast<short>(cx));
            put<short>(static_cast<short>(cy));
            put<DWORD>(static_cast<DWORD>(id));
            if (IS_INTRESOURCE(cls))
            {
                put<WORD>(0xFFFF);
                put<WORD>(static_cast<WORD>(reinterpret_cast<uintptr_t>(cls)));
            }
            else
            {
                string(cls);
            }
            string(text.c_str());
            put<WORD>(0);  // no creation data
            ++m_count;
        }

        LPCDLGTEMPLATEW get()
        {
            std::memcpy(m_data.data() + m_countAt, &m_count, sizeof(m_count));
            return reinterpret_cast<LPCDLGTEMPLATEW>(m_data.data());
        }

    private:
        template <class T>
        void put(T value)
        {
            const size_t at = m_data.size();
            m_data.resize(at + sizeof(T));
            std::memcpy(m_data.data() + at, &value, sizeof(T));
        }

        void string(const wchar_t* s)
        {
            do
            {
                put<wchar_t>(*s);
            } while (*s++);
        }

        std::vector<BYTE> m_data;
        size_t m_countAt = 0;
        WORD m_count = 0;
    };
}

Ui::Control& Ui::Control::tip(std::wstring text)
{
    m_tip = std::move(text);
    return *this;
}

Ui::Control& Ui::Control::enabledIf(Predicate predicate)
{
    m_enabled.push_back(std::move(predicate));
    return *this;
}

Ui::Control& Ui::Control::width(int dialogUnits)
{
    m_fixedWidth = dialogUnits;
    return *this;
}

Ui::Control& Ui::Control::values(const wchar_t* on, const wchar_t* off)
{
    const bool checked = m_initial == m_on;
    m_on = on;
    m_off = off;
    m_initial = m_current = checked ? m_on : m_off;
    return *this;
}

Ui::Control& Ui::Control::digits()
{
    m_digits = true;
    return *this;
}

void Ui::Form::page(const wchar_t* title)
{
    m_pages.push_back({title});
    column();
}

void Ui::Form::column()
{
    m_pages.back().columns.emplace_back().blocks.emplace_back();
    m_inFooter = false;
    m_sameLine = false;
    m_indent = 0;
}

void Ui::Form::group(const wchar_t* title)
{
    m_pages.back().columns.back().blocks.push_back({title});
    m_sameLine = false;
    m_indent = 0;
}

void Ui::Form::footer()
{
    m_inFooter = true;
    m_sameLine = false;
    m_indent = 0;
}

void Ui::Form::sameLine()
{
    m_sameLine = true;
}

void Ui::Form::indent()
{
    ++m_indent;
}

void Ui::Form::unindent()
{
    m_indent = std::max(m_indent - 1, 0);
}

void Ui::Form::beginEnabledIf(Predicate predicate)
{
    m_enabled.push_back(std::move(predicate));
}

void Ui::Form::endEnabledIf()
{
    if (!m_enabled.empty())
    {
        m_enabled.pop_back();
    }
}

Ui::Form::Block& Ui::Form::block()
{
    return m_inFooter ? m_footer : m_pages.back().columns.back().blocks.back();
}

Ui::Control& Ui::Form::add(Control control)
{
    control.m_page = m_inFooter ? -1 : static_cast<int>(m_pages.size()) - 1;
    control.m_enabled = m_enabled;
    control.m_current = control.m_initial;
    control.m_id = kFirstId + 2 * static_cast<int>(m_controls.size());
    Control& added = m_controls.emplace_back(std::move(control));
    Block& target = block();
    if (!m_sameLine || target.rows.empty())
    {
        target.rows.push_back({{}, m_indent});
    }
    target.rows.back().controls.push_back(&added);
    m_sameLine = false;
    return added;
}

Ui::Control& Ui::Form::check(const wchar_t* text, Key key, bool value)
{
    Control control;
    control.m_type = Control::Type::Check;
    control.m_text = text;
    control.m_key = key;
    control.m_initial = value ? control.m_on : control.m_off;
    return add(std::move(control));
}

Ui::Control& Ui::Form::combo(const wchar_t* label, Key key, std::vector<Choice> choices, const std::wstring& value)
{
    if (std::ranges::none_of(choices, [&](const Choice& c) { return c.value == value; }))
    {
        choices.push_back({L"Custom (" + value + L")", value});
    }
    Control control;
    control.m_type = Control::Type::Combo;
    control.m_text = label ? label : L"";
    control.m_key = key;
    control.m_choices = std::move(choices);
    control.m_initial = value;
    return add(std::move(control));
}

Ui::Control& Ui::Form::combo(const wchar_t* label, Key key, std::vector<int> numbers, int value,
    const std::function<std::wstring(int)>& text)
{
    numbers.push_back(value);
    std::ranges::sort(numbers);
    numbers.erase(std::unique(numbers.begin(), numbers.end()), numbers.end());
    std::vector<Choice> choices;
    for (int n : numbers)
    {
        choices.push_back({text(n), std::to_wstring(n)});
    }
    return combo(label, key, std::move(choices), std::to_wstring(value));
}

Ui::Control& Ui::Form::edit(const wchar_t* label, Key key, const std::wstring& value)
{
    Control control;
    control.m_type = Control::Type::Edit;
    control.m_text = label ? label : L"";
    control.m_key = key;
    control.m_initial = value;
    return add(std::move(control));
}

Ui::Control& Ui::Form::label(const wchar_t* text)
{
    Control control;
    control.m_type = Control::Type::Label;
    control.m_text = text;
    return add(std::move(control));
}

Ui::Control& Ui::Form::text(const wchar_t* text)
{
    Control control;
    control.m_type = Control::Type::Text;
    control.m_text = text;
    return add(std::move(control));
}

Ui::Control& Ui::Form::button(const wchar_t* text, std::function<void(Form&)> click)
{
    Control control;
    control.m_type = Control::Type::Button;
    control.m_text = text;
    control.m_click = std::move(click);
    return add(std::move(control));
}

void Ui::Form::buttons(const wchar_t* ok, const wchar_t* cancel)
{
    m_ok = ok;
    m_cancel = cancel;
}

void Ui::Form::icons(HICON big, HICON small)
{
    m_icons[0] = big;
    m_icons[1] = small;
}

void Ui::Form::layout()
{
    using Type = Control::Type;
    const Metrics metrics;

    // A combo or edit box's label goes in front of it; first in a row, in the block's label column.
    const auto hasLabel = [](const Control& c) { return (c.m_type == Type::Combo || c.m_type == Type::Edit) && !c.m_text.empty(); };
    const auto natural = [&](const Control& c) {
        if (c.m_fixedWidth)
        {
            return c.m_fixedWidth;
        }
        switch (c.m_type)
        {
        case Type::Check:
            return metrics.width(c.m_text) + kCheckBoxWidth;
        case Type::Combo:
        {
            int widest = 0;
            for (const Choice& choice : c.m_choices)
            {
                widest = std::max(widest, metrics.width(choice.text));
            }
            return std::max(widest + kComboArrowWidth, kMinInputWidth);
        }
        case Type::Edit:
            return kMinInputWidth;
        case Type::Label:
            return metrics.width(c.m_text);
        case Type::Button:
            return std::max(metrics.width(c.m_text) + 16, kButtonWidth);
        case Type::Text:
            break;
        }
        return 0;
    };
    const auto height = [&](const Control& c, int width) {
        switch (c.m_type)
        {
        case Type::Check:
            return kCheckHeight;
        case Type::Combo:
        case Type::Edit:
            return kInputHeight;
        case Type::Button:
            return kButtonHeight;
        case Type::Text:
            return metrics.height(c.m_text, width);
        case Type::Label:
            break;
        }
        return kTextHeight;
    };
    const auto firstIsLabel = [](const Row& row) {
        const Control& c = *row.controls.front();
        return c.m_type == Type::Label && !c.m_fixedWidth;
    };
    const auto labelColumn = [&](const Block& block) {
        int width = 0;
        for (const Row& row : block.rows)
        {
            const Control& first = *row.controls.front();
            if (firstIsLabel(row))
            {
                width = std::max(width, natural(first));
            }
            else if (hasLabel(first))
            {
                width = std::max(width, metrics.width(first.m_text));
            }
        }
        return width;
    };

    struct Slot
    {
        int label = 0;
        int width = 0;
        bool flex = false;
    };
    const auto slots = [&](const Row& row, int labels) {
        std::vector<Slot> out;
        for (size_t i = 0; i < row.controls.size(); ++i)
        {
            const Control& c = *row.controls[i];
            Slot slot;
            if (i == 0 && firstIsLabel(row))
            {
                slot.width = labels;
            }
            else
            {
                if (hasLabel(c))
                {
                    slot.label = i == 0 ? labels : metrics.width(c.m_text);
                }
                slot.width = natural(c);
                slot.flex = !c.m_fixedWidth && c.m_type != Type::Check && c.m_type != Type::Button;
            }
            out.push_back(slot);
        }
        return out;
    };
    // Width of the row without its flexible controls' share of the room left.
    const auto fixedWidth = [&](const Row& row, const std::vector<Slot>& slots) {
        int width = row.indent * kIndent + kGap * static_cast<int>(slots.size() - 1);
        for (const Slot& slot : slots)
        {
            width += slot.label + (slot.label ? kGap : 0) + slot.width;
        }
        return width;
    };
    const auto blockWidth = [&](const Block& block) {
        const int labels = labelColumn(block);
        int width = 0;
        for (const Row& row : block.rows)
        {
            width = std::max(width, fixedWidth(row, slots(row, labels)));
        }
        if (!block.title.empty())
        {
            width = std::max(width, metrics.width(block.title) + 4) + 2 * kBoxPadding;
        }
        return width;
    };
    const auto placeRow = [&](const Row& row, int x, int width, int y, int labels) {
        std::vector<Slot> rowSlots = slots(row, labels);
        const int flex = static_cast<int>(std::ranges::count_if(rowSlots, [](const Slot& s) { return s.flex; }));
        // The flexible controls share the room left over their natural widths.
        int left = std::max(width - fixedWidth(row, rowSlots), 0);
        for (int i = 0, n = flex; Slot& slot : rowSlots)
        {
            if (slot.flex)
            {
                const int share = left / (n - i++);
                slot.width += share;
                left -= share;
            }
        }
        int rowHeight = 0;
        for (size_t i = 0; i < row.controls.size(); ++i)
        {
            rowHeight = std::max(rowHeight, height(*row.controls[i], rowSlots[i].width));
        }
        x += row.indent * kIndent;
        for (size_t i = 0; i < row.controls.size(); ++i)
        {
            Control& c = *row.controls[i];
            const Slot& slot = rowSlots[i];
            if (slot.label)
            {
                c.m_labelX = static_cast<short>(x);
                c.m_labelY = static_cast<short>(y + (rowHeight - kTextHeight) / 2);
                c.m_labelCx = static_cast<short>(slot.label);
                x += slot.label + kGap;
            }
            const int h = height(c, slot.width);
            c.m_x = static_cast<short>(x);
            c.m_y = static_cast<short>(y + (rowHeight - h) / 2);
            c.m_cx = static_cast<short>(slot.width);
            c.m_cy = static_cast<short>(h);
            x += slot.width + kGap;
        }
        return rowHeight;
    };
    // Returns the bottom.
    const auto placeBlock = [&](Block& block, int x, int width, int y) {
        const bool boxed = !block.title.empty();
        const int pad = boxed ? kBoxPadding : 0;
        const int labels = labelColumn(block);
        int bottom = y + (boxed ? kBoxTop : 0);
        for (size_t i = 0; i < block.rows.size(); ++i)
        {
            bottom += i ? kRowGap : 0;
            bottom += placeRow(block.rows[i], x + pad, width - 2 * pad, bottom, labels);
        }
        bottom += boxed ? kBoxBottom : 0;
        block.x = static_cast<short>(x);
        block.y = static_cast<short>(y);
        block.cx = static_cast<short>(width);
        block.cy = static_cast<short>(bottom - y);
        return bottom;
    };
    const auto columnWidth = [&](const Column& column) {
        int width = kMinColumnWidth;
        for (const Block& block : column.blocks)
        {
            width = std::max(width, blockWidth(block));
        }
        return width;
    };

    // Widths: every page gets the widest page's width, its columns share the difference.
    const int footerWidth = (m_footer.rows.empty() ? 0 : blockWidth(m_footer) + kGap) + 2 * kButtonWidth + kGap;
    int pageWidth = footerWidth - kTabChromeX;
    std::vector<std::vector<int>> widths;
    for (const Page& page : m_pages)
    {
        std::vector<int>& columns = widths.emplace_back();
        int width = 2 * kPagePadding - kColumnGap;
        for (const Column& column : page.columns)
        {
            width += columns.emplace_back(columnWidth(column)) + kColumnGap;
        }
        pageWidth = std::max(pageWidth, width);
    }
    int pageHeight = 0;
    for (size_t p = 0; p < m_pages.size(); ++p)
    {
        std::vector<int>& columns = widths[p];
        int extra = pageWidth - 2 * kPagePadding + kColumnGap;
        for (int width : columns)
        {
            extra -= width + kColumnGap;
        }
        int x = kPagePadding;
        for (size_t c = 0; c < columns.size(); ++c)
        {
            const int share = extra / static_cast<int>(columns.size() - c);
            extra -= share;
            const int width = columns[c] + share;
            int y = kPagePadding;
            bool first = true;
            for (Block& block : m_pages[p].columns[c].blocks)
            {
                if (block.rows.empty() && block.title.empty())
                {
                    continue;
                }
                y = placeBlock(block, x, width, y + (first ? 0 : kBlockGap));
                first = false;
            }
            pageHeight = std::max(pageHeight, y + kPagePadding);
            x += width + kColumnGap;
        }
    }
    m_page = {pageWidth, pageHeight};

    // The footer below the tabs, its controls centered on the buttons' height.
    const int tabWidth = m_page.cx + kTabChromeX;
    m_footerY = kMargin + m_page.cy + kTabChromeY + kMargin;
    const int width = tabWidth - 2 * (kButtonWidth + kGap);
    int footerHeight = placeBlock(m_footer, kMargin, width, m_footerY) - m_footerY;
    if (footerHeight < kButtonHeight)
    {
        placeBlock(m_footer, kMargin, width, m_footerY + (kButtonHeight - footerHeight) / 2);
        footerHeight = kButtonHeight;
    }
    m_window = {tabWidth + 2 * kMargin, m_footerY + footerHeight + kMargin};
}

void Ui::Form::addControl(DialogTemplate& dialog, const Control& c) const
{
    using Type = Control::Type;
    if (c.m_labelCx)
    {
        dialog.add(kStatic, c.m_id + 1, c.m_labelX, c.m_labelY, c.m_labelCx, kTextHeight, SS_LEFT | SS_NOTIFY, 0, c.m_text);
    }
    switch (c.m_type)
    {
    case Type::Check:
        dialog.add(kButton, c.m_id, c.m_x, c.m_y, c.m_cx, c.m_cy, BS_AUTOCHECKBOX | WS_TABSTOP, 0, c.m_text);
        break;
    case Type::Combo:
        dialog.add(kComboBox, c.m_id, c.m_x, c.m_y, c.m_cx, kComboDropHeight, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0, L"");
        break;
    case Type::Edit:
        dialog.add(kEdit, c.m_id, c.m_x, c.m_y, c.m_cx, c.m_cy,
            ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP | (c.m_digits ? ES_NUMBER : 0), 0, L"");
        break;
    case Type::Label:
    case Type::Text:
        dialog.add(kStatic, c.m_id, c.m_x, c.m_y, c.m_cx, c.m_cy, SS_LEFT | SS_NOTIFY, 0, c.m_text);
        break;
    case Type::Button:
        dialog.add(kButton, c.m_id, c.m_x, c.m_y, c.m_cx, c.m_cy, BS_PUSHBUTTON | WS_TABSTOP, 0, c.m_text);
        break;
    }
}

Ui::DialogTemplate Ui::Form::mainTemplate(const wchar_t* title) const
{
    DialogTemplate dialog(DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, WS_EX_APPWINDOW,
        m_window.cx, m_window.cy, title);
    dialog.add(WC_TABCONTROLW, kTabsId, kMargin, kMargin, m_page.cx + kTabChromeX, m_page.cy + kTabChromeY,
        WS_TABSTOP | WS_CLIPSIBLINGS, WS_EX_CONTROLPARENT, L"");
    for (const Row& row : m_footer.rows)
    {
        for (const Control* control : row.controls)
        {
            addControl(dialog, *control);
        }
    }
    const int right = m_window.cx - kMargin;
    dialog.add(kButton, IDOK, right - 2 * kButtonWidth - kGap, m_footerY, kButtonWidth, kButtonHeight,
        BS_DEFPUSHBUTTON | WS_TABSTOP, 0, m_ok);
    dialog.add(kButton, IDCANCEL, right - kButtonWidth, m_footerY, kButtonWidth, kButtonHeight, BS_PUSHBUTTON | WS_TABSTOP, 0, m_cancel);
    return dialog;
}

Ui::DialogTemplate Ui::Form::pageTemplate(const Page& page) const
{
    DialogTemplate dialog(DS_CONTROL | WS_CHILD, 0, m_page.cx, m_page.cy, L"");
    for (const Column& column : page.columns)
    {
        for (const Block& block : column.blocks)
        {
            if (!block.title.empty())
            {
                dialog.add(kButton, -1, block.x, block.y, block.cx, block.cy, BS_GROUPBOX, 0, block.title);
            }
            for (const Row& row : block.rows)
            {
                for (const Control* control : row.controls)
                {
                    addControl(dialog, *control);
                }
            }
        }
    }
    return dialog;
}

Ui::Form::Result Ui::Form::run(HMODULE module, const wchar_t* title)
{
    m_module = module;
    m_dlg = nullptr;
    m_padHeld = 0;
    m_padUsed = false;
    layout();
    DialogTemplate dialog = mainTemplate(title);
    INT_PTR result;
    {
        VisualStyles styles(module);
        result = DialogBoxIndirectParamW(module, dialog.get(), nullptr, dialogProc, reinterpret_cast<LPARAM>(this));
    }
    // The game starts SDL again if it plays with the controller.
    Gamepad::stop();
    for (Control& control : m_controls)
    {
        control.m_hwnd = control.m_label = nullptr;
    }
    for (Page& page : m_pages)
    {
        page.hwnd = nullptr;
    }
    m_dlg = m_tabs = m_tooltip = nullptr;
    if (result == -1)
    {
        LOG("Ui::Form: DialogBoxIndirectParam failed: {}", GetLastError());
        return Result::Failed;
    }
    return result == IDOK ? Result::Ok : Result::Cancel;
}

void Ui::Form::init(HWND dlg)
{
    m_dlg = dlg;
    m_tabs = GetDlgItem(dlg, kTabsId);
    LoadLibraryW(L"uxtheme.dll");
    for (size_t i = 0; i < m_pages.size(); ++i)
    {
        TCITEMW tab = {TCIF_TEXT};
        tab.pszText = m_pages[i].title.data();
        SendMessageW(m_tabs, TCM_INSERTITEMW, i, reinterpret_cast<LPARAM>(&tab));
    }
    RECT area;
    GetClientRect(m_tabs, &area);
    TabCtrl_AdjustRect(m_tabs, FALSE, &area);
    for (Page& page : m_pages)
    {
        DialogTemplate dialog = pageTemplate(page);
        page.hwnd = CreateDialogIndirectParamW(m_module, dialog.get(), m_tabs, dialogProc, reinterpret_cast<LPARAM>(this));
        SetWindowPos(page.hwnd, HWND_TOP, area.left, area.top, area.right - area.left, area.bottom - area.top, 0);
    }
    showPage(0);

    for (Control& c : m_controls)
    {
        const HWND parent = c.m_page < 0 ? dlg : m_pages[c.m_page].hwnd;
        c.m_hwnd = GetDlgItem(parent, c.m_id);
        c.m_label = c.m_labelCx ? GetDlgItem(parent, c.m_id + 1) : nullptr;
        for (size_t i = 0; i < c.m_choices.size(); ++i)
        {
            SendMessageW(c.m_hwnd, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(c.m_choices[i].text.c_str()));
        }
    }
    for (Control& c : m_controls)
    {
        write(c, c.m_current);
    }
    for (int i = 0; i < 2; ++i)
    {
        if (m_icons[i])
        {
            SendMessageW(dlg, WM_SETICON, i == 0 ? ICON_BIG : ICON_SMALL, reinterpret_cast<LPARAM>(m_icons[i]));
        }
    }
    addTooltips();
    updateEnabled();
    SetForegroundWindow(dlg);
    SetTimer(dlg, kGamepadTimer, kGamepadMs, nullptr);
}

void Ui::Form::showPage(int index)
{
    for (int i = 0; i < static_cast<int>(m_pages.size()); ++i)
    {
        ShowWindow(m_pages[i].hwnd, i == index ? SW_SHOW : SW_HIDE);
    }
}

void Ui::Form::addTooltips()
{
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_WIN95_CLASSES};
    InitCommonControlsEx(&icc);
    m_tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, m_dlg, nullptr, m_module, nullptr);
    if (!m_tooltip)
    {
        return;
    }
    // Wrap at the width of 220 dialog units (follows the dialog's DPI scaling), stay up while the mouse rests.
    RECT wrap = {0, 0, 220, 0};
    MapDialogRect(m_dlg, &wrap);
    SendMessageW(m_tooltip, TTM_SETMAXTIPWIDTH, 0, wrap.right);
    SendMessageW(m_tooltip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 30000);
    for (Control& c : m_controls)
    {
        for (HWND hwnd : {c.m_hwnd, c.m_label})
        {
            if (!hwnd || c.m_tip.empty())
            {
                continue;
            }
            TTTOOLINFOW tool = {sizeof(tool)};
            tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
            tool.hwnd = GetParent(hwnd);
            tool.uId = reinterpret_cast<UINT_PTR>(hwnd);
            tool.lpszText = c.m_tip.data();
            SendMessageW(m_tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
        }
    }
}

void Ui::Form::updateEnabled()
{
    for (Control& c : m_controls)
    {
        if (!c.m_hwnd)
        {
            continue;
        }
        const bool enabled = std::ranges::all_of(c.m_enabled, [&](const Predicate& p) { return p(*this); });
        EnableWindow(c.m_hwnd, enabled);
        if (c.m_label)
        {
            EnableWindow(c.m_label, enabled);
        }
    }
}

void Ui::Form::command(WORD id)
{
    const int index = (id - kFirstId) / 2;
    if (id >= kFirstId && (id - kFirstId) % 2 == 0 && index < static_cast<int>(m_controls.size()))
    {
        Control& c = m_controls[index];
        if (c.m_click)
        {
            c.m_click(*this);
        }
    }
    if (m_dlg)
    {
        updateEnabled();
    }
}

void Ui::Form::finish(int result)
{
    for (Control& c : m_controls)
    {
        c.m_current = read(c);
    }
    KillTimer(m_dlg, kGamepadTimer);
    EndDialog(m_dlg, result);
}

void Ui::Form::gamepad()
{
    Gamepad::poll();
    const Gamepad::State& pad = Gamepad::state();
    uint32_t held = pad.buttons;
    held |= pad.ly > kStickPush ? Gamepad::Up : pad.ly < -kStickPush ? Gamepad::Down : 0;
    held |= pad.lx < -kStickPush ? Gamepad::Left : pad.lx > kStickPush ? Gamepad::Right : 0;
    uint32_t act = held & ~m_padHeld;
    m_padHeld = held;
    // SDL reads the pad with the window in the background too.
    if (GetForegroundWindow() != m_dlg)
    {
        return;
    }
    constexpr uint32_t kMoves = Gamepad::Up | Gamepad::Down | Gamepad::Left | Gamepad::Right;
    const DWORD now = GetTickCount();
    if (act & kMoves)
    {
        m_padRepeat = now + kRepeatDelayMs;
    }
    else if ((held & kMoves) && static_cast<int>(now - m_padRepeat) >= 0)
    {
        act |= held & kMoves;
        m_padRepeat = now + kRepeatMs;
    }
    if (!act)
    {
        return;
    }
    if (!m_padUsed)
    {
        // Focus rectangles, as after the first key press.
        m_padUsed = true;
        SendMessageW(m_dlg, WM_CHANGEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS), 0);
    }

    const HWND focus = GetFocus();
    wchar_t cls[32] = {};
    if (focus)
    {
        GetClassNameW(focus, cls, static_cast<int>(std::size(cls)));
    }
    const bool combo = _wcsicmp(cls, WC_COMBOBOXW) == 0;
    const auto key = [&](WPARAM vk) {
        SendMessageW(focus, WM_KEYDOWN, vk, 0);
        SendMessageW(focus, WM_KEYUP, vk, 0xC0000001);
    };
    if (combo && SendMessageW(focus, CB_GETDROPPEDSTATE, 0, 0))
    {
        // The open list: the D-pad picks, A takes it, B keeps the value from before.
        if (act & (Gamepad::Up | Gamepad::Left))
        {
            key(VK_UP);
        }
        else if (act & (Gamepad::Down | Gamepad::Right))
        {
            key(VK_DOWN);
        }
        else if (act & Gamepad::A)
        {
            key(VK_RETURN);
        }
        else if (act & Gamepad::B)
        {
            key(VK_ESCAPE);
        }
        return;
    }
    if (act & Gamepad::Start)
    {
        SendMessageW(m_dlg, WM_COMMAND, IDOK, 0);
    }
    else if (act & (Gamepad::LB | Gamepad::RB))
    {
        padPage(act & Gamepad::RB ? 1 : -1);
    }
    else if (act & (Gamepad::Up | Gamepad::Down))
    {
        // Through the controls in tab order.
        SendMessageW(m_dlg, WM_NEXTDLGCTL, (act & Gamepad::Up) ? 1 : 0, FALSE);
    }
    else if (act & (Gamepad::Left | Gamepad::Right))
    {
        const bool right = act & Gamepad::Right;
        if (combo)
        {
            key(right ? VK_DOWN : VK_UP);
        }
        else if (focus == m_tabs)
        {
            key(right ? VK_RIGHT : VK_LEFT);
        }
        else
        {
            padSideways(focus, right);
        }
    }
    else if (act & Gamepad::A)
    {
        if (combo)
        {
            SendMessageW(focus, CB_SHOWDROPDOWN, TRUE, 0);
        }
        else if (focus == m_tabs)
        {
            SendMessageW(m_dlg, WM_NEXTDLGCTL, 0, FALSE);
        }
        else if (_wcsicmp(cls, WC_BUTTONW) == 0)
        {
            SendMessageW(focus, BM_CLICK, 0, 0);
        }
    }
}

// Left or right of a control: the next one in its row, else the one closest in height in the next column over. In
// the footer along its row, OK and Cancel.
void Ui::Form::padSideways(HWND focus, bool right)
{
    using Type = Control::Type;
    const auto focusable = [](const Control& c) {
        return c.m_hwnd && c.m_type != Type::Label && c.m_type != Type::Text && IsWindowEnabled(c.m_hwnd);
    };
    const int step = right ? 1 : -1;
    const auto at = std::ranges::find(m_controls, focus, &Control::m_hwnd);
    if (at == m_controls.end() || at->m_page < 0)
    {
        std::vector<HWND> line;
        for (const Row& row : m_footer.rows)
        {
            for (const Control* c : row.controls)
            {
                if (focusable(*c))
                {
                    line.push_back(c->m_hwnd);
                }
            }
        }
        line.push_back(GetDlgItem(m_dlg, IDOK));
        line.push_back(GetDlgItem(m_dlg, IDCANCEL));
        const auto it = std::ranges::find(line, focus);
        const ptrdiff_t i = (it - line.begin()) + step;
        if (it != line.end() && i >= 0 && i < static_cast<ptrdiff_t>(line.size()))
        {
            focusOn(line[i]);
        }
        return;
    }

    const Control& from = *at;
    const std::vector<Column>& columns = m_pages[from.m_page].columns;
    int column = -1;
    const Row* row = nullptr;
    for (size_t c = 0; c < columns.size() && !row; ++c)
    {
        for (const Block& block : columns[c].blocks)
        {
            for (const Row& r : block.rows)
            {
                if (std::ranges::find(r.controls, &from) != r.controls.end())
                {
                    column = static_cast<int>(c);
                    row = &r;
                }
            }
        }
    }
    if (!row)
    {
        return;
    }
    const ptrdiff_t count = static_cast<ptrdiff_t>(row->controls.size());
    for (ptrdiff_t i = (std::ranges::find(row->controls, &from) - row->controls.begin()) + step; i >= 0 && i < count; i += step)
    {
        if (focusable(*row->controls[i]))
        {
            focusOn(row->controls[i]->m_hwnd);
            return;
        }
    }
    const int y = from.m_y + from.m_cy / 2;
    for (int c = column + step; c >= 0 && c < static_cast<int>(columns.size()); c += step)
    {
        const Control* best = nullptr;
        int distance = INT_MAX;
        for (const Block& block : columns[c].blocks)
        {
            for (const Row& r : block.rows)
            {
                for (const Control* control : r.controls)
                {
                    const int d = std::abs(control->m_y + control->m_cy / 2 - y);
                    if (focusable(*control) && d < distance)
                    {
                        best = control;
                        distance = d;
                    }
                }
            }
        }
        if (best)
        {
            focusOn(best->m_hwnd);
            return;
        }
    }
}

// The previous or next tab; the focus follows if it was on the page.
void Ui::Form::padPage(int step)
{
    const int from = TabCtrl_GetCurSel(m_tabs);
    const int to = std::clamp(from + step, 0, static_cast<int>(m_pages.size()) - 1);
    if (from < 0 || to == from)
    {
        return;
    }
    const HWND focus = GetFocus();
    const bool onPage = focus && IsChild(m_pages[from].hwnd, focus);
    TabCtrl_SetCurSel(m_tabs, to);
    showPage(to);
    if (onPage)
    {
        const HWND first = GetNextDlgTabItem(m_pages[to].hwnd, nullptr, FALSE);
        focusOn(first ? first : m_tabs);
    }
}

void Ui::Form::focusOn(HWND control)
{
    SendMessageW(m_dlg, WM_NEXTDLGCTL, reinterpret_cast<WPARAM>(control), TRUE);
}

std::wstring Ui::Form::read(const Control& c) const
{
    if (!c.m_hwnd)
    {
        return c.m_current;
    }
    switch (c.m_type)
    {
    case Control::Type::Check:
        return SendMessageW(c.m_hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED ? c.m_on : c.m_off;
    case Control::Type::Combo:
    {
        const LRESULT index = SendMessageW(c.m_hwnd, CB_GETCURSEL, 0, 0);
        return index >= 0 && static_cast<size_t>(index) < c.m_choices.size() ? c.m_choices[index].value : L"";
    }
    case Control::Type::Edit:
    {
        std::wstring text(GetWindowTextLengthW(c.m_hwnd), L'\0');
        GetWindowTextW(c.m_hwnd, text.data(), static_cast<int>(text.size() + 1));
        const size_t first = text.find_first_not_of(L' ');
        return first == std::wstring::npos ? L"" : text.substr(first, text.find_last_not_of(L' ') - first + 1);
    }
    default:
        return L"";
    }
}

void Ui::Form::write(Control& c, const std::wstring& value)
{
    if (!c.m_hwnd)
    {
        c.m_current = value;
        return;
    }
    switch (c.m_type)
    {
    case Control::Type::Check:
        SendMessageW(c.m_hwnd, BM_SETCHECK, value == c.m_on ? BST_CHECKED : BST_UNCHECKED, 0);
        break;
    case Control::Type::Combo:
        for (size_t i = 0; i < c.m_choices.size(); ++i)
        {
            if (c.m_choices[i].value == value)
            {
                SendMessageW(c.m_hwnd, CB_SETCURSEL, i, 0);
            }
        }
        break;
    case Control::Type::Edit:
        SetWindowTextW(c.m_hwnd, value.c_str());
        break;
    default:
        break;
    }
}

std::vector<const Ui::Control*> Ui::Form::bound(const wchar_t* section, const wchar_t* name) const
{
    std::vector<const Control*> controls;
    for (const Control& c : m_controls)
    {
        if (c.m_key.section && c.m_key.name && _wcsicmp(c.m_key.section, section) == 0 && _wcsicmp(c.m_key.name, name) == 0)
        {
            controls.push_back(&c);
        }
    }
    return controls;
}

std::wstring Ui::Form::value(const wchar_t* section, const wchar_t* name) const
{
    const std::vector<const Control*> controls = bound(section, name);
    if (controls.size() == 1 && controls[0]->m_key.part < 0)
    {
        return read(*controls[0]);
    }
    std::vector<std::wstring> parts;
    for (const Control* c : controls)
    {
        const size_t part = static_cast<size_t>(std::max(c->m_key.part, 0));
        parts.resize(std::max(parts.size(), part + 1));
        parts[part] = read(*c);
    }
    return join(parts);
}

bool Ui::Form::on(const wchar_t* section, const wchar_t* name) const
{
    const std::vector<const Control*> controls = bound(section, name);
    if (controls.size() == 1 && controls[0]->m_type == Control::Type::Check)
    {
        return read(*controls[0]) == controls[0]->m_on;
    }
    const std::wstring v = value(section, name);
    return !v.empty() && v != L"0";
}

void Ui::Form::set(const wchar_t* section, const wchar_t* name, const std::wstring& value)
{
    const std::vector<std::wstring> parts = split(value);
    for (const Control* boundControl : bound(section, name))
    {
        Control& c = const_cast<Control&>(*boundControl);
        if (c.m_key.part < 0)
        {
            write(c, value);
        }
        else if (static_cast<size_t>(c.m_key.part) < parts.size())
        {
            write(c, parts[c.m_key.part]);
        }
    }
    if (m_dlg)
    {
        updateEnabled();
    }
}

std::vector<Ui::Change> Ui::Form::changes() const
{
    std::vector<Change> changes;
    std::vector<const Control*> done;
    for (const Control& first : m_controls)
    {
        if (!first.m_key.name || std::ranges::find(done, &first) != done.end())
        {
            continue;
        }
        const std::vector<const Control*> controls = bound(first.m_key.section, first.m_key.name);
        done.insert(done.end(), controls.begin(), controls.end());
        std::vector<std::wstring> before, now;
        for (const Control* c : controls)
        {
            const size_t part = static_cast<size_t>(std::max(c->m_key.part, 0));
            before.resize(std::max(before.size(), part + 1));
            now.resize(std::max(now.size(), part + 1));
            before[part] = c->m_initial;
            now[part] = read(*c);
        }
        // A comma-separated name: each key gets its part of the value.
        const std::vector<std::wstring> names = split(first.m_key.name);
        if (names.size() > 1)
        {
            before = split(join(before));
            now = split(join(now));
            before.resize(names.size());
            now.resize(names.size());
            for (size_t i = 0; i < names.size(); ++i)
            {
                if (before[i] != now[i])
                {
                    changes.push_back({first.m_key.section, names[i], now[i]});
                }
            }
        }
        else if (join(before) != join(now))
        {
            changes.push_back({first.m_key.section, first.m_key.name, join(now)});
        }
    }
    return changes;
}

INT_PTR CALLBACK Ui::Form::dialogProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_INITDIALOG)
    {
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
    }
    auto* form = reinterpret_cast<Form*>(GetWindowLongPtrW(dlg, DWLP_USER));
    if (!form)
    {
        return FALSE;
    }
    // The pages are created while the window initializes.
    const bool page = form->m_dlg && dlg != form->m_dlg;
    switch (msg)
    {
    case WM_INITDIALOG:
        if (page)
        {
            // The tab control's themed background behind the page.
            using EnableThemeDialogTextureFn = HRESULT(WINAPI*)(HWND, DWORD);
            if (auto enable = reinterpret_cast<EnableThemeDialogTextureFn>(
                    GetProcAddress(GetModuleHandleW(L"uxtheme.dll"), "EnableThemeDialogTexture")))
            {
                enable(dlg, ETDT_ENABLETAB);
            }
            return FALSE;
        }
        form->init(dlg);
        return TRUE;
    case WM_NOTIFY:
        if (const auto* header = reinterpret_cast<const NMHDR*>(lp); header->idFrom == kTabsId && header->code == TCN_SELCHANGE)
        {
            form->showPage(TabCtrl_GetCurSel(form->m_tabs));
            return TRUE;
        }
        break;
    case WM_TIMER:
        if (!page && wp == kGamepadTimer)
        {
            form->gamepad();
            return TRUE;
        }
        break;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)
        {
            form->finish(LOWORD(wp));
        }
        else
        {
            form->command(LOWORD(wp));
        }
        return TRUE;
    }
    return FALSE;
}

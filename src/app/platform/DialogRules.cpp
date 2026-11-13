#include "DialogRules.h"

#include "Win32Util.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>

#include <ole2.h> // before uiautomation.h: WIN32_LEAN_AND_MEAN leaves it out of windows.h

#include <uiautomation.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <concepts>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

using Microsoft::WRL::ComPtr;
using namespace std::chrono_literals;
using namespace Qt::StringLiterals;

namespace ws::dialogrules {

namespace {

std::mutex g_mutex;
std::shared_ptr<const std::vector<Rule>> g_rules = std::make_shared<const std::vector<Rule>>();

std::shared_ptr<const std::vector<Rule>> rules()
{
    const std::scoped_lock lock(g_mutex);
    return g_rules;
}

struct TypeName {
    const wchar_t* name;
    CONTROLTYPEID id;
};
constexpr TypeName kTypes[] {
    {L"edit", UIA_EditControlTypeId},
    {L"combobox", UIA_ComboBoxControlTypeId},
    {L"button", UIA_ButtonControlTypeId},
};

std::optional<CONTROLTYPEID> typeId(std::wstring_view name)
{
    const auto it = std::ranges::find_if(kTypes, [&](const TypeName& t) { return name == t.name; });
    return it != std::end(kTypes) ? std::optional(it->id) : std::nullopt;
}

std::wstring typeName(CONTROLTYPEID id)
{
    const auto it = std::ranges::find(kTypes, id, &TypeName::id);
    return it != std::end(kTypes) ? it->name : L"";
}

std::wstring lower(std::wstring text)
{
    if (!text.empty())
        ::CharLowerBuffW(text.data(), static_cast<DWORD>(text.size()));
    return text;
}

std::wstring programOf(HWND window)
{
    DWORD pid = 0;
    ::GetWindowThreadProcessId(window, &pid);
    const win32::UniqueHandle process(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    wchar_t image[1024] {};
    DWORD length = static_cast<DWORD>(std::size(image));
    if (!process.valid() || !::QueryFullProcessImageNameW(process.get(), 0, image, &length))
        return {};
    const std::wstring_view path(image, length);
    return lower(std::wstring(path.substr(path.find_last_of(L'\\') + 1)));
}

std::wstring classOf(HWND window)
{
    wchar_t name[256] {};
    return {name, static_cast<std::size_t>(std::max(::GetClassNameW(window, name, static_cast<int>(std::size(name))), 0))};
}

// The title without a message to the window: a busy program cannot stall this.
std::wstring titleOf(HWND window)
{
    wchar_t text[512] {};
    return {text, static_cast<std::size_t>(std::max(::InternalGetWindowText(window, text, static_cast<int>(std::size(text))), 0))};
}

// The class first: read without opening the program, and most windows stop there.
const Rule* ruleFor(const std::vector<Rule>& all, HWND window)
{
    if (all.empty() || !window)
        return nullptr;
    const std::wstring windowClass = classOf(window);
    std::optional<std::wstring> program;
    for (const Rule& rule : all) {
        if (!matches(rule.windowClass, windowClass))
            continue;
        if (!program)
            program = programOf(window);
        if (rule.program == *program && matches(rule.title, titleOf(window)))
            return &rule;
    }
    return nullptr;
}

bool isFolder(const std::wstring& path)
{
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool waitFor(std::predicate auto done, std::chrono::milliseconds limit)
{
    const auto end = std::chrono::steady_clock::now() + limit;
    while (!done()) {
        if (std::chrono::steady_clock::now() >= end)
            return false;
        std::this_thread::sleep_for(10ms);
    }
    return true;
}

struct Bstr {
    BSTR text = nullptr;
    Bstr() = default;
    explicit Bstr(std::wstring_view value) : text(::SysAllocStringLen(value.data(), static_cast<UINT>(value.size()))) {}
    ~Bstr() { ::SysFreeString(text); }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
    std::wstring str() const { return text ? std::wstring(text, ::SysStringLen(text)) : std::wstring(); }
};

using Element = ComPtr<IUIAutomationElement>;

// The window through UI Automation, made for each use: given up on soon
// should the program hang.
class Window {
public:
    explicit Window(HWND hwnd) : m_hwnd(hwnd)
    {
        if (FAILED(::CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_uia)))
            && FAILED(::CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_uia))))
            return;
        if (ComPtr<IUIAutomation2> timed; SUCCEEDED(m_uia.As(&timed))) {
            timed->put_ConnectionTimeout(1500);
            timed->put_TransactionTimeout(1500);
        }
        m_uia->ElementFromHandle(hwnd, &m_root);
    }

    explicit operator bool() const { return m_root != nullptr; }

    // Every control of the window that fits, in the order UI Automation walks them.
    std::vector<Element> all(const Selector& selector) const
    {
        ComPtr<IUIAutomationCondition> condition;
        m_uia->CreateTrueCondition(&condition);
        const auto narrow = [&](PROPERTYID property, VARIANT value) {
            ComPtr<IUIAutomationCondition> one, both;
            if (SUCCEEDED(m_uia->CreatePropertyCondition(property, value, &one))
                && SUCCEEDED(m_uia->CreateAndCondition(condition.Get(), one.Get(), &both)))
                condition = both;
        };
        const auto text = [&](PROPERTYID property, const std::wstring& wanted) {
            if (wanted.empty())
                return;
            const Bstr value(wanted);
            VARIANT v {};
            v.vt = VT_BSTR;
            v.bstrVal = value.text;
            narrow(property, v);
        };
        if (const auto type = typeId(selector.type)) {
            VARIANT v {};
            v.vt = VT_I4;
            v.lVal = *type;
            narrow(UIA_ControlTypePropertyId, v);
        }
        text(UIA_AutomationIdPropertyId, selector.id);
        text(UIA_NamePropertyId, selector.name);
        text(UIA_ClassNamePropertyId, selector.className);
        std::vector<Element> found;
        ComPtr<IUIAutomationElementArray> array;
        int count = 0;
        if (!condition || FAILED(m_root->FindAll(TreeScope_Descendants, condition.Get(), &array)) || !array
            || FAILED(array->get_Length(&count)))
            return found;
        for (int i = 0; i < count; ++i) {
            if (Element e; SUCCEEDED(array->GetElement(i, &e)) && e)
                found.push_back(std::move(e));
        }
        return found;
    }

    Element find(const Selector& selector) const
    {
        if (!selector.given() || selector.index < 0)
            return nullptr;
        std::vector<Element> found = all(selector);
        return static_cast<std::size_t>(selector.index) < found.size() ? found[static_cast<std::size_t>(selector.index)] : nullptr;
    }

    // What has the keyboard focus, if in this window's program.
    Element focused() const
    {
        Element element;
        int process = 0;
        int ours = 0;
        if (FAILED(m_uia->GetFocusedElement(&element)) || !element || FAILED(element->get_CurrentProcessId(&process))
            || FAILED(m_root->get_CurrentProcessId(&ours)) || process != ours)
            return nullptr;
        return element;
    }

    bool same(IUIAutomationElement* a, IUIAutomationElement* b) const
    {
        BOOL equal = FALSE;
        return a && b && SUCCEEDED(m_uia->CompareElements(a, b, &equal)) && equal;
    }

    bool hasFocus(IUIAutomationElement* element) const { return same(focused().Get(), element); }

    // Enter for `box`, which must hold the focus: to the box's own window
    // where it has one (a Win32 dialog's message loop then presses the
    // default button), else to the window (WPF and Qt hand it to the control
    // with the focus).
    bool enter(IUIAutomationElement* box) const
    {
        box->SetFocus();
        if (!waitFor([&] { return hasFocus(box); }, 500ms))
            return false;
        UIA_HWND native = nullptr;
        box->get_CurrentNativeWindowHandle(&native);
        const HWND target = native ? static_cast<HWND>(native) : m_hwnd;
        return ::PostMessageW(target, WM_KEYDOWN, VK_RETURN, 0x001C0001) && ::PostMessageW(target, WM_KEYUP, VK_RETURN, 0xC01C0001);
    }

private:
    HWND m_hwnd;
    ComPtr<IUIAutomation> m_uia;
    Element m_root;
};

template <typename Getter>
std::wstring text(IUIAutomationElement* element, Getter get)
{
    Bstr value;
    return element && SUCCEEDED((element->*get)(&value.text)) ? value.str() : std::wstring();
}

ComPtr<IUIAutomationValuePattern> valueOf(IUIAutomationElement* element)
{
    ComPtr<IUIAutomationValuePattern> pattern;
    if (element)
        element->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&pattern));
    return pattern;
}

std::wstring valueText(IUIAutomationElement* element)
{
    Bstr value;
    const ComPtr<IUIAutomationValuePattern> pattern = valueOf(element);
    return pattern && SUCCEEDED(pattern->get_CurrentValue(&value.text)) ? value.str() : std::wstring();
}

// Takes text typed by a user: there, enabled, and not read-only.
bool takesText(IUIAutomationElement* element)
{
    BOOL enabled = FALSE;
    BOOL readOnly = TRUE;
    const ComPtr<IUIAutomationValuePattern> pattern = valueOf(element);
    return pattern && SUCCEEDED(element->get_CurrentIsEnabled(&enabled)) && enabled
        && SUCCEEDED(pattern->get_CurrentIsReadOnly(&readOnly)) && !readOnly;
}

bool setValue(IUIAutomationElement* element, const std::wstring& value)
{
    const ComPtr<IUIAutomationValuePattern> pattern = valueOf(element);
    const Bstr typed(value);
    return pattern && SUCCEEDED(pattern->SetValue(typed.text)) && valueText(element) == value;
}

bool invoke(IUIAutomationElement* element)
{
    ComPtr<IUIAutomationInvokePattern> pattern;
    BOOL enabled = FALSE;
    return element && SUCCEEDED(element->get_CurrentIsEnabled(&enabled)) && enabled
        && SUCCEEDED(element->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&pattern))) && pattern
        && SUCCEEDED(pattern->Invoke());
}

// The rule's box and how it is confirmed, found anew for each use (the
// window may have rebuilt its controls).
struct Target {
    Rule rule;
    Window window;
    Element box;

    explicit Target(HWND hwnd) : window(hwnd)
    {
        const auto all = rules();
        if (const Rule* found = ruleFor(*all, hwnd); found && window) {
            rule = *found;
            // The boxes may come a moment after the window is in front.
            waitFor([&] { return (box = window.find(rule.box)) != nullptr; }, 500ms);
        }
    }

    explicit operator bool() const { return box != nullptr; }

    bool confirm() const
    {
        if (!rule.confirm.given())
            return window.enter(box.Get());
        const Element button = window.find(rule.confirm);
        return button && invoke(button.Get());
    }
};

Selector selectorFrom(const QJsonValue& value)
{
    const QJsonObject object = value.toObject();
    Selector s;
    s.type = object.value("type"_L1).toString().toLower().toStdWString();
    s.id = object.value("id"_L1).toVariant().toString().toStdWString(); // a number reads as well
    s.name = object.value("name"_L1).toString().toStdWString();
    s.className = object.value("class"_L1).toString().toStdWString();
    s.index = object.value("index"_L1).toInt();
    return s;
}

QJsonObject toJson(const Selector& s)
{
    QJsonObject object;
    if (!s.type.empty())
        object.insert("type"_L1, QString::fromStdWString(s.type));
    if (!s.id.empty())
        object.insert("id"_L1, QString::fromStdWString(s.id));
    if (!s.name.empty())
        object.insert("name"_L1, QString::fromStdWString(s.name));
    if (!s.className.empty())
        object.insert("class"_L1, QString::fromStdWString(s.className));
    if (s.index)
        object.insert("index"_L1, s.index);
    return object;
}

constexpr std::pair<QLatin1StringView, filedialog::Kind> kKinds[] {
    {"open"_L1, filedialog::Kind::Open},
    {"save"_L1, filedialog::Kind::Save},
    {"folder"_L1, filedialog::Kind::Folder},
};

// "edit  id=1152  name=名称:  class=Edit": one control, for the user to pick from.
QString line(IUIAutomationElement* element)
{
    CONTROLTYPEID type = 0;
    element->get_CurrentControlType(&type);
    QString out = QString::fromStdWString(typeName(type));
    const auto add = [&](QLatin1StringView key, const std::wstring& value) {
        if (!value.empty())
            out += "  "_L1 + key + u'=' + QString::fromStdWString(value);
    };
    add("id"_L1, text(element, &IUIAutomationElement::get_CurrentAutomationId));
    add("name"_L1, text(element, &IUIAutomationElement::get_CurrentName));
    add("class"_L1, text(element, &IUIAutomationElement::get_CurrentClassName));
    return out;
}

// The shortest description that finds `element` again: its ID, else its
// name, else its class, with its place among the ones alike.
Selector selectorFor(const Window& window, IUIAutomationElement* element)
{
    CONTROLTYPEID type = 0;
    element->get_CurrentControlType(&type);
    Selector by;
    by.type = typeName(type);
    const auto resolves = [&](Selector& s) {
        const std::vector<Element> alike = window.all(s);
        const auto it = std::ranges::find_if(alike, [&](const Element& e) { return window.same(e.Get(), element); });
        if (it == alike.end())
            return false;
        s.index = static_cast<int>(it - alike.begin());
        return true;
    };
    Selector s = by;
    s.id = text(element, &IUIAutomationElement::get_CurrentAutomationId);
    if (!s.id.empty() && resolves(s))
        return s;
    s = by;
    s.name = text(element, &IUIAutomationElement::get_CurrentName);
    if (!s.name.empty() && resolves(s))
        return s;
    s = by;
    s.className = text(element, &IUIAutomationElement::get_CurrentClassName);
    return resolves(s) ? s : Selector {};
}

// A window class as a rule should name it: frameworks put a part in it that
// changes from run to run (WPF's "HwndWrapper[App;;<guid>]", WinForms'
// "WindowsForms10.Window.8.app.0.<hash>").
std::wstring lastingClass(const std::wstring& windowClass)
{
    if (windowClass.starts_with(L"HwndWrapper[")) {
        const std::size_t end = windowClass.find(L';');
        return windowClass.substr(0, end) + L'*';
    }
    if (constexpr std::wstring_view forms = L"WindowsForms10."; windowClass.starts_with(forms)) {
        const std::size_t end = windowClass.find(L".app.");
        return end == std::wstring::npos ? windowClass : windowClass.substr(0, end) + L".app.*";
    }
    return windowClass;
}

} // namespace

bool matches(std::wstring_view pattern, std::wstring_view text)
{
    if (pattern.empty())
        return true;
    // '*' takes any run of characters; the rest compares as Windows compares names.
    const auto equal = [](std::wstring_view a, std::wstring_view b) {
        return ::CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
    };
    std::size_t star = pattern.find(L'*');
    if (star == std::wstring_view::npos)
        return equal(pattern, text);
    if (text.size() < star || !equal(pattern.substr(0, star), text.substr(0, star)))
        return false;
    text.remove_prefix(star);
    pattern.remove_prefix(star + 1);
    while ((star = pattern.find(L'*')) != std::wstring_view::npos) {
        const std::wstring_view piece = pattern.substr(0, star);
        bool found = false;
        for (std::size_t at = 0; at + piece.size() <= text.size(); ++at) {
            if (equal(piece, text.substr(at, piece.size()))) {
                text.remove_prefix(at + piece.size());
                found = true;
                break;
            }
        }
        if (!found)
            return false;
        pattern.remove_prefix(star + 1);
    }
    return text.size() >= pattern.size() && equal(pattern, text.substr(text.size() - pattern.size()));
}

std::vector<Rule> parse(const QByteArray& json, QStringList* problems)
{
    std::vector<Rule> out;
    const auto problem = [&](const QString& text) {
        if (problems)
            problems->append(text);
    };
    QJsonParseError error {};
    const QJsonDocument document = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError) {
        if (!json.trimmed().isEmpty())
            problem(u"dialogs.json: %1 (offset %2)"_s.arg(error.errorString()).arg(error.offset));
        return out;
    }
    const QJsonArray entries = document.object().value("dialogs"_L1).toArray();
    for (qsizetype i = 0; i < entries.size(); ++i) {
        const QJsonObject entry = entries.at(i).toObject();
        Rule rule;
        rule.name = entry.value("name"_L1).toString().toStdWString();
        rule.program = lower(entry.value("program"_L1).toString().trimmed().toStdWString());
        rule.windowClass = entry.value("class"_L1).toString().toStdWString();
        rule.title = entry.value("title"_L1).toString().toStdWString();
        rule.box = selectorFrom(entry.value("box"_L1));
        if (entry.value("confirm"_L1).isObject())
            rule.confirm = selectorFrom(entry.value("confirm"_L1));
        const QString kind = entry.value("kind"_L1).toString(u"open"_s).toLower();
        const auto known = std::ranges::find(kKinds, kind, [](const auto& k) { return QString(k.first); });
        const QString label = u"dialogs.json #%1"_s.arg(i + 1);
        if (rule.program.empty() || rule.program.find_first_of(L"\\/") != std::wstring::npos) {
            problem(label + u": \"program\" must be a program's file name (ArcMap.exe)"_s);
        } else if (!rule.box.given()) {
            problem(label + u": \"box\" must say which box takes the path"_s);
        } else if (known == std::end(kKinds)) {
            problem(label + u": \"kind\" must be open, save or folder"_s);
        } else {
            rule.kind = known->second;
            out.push_back(std::move(rule));
        }
    }
    return out;
}

QJsonObject toJson(const Rule& rule)
{
    QJsonObject entry;
    if (!rule.name.empty())
        entry.insert("name"_L1, QString::fromStdWString(rule.name));
    entry.insert("program"_L1, QString::fromStdWString(rule.program));
    if (!rule.windowClass.empty())
        entry.insert("class"_L1, QString::fromStdWString(rule.windowClass));
    if (!rule.title.empty())
        entry.insert("title"_L1, QString::fromStdWString(rule.title));
    entry.insert("kind"_L1, QString(std::ranges::find(kKinds, rule.kind, [](const auto& k) { return k.second; })->first));
    entry.insert("box"_L1, toJson(rule.box));
    entry.insert("confirm"_L1, rule.confirm.given() ? QJsonValue(toJson(rule.confirm)) : QJsonValue(u"enter"_s));
    return entry;
}

void setRules(std::vector<Rule> next)
{
    auto shared = std::make_shared<const std::vector<Rule>>(std::move(next));
    const std::scoped_lock lock(g_mutex);
    g_rules = std::move(shared);
}

bool any()
{
    return !rules()->empty();
}

bool isOne(HWND window)
{
    const auto all = rules();
    return ruleFor(*all, window) != nullptr;
}

filedialog::Kind kind(HWND window)
{
    const auto all = rules();
    const Rule* rule = ruleFor(*all, window);
    return rule ? rule->kind : filedialog::Kind::None;
}

bool goTo(HWND window, const std::wstring& folder)
{
    if (!isFolder(folder))
        return false; // gone (or a file): nothing is typed anywhere
    const Target target(window);
    if (!target)
        return false;
    // With the trailing backslash it names a folder or nothing: never a file
    // the window would open, or a name it would save under.
    const std::wstring before = valueText(target.box.Get());
    const std::wstring typed = folder.ends_with(L'\\') ? folder : folder + L'\\';
    if (!setValue(target.box.Get(), typed) || !target.confirm())
        return false;
    // Gone there, most windows empty the box themselves. One that leaves the
    // folder in it gets back the name it held.
    if (!waitFor([&] { return valueText(target.box.Get()) != typed; }, 1000ms) && ::IsWindow(window))
        setValue(target.box.Get(), before);
    return ::IsWindow(window) != FALSE;
}

bool setFileName(HWND window, const std::wstring& path)
{
    const Target target(window);
    if (!target || !setValue(target.box.Get(), path))
        return false;
    target.box->SetFocus();
    return true;
}

bool accept(HWND window)
{
    const Target target(window);
    return target && target.confirm();
}

Seen describe(HWND window)
{
    Seen seen;
    const Window uia(window);
    const std::wstring program = programOf(window);
    if (!uia || program.empty())
        return seen;
    Selector kindOf;
    Element lowest;
    // A window just shown tells its controls a moment later (WPF builds
    // them when first asked).
    waitFor([&] {
        long lowestTop = 0;
        seen.boxes.clear();
        for (const wchar_t* type : {L"edit", L"combobox"}) {
            kindOf.type = type;
            for (const Element& e : uia.all(kindOf)) {
                if (!takesText(e.Get()))
                    continue;
                seen.boxes.append(line(e.Get()));
                RECT bounds {};
                if (SUCCEEDED(e->get_CurrentBoundingRectangle(&bounds)) && (!lowest || bounds.top > lowestTop)) {
                    lowest = e;
                    lowestTop = bounds.top;
                }
            }
        }
        return lowest != nullptr;
    }, 1500ms);
    kindOf.type = L"button";
    for (const Element& e : uia.all(kindOf)) {
        if (!text(e.Get(), &IUIAutomationElement::get_CurrentName).empty())
            seen.buttons.append(line(e.Get()));
    }
    const Element focus = uia.focused();
    const Element box = focus && takesText(focus.Get()) ? focus : lowest;
    if (!box)
        return seen;
    seen.rule.box = selectorFor(uia, box.Get());
    if (!seen.rule.box.given())
        return seen;
    seen.rule.program = program;
    seen.rule.windowClass = lastingClass(classOf(window));
    seen.rule.title = titleOf(window);
    seen.rule.name = seen.rule.title.empty() ? program : seen.rule.title;
    return seen;
}

} // namespace ws::dialogrules

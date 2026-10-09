// PowerPoint 97–2003 (.ppt, and WPS's .dps saved in it), by [MS-PPT]: the
// "PowerPoint Document" stream is a tree of records. The text of each
// slide's placeholders (title, body) is in the slide list (SlideListWithText,
// slide by slide); that of other text boxes and of the notes is in the
// slides' and notes' own records, read after it with no slide number.

#include "Cfb.h"
#include "Formats.h"

#include <cstring>

namespace ws::extract {

namespace {

using doctext::PlaceKind;

constexpr std::size_t kMaxStream = 512u << 20;
constexpr int kMaxDepth = 32;

enum : std::uint16_t {
    kSlide = 0x03EE,
    kNotes = 0x03F0,
    kSlidePersist = 0x03F3,
    kMainMaster = 0x03F8,
    kTextChars = 0x0FA0, // UTF-16
    kTextBytes = 0x0FA8, // the low bytes of UTF-16 (Latin-1)
    kSlideList = 0x0FF0,
    kHandout = 0x0FC9,
};

class Reader {
public:
    explicit Reader(Writer& out)
        : m_out(out)
    {
    }

    enum class Where { Other, SlideList, Shapes };

    void walk(std::string_view data, int depth, Where where)
    {
        std::size_t pos = 0;
        while (pos + 8 <= data.size() && !m_out.full()) {
            std::uint16_t verInstance, type;
            std::uint32_t length;
            std::memcpy(&verInstance, data.data() + pos, 2);
            std::memcpy(&type, data.data() + pos + 2, 2);
            std::memcpy(&length, data.data() + pos + 4, 4);
            const std::size_t room = data.size() - pos - 8;
            const std::string_view body = data.substr(pos + 8, std::min<std::size_t>(length, room));
            pos += 8 + body.size();
            if ((verInstance & 0x0F) == 0x0F) { // a container
                if (depth >= kMaxDepth || type == kMainMaster || type == kHandout)
                    continue;
                Where inner = where;
                if (type == kSlideList) {
                    if ((verInstance >> 4) != 0)
                        continue; // the masters' or the notes' list
                    inner = Where::SlideList;
                } else if (type == kSlide || type == kNotes) {
                    inner = Where::Shapes;
                    if (!m_shapes) {
                        m_shapes = true;
                        m_out.place(PlaceKind::None, 0);
                    }
                }
                walk(body, depth + 1, inner);
                m_out.newline();
                continue;
            }
            if (where == Where::SlideList && type == kSlidePersist) {
                m_out.place(PlaceKind::Slide, ++m_slide);
            } else if (where != Where::Other && type == kTextChars) {
                for (std::size_t k = 0; k + 1 < body.size(); k += 2) {
                    char16_t c;
                    std::memcpy(&c, body.data() + k, 2);
                    put(c);
                }
                m_out.newline();
            } else if (where != Where::Other && type == kTextBytes) {
                for (const char b : body)
                    put(static_cast<unsigned char>(b));
                m_out.newline();
            }
        }
    }

private:
    void put(char16_t c)
    {
        if (c == 0x0D)
            m_out.newline(); // paragraph end
        else if (c == 0x0B)
            m_out.space(); // line break
        else
            m_out.put(std::u16string_view(&c, 1));
    }

    Writer& m_out;
    std::uint32_t m_slide = 0;
    bool m_shapes = false;
};

} // namespace

Status extractPpt(Cfb& cfb, Writer& out)
{
    std::string stream;
    if (!cfb.read(u"PowerPoint Document", stream, kMaxStream))
        return Status::Broken;
    Reader reader(out);
    reader.walk(stream, 0, Reader::Where::Other);
    out.newline();
    return Status::Ok;
}

} // namespace ws::extract

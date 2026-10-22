#include "game/tablefile.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace bm::tablefile
{
    namespace
    {
        uint64_t ReadKey(const uint8_t* p, unsigned size)
        {
            uint64_t v = 0;
            memcpy(&v, p, size);
            return v;
        }

        template <typename T> void Put(std::string& s, T v) { s.append(reinterpret_cast<const char*>(&v), sizeof v); }
    }

    uint32_t Crc32(const void* data, size_t n)
    {
        static uint32_t table[256];
        static bool built = false;
        if (!built)
        {
            for (uint32_t i = 0; i < 256; ++i)
            {
                uint32_t c = i;
                for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                table[i] = c;
            }
            built = true;
        }
        const uint8_t* p = static_cast<const uint8_t*>(data);
        uint32_t c = 0xFFFFFFFFu;
        for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
        return c ^ 0xFFFFFFFFu;
    }

    bool Table::Parse(const std::string& header, const std::string& body, std::string& why)
    {
        *this = Table();
        const uint8_t* h = reinterpret_cast<const uint8_t*>(header.data());
        const size_t hn = header.size();
        if (hn < 2)
        {
            why = "the header is under two bytes";
            return false;
        }
        std::vector<std::pair<Key, uint32_t>> entries;
        const uint16_t count16 = static_cast<uint16_t>(h[0] | h[1] << 8);
        unsigned width = 0;
        if (count16 && (hn - 2) % count16 == 0) width = static_cast<unsigned>((hn - 2) / count16) - 4;
        if (width == 1 || width == 2 || width == 4 || width == 8)
        {
            keySize_ = width;
            for (size_t i = 0; i < count16; ++i)
            {
                const uint8_t* e = h + 2 + i * (width + 4);
                uint32_t off;
                memcpy(&off, e + width, 4);
                entries.push_back({ Key{ ReadKey(e, width), 0 }, off });
            }
        }
        else
        {
            uint32_t count32 = 0;
            if (hn >= 4) memcpy(&count32, h, 4);
            if (!count32 || hn - 4 != static_cast<size_t>(count32) * 12)
            {
                char text[120];
                snprintf(text, sizeof text, "a header of %zu bytes fits neither a u16 count of %u nor 12-byte entries",
                         hn, count16);
                why = text;
                return false;
            }
            wide_ = true;
            keySize_ = 4;
            for (size_t i = 0; i < count32; ++i)
            {
                const uint8_t* e = h + 4 + i * 12;
                uint32_t k, off;
                int32_t t;
                memcpy(&k, e, 4);
                memcpy(&t, e + 4, 4);
                memcpy(&off, e + 8, 4);
                entries.push_back({ Key{ k, t }, off });
            }
        }

        // A record runs from its offset to the next distinct one.
        std::vector<uint32_t> offs;
        offs.reserve(entries.size() + 1);
        for (const auto& e : entries) offs.push_back(e.second);
        std::sort(offs.begin(), offs.end());
        offs.erase(std::unique(offs.begin(), offs.end()), offs.end());
        if (!offs.empty() && offs.back() > body.size())
        {
            why = "a header offset runs past the end of the body";
            return false;
        }
        offs.push_back(static_cast<uint32_t>(body.size()));

        keys_.reserve(entries.size());
        records_.reserve(entries.size());
        std::vector<std::pair<uint32_t, size_t>> byOffset;
        byOffset.reserve(entries.size());
        for (size_t row = 0; row < entries.size(); ++row)
        {
            const uint32_t off = entries[row].second;
            // The last record's offset may equal body.size(), an empty record
            // at the very end. Nothing in `offs` is larger than it then, so
            // upper_bound returns end() and the sentinel is the answer.
            const auto next = std::upper_bound(offs.begin(), offs.end(), off);
            const uint32_t end = next == offs.end() ? static_cast<uint32_t>(body.size()) : *next;
            if (end < off)
            {
                why = "a header offset runs past the end of the body";
                return false;
            }
            keys_.push_back(entries[row].first);
            records_.emplace_back(body.data() + off, end - off);
            byOffset.push_back({ off, row });
            index_.emplace(Slot(entries[row].first.key, entries[row].first.tag), row);
        }
        std::stable_sort(byOffset.begin(), byOffset.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& b : byOffset) bodyOrder_.push_back(b.second);

        std::string h2, b2;
        if (!Build(h2, b2, why)) return false;
        if (h2 != header || b2 != body)
        {
            why = "it does not write back byte for byte (a duplicate key or offset, or a layout this reader does not know)";
            return false;
        }
        return true;
    }

    bool Table::Build(std::string& header, std::string& body, std::string& why) const
    {
        header.clear();
        body.clear();
        std::vector<uint32_t> offs(keys_.size());
        size_t total = 0;
        for (const std::string& r : records_) total += r.size();
        body.reserve(total);
        for (size_t row : bodyOrder_)
        {
            if (body.size() > 0xFFFFFFFFull)
            {
                why = "the body outgrew a u32 offset";
                return false;
            }
            offs[row] = static_cast<uint32_t>(body.size());
            body += records_[row];
        }
        if (wide_)
        {
            header.reserve(4 + keys_.size() * 12);
            Put<uint32_t>(header, static_cast<uint32_t>(keys_.size()));
            for (size_t row = 0; row < keys_.size(); ++row)
            {
                Put<uint32_t>(header, static_cast<uint32_t>(keys_[row].key));
                Put<int32_t>(header, keys_[row].tag);
                Put<uint32_t>(header, offs[row]);
            }
            return true;
        }
        if (keys_.size() > 0xFFFF)
        {
            why = "more rows than a u16 count holds";
            return false;
        }
        header.reserve(2 + keys_.size() * (keySize_ + 4));
        Put<uint16_t>(header, static_cast<uint16_t>(keys_.size()));
        for (size_t row = 0; row < keys_.size(); ++row)
        {
            header.append(reinterpret_cast<const char*>(&keys_[row].key), keySize_);
            Put<uint32_t>(header, offs[row]);
        }
        return true;
    }

    int Table::RowOf(uint64_t key, int32_t tag) const
    {
        auto range = index_.equal_range(Slot(key, tag));
        for (auto it = range.first; it != range.second; ++it)
            if (keys_[it->second] == Key{ key, tag }) return static_cast<int>(it->second);
        return -1;
    }

    const std::string* Table::Record(uint64_t key, int32_t tag) const
    {
        const int row = RowOf(key, tag);
        return row < 0 ? nullptr : &records_[row];
    }

    bool Table::Set(uint64_t key, const std::string& record, int32_t tag)
    {
        const int row = RowOf(key, tag);
        if (row < 0) return false;
        records_[row] = record;
        return true;
    }

    bool Table::Append(uint64_t key, const std::string& record, int32_t tag)
    {
        if (RowOf(key, tag) >= 0) return false;
        if (!wide_ && keySize_ < 8 && key >> (keySize_ * 8)) return false;
        const size_t row = keys_.size();
        keys_.push_back(Key{ key, tag });
        records_.push_back(record);
        bodyOrder_.push_back(row);
        index_.emplace(Slot(key, tag), row);
        return true;
    }
}
